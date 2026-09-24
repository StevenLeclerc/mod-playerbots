/*
 * Conquest of Azeroth — registre des rencontres de combat. Voir l'en-tete pour
 * le plan d'experience que ce fichier sert.
 */

#include "CoaRegistreCombat.h"

#include "DBCStores.h"

#include "CoaLayaOracle.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Timer.h"
#include "Unit.h"

namespace
{
    // Une rencontre plus courte que cela n'est pas un combat : c'est un
    // scintillement de l'etat, un coup recu en passant, une entree en combat
    // annulee au tick suivant. La compter ferait du bruit.
    constexpr uint32 DUREE_MINIMALE_MS = 2000;

    // Au-dela, le bot est coince en combat sans se battre — cela arrive quand
    // une creature reste attachee hors de portee. Ce n'est plus une rencontre
    // mesurable, on la clot en le disant plutot que de la laisser fausser la
    // moyenne.
    constexpr uint32 DUREE_MAXIMALE_MS = 10 * 60 * 1000;

    // UN TICK SUR SEIZE POUR LA COUVERTURE, ET C'EST UNE CORRECTION.
    //
    // J'avais suppose que UpdateAI suivait la cadence de reaction des bots,
    // soit quelques appels par seconde. Le registre lui-meme m'a detrompe :
    // les premieres rencontres donnent 109 a 136 ticks par seconde et par bot.
    // Interroger le cache de l'oracle a chaque tick revenait donc a prendre un
    // verrou partage 246 fois 120 = pres de 30 000 fois par seconde, en
    // concurrence avec le thread receveur.
    //
    // Un tick sur seize laisse sept mesures par seconde et par bot, largement
    // de quoi etablir une part de couverture, pour un seizieme du cout.
    constexpr uint32 PAS_DE_SONDAGE = 16;

    // LA FENETRE D'OUVERTURE, ET POURQUOI ELLE EST INDISPENSABLE.
    //
    // Classer une rencontre « servie » d'apres sa couverture MOYENNE est un
    // piege de causalite inverse, et le premier rapport l'a montre : les
    // rencontres servies duraient 13,8 s de plus que les non servies, ecart
    // declare significatif. Or une rencontre de 5 s ne donne que deux sondages
    // et une de 40 s en donne trois cents : plus un combat dure, plus il a de
    // chances qu'au moins un sondage trouve une reponse. Ce n'est donc pas
    // l'oracle qui allonge les combats, c'est la duree qui fabrique la
    // couverture.
    //
    // La couverture des premieres secondes echappe a ce biais : elle est
    // mesuree sur une fenetre de largeur FIXE, avant que l'issue ne soit
    // jouee. C'est elle qui doit classer la rencontre.
    constexpr uint32 FENETRE_OUVERTURE_MS = 5000;

    // FNV-1a, graine et facteur canoniques 64 bits. Deja employes par
    // PlayerbotAIConfig::IsMercenary et IsLayaElite : on reprend la technique
    // du projet plutot que d'en introduire une seconde.
    constexpr uint64 FNV_GRAINE = 14695981039346656037ULL;
    constexpr uint64 FNV_FACTEUR = 1099511628211ULL;
}

CoaRegistreCombat& CoaRegistreCombat::Instance()
{
    static CoaRegistreCombat instance;
    return instance;
}

/*
 * LE TIRAGE. Deterministe, donc rejouable : le meme journal redonne les memes
 * bras, et personne n'a a croire sur parole qu'une rencontre etait pilotee.
 *
 * POURQUOI OCTET PAR OCTET, ET DANS CET ORDRE — c'est une mesure, pas un gout.
 * IsMercenary hache le GUID en UNE etape (`h ^= guid; h *= P`). Cela suffit
 * pour un GUID seul, dont on ne tire qu'une valeur par bot et pour toujours.
 * Ici on tire une valeur par RENCONTRE, donc plusieurs fois pour le meme bot,
 * avec un debutMs qui varie peu : la question n'est plus l'equirepartition
 * globale mais l'absence de correlation entre tirages successifs.
 *
 * Les trois formes ont ete mesurees sur 2 000 bots x 40 rencontres
 * (outils/sonde-tirage-rencontre.py). Toutes donnent une part pilotee juste et
 * un khi-deux sain. Mais sur le cas ou les rencontres s'enchainent a PAS FIXE
 * de 1024 ms — une puissance de deux, donc les bits de poids faible ne bougent
 * pas —, la forme en une etape alterne 60,8 % du temps au lieu de 50 % : les
 * bras se mettent a s'alterner au lieu d'etre tires. La forme octet par octet
 * reste a 49,3 %, et son alternance suit 2p(1-p) a tous les seuils essayes
 * (10, 25, 50, 75 %), ce qui est la signature d'un tirage sans memoire.
 *
 * debutMs est hache EN PREMIER pour que ses bits traversent les douze
 * multiplications, et non les quatre dernieres seulement.
 */
bool CoaRegistreCombat::TirerPilotage(uint64 guid, uint32 debutMs)
{
    uint32 const seuil = sPlayerbotAIConfig.layaTirageAuSort;
    // Les deux bouts sont traites sans hacher : a 100 la greffe est sans effet,
    // a 0 l'oracle ne pilote plus rien.
    if (seuil >= 100)
        return true;
    if (seuil == 0)
        return false;

    uint64 hash = FNV_GRAINE;
    for (uint32 i = 0; i < 4; ++i)
    {
        hash ^= static_cast<uint8>((debutMs >> (8 * i)) & 0xFF);
        hash *= FNV_FACTEUR;
    }
    for (uint32 i = 0; i < 8; ++i)
    {
        hash ^= static_cast<uint8>((guid >> (8 * i)) & 0xFF);
        hash *= FNV_FACTEUR;
    }
    return (hash % 100) < seuil;
}

void CoaRegistreCombat::Observer(Player* bot, PlayerbotAI* botAI)
{
    if (!bot || !botAI || !sPlayerbotAIConfig.layaEnabled)
        return;

    uint64 const guid = bot->GetGUID().GetRawValue();

    // On ne suit que les deux groupes qu'on compare — elites et temoins, donc
    // les mercenaires. Suivre les 500 bots multiplierait le journal par deux
    // sans rien apporter a la comparaison.
    if (!sPlayerbotAIConfig.IsMercenary(guid))
        return;

    bool const enCombat = bot->IsInCombat() && bot->IsAlive();

    // Ce qui suit touche _encours, partage par les dix MapUpdate.Threads : tout
    // passe par le verrou. La cloture, elle, ecrit dans le journal et se fait
    // donc dehors, sur une copie deja retiree de la table.
    Rencontre close;
    char const* raison = nullptr;

    {
        std::lock_guard<std::mutex> garde(_verrou);

        auto trouve = _encours.find(guid);

        if (trouve == _encours.end())
        {
            if (!enCombat)
                return;
            Rencontre r;
            r.debutMs = getMSTime();
            // LE BRAS EST TIRE ICI, ET NULLE PART AILLEURS. A l'ouverture, donc
            // avant que quoi que ce soit du combat ne soit connu, et sans
            // regarder ce que l'oracle a en cache : c'est toute la difference
            // entre une experience et une observation.
            r.pilote = TirerPilotage(guid, r.debutMs);
            r.pvDebut = static_cast<uint8>(bot->GetHealthPct());
            r.pvMin = r.pvDebut;
            r.niveau = bot->GetLevel();
            r.killsDebut = bot->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS);
            r.assaillantsOuverture = static_cast<uint32>(bot->getAttackers().size());
            r.victimeOuverture = bot->GetVictim() != nullptr;
            bot->GetZoneAndAreaId(r.zone, r.aire);
            if (AreaTableEntry const* aire = sAreaTableStore.LookupEntry(r.aire))
                r.aireFlags = aire->flags;
            if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(r.zone))
                r.zoneFlags = zone->flags;
            _encours.emplace(guid, r);
            _ouvertes.fetch_add(1, std::memory_order_relaxed);
            if (r.pilote)
                _pilotees.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        Rencontre& r = trouve->second;
        ++r.ticks;

        // La couverture Laya, sondee un tick sur seize. C'est elle qui decoupe les
        // rencontres en servies et non servies, et c'est tout l'interet du
        // registre. Le denominateur est donc `sondages`, pas `ticks`.
        //
        // Le sondage reste utile sur les DEUX bras : sur le bras non pilote il
        // mesure ce que la couverture AURAIT ete, ce qui permet de verifier que
        // le tirage n'a pas deplace la disponibilite de l'oracle.
        if ((r.ticks % PAS_DE_SONDAGE) == 0)
        {
            bool const servi =
                CoaLayaOracle::Instance().MeilleureProbabilite(guid, CoaLayaOracle::CANAL_SORT) > 0.0f;
            ++r.sondages;
            if (servi)
                ++r.ticksServis;
            if (GetMSTimeDiffToNow(r.debutMs) <= FENETRE_OUVERTURE_MS)
            {
                ++r.sondagesDebut;
                if (servi)
                    ++r.servisDebut;
            }
        }

        uint8 const pv = static_cast<uint8>(bot->GetHealthPct());
        if (pv < r.pvMin)
            r.pvMin = pv;
        uint32 const assaillants = static_cast<uint32>(bot->getAttackers().size());
        if (assaillants > r.assaillantsMax)
            r.assaillantsMax = assaillants;

        // La victime du tick, comptee par nature. `GetVictim` rend la cible de
        // la melee en cours ; elle est nulle entre deux echanges, et ce tick-la
        // ne compte ni d'un cote ni de l'autre.
        if (Unit const* victime = bot->GetVictim())
        {
            if (victime->GetTypeId() == TYPEID_PLAYER)
                ++r.ticksJoueur;
            else
                ++r.ticksCreature;
        }

        if (!bot->IsAlive())
        {
            r.mort = true;
            raison = "mort";
        }
        else if (!bot->IsInCombat())
        {
            raison = "sortie";
        }
        else if (GetMSTimeDiffToNow(r.debutMs) > DUREE_MAXIMALE_MS)
        {
            raison = "enlisee";
        }

        if (!raison)
            return;

        close = r;
        _encours.erase(trouve);
    }

    Clore(bot, close, raison);
}

bool CoaRegistreCombat::EstPilotee(uint64 guid) const
{
    std::lock_guard<std::mutex> garde(_verrou);
    auto const trouve = _encours.find(guid);
    // Pas de rencontre ouverte — donc hors combat, ou bot non suivi — vaut
    // « non pilotee ». C'est le choix conservateur : sans rencontre, il n'y a
    // pas de bras, et le tourniquet d'origine reprend la main.
    return trouve != _encours.end() && trouve->second.pilote;
}

uint32 CoaRegistreCombat::DebutMs(uint64 guid) const
{
    std::lock_guard<std::mutex> tenu(_verrou);
    auto it = _encours.find(guid);
    return it == _encours.end() ? 0u : it->second.debutMs;
}

void CoaRegistreCombat::Clore(Player* bot, Rencontre const& r, char const* raison)
{
    uint32 const duree = GetMSTimeDiffToNow(r.debutMs);
    if (duree < DUREE_MINIMALE_MS)
    {
        _ignorees.fetch_add(1, std::memory_order_relaxed);
        if (r.pilote)
            _ignoreesPilotees.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    _closes.fetch_add(1, std::memory_order_relaxed);

    uint64 const guid = bot->GetGUID().GetRawValue();
    uint32 const kills = bot->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS);

    // Une ligne par rencontre, analysable par outils/analyse-rencontres.py.
    // Champs nommes plutot que positionnels : une colonne ajoutee demain ne
    // cassera pas la lecture d'aujourd'hui.
    LOG_INFO("playerbots.coa",
             "coa rencontre: guid={} elite={} classe={} niveau={} duree_ms={} "
             "pv_debut={} pv_min={} pv_fin={} assaillants_max={} kills={} "
             "ticks={} sondages={} servis={} sondages_debut={} servis_debut={} "
             "ticks_joueur={} ticks_creature={} initie={} assaillants_ouverture={} "
             "aire={} zone={} sanctuaire={} capitale={} pilote={} ffa={} issue={}",
             guid,
             sPlayerbotAIConfig.IsLayaElite(guid) ? 1 : 0,
             static_cast<uint32>(bot->getClass()),
             static_cast<uint32>(r.niveau),
             duree,
             static_cast<uint32>(r.pvDebut),
             static_cast<uint32>(r.pvMin),
             static_cast<uint32>(bot->GetHealthPct()),
             r.assaillantsMax,
             kills - r.killsDebut,
             r.ticks,
             r.sondages,
             r.ticksServis,
             r.sondagesDebut,
             r.servisDebut,
             r.ticksJoueur,
             r.ticksCreature,
             // « initie » est une DEDUCTION, pas une observation directe : le
             // coeur ne dit nulle part qui a porte le premier coup. Personne ne
             // l'attaquait et il avait deja une victime, donc c'est lui qui est
             // alle la chercher. `assaillants_ouverture` est publie a cote pour
             // que la deduction reste verifiable et refutable.
             (r.assaillantsOuverture == 0 && r.victimeOuverture) ? 1 : 0,
             r.assaillantsOuverture,
             r.aire,
             r.zone,
             // Sanctuaire sur l'AIRE, capitale sur la ZONE. Voir l'en-tete :
             // les lire tous deux sur l'aire etait le defaut de la premiere
             // version, et il rendait `capitale=0` dans la moitie d'Orgrimmar.
             (r.aireFlags & 0x800) ? 1 : 0,
             (r.zoneFlags & 0x100) ? 1 : 0,
             r.pilote ? 1 : 0,
             // LE DRAPEAU FFA, LU A LA CLOTURE ET PAS A L'OUVERTURE.
             // PlayerbotAI::UpdateAI le repose a chaque tick parce que
             // Player::UpdateArea le recalcule a chaque changement de zone : une
             // valeur d'ouverture aurait pu etre ecrasee depuis. Ce qui
             // interesse est l'etat constate en fin de combat.
             //
             // C'est bien ce drapeau que lit Unit::GetReactionTo pour rendre
             // REP_HOSTILE entre deux unites de meme faction
             // (`IsFFAPvP() && target->IsFFAPvP()`, Unit.cpp). Player::IsFFAPvP
             // n'ajoute que le hook OnPlayerIsFFAPvP, qu'AUCUN script
             // n'implemente dans cet arbre — les deux valeurs sont donc egales
             // aujourd'hui. On consigne, on ne conclut rien.
             bot->IsFFAPvP() ? 1 : 0,
             raison);
}

void CoaRegistreCombat::Oublier(uint64 guid)
{
    // On compte ce qu'on jette, et de quel bras : une rencontre ouverte qu'un
    // bot emporte en se deconnectant disparait sans laisser de ligne, et si
    // cela frappait un bras plus que l'autre, tout le plan d'experience
    // pencherait sans que rien ne le dise.
    std::lock_guard<std::mutex> garde(_verrou);
    auto trouve = _encours.find(guid);
    if (trouve == _encours.end())
        return;
    _oubliees.fetch_add(1, std::memory_order_relaxed);
    if (trouve->second.pilote)
        _oublieesPilotees.fetch_add(1, std::memory_order_relaxed);
    _encours.erase(trouve);
}

std::string CoaRegistreCombat::Compteurs() const
{
    size_t encours;
    {
        std::lock_guard<std::mutex> garde(_verrou);
        encours = _encours.size();
    }
    return "ouvertes=" + std::to_string(_ouvertes.load(std::memory_order_relaxed)) +
           " ouvertes_pilotees=" + std::to_string(_pilotees.load(std::memory_order_relaxed)) +
           " closes=" + std::to_string(_closes.load(std::memory_order_relaxed)) +
           " trop_courtes=" + std::to_string(_ignorees.load(std::memory_order_relaxed)) +
           " trop_courtes_pilotees=" + std::to_string(_ignoreesPilotees.load(std::memory_order_relaxed)) +
           " oubliees=" + std::to_string(_oubliees.load(std::memory_order_relaxed)) +
           " oubliees_pilotees=" + std::to_string(_oublieesPilotees.load(std::memory_order_relaxed)) +
           " en_cours=" + std::to_string(encours);
}
