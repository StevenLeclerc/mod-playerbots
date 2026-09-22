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
#include <shared_mutex>
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
// chaque bot. Les deux tables sont donc protegees par un verrou unique. Ce soin
// n'est pas de la prudence de principe : c'est P-050, deux conteneurs globaux
// sans verrou touches depuis dix fils, et le monde qui tombe.
//
// LE VERROU EST UN std::shared_mutex, ET C'EST LA LECTURE QUI L'EXIGE. La
// version precedente prenait un std::mutex et se reposait sur un chemin rapide
// << aucune treve armee, aucun verrou >>, garde par le compteur atomique
// _activeTruces. Ce chemin rapide ne se realise JAMAIS en service, et l'en-tete
// affirmait donc une propriete fausse : _activeTruces est recopie de
// _truces.size(), qui compte aussi les treves EXPIREES jusqu'a la purge, et la
// purge ne passe que toutes les 60 s (PURGE_PERIOD_MS) alors que
// TruceAfterDeathSec vaut 60 pour ~250 mercenaires qui se battent en continu :
// la table ne se vide jamais. Chaque appel prenait donc le verrou exclusif, et
// l'appelant est NearestEnemyPlayersValue::AcceptUnit (EnemyPlayerValue.cpp),
// evalue pour chaque candidat de chaque mercenaire, depuis les trois fils de
// carte les plus charges. C'etait un point de serialisation unique sur le
// chemin le plus frequent du serveur. En shared_mutex, ces lectures ne se
// bloquent plus entre elles ; seules les poses de treve et les purges, rares,
// prennent l'exclusivite. Le chemin rapide _activeTruces est conserve : il ne
// mord pas en service, mais il reste juste et gratuit quand WildPvp est eteint.
//
// CE QUE CE CHOIX COUTE, DIT FRANCHEMENT. Un shared_mutex de la glibc est par
// defaut favorable aux LECTEURS : un ecrivain peut theoriquement attendre tant
// que des lecteurs se succedent sans interruption. Le risque est ici tenu pour
// negligeable, et par raisonnement, pas par mesure : les sections critiques en
// lecture sont d'une recherche dans une table de hachage, trois fils de carte
// seulement les prennent, et les ecrivains sont rares (une pose par mort PvP,
// une purge par minute). Si un jour le nombre de fils de carte monte ou que la
// section critique s'allonge, c'est la premiere hypothese a remettre en cause.
//
// CE QUE CETTE CLASSE NE PROTEGE PAS, ET QU'ELLE EXPOSE DAVANTAGE. Le verrou
// ci-dessus ne couvre que _killers et _truces. EstMercenaire, lui, appelle
// RandomPlayerbotMgr::IsRandomBot (RandomPlayerbotMgr.cpp:2206-2213), qui lit
// `currentBots` -- un std::unordered_set SANS AUCUN VERROU
// (RandomPlayerbotMgr.h:252), mute depuis le fil du monde : insert a
// RandomPlayerbotMgr.cpp:756 et :2277, erase a :1355, :1436 et :2748. Un
// `contains()` concurrent d'un rehash est un COMPORTEMENT INDEFINI, pas une
// lecture perimee : c'est la forme exacte de P-050. Le defaut preexiste a cette
// classe et sa correction est dans RandomPlayerbotMgr, pas ici -- mais c'est
// EstMercenaire qui l'a amene sur le chemin de selection de cible
// (NearestEnemyPlayersValue::AcceptUnit, trois fils de carte, a chaque tick),
// donc c'est ici qu'il doit etre ecrit. Attenuation en place, et rien de plus :
// EnemyPlayerValue.cpp memorise le resultat par bot et par tick, ce qui divise
// la frequence par le nombre de candidats evalues sans rendre l'appel sur.
// Quiconque ajoute un appelant de EstMercenaire sur un chemin chaud herite de
// ce risque et doit faire de meme.
//
// AUCUN VERROU N'EST TENU PENDANT UN APPEL AU CODE DE JEU. Player::GiveXP part
// de OnPvpKill hors de toute section critique, et la consignation du kill
// (NoteKillPaid) ne vient qu'apres. C'est une regle du projet, pas une
// coincidence : monter de niveau declenche des crochets de script dont on ne
// sait pas ce qu'ils reprennent.
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

    // LE camp mercenaire, en un seul endroit. Deux conditions, dans cet ordre :
    // le hachage de GUID (IsMercenary, quelques instructions) puis la nature du
    // bot (IsRandomBot, une recherche de cache et un parcours de la liste des
    // comptes aleatoires). Sans IsRandomBot, IsMercenary hache aussi bien le
    // GUID d'un personnage humain ou d'un alt ajoute par `.bot add`, et en fait
    // un mercenaire a l'insu de son proprietaire.
    //
    // Ce predicat existe parce que la regle etait ecrite a la main sur plusieurs
    // sites qui ne la traduisaient pas tous pareil : ComputeReward exigeait
    // IsRandomBot, MercenaryAttackSlotFree non. Une regle, un endroit.
    //
    // La forme par uint64 (PlayerbotAIConfig::IsMercenary) reste la bonne pour
    // le panneau et les sondes, qui n'ont pas de Player* sous la main.
    static bool EstMercenaire(Player* bot);

    // Comptes rendus, pour la sonde et la console. Le releve periodique de
    // Update() les emploie ; ce sont eux, et non les membres, qui sont la
    // surface publique de ces compteurs.
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

    // << Puis-je payer, et combien ? >> Renvoie 0 quand un garde-fou refuse, et
    // incremente alors le compteur de la cause. N'ECRIT RIEN dans _killers :
    // c'est tout l'objet de la separation d'avec NoteKillPaid. Prend le verrou
    // en lecture partagee.
    //
    // Avant, cette fonction consignait le kill dans les tables anti-farm au
    // moment ou elle accordait l'experience -- c'est-a-dire AVANT que
    // Player::GiveXP ait dit s'il versait quoi que ce soit. GiveXP sort sans un
    // mot dans plusieurs cas (Player.cpp:2469 xp<1, :2472 tueur mort hors champ
    // de bataille, :2475 PLAYER_FLAGS_NO_XP_GAIN / NO_PLAY_TIME, :2501 niveau
    // maximum, :2518 xp retombe sous 1 apres multiplicateurs). Cas concret et
    // frequent : une mort mutuelle, ou un poison qui acheve la cible au tick
    // suivant -- Unit::Kill declenche le crochet avec un tueur DEJA MORT. Le
    // mercenaire brulait alors un de ses credits horaires et posait 900 s de
    // delai anti-recidive sur cette victime pour un paiement nul.
    uint32 ComputeReward(Player* killer, Player* killed);

    // << Le paiement a eu lieu : je le consigne. >> Debite le quota horaire et
    // arme le delai anti-recidive. Appele UNIQUEMENT apres avoir constate que
    // l'experience du tueur a effectivement bouge. Prend le verrou en exclusif.
    void NoteKillPaid(uint64 killerGuid, uint64 victimGuid);

    // Un bot compte dans la mediane s'il fait partie de la population CHASSEE :
    // bot aleatoire, et pas mercenaire lui-meme.
    static bool CompteDansLaMediane(Player* bot);

    // Rebalaie la population en ligne et republie la mediane. Appele depuis le
    // fil du monde uniquement (MercenaryRewardsWorldScript::OnUpdate). Le
    // parcours se fait sans verrou : la demonstration, qui ne tient PAS a la
    // seule nature de la table, est dans le corps de la fonction, paragraphe
    // FIL.
    void RecomputeMedian();

    mutable std::shared_mutex _mutex;
    std::unordered_map<uint64, KillerRecord> _killers;
    std::unordered_map<uint64, time_t> _truces;

    // Chemin chaud : tant qu'il vaut 0, IsUnderTruce sort sans prendre meme le
    // verrou partage. Il compte les treves POSEES, expirees comprises, jusqu'a
    // la purge : il ne retombe donc a 0 que quand plus personne ne meurt. Voir
    // l'en-tete, paragraphe LE VERROU.
    std::atomic<uint32> _activeTruces{ 0 };

    std::atomic<uint8> _medianBotLevel{ 0 };

    // Compteurs separes par CAUSE. Un seul compteur de refus ne permettait pas
    // de distinguer << personne ne tue >> de << des gens tuent, mais jamais un
    // mercenaire >> : les deux donnaient zero recompense et zero refus, parce
    // qu'un tueur non mercenaire sortait sans rien compter du tout.
    //
    // L'INVARIANT QUE CES COMPTEURS DOIVENT FERMER, et que le releve publie :
    //   _pvpKillsSeen == _killerNotMercenary + _killsRewarded + _killsRefused
    //   _killsRefused == gris + plafond + recidive + cadence
    //                  + desarme + niveauMax + tiers + nonVerse
    // Les quatre dernieres causes sont NOUVELLES : elles etaient des `return 0`
    // muets, et l'ecart qu'elles creusaient ne se voyait nulle part. La plus
    // frequente en service est le niveau maximum -- des qu'un mercenaire atteint
    // CONFIG_MAX_PLAYER_LEVEL, chacun de ses meurtres faisait monter
    // _pvpKillsSeen et rien d'autre. Le releve emet desormais l'ecart sous le
    // nom `non explique` : un futur chemin muet se denoncera tout seul.
    std::atomic<uint32> _pvpKillsSeen{ 0 };       // toute mort joueur par joueur
    std::atomic<uint32> _killerNotMercenary{ 0 }; // tueur hors du camp mercenaire
    std::atomic<uint32> _killsRewarded{ 0 };
    std::atomic<uint32> _killsRefused{ 0 };
    std::atomic<uint32> _refusedGrey{ 0 };
    std::atomic<uint32> _refusedLevelCap{ 0 };
    std::atomic<uint32> _refusedRepeat{ 0 };
    std::atomic<uint32> _refusedRate{ 0 };
    // KillXpDivisor = 0 : la conf annonce << aucune experience versee >>, et le
    // systeme entier se desarme. Sans ce compteur, rien dans le releve ne
    // changeait de forme.
    std::atomic<uint32> _refusedDisabled{ 0 };
    std::atomic<uint32> _refusedMaxLevel{ 0 };
    // Un autre module a ramene le montant a zero dans OnPlayerGiveXP, qui prend
    // le montant par reference NON CONSTANTE (mod-dynamic-xp est charge ici).
    std::atomic<uint32> _refusedByHook{ 0 };
    // Aucune experience n'a atteint le tueur : soit GiveXP est rentre sans rien
    // verser (voir ComputeReward pour la liste de ses cinq sorties muettes),
    // soit le montant calcule etait deja nul.
    std::atomic<uint32> _refusedNotPaid{ 0 };

    // Numero de sequence du releve. Avec l'uptime publie sur la meme ligne, il
    // rend la differenciation sure cote panneau : un redemarrage remet les deux
    // a zero, ce qu'aucune heuristique n'avait a deviner jusqu'ici.
    uint32 _rapportSeq = 0;

    uint32 _purgeTimer = 0;
    uint32 _medianTimer = 0;
    uint32 _rapportTimer = 0;
};

void AddSC_mercenary_rewards();

#endif // PLAYERBOTS_MERCENARYREWARDS_H
