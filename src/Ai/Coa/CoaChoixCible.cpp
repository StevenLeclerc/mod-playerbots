/*
 * Conquest of Azeroth — l'oracle choisit qui frapper. Voir l'en-tete pour le
 * pourquoi, la regle d'acquisition et le contrat de repli.
 */

#include "CoaChoixCible.h"

#include "CoaLayaEtat.h"
#include "CoaLayaOracle.h"
#include "CoaRegistreCombat.h"
#include "CoaSpecLookup.h"

#include "AiObjectContext.h"
#include "Creature.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Unit.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    // L'oracle refuse au-dela de douze options ; on s'arrete la. Contrairement
    // au canal sort, ou la troncature cachait toujours les memes sorts faute de
    // classement, les candidats sont ici tries par distance avant d'etre
    // coupes : ce qu'on ecarte, ce sont les plus lointains.
    constexpr size_t MAX_CIBLES = 12;

    // Au-dela, ce n'est plus une cible de melee ni de sort : la portee la plus
    // longue du jeu tourne autour de 40 metres.
    constexpr float PORTEE_MAX = 45.0f;

    // L'attente d'une reponse, bornee. L'aller-retour mesure est de 35 ms au
    // P95 (P-085) : 400 ms couvrent un Mac charge sans laisser un bot plante
    // la sous les coups. Au-dela, le moteur reprend la main, et pendant le
    // repit qui suit on n'attend plus du tout : un oracle muet ne doit pas
    // retarder chaque acquisition de 400 ms.
    constexpr uint32 ATTENTE_MAX_MS = 400;
    constexpr uint32 REPIT_MS = 2000;

    // Ce qu'un candidat est, en une phrase que le modele lit. Que des faits :
    // ce qu'il faut en conclure est precisement la question qu'on lui pose.
    std::string Decrire(Player* bot, Unit* cible)
    {
        char tampon[192];
        int const vie = static_cast<int>(cible->GetHealthPct());
        int const distance = static_cast<int>(bot->GetExactDist2d(cible));
        size_t const assaillants = cible->getAttackers().size();

        if (Player* joueur = cible->ToPlayer())
        {
            CoaSpecStrategy const* spec = GetCoaSpecStrategyFor(joueur);
            char const* role = "joueur";
            if (spec)
                switch (spec->role)
                {
                    case CoaSpecRole::Tank: role = "tank"; break;
                    case CoaSpecRole::Heal: role = "soigneur"; break;
                    default:                role = "degats"; break;
                }
            snprintf(tampon, sizeof(tampon),
                     "Joueur %s %s, niveau %u, vie %d%%, a %d metres, %zu assaillants.%s",
                     spec ? spec->specName : "", role,
                     static_cast<unsigned>(cible->GetLevel()), vie, distance, assaillants,
                     cible->IsNonMeleeSpellCast(false) ? " Il incante." : "");
        }
        else
        {
            char const* nature = "Monstre";
            if (Creature* creature = cible->ToCreature())
            {
                if (creature->isWorldBoss())
                    nature = "Boss";
                else if (creature->isElite())
                    nature = "Monstre d'elite";
            }
            snprintf(tampon, sizeof(tampon),
                     "%s, niveau %u, vie %d%%, a %d metres, %zu assaillants.%s",
                     nature, static_cast<unsigned>(cible->GetLevel()), vie, distance,
                     assaillants,
                     cible->IsNonMeleeSpellCast(false) ? " Il incante." : "");
        }
        return std::string(tampon);
    }

    // Le libelle qui voyage sur le fil et sert de cle dans le cache. Le NOM ne
    // convient pas : deux bots peuvent le partager, et l'assainisseur tronque a
    // 48 caracteres. L'identifiant bas du GUID est court et se reapparie sans
    // ambiguite.
    //
    // LE PREFIXE N'EST PAS DECORATIF. `GetCounter()` n'est unique QUE dans son
    // type : un joueur et une creature peuvent porter le meme compteur, et
    // l'oracle rejette une requete qui contient deux options identiques — la
    // demande entiere serait perdue, en silence, uniquement quand un joueur et
    // un monstre de meme compteur sont candidats en meme temps. Un defaut rare,
    // donc un defaut qu'on ne trouve jamais.
    //
    // ET LE COMPTEUR NE SUFFIT PAS NON PLUS : un familier et une creature sont
    // tous deux TYPEID_UNIT, tires de generateurs differents, et peuvent donc
    // partager un compteur sous le meme prefixe. Le GUID brut, lui, est unique
    // tous types confondus ; le prefixe ne reste que pour la lisibilite.
    std::string Libelle(Unit* cible)
    {
        return (cible->GetTypeId() == TYPEID_PLAYER ? "j" : "c") +
               std::to_string(cible->GetGUID().GetRawValue());
    }
}

Unit* CoaChoisirCibleParLaya(PlayerbotAI* botAI, Player* bot, Unit* choixMoteur)
{
    // LE REPLI EST LE CHEMIN PAR DEFAUT. Chaque sortie de cette fonction avant
    // la fin rend la cible du moteur, telle quelle.
    if (!botAI || !bot || !choixMoteur)
        return choixMoteur;
    if (!sPlayerbotAIConfig.layaCibles)
        return choixMoteur;

    uint64 const guid = bot->GetGUID().GetRawValue();
    if (!sPlayerbotAIConfig.IsLayaElite(guid))
        return choixMoteur;

    // SEULEMENT A L'ACQUISITION. Voir l'en-tete : rearbitrer a chaque tour
    // ferait osciller la cible d'une seconde a l'autre sans jamais rien tuer.
    if (bot->GetVictim())
        return choixMoteur;

    // LE BRAS DE LA RENCONTRE. A 100 — le defaut — tout est pilote et on
    // n'interroge pas le registre. En dessous, hors rencontre ouverte il n'y a
    // pas de bras, donc ni decision ni attente : c'est le cas de l'acquisition
    // d'OUVERTURE, que le registre ne tire qu'une fois le combat engage
    // (P-133, limite connue). Le TEMOIN, lui, passe par tout ce qui suit — la
    // demande, l'attente — et ignore la reponse a la fin : les deux bras paient
    // le meme delai, et seule la decision les distingue.
    bool pilote = true;
    if (sPlayerbotAIConfig.layaTirageAuSort < 100)
    {
        int const bras = CoaRegistreCombat::Instance().Bras(guid);
        if (bras < 0)
            return choixMoteur;
        pilote = bras == 1;
    }

    CoaLayaOracle& oracle = CoaLayaOracle::Instance();
    if (!oracle.Actif())
        return choixMoteur;

    AiObjectContext* contexte = botAI->GetAiObjectContext();
    if (!contexte)
        return choixMoteur;

    /* ---------------------------------------------------------------- */
    /* Les candidats                                                     */
    /* ---------------------------------------------------------------- */

    // LE CHOIX DU MOTEUR EST TOUJOURS DANS LA LISTE, et en premier. Sans lui,
    // « garder ce que le moteur a decide » ne serait pas exprimable : l'oracle
    // serait force de changer quelque chose, ce qui n'est pas une decision.
    std::vector<Unit*> candidats;
    candidats.push_back(choixMoteur);

    if (Value<GuidVector>* v = contexte->GetValue<GuidVector>("possible targets"))
    {
        for (ObjectGuid const& id : v->Get())
        {
            Unit* unite = botAI->GetUnit(id);
            if (!unite || unite == choixMoteur || !unite->IsAlive() || !unite->IsInWorld())
                continue;
            // On ne propose que ce que le bot pourrait reellement frapper : un
            // candidat refuse par le coeur ferait perdre le tour au lieu de
            // changer la cible.
            if (!bot->IsValidAttackTarget(unite))
                continue;
            if (bot->GetExactDist2d(unite) > PORTEE_MAX)
                continue;
            // `possible targets` est construit en ignorant la ligne de vue.
            // Un candidat hors de vue, elu, ferait echouer Attack() — et le bras
            // pilote serait le seul a payer cet echec.
            if (!bot->IsWithinLOSInMap(unite))
                continue;
            candidats.push_back(unite);
        }
    }
    if (candidats.size() < 2)
        return choixMoteur;

    // Trier par distance, le choix du moteur excepte : il garde sa place de
    // tete. La troncature ecarte donc les plus lointains, et non des candidats
    // pris au hasard dans l'ordre d'un conteneur.
    std::sort(candidats.begin() + 1, candidats.end(),
              [bot](Unit* a, Unit* b) { return bot->GetExactDist2d(a) < bot->GetExactDist2d(b); });
    if (candidats.size() > MAX_CIBLES)
        candidats.resize(MAX_CIBLES);

    /* ---------------------------------------------------------------- */
    /* 1. Ou en est l'attente                                            */
    /* ---------------------------------------------------------------- */

    // POURQUOI ATTENDRE (P-133). La premiere version demandait a
    // l'acquisition et lisait le cache a l'acquisition SUIVANTE : la reponse
    // avait alors des secondes, bien plus que Laya.MaxAgeMs, et n'etait
    // jamais utilisable. Ici la reponse sert l'acquisition qui l'a demandee :
    // on rend nullptr — « pas cette fois » — et l'action d'attaque, toujours
    // declenchee, repasse au tick suivant. Rien ne bloque le thread monde.
    std::string const libelleMoteur = Libelle(choixMoteur);
    switch (oracle.SuivreAttente(guid, CoaLayaOracle::CANAL_CIBLE, ATTENTE_MAX_MS, REPIT_MS,
                                 libelleMoteur))
    {
        case CoaLayaOracle::Attente::EnVol:
            return nullptr;
        case CoaLayaOracle::Attente::Expiree:
            oracle.CompterCibleExpiree();
            return choixMoteur;
        case CoaLayaOracle::Attente::Discordante:
            // Le moteur propose une autre cible que celle de la demande (deux
            // actions d'attaque dans le meme tick, une valeur recalculee) : la
            // reponse ne classe pas son choix actuel, elle ne decide rien.
            oracle.CompterCibleDiscordante();
            return choixMoteur;
        case CoaLayaOracle::Attente::Repit:
            return choixMoteur;
        case CoaLayaOracle::Attente::Libre:
        {
            std::vector<std::pair<std::string, float>> lot;
            std::vector<std::string> descriptions;
            lot.reserve(candidats.size());
            descriptions.reserve(candidats.size());
            for (Unit* cible : candidats)
            {
                lot.emplace_back(Libelle(cible), 0.0f);
                descriptions.push_back(Decrire(bot, cible));
            }
            // Pas de question annexe : elle doublerait le cout d'inference sans
            // rien apporter au choix de la cible.
            // Aucun datagramme parti (oracle eteint entre-temps, envoi refuse,
            // options en double) : ne pas attendre une reponse qui ne viendra
            // pas.
            if (!oracle.Demander(guid, CoaLayaOracle::CANAL_CIBLE,
                                 CoaDecrireEtatLaya(botAI, bot, choixMoteur), lot, descriptions,
                                 false))
                return choixMoteur;
            if (!oracle.OuvrirAttente(guid, CoaLayaOracle::CANAL_CIBLE, libelleMoteur))
                return choixMoteur;
            oracle.CompterCibleAttente();
            return nullptr;
        }
        case CoaLayaOracle::Attente::Repondue:
            break;
    }

    /* ---------------------------------------------------------------- */
    /* 2. La reponse est la : decider, ou l'ignorer sur le temoin         */
    /* ---------------------------------------------------------------- */

    // Les deux compteurs au meme endroit, avant tout test sur la reponse :
    // `cibles_decidees` et `cibles_temoins` doivent etre comparables.
    if (!pilote)
    {
        oracle.CompterCibleTemoin();
        return choixMoteur;
    }
    oracle.CompterCibleDecidee();

    float const meilleure = oracle.MeilleureProbabilite(guid, CoaLayaOracle::CANAL_CIBLE);
    if (meilleure <= 0.0f)
        return choixMoteur;

    // La reponse porte sur les candidats de la demande, relevee un ou deux
    // ticks plus tot. Un candidat mort ou sorti entre-temps n'est plus dans
    // `candidats` et ne peut pas gagner ; un nouveau venu n'est pas dans la
    // reponse, rend -1, et ne bat donc jamais la cible du moteur, gardee a
    // un score >= 0 ci-dessous.
    Unit* elu = choixMoteur;
    float scoreElu = oracle.Probabilite(guid, CoaLayaOracle::CANAL_CIBLE, libelleMoteur);
    // Garde : la cible du moteur est dans la demande (Discordante l'a verifie),
    // donc dans la reponse. Si elle ne l'etait pas, -1 la ferait battre par
    // n'importe quel candidat, meme a zero : on ne decide pas sur cette base.
    if (scoreElu < 0.0f)
        return choixMoteur;
    for (Unit* cible : candidats)
    {
        if (cible == choixMoteur)
            continue;
        float const score = oracle.Probabilite(guid, CoaLayaOracle::CANAL_CIBLE, Libelle(cible));
        if (score > scoreElu)
        {
            scoreElu = score;
            elu = cible;
        }
    }

    // Son propre compteur, et pas `CompterChoix` : celui-la appartient au canal
    // sort, et melanger les deux rendrait le releve incapable de dire laquelle
    // des deux greffes a fait quelque chose.
    if (elu != choixMoteur)
        oracle.CompterCibleChangee();
    return elu;
}
