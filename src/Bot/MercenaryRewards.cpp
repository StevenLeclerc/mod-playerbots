/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MercenaryRewards.h"
#include "Formulas.h"
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
#include <limits>

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

        // TOUJOURS emettre, meme quand tous les compteurs sont a zero.
        // La version precedente se taisait tant que rien ne bougeait : c'est
        // exactement le moment ou l'on a besoin de savoir que le systeme est
        // vivant. Un silence devenait indistinguable d'une panne, et il a
        // coute une heure le 20/09 (P-061).
        //
        // Logger "playerbots.coa" et non "playerbots" : SEUL le premier a une
        // ligne Logger dans worldserver.conf. "playerbots" retombe sur
        // Logger.root = 2 (ERROR) et ses INFO sont jetes SANS un mot.
        LOG_INFO("playerbots.coa",
            "[Mercenaire] mediane {} - morts PvP vues {} - tueur hors camp {} - payes {} - "
            "refus {} (gris {}, plafond {}, recidive {}, cadence {}) - treves {}",
            static_cast<uint32>(MedianBotLevel()),
            _pvpKillsSeen.load(std::memory_order_relaxed),
            _killerNotMercenary.load(std::memory_order_relaxed),
            _killsRewarded.load(std::memory_order_relaxed),
            _killsRefused.load(std::memory_order_relaxed),
            _refusedGrey.load(std::memory_order_relaxed),
            _refusedLevelCap.load(std::memory_order_relaxed),
            _refusedRepeat.load(std::memory_order_relaxed),
            _refusedRate.load(std::memory_order_relaxed),
            _activeTruces.load(std::memory_order_relaxed));
    }

    _purgeTimer += diff;
    if (_purgeTimer < PURGE_PERIOD_MS)
        return;

    _purgeTimer = 0;

    time_t const now = time(nullptr);
    std::lock_guard<std::mutex> guard(_mutex);

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
    // Chemin chaud : appele pour chaque cible envisagee par chaque mercenaire.
    // Tant qu'aucune treve n'est armee, on ne prend aucun verrou.
    if (!_activeTruces.load(std::memory_order_relaxed))
        return false;

    std::lock_guard<std::mutex> lock(_mutex);
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
        std::lock_guard<std::mutex> guard(_mutex);
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
        return;

    killer->GiveXP(xp, nullptr);
    _killsRewarded.fetch_add(1, std::memory_order_relaxed);

    LOG_DEBUG("playerbots.coa", "[Mercenaire] {} (niveau {}) gagne {} xp sur {} (niveau {})",
        killer->GetName(), static_cast<uint32>(killer->GetLevel()), xp, killed->GetName(),
        static_cast<uint32>(killed->GetLevel()));
}

uint32 MercenaryRewards::ComputeReward(Player* killer, Player* killed)
{
    uint32 const divisor = sPlayerbotAIConfig.wildPvpKillXpDivisor;
    if (!divisor)
        return 0;

    // Seuls les bots aleatoires mercenaires gagnent a ce jeu. Sans le test
    // IsRandomBot, IsMercenary hacherait aussi le GUID d'un joueur humain et
    // en ferait un mercenaire a son insu.
    if (!sRandomPlayerbotMgr.IsRandomBot(killer) ||
        !sPlayerbotAIConfig.IsMercenary(killer->GetGUID().GetRawValue()))
    {
        _killerNotMercenary.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    uint8 const killerLevel = killer->GetLevel();
    uint8 const victimLevel = killed->GetLevel();

    if (killerLevel >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        return 0;

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
        std::lock_guard<std::mutex> guard(_mutex);
        KillerRecord& record = _killers[killerGuid];

        // Recidive sur la meme victime. Le coeur n'a aucune decroissance pour
        // les kills repetes hors champ de bataille : sans ce garde-fou, deux
        // mercenaires qui se tuent en boucle atteignent le niveau 80 en
        // quelques minutes.
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

        // Plafond horaire, toutes victimes confondues : la borne dure.
        while (!record.RecentKills.empty() && record.RecentKills.front() < now - KILL_RATE_WINDOW_S)
            record.RecentKills.pop_front();

        if (uint32 perHour = sPlayerbotAIConfig.wildPvpKillsPerHourCap)
        {
            if (record.RecentKills.size() >= perHour)
            {
                _killsRefused.fetch_add(1, std::memory_order_relaxed);
                _refusedRate.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
        }

        record.LastKillOf[victimGuid] = now;
        record.RecentKills.push_back(now);
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

    return static_cast<uint32>(std::min<uint64>(xp, std::numeric_limits<uint32>::max()));
}

// La mediane ne porte QUE sur la population chassee : les mercenaires en sont
// exclus. Les y inclure creerait une boucle — leur propre montee releverait la
// mediane, donc leur propre plafond, qui ne freinerait plus rien.
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

    auto const botsEnd = sRandomPlayerbotMgr.GetPlayerBotsEnd();
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != botsEnd; ++it)
    {
        Player* bot = it->second;
        if (!CompteDansLaMediane(bot))
            continue;

        uint8 const level = std::min<uint8>(bot->GetLevel(), HISTOGRAM_MAX_LEVEL);
        ++histogram[level];
        ++total;
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
