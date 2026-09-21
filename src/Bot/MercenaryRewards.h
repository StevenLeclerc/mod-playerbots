/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MERCENARYREWARDS_H
#define PLAYERBOTS_MERCENARYREWARDS_H

#include "ObjectGuid.h"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <unordered_map>

class Player;

// --- PvP mercenaire (CoA) ---------------------------------------------------
//
// Progression des mercenaires par le PvP seul. Hors champ de bataille, le coeur
// ne donne AUCUNE experience pour un joueur tue : KillRewarder::_InitXP exige
// `!_isPvP`, et _RewardPlayer exige `!_isPvP || _isBattleGround`. Rien non plus
// du cote de l'honneur quand la victime est de la meme faction, le royaume
// n'etant pas FFA (World::IsFFAPvPRealm, GameType != 16). Un mercenaire qui tue
// un autre bot ne gagne donc rien : c'est ce que cette classe repare.
//
// Elle porte quatre choses, et rien d'autre :
//   1. la recompense d'experience, avec ses garde-fous ;
//   2. la treve de ciblage qui suit une mort, pour qu'on ne soit pas retue au
//      cimetiere ;
//   3. la mediane des niveaux des bots en ligne, qui plafonne la montee d'un
//      mercenaire ;
//   4. la purge periodique de ses propres tables.
//
// CONCURRENCE. OnPlayerPVPKill part depuis Unit::Kill, donc depuis l'un des
// fils de MapUpdate.Threads, et IsUnderTruce est lu depuis le choix de cible de
// chaque bot. Les deux tables sont donc protegees par un verrou unique. Le
// chemin chaud (aucune treve active) n'en prend aucun, grace au compteur
// atomique ActiveTruces. Ce soin n'est pas de la prudence de principe : c'est
// P-050, deux conteneurs globaux sans verrou touches depuis dix fils, et le
// monde qui tombe.
//
// LA MEDIANE, ELLE, N'A AUCUN ETAT. Elle etait tenue par un histogramme
// incremente a la connexion et decremente a la deconnexion ; ce comptage etait
// faux de deux facons, et muettement :
//   - RandomPlayerbotMgr::ProcessBot fait `currentBots.erase(bot)` AVANT
//     `LogoutPlayerBot` dans son chemin << bot hors groupe >>
//     (RandomPlayerbotMgr.cpp:1356-1358 ; son autre chemin, :1435-1436, fait
//     l'inverse), or IsRandomBot n'est rien d'autre que `currentBots.contains`
//     : le crochet OnPlayerLogout trouvait alors deja faux le predicat qui
//     avait ete vrai a la connexion, et le decrement etait saute ;
//   - le predicat depend aussi de IsMercenary, donc de WildPvp.Enabled et de
//     MercenaryPercent, rechargeables a chaud par `.bot reload` et
//     `.rndbot reload` : un rechargement laissait des entrees fantomes pour la
//     duree de vie du processus.
// Les entrees fantomes ne se voyaient nulle part, et la mediane est l'UNIQUE
// entree du plafond de montee (ComputeReward). On rebalaie donc la population
// en ligne toutes les 30 s : sans etat, rien ne peut deriver.
class MercenaryRewards
{
public:
    static MercenaryRewards& instance()
    {
        static MercenaryRewards instance;
        return instance;
    }

    // Appele depuis un WorldScript : purge les tables et recalcule la mediane.
    void Update(uint32 diff);

    // Un joueur (bot ou humain) en a tue un autre. Decide et verse la
    // recompense, puis arme la treve de la victime. Les deux parties sont
    // independantes : une victime beneficie de la treve meme quand le tueur
    // n'a droit a rien.
    void OnPvpKill(Player* killer, Player* killed);

    // Vrai tant que la victime est sous treve : aucun mercenaire ne doit la
    // convoiter. Chemin chaud, appele a chaque choix de cible.
    bool IsUnderTruce(uint64 guid) const;

    // 0 quand la population est trop maigre pour qu'une mediane veuille dire
    // quelque chose : l'appelant doit alors renoncer au plafond, pas le serrer.
    uint8 MedianBotLevel() const { return _medianBotLevel.load(std::memory_order_relaxed); }

    // Comptes rendus, pour la sonde et la console.
    uint32 KillsRewarded() const { return _killsRewarded.load(std::memory_order_relaxed); }
    uint32 KillsRefused() const { return _killsRefused.load(std::memory_order_relaxed); }

private:
    MercenaryRewards() = default;
    ~MercenaryRewards() = default;

    MercenaryRewards(MercenaryRewards const&) = delete;
    MercenaryRewards& operator=(MercenaryRewards const&) = delete;

    // Ce que le tueur a fait recemment. Deux garde-fous distincts : ne pas
    // payer deux fois la meme victime, et ne pas payer plus de N fois par
    // heure quelle que soit la victime.
    struct KillerRecord
    {
        std::unordered_map<uint64, time_t> LastKillOf;
        std::deque<time_t> RecentKills;
    };

    // Renvoie 0 quand un garde-fou refuse. Prend le verrou.
    uint32 ComputeReward(Player* killer, Player* killed);

    // Un bot compte dans la mediane s'il fait partie de la population CHASSEE :
    // bot aleatoire, et pas mercenaire lui-meme.
    static bool CompteDansLaMediane(Player* bot);

    // Rebalaie la population en ligne et republie la mediane. Appele depuis le
    // fil du monde uniquement (MercenaryRewardsWorldScript::OnUpdate). Le
    // parcours se fait sans verrou : la demonstration, qui ne tient PAS a la
    // seule nature de la table, est dans le corps de la fonction, paragraphe
    // FIL.
    void RecomputeMedian();

    mutable std::mutex _mutex;
    std::unordered_map<uint64, KillerRecord> _killers;
    std::unordered_map<uint64, time_t> _truces;

    // Chemin chaud : tant qu'il vaut 0, IsUnderTruce sort sans verrou.
    std::atomic<uint32> _activeTruces{ 0 };

    std::atomic<uint8> _medianBotLevel{ 0 };

    // Compteurs separes par CAUSE. Un seul compteur de refus ne permettait pas
    // de distinguer << personne ne tue >> de << des gens tuent, mais jamais un
    // mercenaire >> : les deux donnaient zero recompense et zero refus, parce
    // qu'un tueur non mercenaire sortait sans rien compter du tout.
    std::atomic<uint32> _pvpKillsSeen{ 0 };       // toute mort joueur par joueur
    std::atomic<uint32> _killerNotMercenary{ 0 }; // tueur hors du camp mercenaire
    std::atomic<uint32> _killsRewarded{ 0 };
    std::atomic<uint32> _killsRefused{ 0 };
    std::atomic<uint32> _refusedGrey{ 0 };
    std::atomic<uint32> _refusedLevelCap{ 0 };
    std::atomic<uint32> _refusedRepeat{ 0 };
    std::atomic<uint32> _refusedRate{ 0 };

    uint32 _purgeTimer = 0;
    uint32 _medianTimer = 0;
    uint32 _rapportTimer = 0;
};

void AddSC_mercenary_rewards();

#endif // PLAYERBOTS_MERCENARYREWARDS_H
