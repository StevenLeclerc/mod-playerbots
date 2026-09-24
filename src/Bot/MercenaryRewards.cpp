/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MercenaryRewards.h"
#include "Formulas.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <mutex>
#include <shared_mutex>

namespace
{
    constexpr uint32 PURGE_PERIOD_MS = 60 * IN_MILLISECONDS;
    constexpr uint32 MEDIAN_PERIOD_MS = 30 * IN_MILLISECONDS;

    // En dessous, une mediane ne veut rien dire et le plafond ferait plus de
    // mal que de bien : on l'abandonne plutot que de le serrer au hasard.
    constexpr uint32 MEDIAN_MIN_POPULATION = 10;

    constexpr time_t KILL_RATE_WINDOW_S = 3600;

    // Un releve periodique dans CoaBots.log : sans lui, on ne saurait pas
    // distinguer << aucun mercenaire ne tue >> de << les recompenses sont
    // refusees par un garde-fou >>. Le projet ne conclut pas sans mesure.
    constexpr uint32 RAPPORT_PERIOD_MS = 5 * MINUTE * IN_MILLISECONDS;

    // Plus haut niveau adressable par l'histogramme de travail. Le tableau en
    // compte 128, ce qui laisse de la marge si CONFIG_MAX_PLAYER_LEVEL monte
    // un jour. HISTOGRAM_LEVELS et HISTOGRAM_MAX_LEVEL ne peuvent pas diverger.
    constexpr size_t HISTOGRAM_LEVELS = 128;
    constexpr uint8 HISTOGRAM_MAX_LEVEL = static_cast<uint8>(HISTOGRAM_LEVELS - 1);
}

void MercenaryRewards::Update(uint32 diff)
{
    _medianTimer += diff;
    if (_medianTimer >= MEDIAN_PERIOD_MS)
    {
        _medianTimer = 0;
        RecomputeMedian();
    }

    _rapportTimer += diff;
    if (_rapportTimer >= RAPPORT_PERIOD_MS)
    {
        _rapportTimer = 0;
        ++_rapportSeq;

        // L'ECART NON EXPLIQUE, publie en clair. Les causes doivent additionner
        // exactement les morts vues ; tant qu'un chemin sort sans compter, la
        // difference est non nulle et se lit sur la ligne. C'est la seule facon
        // qu'un futur `return 0` muet se denonce tout seul au lieu de creuser un
        // trou que l'exploitant attribuera a un bug ailleurs. Signe : int64, et
        // non uint32, pour qu'un sur-comptage se lise en negatif plutot que de
        // s'enrouler autour de quatre milliards.
        uint32 const vues = _pvpKillsSeen.load(std::memory_order_relaxed);
        uint32 const horsCamp = _killerNotMercenary.load(std::memory_order_relaxed);
        uint32 const payes = KillsRewarded();
        uint32 const refus = KillsRefused();
        int64 const nonExplique = static_cast<int64>(vues) -
            (static_cast<int64>(horsCamp) + static_cast<int64>(payes) + static_cast<int64>(refus));

        // TOUJOURS emettre, meme quand tous les compteurs sont a zero.
        // La version precedente se taisait tant que rien ne bougeait : c'est
        // exactement le moment ou l'on a besoin de savoir que le systeme est
        // vivant. Un silence devenait indistinguable d'une panne, et il a
        // coute une heure le 20/09 (P-061).
        //
        // Logger "playerbots.coa" et non "playerbots" : SEUL le premier a une
        // ligne Logger dans worldserver.conf. "playerbots" retombe sur
        // Logger.root = 2 (ERROR) et ses INFO sont jetes SANS un mot.
        //
        // `seq` et `uptime` sont la pour le consommateur qui derive un debit en
        // differenciant deux releves (le panneau echantillonne toutes les
        // 300 s) : apres un redemarrage du worldserver, la difference est
        // negative ou absurde, et rien sur la ligne ne permettait de le
        // detecter. Deux champs, et la differenciation devient sure. La
        // persistance en base reste un chantier separe : ces compteurs vivent
        // en memoire seule et tout ce qui precede le redemarrage est perdu.
        //
        // L'ORDRE DES CHAMPS N'EST PAS LIBRE : CETTE LIGNE A DES PARSEURS EN
        // SERVICE. Le prefixe
        //     [Mercenaire] mediane N - morts PvP vues N - tueur hors camp N -
        //     payes N - refus N (gris N, plafond N, recidive N, cadence N) -
        //     treves N
        // est un CONTRAT, mot pour mot et dans cet ordre, avec le depot
        // coa-project. Lus, pas supposes :
        //   - phase8-panel/panel.py, MercenairesCompteurs.RE_VENTILE : c'est
        //     LE consommateur en service, et le seul que la premiere redaction
        //     cassait pour de bon ;
        //   - outils/sonde-banc-mercenaires.py, qui eprouve RE_VENTILE sur un
        //     echantillon fige a la forme de ce prefixe ;
        //   - outils/sonde-mercenaires-pvp.py, en revanche, ne fait que filtrer
        //     les lignes contenant `[Mercenaire]` et les reafficher telles
        //     quelles : il ne depend PAS de l'ordre des champs.
        // RE_VENTILE exige DEUX ancres que la premiere redaction de ce releve
        // cassait sans que rien ne le signale : `[Mercenaire]` suivi
        // IMMEDIATEMENT de `mediane` (seq et uptime etaient passes devant), et
        // le groupe entre parentheses FERME sur `cadence N)` suivi de
        // `- treves N` (les quatre nouvelles causes avaient ete glissees dans
        // ce groupe). Le parseur ne degrade pas : il rend
        // << aucune ligne [Mercenaire] >>, et la vue mercenaires du panneau
        // tombe en erreur.
        // Tout ce qui est NOUVEAU se met donc APRES `treves`, ou la recherche
        // du motif a deja abouti : un second groupe pour les causes ajoutees,
        // puis l'ecart, puis seq et uptime. Ajouter un champ ici est sans
        // danger ; en inserer un AVANT `treves`, ou renommer l'un des champs
        // du prefixe, casse les trois consommateurs et exige de les mettre a
        // jour dans la meme vague.
        LOG_INFO("playerbots.coa",
            "[Mercenaire] mediane {} - morts PvP vues {} - tueur hors camp {} - "
            "payes {} - refus {} "
            "(gris {}, plafond {}, recidive {}, cadence {}) - treves {} - "
            "autres refus (desarme {}, niveau max {}, tiers {}, non verse {}) - "
            "non explique {} - seq {} - uptime {}s",
            static_cast<uint32>(MedianBotLevel()),
            vues,
            horsCamp,
            payes,
            refus,
            _refusedGrey.load(std::memory_order_relaxed),
            _refusedLevelCap.load(std::memory_order_relaxed),
            _refusedRepeat.load(std::memory_order_relaxed),
            _refusedRate.load(std::memory_order_relaxed),
            _activeTruces.load(std::memory_order_relaxed),
            _refusedDisabled.load(std::memory_order_relaxed),
            _refusedMaxLevel.load(std::memory_order_relaxed),
            _refusedByHook.load(std::memory_order_relaxed),
            _refusedNotPaid.load(std::memory_order_relaxed),
            nonExplique,
            _rapportSeq,
            static_cast<int64>(GameTime::GetUptime().count()));

        // LOT 5, SUR SA PROPRE LIGNE ET PAS SUR CELLE DU DESSUS. Le prefixe
        // `[Mercenaire] ... treves N` est un CONTRAT avec trois consommateurs du
        // depot coa-project (voir le pave ci-dessus, piege 35 du dossier) : y
        // inserer un champ casse la vue mercenaires du panneau, qui tombe en
        // erreur au lieu de se degrader. Une ligne NOUVELLE, de prefixe
        // different, n'est lue par aucun d'eux.
        //
        // CE QUE CETTE LIGNE PROUVE. `proies` contre le nombre de bots en ligne
        // dit quelle part de la population est reellement chassable -- le
        // dossier mesure un tiers de mercenaires inertes par zone, plus de la
        // moitie par aire. `cellules` dit si l'index a de la matiere ou s'il
        // est vide. `debordements` doit rester a zero : au-dela, des proies ne
        // sont pas comptees et le tirage se fait sur une carte trouee.
        //
        // `dont mercenaires` EST CE QUI REND LA LIGNE FALSIFIABLE. Sans lui, nul
        // ne pouvait dire si un facteur eleve venait des proies ou des chasseurs
        // que l'index y avait lui-meme envoyes. Ce sous-compte ne pese PAS sur
        // le tirage -- FacteurPresence n'emploie que `proies - dont
        // mercenaires`, voir << L'INDEX NE SE MANGE PAS LUI-MEME >> dans
        // MercenaryRewards.h -- et il est publie pour qu'on puisse le lire.
        // Attendu en service : environ la moitie de `proies`, puisque
        // MercenaryPercent vaut 50.
        //
        // `sous treve` compte les proies ECARTEES par la treve de
        // TruceAfterDeathSec. Il n'entre donc pas dans `proies`. Un nombre eleve
        // dit qu'on vient de beaucoup mourir, et que l'index a evite d'y
        // renvoyer du monde ; a zero en permanence alors que des kills sont
        // payes, il dit que la treve ne s'arme pas.
        if (sPlayerbotAIConfig.wildPvpIndexProies)
        {
            CoaIndexProies const& index = CoaIndexProies::instance();
            LOG_INFO("playerbots.coa",
                "[CoA index] proies {} (dont mercenaires {}) - sous treve {} - cellules {} - "
                "debordements {} - hors bornes {} - cote {} yd - bloc 2x2 - seq {}",
                index.Recensees(), index.Mercenaires(), index.SousTreve(), index.Cellules(),
                index.Debordements(), index.HorsBornes(),
                uint32(CoaIndexProies::TailleCellule), _rapportSeq);
        }
    }

    _purgeTimer += diff;
    if (_purgeTimer < PURGE_PERIOD_MS)
        return;

    _purgeTimer = 0;

    time_t const now = time(nullptr);
    std::unique_lock<std::shared_mutex> guard(_mutex);

    for (auto it = _truces.begin(); it != _truces.end();)
    {
        if (it->second <= now)
            it = _truces.erase(it);
        else
            ++it;
    }
    _activeTruces.store(static_cast<uint32>(_truces.size()), std::memory_order_relaxed);

    // Un tueur dont plus rien n'est recent ne sert plus a rien : sans cette
    // purge la table croit avec le nombre de bots ayant tue une fois.
    time_t const staleBefore = now - std::max<time_t>(KILL_RATE_WINDOW_S,
        static_cast<time_t>(sPlayerbotAIConfig.wildPvpKillRepeatCooldown));

    for (auto it = _killers.begin(); it != _killers.end();)
    {
        KillerRecord& record = it->second;

        while (!record.RecentKills.empty() && record.RecentKills.front() < now - KILL_RATE_WINDOW_S)
            record.RecentKills.pop_front();

        for (auto victim = record.LastKillOf.begin(); victim != record.LastKillOf.end();)
        {
            if (victim->second < staleBefore)
                victim = record.LastKillOf.erase(victim);
            else
                ++victim;
        }

        if (record.RecentKills.empty() && record.LastKillOf.empty())
            it = _killers.erase(it);
        else
            ++it;
    }
}

bool MercenaryRewards::IsUnderTruce(uint64 guid) const
{
    // Chemin chaud : appele pour chaque cible envisagee par chaque mercenaire,
    // depuis les fils de carte. Tant qu'aucune treve n'est armee on ne prend
    // rien du tout -- mais cette condition ne se realise pas en service, voir
    // l'en-tete : c'est pour cela que la lecture ci-dessous est PARTAGEE. Les
    // fils de carte ne se serialisent plus les uns sur les autres ici ; seules
    // les poses de treve et la purge d'une minute prennent l'exclusivite.
    if (!_activeTruces.load(std::memory_order_relaxed))
        return false;

    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto it = _truces.find(guid);
    return it != _truces.end() && it->second > time(nullptr);
}

void MercenaryRewards::OnPvpKill(Player* killer, Player* killed)
{
    if (!killer || !killed || killer == killed)
        return;

    if (!sPlayerbotAIConfig.wildPvpEnabled)
        return;

    _pvpKillsSeen.fetch_add(1, std::memory_order_relaxed);

    // La treve protege la victime quoi qu'il arrive, y compris quand le tueur
    // n'a droit a aucune recompense : les deux decisions sont independantes.
    if (uint32 truceSeconds = sPlayerbotAIConfig.wildPvpTruceAfterDeathSec)
    {
        time_t const until = time(nullptr) + static_cast<time_t>(truceSeconds);
        std::unique_lock<std::shared_mutex> guard(_mutex);
        _truces[killed->GetGUID().GetRawValue()] = until;
        _activeTruces.store(static_cast<uint32>(_truces.size()), std::memory_order_relaxed);
    }

    uint32 xp = ComputeReward(killer, killed);
    if (!xp)
        return;

    // OnPlayerGiveXP prend le montant par REFERENCE NON CONSTANTE : un autre
    // module (mod-dynamic-xp ici) a le droit de le modifier, voire de
    // l'annuler. On relit donc apres l'appel plutot que de supposer.
    sScriptMgr->OnPlayerGiveXP(killer, xp, killed, PlayerXPSource::XPSOURCE_KILL);
    if (!xp)
    {
        _killsRefused.fetch_add(1, std::memory_order_relaxed);
        _refusedByHook.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // LE VERSEMENT SE CONSTATE, IL NE SE SUPPOSE PAS. Player::GiveXP est `void`
    // et sort sans rien dire dans cinq cas (voir ComputeReward). On releve donc
    // le couple (niveau, PLAYER_XP) avant et apres : c'est exactement ce que
    // GiveXP lui-meme ecrit (Player.cpp:2523 lit GetUInt32Value(PLAYER_XP)), et
    // le niveau doit y figurer parce qu'un passage de palier fait REDESCENDRE
    // PLAYER_XP -- le tester seul rendrait un faux negatif a chaque montee.
    uint32 const xpAvant = killer->GetUInt32Value(PLAYER_XP);
    uint8 const niveauAvant = killer->GetLevel();

    killer->GiveXP(xp, nullptr);

    if (killer->GetLevel() == niveauAvant && killer->GetUInt32Value(PLAYER_XP) == xpAvant)
    {
        // Rien n'a bouge : le plus frequent est la mort mutuelle, ou le poison
        // qui acheve la cible au tick suivant -- Unit::Kill declenche le
        // crochet avec un tueur deja mort et GiveXP sort a Player.cpp:2472.
        // On ne consigne alors NI le quota horaire NI le delai anti-recidive :
        // un kill non paye ne doit rien couter au mercenaire.
        _killsRefused.fetch_add(1, std::memory_order_relaxed);
        _refusedNotPaid.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    NoteKillPaid(killer->GetGUID().GetRawValue(), killed->GetGUID().GetRawValue());
    _killsRewarded.fetch_add(1, std::memory_order_relaxed);

    LOG_DEBUG("playerbots.coa", "[Mercenaire] {} (niveau {}) gagne {} xp sur {} (niveau {})",
        killer->GetName(), static_cast<uint32>(killer->GetLevel()), xp, killed->GetName(),
        static_cast<uint32>(killed->GetLevel()));
}

void MercenaryRewards::NoteKillPaid(uint64 killerGuid, uint64 victimGuid)
{
    time_t const now = time(nullptr);

    std::unique_lock<std::shared_mutex> guard(_mutex);
    KillerRecord& record = _killers[killerGuid];

    record.LastKillOf[victimGuid] = now;

    // Elaguer ici plutot que de laisser la seule purge d'une minute le faire :
    // ComputeReward ne peut plus elaguer, puisqu'il ne prend que le verrou
    // partage. Sans cette ligne, RecentKills grossirait d'un kill par kill
    // jusqu'a la purge suivante, et le comptage de cadence -- qui ignore deja
    // les entrees perimees -- resterait juste mais paierait un parcours plus
    // long.
    while (!record.RecentKills.empty() && record.RecentKills.front() < now - KILL_RATE_WINDOW_S)
        record.RecentKills.pop_front();

    record.RecentKills.push_back(now);
}

uint32 MercenaryRewards::ComputeReward(Player* killer, Player* killed)
{
    uint32 const divisor = sPlayerbotAIConfig.wildPvpKillXpDivisor;
    if (!divisor)
    {
        // KillXpDivisor = 0 desarme tout le systeme, et la conf l'annonce
        // ainsi. Sans ce compteur, le releve affichait << payes 0 - refus 0 >>,
        // exactement la signature d'un systeme vivant mais sans proie.
        _killsRefused.fetch_add(1, std::memory_order_relaxed);
        _refusedDisabled.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Seuls les bots aleatoires mercenaires gagnent a ce jeu : EstMercenaire
    // porte la regle, et l'ordre de ses deux tests est explique a sa
    // definition.
    if (!EstMercenaire(killer))
    {
        _killerNotMercenary.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    uint8 const killerLevel = killer->GetLevel();
    uint8 const victimLevel = killed->GetLevel();

    if (killerLevel >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
    {
        // La cause de refus la plus frequente en regime etabli : un mercenaire
        // au plafond du royaume tue encore, mais ne peut plus rien gagner.
        // Elle etait muette, et c'est elle qui creusait l'essentiel de l'ecart.
        _killsRefused.fetch_add(1, std::memory_order_relaxed);
        _refusedMaxLevel.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Niveau gris : la regle que RewardHonor applique deja a l'honneur
    // (Player.cpp:6390). Massacrer plus faible que soi ne paie pas.
    uint8 const grey = Acore::XP::GetGrayLevel(killerLevel);
    if (victimLevel <= grey || killerLevel <= grey)
    {
        _killsRefused.fetch_add(1, std::memory_order_relaxed);
        _refusedGrey.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Plafond indexe sur la mediane des bots en ligne. Un mercenaire qui monte
    // par le PvP monte plus vite que la population qui l'entoure ; sans ce
    // plafond il finit par n'avoir plus aucune proie a sa portee, sauf le
    // joueur humain. Le plafond le fait patienter, il ne le redescend pas.
    if (sPlayerbotAIConfig.wildPvpLevelCapEnabled)
    {
        uint8 const median = MedianBotLevel();
        if (median && static_cast<int32>(killerLevel) >=
                static_cast<int32>(median) + sPlayerbotAIConfig.wildPvpLevelCapMedianOffset)
        {
            _killsRefused.fetch_add(1, std::memory_order_relaxed);
            _refusedLevelCap.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }

    uint64 const killerGuid = killer->GetGUID().GetRawValue();
    uint64 const victimGuid = killed->GetGUID().GetRawValue();
    time_t const now = time(nullptr);

    {
        // LECTURE SEULE, ET VERROU PARTAGE. Rien n'est ecrit ici : la
        // consignation du kill n'a lieu qu'une fois le versement CONSTATE, dans
        // NoteKillPaid. On emploie donc find() et non operator[], qui
        // insererait une entree vide pour tout tueur refuse.
        std::shared_lock<std::shared_mutex> guard(_mutex);
        auto const recordIt = _killers.find(killerGuid);
        if (recordIt != _killers.end())
        {
            KillerRecord const& record = recordIt->second;

            // Recidive sur la meme victime. Le coeur n'a aucune decroissance
            // pour les kills repetes hors champ de bataille : sans ce
            // garde-fou, deux mercenaires qui se tuent en boucle atteignent le
            // niveau 80 en quelques minutes.
            if (uint32 cooldown = sPlayerbotAIConfig.wildPvpKillRepeatCooldown)
            {
                auto it = record.LastKillOf.find(victimGuid);
                if (it != record.LastKillOf.end() && now - it->second < static_cast<time_t>(cooldown))
                {
                    _killsRefused.fetch_add(1, std::memory_order_relaxed);
                    _refusedRepeat.fetch_add(1, std::memory_order_relaxed);
                    return 0;
                }
            }

            // Plafond horaire, toutes victimes confondues : la borne dure. On
            // ne peut plus elaguer la file sous verrou partage : on COMPTE donc
            // les entrees non perimees au lieu de retirer les autres. La file
            // est chronologique, ses entrees perimees sont donc toutes en tete
            // et lower_bound les saute en O(log n). L'elagage reste fait par
            // NoteKillPaid et par la purge d'une minute.
            if (uint32 perHour = sPlayerbotAIConfig.wildPvpKillsPerHourCap)
            {
                auto const premierValide = std::lower_bound(
                    record.RecentKills.begin(), record.RecentKills.end(), now - KILL_RATE_WINDOW_S);
                size_t const recents =
                    static_cast<size_t>(std::distance(premierValide, record.RecentKills.end()));

                if (recents >= perHour)
                {
                    _killsRefused.fetch_add(1, std::memory_order_relaxed);
                    _refusedRate.fetch_add(1, std::memory_order_relaxed);
                    return 0;
                }
            }
        }
    }

    // Recompense exprimee en fraction du niveau EN COURS, et non en valeur
    // absolue : c'est ce qui la rend lisible a tous les niveaux. A divisor = 4,
    // il faut quatre victimes de son niveau pour passer un palier, que l'on
    // soit niveau 12 ou niveau 60.
    uint64 xp = static_cast<uint64>(sObjectMgr->GetXPForLevel(killerLevel)) / divisor;

    // Decroissance vers le niveau gris, de la meme forme que celle dont le
    // coeur se sert pour l'honneur (Player.cpp:6399). Une victime de niveau
    // egal ou superieur paie plein tarif, sans prime : on ne veut pas que
    // s'attaquer a plus fort que soi devienne le raccourci.
    if (victimLevel < killerLevel)
        xp = xp * static_cast<uint64>(victimLevel - grey) / static_cast<uint64>(killerLevel - grey);

    uint32 const recompense =
        static_cast<uint32>(std::min<uint64>(xp, std::numeric_limits<uint32>::max()));

    // LA DERNIERE SORTIE MUETTE. Un montant nul est refuse par l'appelant comme
    // tous les autres zeros, et il ne doit pas non plus creuser l'ecart. Le cas
    // n'est pas atteignable avec les formules actuelles -- GetXPForLevel rend
    // au moins quelques centaines des le niveau 1, le diviseur est borne par la
    // conf et la decroissance a un numerateur >= 1 puisque victimLevel > grey
    // est deja verifie plus haut -- mais un diviseur demesure ou une refonte de
    // la formule le rendrait atteignable, et il sortirait alors sans un mot.
    // L'invariant du releve doit fermer par CONSTRUCTION, pas par chance.
    if (!recompense)
    {
        _killsRefused.fetch_add(1, std::memory_order_relaxed);
        _refusedNotPaid.fetch_add(1, std::memory_order_relaxed);
    }

    return recompense;
}

bool MercenaryRewards::EstMercenaire(Player* bot)
{
    // IsMercenary d'abord : c'est un FNV-1a sur le GUID, quelques instructions,
    // et il ecarte la moitie de la population avant qu'on paie la recherche de
    // IsRandomBot (cache de personnage + parcours de la liste des comptes
    // aleatoires). Il rend aussi false d'emblee quand WildPvp est eteint.
    return bot && sPlayerbotAIConfig.IsMercenary(bot->GetGUID().GetRawValue()) &&
           sRandomPlayerbotMgr.IsRandomBot(bot);
}

// La mediane ne porte QUE sur la population chassee : les mercenaires en sont
// exclus. Les y inclure creerait une boucle — leur propre montee releverait la
// mediane, donc leur propre plafond, qui ne freinerait plus rien.
//
// Ce n'est PAS !EstMercenaire : un bot non aleatoire n'est ni mercenaire ni
// chasse, il est simplement hors sujet. Les deux predicats exigent tous deux
// IsRandomBot, et ne different que sur IsMercenary.
bool MercenaryRewards::CompteDansLaMediane(Player* bot)
{
    return bot && sRandomPlayerbotMgr.IsRandomBot(bot) &&
           !sPlayerbotAIConfig.IsMercenary(bot->GetGUID().GetRawValue());
}

void MercenaryRewards::RecomputeMedian()
{
    // COMPTAGE SANS ETAT. On rebalaie la population en ligne a chaque passage
    // plutot que de tenir un histogramme par evenements : voir l'en-tete pour
    // les deux facons dont ce comptage incremental etait faux. Un balayage ne
    // peut pas deriver, puisqu'il ne se souvient de rien.
    //
    // FIL. Cette fonction ne part que de MercenaryRewardsWorldScript::OnUpdate
    // (plus bas dans ce fichier : un WorldScript declare avec
    // WORLDHOOK_ON_UPDATE), donc du fil du monde. La table parcourue est
    // PlayerbotHolder::playerBots de sRandomPlayerbotMgr ; elle n'a pas de
    // verrou, et voici pourquoi il n'en faut pas ici.
    //
    // Ses deux seules ecritures sont playerBots[bot->GetGUID()] = bot
    // (PlayerbotMgr.cpp:470, dans PlayerbotHolder::OnBotLogin) et
    // playerBots.erase(guid) (PlayerbotMgr.cpp:445, dans
    // PlayerbotHolder::RemoveFromPlayerbotsMap). Leurs appelants reels :
    //   - OnBotLogin n'a qu'un seul appelant vivant, OnBotLoginOperation::
    //     Execute (Script/WorldThr/PlayerbotOperations.h:512), qu'execute
    //     PlayerbotWorldThreadProcessor::Update ;
    //   - RemoveFromPlayerbotsMap part de PlayerbotHolder::LogoutPlayerBot
    //     (PlayerbotMgr.cpp:406) et de PlayerbotHolder::DisablePlayerBot
    //     (PlayerbotMgr.cpp:437) ; sur sRandomPlayerbotMgr, LogoutPlayerBot est
    //     appele par RandomPlayerbotMgr::ProcessBot (RandomPlayerbotMgr.cpp:1358
    //     et 1435) et par RandomPlayerbotMgr::Remove (RandomPlayerbotMgr.cpp:3137).
    // Le processeur d'operations et ProcessBot tournent tous deux sous
    // PlayerbotsWorldScript::OnUpdate (Playerbots.cpp:397-401, qui commente
    // lui-meme sRandomPlayerbotMgr.UpdateAI << World thread only >>). Deux
    // WorldScript::OnUpdate sont appeles en sequence depuis World::Update : ils
    // ne peuvent pas s'entrelacer avec ce balayage.
    //
    // Le seul ecrivain potentiel HORS du fil du monde est
    // PlayerbotAI::UpdateAIInternal (PlayerbotAI.cpp:626), qui appelle
    // sRandomPlayerbotMgr.LogoutPlayerBot ; UpdateAIInternal tourne sous
    // Player::Update via OnPlayerAfterUpdate (PlayerUpdates.cpp:432 et
    // Playerbots.cpp:185-192), donc sur l'un des fils de MapUpdate
    // (MapUpdate.Threads = 10 dans la conf en service). Cette branche est
    // aujourd'hui inatteignable pour un bot aleatoire : elle est gardee par
    // bot->GetSession()->isLogingOut(), soit _logoutTime || m_playerLogout
    // (WorldSession.h:544). _logoutTime n'est arme que par le traitement de
    // CMSG_LOGOUT_REQUEST (MiscHandler.cpp:566), qu'aucun code n'envoie a la
    // session d'un bot aleatoire (PlayerbotMgr.cpp:1606 ne traite que le paquet
    // du MAITRE), et m_playerLogout n'est vrai que DANS
    // WorldSession::LogoutPlayer (WorldSession.cpp:714), qui detruit la session.
    // Si un jour un chemin arme ce garde pour un bot aleatoire, ce balayage
    // devra prendre un verrou : la conclusion tient a ce garde, pas a la
    // nature de la table.
    //
    // POINTEURS. playerBots ne retient jamais de pointeur mort :
    // PlayerbotMgr.cpp appelle RemoveFromPlayerbotsMap AVANT
    // WorldSession::LogoutPlayer, qui est ce qui detruit l'objet Player.
    //
    // COUT ESTIME PAR LECTURE, NON MESURE, SUR LA CONF EN SERVICE :
    // MinRandomBots = MaxRandomBots = 500.
    // Par bot, CompteDansLaMediane fait une recherche dans une table de hachage
    // (CharacterCache::GetCharacterAccountIdByGuid, sans verrou), un parcours
    // lineaire de randomBotAccounts (une centaine d'entrees pour 500 bots), une
    // recherche dans currentBots et un FNV-1a. Soit ~50 000 comparaisons
    // d'uint32 toutes les 30 s sur le fil du monde : de l'ordre de la
    // milliseconde au plus, et
    // hors de tout chemin chaud. A comparer au defaut qu'il remplace, qui
    // faussait le plafond de montee pour la duree de vie du processus.
    std::array<uint32, HISTOGRAM_LEVELS> histogram{};
    uint32 total = 0;

    /*
     * LOT 5. L'INDEX DES PROIES SE REMPLIT DANS CE BALAYAGE, ET DANS AUCUN AUTRE.
     *
     * C'est tout l'interet de l'accrocher ici : le parcours de la population
     * existe deja, il tourne deja toutes les 30 s, et la demonstration qu'il est
     * sur sans verrou est ecrite ci-dessus, paragraphe FIL. L'index n'ajoute que
     * quelques instructions par bot -- un predicat et un rangement dans un
     * tableau de taille fixe -- et AUCUN parcours nouveau.
     *
     * DEUX POPULATIONS DISTINCTES, DANS LA MEME BOUCLE, ET C'EST VOULU. La
     * mediane ne compte QUE les bots chasses (CompteDansLaMediane : bot
     * aleatoire et pas mercenaire) ; l'index, lui, RECENSE toute proie -- donc
     * aussi les mercenaires, qui sont des cibles legitimes les uns pour les
     * autres des lors que BotsFightBots vaut 1 (EnemyPlayerValue.cpp, terme
     * `mercenaire`). Compter() est donc appele AVANT le `continue` de la
     * mediane : les fondre serait une erreur silencieuse, puisque les deux
     * predicats se ressemblent sans se valoir.
     *
     * RECENSER N'EST PAS PONDERER, et c'est la distinction que le lot 5 a du
     * apprendre a ses depens. L'index range la part mercenaire de chaque cellule
     * a part, et le facteur de tirage (CoaChasse.h, FacteurPresence) n'emploie
     * que la population EXOGENE, c'est-a-dire le total moins cette part. La
     * raison tient en une phrase : la position des mercenaires est la SORTIE de
     * ce lot, et la remettre dans son entree ne mesure plus rien. Voir
     * << L'INDEX NE SE MANGE PAS LUI-MEME >> dans MercenaryRewards.h.
     *
     * L'INTERRUPTEUR EST LU UNE FOIS, hors de la boucle : le relire par bot
     * laisserait un balayage a moitie rempli si la conf etait rechargee pendant.
     */
    bool const indexProies = sPlayerbotAIConfig.wildPvpIndexProies;
    if (indexProies)
        CoaIndexProies::instance().Debuter();

    auto const botsEnd = sRandomPlayerbotMgr.GetPlayerBotsEnd();
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != botsEnd; ++it)
    {
        Player* bot = it->second;

        if (indexProies)
            CoaIndexProies::instance().Compter(bot);

        if (!CompteDansLaMediane(bot))
            continue;

        uint8 const level = std::min<uint8>(bot->GetLevel(), HISTOGRAM_MAX_LEVEL);
        ++histogram[level];
        ++total;
    }

    // PUBLIER AVANT TOUTE SORTIE. Les deux `return` du calcul de mediane, plus
    // bas, sont des sorties normales et frequentes -- MEDIAN_MIN_POPULATION vaut
    // 10, et un serveur qui demarre passe par la. Publier apres eux laisserait
    // l'index sur sa generation precedente pour 30 s de plus, sans que rien ne
    // le dise.
    if (indexProies)
    {
        CoaIndexProies::instance().Publier();
    }
    else if (CoaIndexProies::instance().Cellules())
    {
        // L'INTERRUPTEUR VIENT D'ETRE ETEINT : ON VIDE, UNE FOIS. Sans cela,
        // l'index garderait sa derniere generation pour toute la duree de
        // l'extinction, et le rallumer donnerait, pendant les 30 s qui separent
        // du balayage suivant, un tirage pondere par une carte vieille de
        // plusieurs heures. Le cout est nul : la condition ne devient vraie
        // qu'au premier passage apres l'extinction, puisque le vidage remet
        // Cellules() a zero.
        //
        // CE VIDAGE NE COUVRE QUE WildPvp.IndexProies, ET C'EST UNE BORNE QU'IL
        // FAUT CONNAITRE : il vit dans RecomputeMedian, que
        // MercenaryRewardsWorldScript::OnUpdate n'appelle plus du tout des que
        // wildPvpEnabled est faux (sortie en une instruction, plus bas dans ce
        // fichier). Couper WildPvp.Enabled en laissant IndexProies a 1 laisse
        // donc ce tampon en place pour une duree illimitee. Ce qui ferme ce
        // second chemin n'est pas ici mais du cote LECTURE :
        // CoaChasseAction::IndexProiesActif (Ai/Coa/CoaChasse.h) exige
        // wildPvpEnabled ET wildPvpIndexProies, ce qui rend le tampon perime
        // ILLISIBLE au lieu de le rendre absent. Corriger au lecteur plutot
        // qu'a l'ecrivain est ce qui rend impossible, par construction, tout
        // regime ou un cote de l'interrupteur tourne sans l'autre.
        CoaIndexProies::instance().Debuter();
        CoaIndexProies::instance().Publier();
    }

    if (total < MEDIAN_MIN_POPULATION)
    {
        _medianBotLevel.store(0, std::memory_order_relaxed);
        return;
    }

    uint32 const half = total / 2;
    uint32 running = 0;
    for (size_t level = 0; level < histogram.size(); ++level)
    {
        running += histogram[level];
        if (running > half)
        {
            _medianBotLevel.store(static_cast<uint8>(level), std::memory_order_relaxed);
            return;
        }
    }

    _medianBotLevel.store(0, std::memory_order_relaxed);
}

// =============================================================================
// L'INDEX << OU SONT LES PROIES >> -- lot 5
// =============================================================================
//
// ENCODAGE D'UNE CASE, et pourquoi tout tient dans UN SEUL uint64.
//
//   bit  63      : case occupee. 0 signifie << vide >>, sans ambiguite.
//   bits 51..62  : la part MERCENAIRE de ce nombre, saturee a 4095.
//   bits 16..50  : la cle -- carte (12), celluleX (10), celluleY (10), palier (3).
//   bits 0..15   : le nombre de proies, sature a 65535.
//
// LES DOUZE BITS DU MILIEU ETAIENT LIBRES, et c'est pour cela qu'ils portent la
// ventilation mercenaire plutot qu'un second tableau : la paire (compte, part
// mercenaire) reste ainsi dans le MEME mot atomique que sa cle, donc indivisible
// pour un lecteur, exactement comme le compte seul l'etait. Un second tableau
// aurait rouvert la fenetre que le paragraphe ci-dessous ferme.
//
// LES DEUX SATURATIONS NE SONT PAS AU MEME NIVEAU -- 4095 pour la part
// mercenaire, 65535 pour le total -- et la difference total - part peut donc
// SUR-ESTIMER la population exogene au-dela de 4095 mercenaires dans une seule
// cellule de 250 yd. Avec MaxRandomBots = 500 en service, c'est hors d'atteinte
// d'un facteur huit ; et l'erreur resterait bornee par le plafond a 4 du
// facteur de tirage. C'est ecrit ici pour que ce soit un choix lu et non une
// coincidence trouvee plus tard.
//
// UN SEUL MOT, ET C'EST LA RAISON D'ETRE DE L'ENCODAGE. Ranger la cle et le
// compte dans DEUX atomiques separes ouvrirait la seule fenetre dangereuse qui
// reste : un lecteur pourrait lire la cle d'une generation et le compte de
// l'autre, et rendre le nombre de proies d'une cellule pour une AUTRE cellule.
// Dans un seul mot, la paire est indivisible : un lecteur voit soit l'ancienne
// entiere, soit la nouvelle entiere.
namespace
{
    constexpr uint64 COA_INDEX_OCCUPE = 1ull << 63;
    constexpr uint32 COA_INDEX_BITS_COMPTE = 16;
    constexpr uint64 COA_INDEX_MAX_COMPTE = 0xFFFFull;
    constexpr uint32 COA_INDEX_DEC_MERC = 51;
    constexpr uint64 COA_INDEX_MAX_MERC = 0xFFFull;  // 12 bits, bits 51..62
    // 12 + 10 + 10 + 3 = 35 bits de cle.
    constexpr uint64 COA_INDEX_MASQUE_CLE = (1ull << 35) - 1;

    // Decalages, du plus fort au plus faible. Ils sont ecrits ici et nulle part
    // ailleurs : CleDuLieu les compose, Proies() n'ajoute que le palier.
    constexpr uint32 COA_INDEX_DEC_CARTE = 23;
    constexpr uint32 COA_INDEX_DEC_X = 13;
    constexpr uint32 COA_INDEX_DEC_Y = 3;

    constexpr uint32 COA_INDEX_MAX_CARTE = 0xFFF;    // 12 bits
    constexpr int32 COA_INDEX_MAX_CELLULE = 1023;    // 10 bits
    // Les coordonnees de monde tiennent dans +/- 17066 yd ; a 250 yd la cellule,
    // l'indice signe tient dans [-69, 68]. Le biais le ramene au milieu de la
    // plage non signee, avec une marge d'un ordre de grandeur de chaque cote.
    constexpr int32 COA_INDEX_BIAIS_CELLULE = 512;
}

CoaIndexProies::CoaIndexProies()
{
    // MISE A ZERO EXPLICITE. En C++20 le constructeur par defaut de
    // std::atomic initialise bien la valeur, et l'objet est de toute facon de
    // duree statique donc zero-initialise avant toute initialisation dynamique.
    // On l'ecrit quand meme : la sur-mesure est gratuite (une fois au demarrage)
    // et elle rend la lecture du code independante de ces deux garanties.
    for (uint8 tampon = 0; tampon < 2; ++tampon)
        for (uint32 i = 0; i < Cases; ++i)
            _table[tampon][i].store(0, std::memory_order_relaxed);
}

// Melangeur final de splitmix64. On ne prend PAS la cle telle quelle : ses bits
// de poids faible sont le palier et l'indice de cellule Y, donc deux cles
// voisines se suivraient dans la table et le sondage lineaire degenererait en
// grappes. Le melangeur diffuse chaque bit sur les 64.
uint32 CoaIndexProies::Empreinte(uint64 cle)
{
    uint64 x = cle + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x = x ^ (x >> 31);
    return uint32(x);
}

bool CoaIndexProies::IndicesDuLieu(uint32 carte, float x, float y, int32& celluleX,
                                   int32& celluleY, int32& voisinX, int32& voisinY)
{
    // NON FINI D'ABORD. std::floor d'un NaN rend un NaN, et convertir un NaN en
    // int32 est un COMPORTEMENT INDEFINI -- pas une valeur absurde. Une position
    // ne devrait jamais l'etre, mais l'index recense des Player* en cours de
    // teleportation et lit des points venus d'une table SQL : on ne le suppose
    // pas.
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;

    if (carte > COA_INDEX_MAX_CARTE)
        return false;

    float const bruteX = std::floor(x / TailleCellule);
    float const bruteY = std::floor(y / TailleCellule);

    // Borne AVANT la conversion, et sur le flottant : une coordonnee aberrante
    // mais finie (1e30) deborderait l'int32 a la conversion, ce qui est encore
    // un comportement indefini.
    // Les bornes sont celles de l'indice BIAISE, exprimees avant le biais :
    // [-512, 511] une fois biaise donne [0, 1023], soit les dix bits exacts. Une
    // premiere redaction bornait a COA_INDEX_MAX_CELLULE des DEUX cotes, ce qui
    // laissait passer jusqu'a 1023 avant biais -- rattrape par le controle
    // suivant, mais la borne annoncee n'etait pas celle appliquee.
    constexpr float minCellule = -float(COA_INDEX_BIAIS_CELLULE);
    constexpr float maxCellule = float(COA_INDEX_MAX_CELLULE - COA_INDEX_BIAIS_CELLULE);

    if (bruteX < minCellule || bruteX > maxCellule ||
        bruteY < minCellule || bruteY > maxCellule)
    {
        return false;
    }

    celluleX = int32(bruteX) + COA_INDEX_BIAIS_CELLULE;
    celluleY = int32(bruteY) + COA_INDEX_BIAIS_CELLULE;

    // LA VOISINE EST CELLE DU COTE OU LE POINT TOMBE, et c'est tout ce qui fait
    // la difference entre un bloc 2x2 utile et un bloc pris au hasard. Un poste
    // a 3 yd du bord bas de sa cellule doit regarder la cellule d'EN DESSOUS, et
    // surtout pas celle d'au-dessus, dont le bord le plus proche est deja a
    // 247 yd. Le reste de la division dit de quel cote, et il se calcule sans
    // second floor puisque bruteX/bruteY sont deja la.
    //
    // Les deux soustractions sont sures : bruteX est borne a [-512, 511], donc
    // x l'est a [-128000, 128250) et le reste tient largement dans la precision
    // d'un float a cette magnitude (ulp ~ 0,0078 yd). Une erreur de bord ne
    // ferait que choisir l'autre voisine, jamais sortir d'un tableau.
    float const resteX = x - bruteX * TailleCellule;
    float const resteY = y - bruteY * TailleCellule;

    voisinX = celluleX + (resteX < TailleCellule * 0.5f ? -1 : 1);
    voisinY = celluleY + (resteY < TailleCellule * 0.5f ? -1 : 1);
    return true;
}

bool CoaIndexProies::CleDeCellule(uint32 carte, int32 celluleX, int32 celluleY, uint64& cle)
{
    if (carte > COA_INDEX_MAX_CARTE)
        return false;

    // C'EST ICI QU'UNE VOISINE HORS DU MONDE EST ECARTEE, et c'est pour cela que
    // IndicesDuLieu ne borne PAS les voisines : au bord de la plage, celluleX
    // vaut 0 ou 1023 et la voisine tombe a -1 ou 1024. Le refus est rendu au
    // lecteur, qui saute simplement cette case du bloc.
    //
    // Le controle est aussi celui que CleDuLieu portait en garde de coherence :
    // si l'un des trois nombres (biais, nombre de bits, taille de cellule)
    // bougeait un jour, c'est ici que l'incoherence est arretee au lieu d'ecrire
    // une cle qui empiete sur les bits voisins.
    if (celluleX < 0 || celluleX > COA_INDEX_MAX_CELLULE ||
        celluleY < 0 || celluleY > COA_INDEX_MAX_CELLULE)
    {
        return false;
    }

    cle = (uint64(carte) << COA_INDEX_DEC_CARTE) |
          (uint64(uint32(celluleX)) << COA_INDEX_DEC_X) |
          (uint64(uint32(celluleY)) << COA_INDEX_DEC_Y);
    return true;
}

// ECRITURE SEULEMENT : une proie est rangee dans SA cellule, pas dans un bloc.
// C'est la lecture, et elle seule, qui elargit -- ranger une proie dans quatre
// cellules la compterait quatre fois et saturerait la table pour rien.
bool CoaIndexProies::CleDuLieu(uint32 carte, float x, float y, uint64& cle)
{
    int32 celluleX = 0;
    int32 celluleY = 0;
    int32 voisinX = 0;
    int32 voisinY = 0;

    if (!IndicesDuLieu(carte, x, y, celluleX, celluleY, voisinX, voisinY))
        return false;

    return CleDeCellule(carte, celluleX, celluleY, cle);
}

bool CoaIndexProies::EstProie(Player* joueur)
{
    if (!joueur || !joueur->IsInWorld() || !joueur->IsAlive())
        return false;

    // EN COURS DE TELEPORTATION, LA POSITION EST CELLE DU DEPART OU CELLE DE
    // L'ARRIVEE selon l'etape : on ne recense pas un bot dont on ne sait pas ou
    // il est. Player::IsBeingTeleported (Player.h:2171) est la question exacte.
    if (joueur->IsBeingTeleported())
        return false;

    // Player::IsFFAPvP et non Unit::IsFFAPvP : c'est celle que le choix de cible
    // emploie (EnemyPlayerValue.cpp, << enemy->IsFFAPvP() >> sur un Player*), et
    // elle passe par le crochet OnPlayerIsFFAPvP (Player.cpp:16941-16948). Un
    // module qui modifierait la reponse la modifierait des deux cotes : l'index
    // ne peut pas diverger du predicat qu'il est cense predire.
    return joueur->IsFFAPvP();
}

void CoaIndexProies::Debuter()
{
    _enEcriture = uint8(1 - _publie.load(std::memory_order_relaxed));

    std::atomic<uint64>* table = _table[_enEcriture];
    for (uint32 i = 0; i < Cases; ++i)
        table[i].store(0, std::memory_order_relaxed);

    _cellulesEnCours = 0;
    _recenseesEnCours = 0;
    _debordementsEnCours = 0;
    _horsBornesEnCours = 0;
    _mercenairesEnCours = 0;
    _sousTreveEnCours = 0;
}

void CoaIndexProies::Ajouter(uint64 cle, bool estMercenaire)
{
    std::atomic<uint64>* table = _table[_enEcriture];
    uint32 indice = Empreinte(cle) & (Cases - 1);

    for (uint32 sondage = 0; sondage < MaxSondages; ++sondage)
    {
        // relaxed : le fil du monde est le SEUL ecrivain de ce tampon, et ce
        // tampon n'est pas encore publie. Il n'y a donc rien a ordonner ici ;
        // l'ordre est etabli une seule fois, par le store release de Publier().
        uint64 const valeur = table[indice].load(std::memory_order_relaxed);

        if (!valeur)
        {
            uint64 const merc = estMercenaire ? 1ull : 0ull;
            table[indice].store(COA_INDEX_OCCUPE | (merc << COA_INDEX_DEC_MERC) |
                                    (cle << COA_INDEX_BITS_COMPTE) | 1,
                                std::memory_order_relaxed);
            ++_cellulesEnCours;
            return;
        }

        if (((valeur >> COA_INDEX_BITS_COMPTE) & COA_INDEX_MASQUE_CLE) == cle)
        {
            uint64 compte = valeur & COA_INDEX_MAX_COMPTE;
            // SATURATION PLUTOT QU'ENROULEMENT. A 65536 proies dans une cellule
            // de 250 yd le serveur a d'autres problemes, mais un compte qui
            // repasse a zero ferait passer la cellule la plus peuplee du monde
            // pour un desert, sans un mot.
            if (compte < COA_INDEX_MAX_COMPTE)
                ++compte;

            // MEME REGLE POUR LA PART MERCENAIRE, sur douze bits. Elle sature
            // donc AVANT le total ; l'en-tete de l'encodage dit ce que cet ecart
            // coute, et pourquoi il est hors d'atteinte en service.
            uint64 merc = (valeur >> COA_INDEX_DEC_MERC) & COA_INDEX_MAX_MERC;
            if (estMercenaire && merc < COA_INDEX_MAX_MERC)
                ++merc;

            table[indice].store(COA_INDEX_OCCUPE | (merc << COA_INDEX_DEC_MERC) |
                                    (cle << COA_INDEX_BITS_COMPTE) | compte,
                                std::memory_order_relaxed);
            return;
        }

        indice = (indice + 1) & (Cases - 1);
    }

    // Table saturee ou grappe trop longue : la proie n'est pas comptee. On ne
    // la range pas de force ailleurs -- une entree posee hors de sa sequence de
    // sondage serait INTROUVABLE a la lecture, donc pire qu'absente.
    ++_debordementsEnCours;
}

void CoaIndexProies::Compter(Player* joueur)
{
    if (!EstProie(joueur))
        return;

    // LA TREVE SE FILTRE ICI, ET C'EST LE SEUL ENDROIT OU ELLE NE COUTE RIEN.
    // AcceptUnit refuse une cible sous treve (EnemyPlayerValue.cpp:215-220) ;
    // sans ce filtre, les cellules ou l'on vient de se battre -- cimetieres, et
    // plus generalement tout lieu ou k bots viennent de mourir et de relacher --
    // sortaient au facteur maximal pendant deux generations d'index, et y
    // tiraient les mercenaires du palier vers le seul endroit ou, pour
    // TruceAfterDeathSec secondes, rien n'etait frappable. L'index designait
    // ainsi comme riche exactement ce qui etait vide.
    //
    // CE QUE L'APPEL COUTE ICI : une fois par bot toutes les 30 s, sur le fil du
    // monde. IsUnderTruce sort sur un load relaxe tant qu'aucune treve n'est
    // armee, et ne prend qu'un shared_lock sinon -- les fils de carte prennent
    // le meme en partage, donc aucune serialisation contre eux. L'interdit du
    // dossier porte sur le cote LECTURE, ou l'appel se paierait par candidat par
    // mercenaire depuis trois fils de carte ; voir l'en-tete de CoaIndexProies.
    if (MercenaryRewards::instance().IsUnderTruce(joueur->GetGUID().GetRawValue()))
    {
        ++_sousTreveEnCours;
        return;
    }

    uint64 cle = 0;
    if (!CleDuLieu(joueur->GetMapId(), joueur->GetPositionX(), joueur->GetPositionY(), cle))
    {
        // UN REFUS NE SE TAIT PAS. Sans ce compteur, une carte au-dela de 4095
        // ou un lot de positions aberrantes retirerait silencieusement une part
        // de la population de l'index, et le tirage se ferait sur une carte
        // trouee sans que rien ne l'indique.
        ++_horsBornesEnCours;
        return;
    }

    // LE PALIER EST CELUI DE LA PROIE, pas celui du chasseur. La lecture, elle,
    // somme les paliers que le chasseur peut attaquer -- voir Proies().
    uint64 const palier = std::min<uint64>(joueur->GetLevel() / TaillePalier, Paliers - 1);

    // LE CAMP DU RECENSE, ET POURQUOI C'EST EstMercenaire ET PAS IsMercenary.
    // Piege 27 du dossier : deux predicats concurrents definissent le camp, et
    // ils ne coincident pas. IsMercenary est un hachage seul, donc un alt humain
    // au hachage favorable y entre ; EstMercenaire y ajoute IsRandomBot. Ici on
    // veut savoir si ce bot est un CHASSEUR pilote par ce lot, c'est-a-dire un
    // bot aleatoire mercenaire : c'est EstMercenaire, celui que
    // CompteDansLaMediane emploie deja deux lignes plus bas dans la meme boucle.
    //
    // L'appel n'est paye que pour les proies, apres EstProie et apres la treve,
    // et il est sur parce que ce code est sur le fil du monde -- voir la
    // declaration de Compter dans MercenaryRewards.h.
    //
    // LA SEULE FUITE CONNUE, ecrite pour ne pas etre redecouverte : la boucle
    // porteuse parcourt le `playerBots` de sRandomPlayerbotMgr, tandis que
    // IsRandomBot interroge `currentBots`. Les deux ne sont pas peuples au meme
    // instant a la connexion d'un bot ; pendant cette fenetre, un mercenaire
    // aleatoire sort de EstMercenaire a false et compte donc comme proie
    // exogene. La fuite est bornee a quelques bots et a une seule generation
    // d'index (30 s), et son seul effet est de majorer un facteur deja plafonne
    // a 4. La refermer demanderait de toucher currentBots, c'est-a-dire P-050,
    // qui n'est pas dans ce mandat.
    bool const estMercenaire = MercenaryRewards::EstMercenaire(joueur);

    Ajouter(cle | palier, estMercenaire);
    ++_recenseesEnCours;
    if (estMercenaire)
        ++_mercenairesEnCours;
}

void CoaIndexProies::Publier()
{
    _cellules.store(_cellulesEnCours, std::memory_order_relaxed);
    _recensees.store(_recenseesEnCours, std::memory_order_relaxed);
    _debordements.store(_debordementsEnCours, std::memory_order_relaxed);
    _horsBornes.store(_horsBornesEnCours, std::memory_order_relaxed);
    _mercenaires.store(_mercenairesEnCours, std::memory_order_relaxed);
    _sousTreve.store(_sousTreveEnCours, std::memory_order_relaxed);

    // L'ECHANGE, ET LA SEULE BARRIERE DE TOUT CE FICHIER. Le store release
    // garantit que toutes les ecritures de cases faites plus haut sont visibles
    // par tout lecteur qui verra ce nouvel indice par son load acquire.
    _publie.store(_enEcriture, std::memory_order_release);
}

uint32 CoaIndexProies::Proies(uint32 carte, float x, float y, uint8 niveauChasseur,
                              uint32* mercenaires) const
{
    // MIS A ZERO D'ENTREE, ET AVANT TOUTE SORTIE. Il y en a trois ci-dessous,
    // et un appelant qui lirait une sortie non ecrite retrancherait une valeur
    // de pile a un total nul.
    if (mercenaires)
        *mercenaires = 0;

    uint8 const tampon = _publie.load(std::memory_order_acquire);
    if (tampon > 1)
        return 0;  // ne peut pas arriver ; une lecture hors tableau, si

    int32 celluleX = 0;
    int32 celluleY = 0;
    int32 voisinX = 0;
    int32 voisinY = 0;
    if (!IndicesDuLieu(carte, x, y, celluleX, celluleY, voisinX, voisinY))
        return 0;

    /*
     * QUELS PALIERS COMPTER, et pourquoi c'est un plafond et pas une fenetre.
     *
     * Le SEUL refus categorique du filtre de niveau de l'attaque est le haut :
     * PossibleTargetsValue.cpp:94-95 fait
     *     if (levelDifference >= EXTREME_LEVEL_DIFF) return false;
     * avec EXTREME_LEVEL_DIFF = 5 (:22). Une cible de niveau bot+5 ou plus n'est
     * JAMAIS attaquee. En dessous, tout reste possible : l'ecart ne fait que
     * baisser la probabilite a 75 %, 50 % ou 25 % (:101-110). Une proie tres
     * inferieure compte donc, a juste titre.
     *
     * On somme les paliers 0 a (niveau + 4) / 10. Une proie de niveau L' <= L+4
     * est dans le palier L'/10 <= (L+4)/10 : elle est donc TOUJOURS comptee --
     * l'index ne sous-estime jamais. Il sur-estime en revanche jusqu'a neuf
     * niveaux dans le palier du haut : un chasseur de niveau 21 compte le palier
     * 2, qui contient des proies de niveau 29 qu'il ne frappera jamais. C'est le
     * prix d'un palier de dix, assume : l'index PONDERE un tirage, il ne decide
     * pas d'une cible. Le juge de la cible reste ChoisirProie
     * (Ai/Coa/CoaChasse.h), qui rejoue AttackersValue::IsPossibleTarget puis ce
     * meme filtre de niveau -- INVARIANT 2 du dossier.
     */
    uint32 const plafond =
        std::min<uint32>(Paliers - 1, (uint32(niveauChasseur) + 4) / TaillePalier);

    std::atomic<uint64> const* table = _table[tampon];
    uint32 total = 0;
    uint32 totalMercenaires = 0;

    /*
     * LE BLOC 2x2, ET CE QU'IL COUTE. Quatre cellules -- celle du point et les
     * trois voisines du cote ou il tombe -- au lieu d'une seule. La raison est
     * a TailleCellule (MercenaryRewards.h) : lire une seule case faisait
     * dependre le compte de la POSITION du poste dans sa cellule et non de la
     * densite reelle, et le plafond a 4 du facteur amplifiait ce biais au lieu
     * de l'amortir.
     *
     * COUT : au plus 4 x 8 = 32 sequences de sondage au lieu de 8, chacune
     * s'arretant a la premiere case vide ou a la cle trouvee. Sur le pire cas
     * mesure du catalogue livre (carte 530, niveau 70, 1500 yd : 118 candidats),
     * cela fait ~3 800 chargements atomiques relaxed au lieu de ~950 -- soit
     * 30 Kio de tampons parcourus dans le pire des cas, et toujours un ordre de
     * grandeur sous le prix d'UN SEUL EstChassable, qui peut faire lire une
     * tuile .map sur disque sous le mutex de grille.
     *
     * PAS DE DOUBLE COMPTE : les quatre cles sont distinctes par construction
     * (la voisine est a +/-1 de la cellule, jamais elle-meme), et une proie
     * n'est ecrite que dans UNE cellule (voir CleDuLieu).
     *
     * UNE VOISINE HORS DU MONDE EST SIMPLEMENT SAUTEE : CleDeCellule rend false
     * et le bloc se reduit alors a deux ou une cellule, au bord de la plage
     * d'encodage. Aucun cas particulier ici.
     */
    int32 const colonnes[2] = { celluleX, voisinX };
    int32 const lignes[2] = { celluleY, voisinY };

    for (uint8 c = 0; c < 2; ++c)
    {
        for (uint8 l = 0; l < 2; ++l)
        {
            uint64 base = 0;
            if (!CleDeCellule(carte, colonnes[c], lignes[l], base))
                continue;

            for (uint32 palier = 0; palier <= plafond; ++palier)
            {
                uint64 const cle = base | uint64(palier);
                uint32 indice = Empreinte(cle) & (Cases - 1);

                for (uint32 sondage = 0; sondage < MaxSondages; ++sondage)
                {
                    uint64 const valeur = table[indice].load(std::memory_order_relaxed);
                    if (!valeur)
                        break;  // case vide : la cle n'est pas dans cette sequence

                    if (((valeur >> COA_INDEX_BITS_COMPTE) & COA_INDEX_MASQUE_CLE) == cle)
                    {
                        total += uint32(valeur & COA_INDEX_MAX_COMPTE);
                        totalMercenaires +=
                            uint32((valeur >> COA_INDEX_DEC_MERC) & COA_INDEX_MAX_MERC);
                        break;
                    }

                    indice = (indice + 1) & (Cases - 1);
                }
            }
        }
    }

    // Aucun debordement possible : chaque case vaut au plus 65535, on en somme
    // au plus 4 x 8 = 32, soit 2 097 120 -- tres loin du plafond d'un uint32.
    if (mercenaires)
        *mercenaires = totalMercenaires;

    return total;
}

// =============================================================================
// SCRIPTS
// =============================================================================
class MercenaryRewardsWorldScript : public WorldScript
{
public:
    MercenaryRewardsWorldScript()
        : WorldScript("MercenaryRewardsWorldScript", { WORLDHOOK_ON_UPDATE })
    {
    }

    void OnUpdate(uint32 diff) override
    {
        if (!sPlayerbotAIConfig.wildPvpEnabled)
            return;

        MercenaryRewards::instance().Update(diff);
    }
};

class MercenaryRewardsPlayerScript : public PlayerScript
{
public:
    MercenaryRewardsPlayerScript()
        // Un seul crochet depuis que la mediane se recalcule par balayage :
        // les trois crochets de connexion, de deconnexion et de changement de
        // niveau n'avaient d'objet que pour tenir l'histogramme incremental.
        : PlayerScript("MercenaryRewardsPlayerScript", { PLAYERHOOK_ON_PVP_KILL })
    {
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        MercenaryRewards::instance().OnPvpKill(killer, killed);
    }
};

void AddSC_mercenary_rewards()
{
    new MercenaryRewardsWorldScript();
    new MercenaryRewardsPlayerScript();
}
