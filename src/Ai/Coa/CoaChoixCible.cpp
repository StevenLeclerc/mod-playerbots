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
    std::string Libelle(Unit* cible)
    {
        return (cible->GetTypeId() == TYPEID_PLAYER ? "j" : "c") +
               std::to_string(cible->GetGUID().GetCounter());
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

    // A 100 — le defaut — on n'interroge PAS le registre : ni verrou pris, ni
    // comportement change. En dessous, une rencontre non ouverte vaut « non
    // pilotee », ce qui dilue le bras pilote sans le melanger au temoin : la
    // mesure est sous-estimee, jamais fabriquee.
    if (sPlayerbotAIConfig.layaTirageAuSort < 100 &&
        !CoaRegistreCombat::Instance().EstPilotee(guid))
        return choixMoteur;

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
    /* 1. Demander, pour la prochaine acquisition                        */
    /* ---------------------------------------------------------------- */

    if (oracle.PeutRedemander(guid, CoaLayaOracle::CANAL_CIBLE))
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
        oracle.Demander(guid, CoaLayaOracle::CANAL_CIBLE,
                        CoaDecrireEtatLaya(botAI, bot, choixMoteur), lot, descriptions, false);
    }

    /* ---------------------------------------------------------------- */
    /* 2. Lire le cache, et rien que le cache                            */
    /* ---------------------------------------------------------------- */

    float const meilleure = oracle.MeilleureProbabilite(guid, CoaLayaOracle::CANAL_CIBLE);
    if (meilleure <= 0.0f)
        return choixMoteur;

    // La reponse en cache porte sur les candidats de la demande PRECEDENTE.
    // Ceux qui ne sont plus la n'y repondent pas, et un libelle inconnu rend
    // -1 : un candidat absent de la reponse ne peut donc pas gagner.
    Unit* elu = choixMoteur;
    float scoreElu = oracle.Probabilite(guid, CoaLayaOracle::CANAL_CIBLE, Libelle(choixMoteur));
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
