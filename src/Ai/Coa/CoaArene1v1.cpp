/*
 * Conquest of Azeroth -- arene 1 contre 1 non classee (escarmouche).
 *
 * Conception : coa-project/docs/CONCEPTION-arene-1v1.md. Chemins:lignes du coeur
 * relus le 10/10/2026 dans /opt/coa/core-main (aucun fichier du coeur n'est touche).
 *
 * LE TYPE 1 ET LA FILE 11
 *
 *  - Le coeur ne connait que les types 2, 3, 5 (Battleground.h:206-212) et les files
 *    8, 9, 10 (SharedDefines.h:3973-3975) ; les tables de correspondance sont des
 *    `static` publiques de BattlegroundMgr (queueToBg, ArenaTypeToQueue,
 *    QueueToArenaType, BattlegroundMgr.h:137-144) et ArenaTeam::ArenaReqPlayersForType
 *    (ArenaTeam.h:218). PoserTables() y ajoute 11 -> BATTLEGROUND_AA, 1 -> 11, 11 -> 1
 *    et 1 -> 2 joueurs, AU FIL MONDE, seulement si Enabled, et refuse si une de ces
 *    valeurs est deja prise par autre chose.
 *  - Les quatre crochets ArenaTeamScript servent de FILET (memes reponses).
 *    OnGetSlotByType n'est PAS utilise : ArenaTeam::GetSlotByType ne l'appelle que pour
 *    un type deja connu (ArenaTeam.cpp:630-640). Le type 1 n'a donc aucun « slot »
 *    d'equipe d'arene, ce qui est voulu (escarmouche) : les lecteurs du slot bornent
 *    (PetitionsHandler.cpp:440-442, ArenaTeam.cpp:546) ou ne servent qu'au classe
 *    (Arena.cpp:207-380). Prix connu : AchievementMgr.cpp:1128 journalise « Unknown arena
 *    team type 1 » a une mort en arene 1v1 (une ligne d'erreur, aucun effet de jeu).
 *  - En arene, Min = Max = arenaType (BattlegroundQueue.cpp:850-854) : type 1 = 1v1.
 *    CreateNewBattleground lit GetReqPlayersForType(1) / 2 = 1 (BattlegroundMgr.cpp:420).
 *  - HandleBattlemasterJoinArena refuse tout slot hors 0/1/2 (BattleGroundHandler.cpp:
 *    729-743) : on passe par BattlegroundQueue::AddGroup, comme lui, puis
 *    ScheduleQueueUpdate. La mise a jour PERIODIQUE des files >= 8 est « classee »
 *    (BattlegroundMgr.cpp:158-165) : seule la mise a jour PROGRAMMEE (cote 0, donc non
 *    classee) apparie une escarmouche. D'ou la relance toutes les 5 s (Suivre).
 *
 * L'APPARIEMENT EST FERME
 *
 * La file 11 n'apparie que les deux membres d'UN match inscrit ici, le plus ancien dont
 * les deux sont en file et non invites (CanAddGroupToMatchingPool), et ne remplit
 * jamais une arene deja creee (CanFillPlayersToBG). Deux humains inscrits en meme temps
 * ne se rencontrent donc pas : chacun a son gladiateur. Meme faction ou non : le coeur
 * sait faire une escarmouche entre membres d'une meme faction
 * (CheckSkirmishForSameFaction, BattlegroundQueue.cpp:710).
 *
 * FILS (P-137)
 *
 *  - Inscrire, la commande, le suivi (WorldScript::OnUpdate), l'appariement et le debut
 *    de combat tournent au FIL MONDE (CMSG_GOSSIP_SELECT_OPTION est PROCESS_THREADUNSAFE,
 *    Opcodes.cpp:511 ; BattlegroundMgr::Update et OnWorldUpdate, World.cpp:1260 et 1346,
 *    apres sMapMgr->Update) : c'est la seule place ou un objet de jeu d'un AUTRE joueur
 *    est lu ou ecrit (gladiateur, files).
 *  - OnBattlegroundEnd peut tourner sur le fil de la carte d'arene (mort -> Arena::
 *    HandleKillPlayer -> CheckWinConditions -> EndBattleground) ; OnBattlegroundAddPlayer
 *    selon le fil de l'accuse de teleportation. Ils ne lisent que l'arene et ses joueurs
 *    (meme carte), et la table des matchs sous `verrou`.
 *  - CanCreatureGossipHello (CMSG_GOSSIP_HELLO, PROCESS_INPLACE) ne lit que le joueur
 *    qui parle et deux options scalaires.
 *  - Aucun appel du coeur n'est fait en tenant `verrou` (RemovePlayer, Randomize...).
 */

#include "CoaArene1v1.h"

#include "ArenaTeam.h"
#include "BattleGroundJoinAction.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CoaDirecteur.h"
#include "CoaSpecialization.h"
#include "mod-ascension-compat/src/AscensionSpecialization.h"
#include "Creature.h"
#include "GossipDef.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "DisableMgr.h"
#include "GameTime.h"
#include "Group.h"
#include "LFGMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
BattlegroundQueueTypeId const FILE_1V1 = static_cast<BattlegroundQueueTypeId>(CoaArene1v1::FILE_ARENE);
uint8 const TYPE_1V1 = CoaArene1v1::TYPE_ARENE;

// Option de dialogue : un expediteur que ni gossip_menu_option (0, PlayerGossip.cpp)
// ni GOSSIP_SENDER_MAIN (1) n'emploient.
constexpr uint32 GOSSIP_EXPEDITEUR = 0xC0A11;
constexpr uint32 GOSSIP_ACTION_INSCRIRE = 1;
constexpr uint32 GOSSIP_ACTION_CHOISIR = 2;      // sous-menu « Choisir la classe adverse »
constexpr uint32 GOSSIP_ACTION_CLASSE = 100;     // + id de classe CoA (12-32) : inscription contre cette classe

constexpr uint32 RELANCE_FILE_S = 5;            // ScheduleQueueUpdate tant que non apparie
constexpr uint32 ATTENTE_APPARIEMENT_S = 30;    // au-dela : abandon `sans_appariement`
constexpr uint32 OUBLI_S = 3600;                // filet : un match qu'aucune fin n'a clos
constexpr uint32 MAINTIEN_EN_LIGNE_S = 300;
constexpr uint32 PREPARATION_SCORE_ECART = 6;   // meme valeur que CoaDirecteur.cpp

// Banc bot contre bot (`.coa arene banc`, AiPlayerbot.Arene1v1.Banc). Le coeur ne borne une arene qu'a 46 min
// (Battleground.cpp:305) : le banc la termine lui-meme, nulle (EndBattleground(TEAM_NEUTRAL)), apres
// BANC_COMBAT_MAX_S de combat, ou BANC_SANS_DEBUT_S apres l'inscription si les portes ne se sont jamais ouvertes
// (preparation d'arene : 60 s). 150 + ~65 s < 4 min, le delai d'abandon de outils/banc-arene.py.
constexpr uint32 BANC_COMBAT_MAX_S = 150;
constexpr uint32 BANC_SANS_DEBUT_S = 150;
// Apres toute demande humaine (dialogue d'un maitre d'arene, inscription), le banc refuse `occupe` ce temps-la.
constexpr uint32 BANC_PAUSE_HUMAIN_S = 900;
// Matchs de banc simultanes, au plus (charge du fil monde : chaque preparation passe par la fabrique).
constexpr uint32 BANC_MAX = 4;

enum class Etat : uint8
{
    EN_FILE,    // les deux en file 11, non invites
    INVITE,     // invites (instance connue), pas encore entres
    EN_ARENE,   // au moins un est entre
    FINI        // fin journalisee ; retire au tick suivant
};

char const* NomEtat(Etat e)
{
    switch (e)
    {
        case Etat::EN_FILE: return "en_file";
        case Etat::INVITE: return "invite";
        case Etat::EN_ARENE: return "en_arene";
        case Etat::FINI: return "fini";
    }
    return "?";
}

struct Match
{
    uint32 id = 0;
    uint32 joueur = 0;        // guid bas
    uint32 gladiateur = 0;    // guid bas
    std::string nomJoueur;
    std::string nomGladiateur;
    uint8 classe = 0;         // classe du gladiateur (journal de fin)
    uint8 tranche = 0;        // BattlegroundBracketId
    uint32 inscritS = 0;
    uint32 relanceS = 0;
    uint32 instance = 0;      // IsInvitedToBGInstanceGUID, puis instance de l'arene
    uint32 debutCombatS = 0;
    bool joueurEntre = false;
    bool gladiateurEntre = false;
    bool strategieVue = false;
    uint32 essaisEntree = 0;  // entrees forcees du gladiateur tentees
    Etat etat = Etat::EN_FILE;
    // Apres la fin (FINI) : le match reste dans la table tant que le gladiateur est dans
    // l'arene, pour retenir son depart (RetenirDepart) puis le faire partir.
    uint32 finS = 0;          // Maintenant() a la fin (OnBattlegroundEnd / Destroy)
    bool departLibre = false; // ecrit au FIL MONDE sous `verrou`, lu par RetenirDepart
    uint32 essaisDepart = 0;  // LeaveBG tentes par le suivi
    bool apaise = false;      // UNIT_FLAG_IMMUNE_TO_PC pose pendant la retenue (Apaiser)
    // Banc bot contre bot (`.coa arene banc`) : `joueur` est alors le bot A, traite comme un second gladiateur
    // (protection, entree forcee, depart). Jamais vrai pour un match humain.
    bool banc = false;
    uint8 classeJoueur = 0;   // classe du bot A
    uint8 cacheur = 0;        // 0 aucun, 1 A (joueur), 2 B (gladiateur)
    uint32 essaisEntreeJ = 0; // entrees forcees du bot A tentees
    bool strategieVueJ = false;
    bool limiteFaite = false; // fin nulle du banc deja demandee (BANC_COMBAT_MAX_S)
};

std::mutex verrou;               // matchs, prochainId
std::vector<Match> matchs;
uint32 prochainId = 1;
std::atomic<bool> tablesPosees{false};
std::atomic<uint32> nbMatchs{0};   // = matchs.size(), pour GladiateurEngage (tout fil)
// Maintenant() de la derniere demande d'un HUMAIN (inscription, dialogue d'un maitre d'arene) : le banc lui cede.
// Ecrit au fil monde (Inscrire) et au fil du paquet CMSG_GOSSIP_HELLO (CanCreatureGossipHello) : atomique.
std::atomic<uint32> dernierHumainS{0};

// Gladiateurs PROTEGES (inattaquables, passifs) : FIL MONDE SEUL, sans verrou.
std::unordered_set<uint32> proteges;

// Immunite aux joueurs et aux creatures, combat arrete, strategies passives : entre la
// mise en file et l'entree en arene, un mercenaire ou un monstre ne peut plus le mettre
// en combat (HandleBattleFieldPortOpcode refuse IsInCombat, BattleGroundHandler.cpp:420).
// Les drapeaux d'unite ne sont pas sauvegardes : une deconnexion les efface.
void Proteger(Player* bot, uint32 idMatch)
{
    if (!bot || proteges.count(bot->GetGUID().GetCounter()))
        return;
    bot->AttackStop();
    bot->CombatStop(true);
    bot->SetUnitFlag(UnitFlags(UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_NON_ATTACKABLE));
    if (PlayerbotAI* ia = GET_PLAYERBOT_AI(bot))
    {
        ia->ChangeStrategy("+passive,+stay", BOT_STATE_NON_COMBAT);
        ia->ChangeStrategy("+passive", BOT_STATE_COMBAT);
    }
    proteges.insert(bot->GetGUID().GetCounter());
    LOG_INFO("playerbots", "coa arene1v1: protection id={} gladiateur={} posee=1", idMatch, bot->GetGUID().GetCounter());
}

// Leve la protection (entree en arene ou abandon) et rend la composition ordinaire
// (ResetStrategies : AiFactory, qui pose `arena` si InArena()).
void Liberer(uint32 guid, uint32 idMatch, char const* raison)
{
    if (!proteges.erase(guid))
        return;
    Player* bot = ObjectAccessor::FindPlayerByLowGUID(guid);
    if (bot)
    {
        // PACIFIED : celui de la retenue d'un match precedent, si la protection l'a pris dans la seconde
        // ou il sortait de l'arene (Desapaiser n'y touche pas alors). Aucune autre source ici.
        bot->RemoveUnitFlag(UnitFlags(UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_NON_ATTACKABLE |
                                      UNIT_FLAG_PACIFIED));
        if (PlayerbotAI* ia = GET_PLAYERBOT_AI(bot))
            ia->ResetStrategies(false);
    }
    LOG_INFO("playerbots", "coa arene1v1: protection id={} gladiateur={} levee=1 raison={} en_ligne={}", idMatch, guid,
             raison, bot ? 1 : 0);
}

/*
 * Retenue apres la fin (FIL MONDE), A CHAQUE TICK DU SUIVI tant que le gladiateur est dans l'arene, depart
 * compris (LeaveBG ne le sort qu'a l'accuse de teleportation).
 *
 * Pourquoi la premiere version (AttackStop + CombatStop + UNIT_FLAG_IMMUNE_TO_PC, une fois par seconde) n'a pas
 * suffi -- le 10/10 a 21:12 (Paris), le gladiateur 682 est venu frapper le joueur pendant la retenue :
 *  - UNIT_FLAG_IMMUNE_TO_PC sur l'ATTAQUANT n'est lu que par Unit::_IsValidAttackTarget (Unit.cpp:11389-11392) :
 *    il fait refuser les SORTS visant un joueur (Spell::CheckTarget) et le ciblage, pas le corps a corps.
 *    Unit::Attack (Unit.cpp:7676) ne teste aucune validite, Player::Update frappe tant que
 *    UNIT_STATE_MELEE_ATTACKING et une victime (PlayerUpdates.cpp:160), et Unit::AttackerStateUpdate ne s'arrete
 *    que sur UNIT_FLAG_PACIFIED (Unit.cpp:2808).
 *  - l'IA du bot tourne plusieurs fois par seconde, la retenue une : entre deux passages, « reach melee » le
 *    ramenait au contact (682 est de specialisation 30, melee) et l'attaque relancait Attack().
 * D'ou : PACIFIED (plus aucun coup blanc ni sort a prevention « pacify »), IMMUNE_TO_PC (plus aucun sort sur le
 * joueur, et le joueur ne peut plus le cibler), et les strategies de la protection en file : « passive » annule
 * toute action sauf follow/stay (PassiveMultiplier.cpp), « stay » le fige. Il ne bouge ni ne frappe plus, meme
 * entre deux passages. Drapeaux non sauvegardes (une deconnexion les efface), strategies rendues par Desapaiser.
 */
void Apaiser(Player* g, bool& apaise)
{
    if (!g)
        return;
    if (g->GetVictim() || g->IsInCombat())
    {
        g->AttackStop();
        g->CombatStop(true);
    }
    g->SetUnitFlag(UnitFlags(UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_PACIFIED));
    if (PlayerbotAI* ia = GET_PLAYERBOT_AI(g))
    {
        // Seulement s'il les a perdues (LeaveBG, ou une composition) : ChangeStrategy recompose le moteur.
        if (!ia->HasStrategy("passive", BOT_STATE_NON_COMBAT) || !ia->HasStrategy("stay", BOT_STATE_NON_COMBAT))
            ia->ChangeStrategy("+passive,+stay", BOT_STATE_NON_COMBAT);
        if (!ia->HasStrategy("passive", BOT_STATE_COMBAT))
            ia->ChangeStrategy("+passive", BOT_STATE_COMBAT);
    }
    if (g->isMoving())
        g->StopMoving();
    apaise = true;
}

// Fin de la retenue (sortie de l'arene, oubli, arene detruite) : drapeaux leves et composition ordinaire rendue.
// Si la protection de file l'a repris entre-temps (nouveau match dans la seconde), elle a ses propres drapeaux et
// strategies : on ne retire alors que PACIFIED, qu'elle ne pose pas.
void Desapaiser(uint32 guid)
{
    Player* g = ObjectAccessor::FindPlayerByLowGUID(guid);
    if (!g)
        return;   // hors ligne : drapeaux effaces, composition refaite a la connexion
    if (proteges.count(guid))
    {
        g->RemoveUnitFlag(UNIT_FLAG_PACIFIED);
        return;
    }
    g->RemoveUnitFlag(UnitFlags(UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_PACIFIED));
    if (PlayerbotAI* ia = GET_PLAYERBOT_AI(g))
        ia->ResetStrategies(false);
}

uint32 Maintenant() { return static_cast<uint32>(GameTime::GetGameTime().count()); }

BattlegroundQueue& La1v1() { return sBattlegroundMgr->GetBattlegroundQueue(FILE_1V1); }

bool EstArene1v1(Battleground const* bg)
{
    return bg && tablesPosees.load(std::memory_order_acquire) && bg->isArena() && bg->GetArenaType() == TYPE_1V1;
}

// ------------------------------------------------------------------ tables du coeur

// FIL MONDE. Idempotent. Faux : une valeur est deja prise par autre chose (aucune
// ecriture n'est alors faite) ou la file depasse MAX_BATTLEGROUND_QUEUE_TYPES.
bool PoserTables()
{
    if (tablesPosees.load(std::memory_order_acquire))
        return true;
    if (CoaArene1v1::FILE_ARENE >= MAX_BATTLEGROUND_QUEUE_TYPES)
    {
        LOG_ERROR("playerbots", "coa arene1v1: tables refus=file_hors_bornes file={} max={}", CoaArene1v1::FILE_ARENE,
                  uint32(MAX_BATTLEGROUND_QUEUE_TYPES));
        return false;
    }
    auto& q2b = BattlegroundMgr::queueToBg;
    auto& a2q = BattlegroundMgr::ArenaTypeToQueue;
    auto& q2a = BattlegroundMgr::QueueToArenaType;
    auto& b2q = BattlegroundMgr::bgToQueue;
    auto& req = ArenaTeam::ArenaReqPlayersForType;
    std::string conflit;
    auto itq2b = q2b.find(int(FILE_1V1));
    if (itq2b != q2b.end() && itq2b->second != BATTLEGROUND_AA)
        conflit = "queueToBg[11]";
    for (auto const& [type, file] : a2q)
        if ((type == TYPE_1V1 && file != FILE_1V1) || (type != TYPE_1V1 && file == FILE_1V1))
            conflit = "ArenaTypeToQueue";
    for (auto const& [file, type] : q2a)
        if ((file == uint32(FILE_1V1) && type != TYPE_1V1) || (file != uint32(FILE_1V1) && type == TYPE_1V1))
            conflit = "QueueToArenaType";
    for (auto const& [bg, file] : b2q)
        if (file == FILE_1V1)
            conflit = "bgToQueue";
    auto itreq = req.find(TYPE_1V1);
    if (itreq != req.end() && itreq->second != 2)
        conflit = "ArenaReqPlayersForType";
    if (!conflit.empty())
    {
        LOG_ERROR("playerbots", "coa arene1v1: tables refus=deja_prise table={} file={} type={}", conflit,
                  CoaArene1v1::FILE_ARENE, uint32(TYPE_1V1));
        return false;
    }
    q2b[int(FILE_1V1)] = BATTLEGROUND_AA;
    a2q[TYPE_1V1] = FILE_1V1;
    q2a[uint32(FILE_1V1)] = static_cast<ArenaType>(TYPE_1V1);
    req[TYPE_1V1] = 2;
    tablesPosees.store(true, std::memory_order_release);
    LOG_INFO("playerbots", "coa arene1v1: tables posees file={} type={} bg={}", CoaArene1v1::FILE_ARENE,
             uint32(TYPE_1V1), uint32(BATTLEGROUND_AA));
    return true;
}

// ------------------------------------------------------------------ gladiateurs

// Lecture STRICTE (comme PlayerbotAIConfig::GuildesDeReserve) : un morceau mal forme,
// vide compris, et AUCUN gladiateur. FIL MONDE (la chaine n'est reecrite que par
// `reload config`, au fil monde).
std::vector<uint32> Gladiateurs()
{
    std::vector<uint32> ids;
    std::string const& liste = sPlayerbotAIConfig.arene1v1Gladiateurs;
    if (liste.find_first_not_of(" \t") == std::string::npos)
        return ids;
    size_t debut = 0;
    while (debut <= liste.size())
    {
        size_t fin = liste.find(',', debut);
        if (fin == std::string::npos)
            fin = liste.size();
        std::string morceau = liste.substr(debut, fin - debut);
        morceau.erase(std::remove_if(morceau.begin(), morceau.end(), [](char c) { return c == ' ' || c == '\t'; }),
                      morceau.end());
        if (morceau.empty() || morceau.size() > 10 ||
            !std::all_of(morceau.begin(), morceau.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return {};
        unsigned long long const v = std::strtoull(morceau.c_str(), nullptr, 10);
        if (v == 0 || v > 0xFFFFFFFFull)
            return {};
        if (std::find(ids.begin(), ids.end(), static_cast<uint32>(v)) == ids.end())
            ids.push_back(static_cast<uint32>(v));
        debut = fin + 1;
    }
    return ids;
}

// AiPlayerbot.Arene1v1.Classes : ids de classes CoA separes par des virgules ; vide = toutes. Un morceau qui
// n'est pas un id de classe CoA est ignore (une liste sans aucun id valable n'admet donc aucune classe). FIL MONDE.
bool ClasseAdmise(uint8 classe)
{
    if (!IsAscensionClass(classe))
        return false;
    std::string const& liste = sPlayerbotAIConfig.arene1v1Classes;
    if (liste.find_first_not_of(" \t,") == std::string::npos)
        return true;
    size_t debut = 0;
    while (debut <= liste.size())
    {
        size_t fin = liste.find(',', debut);
        if (fin == std::string::npos)
            fin = liste.size();
        std::string morceau = liste.substr(debut, fin - debut);
        morceau.erase(std::remove_if(morceau.begin(), morceau.end(), [](char c) { return c == ' ' || c == '\t'; }),
                      morceau.end());
        if (!morceau.empty() && morceau.size() <= 3 &&
            std::all_of(morceau.begin(), morceau.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
            std::strtoul(morceau.c_str(), nullptr, 10) == classe)
            return true;
        debut = fin + 1;
    }
    return false;
}

// Le nom de la classe, lu dans ChrClasses.dbc (comme ChatHelper::FormatClass pour les classes CoA).
std::string NomClasse(uint8 classe)
{
    if (ChrClassesEntry const* entree = sChrClassesStore.LookupEntry(classe))
        if (entree->name[0] && *entree->name[0])
            return entree->name[0];
    return "classe " + std::to_string(classe);
}

bool DejaEngage(uint32 guid)
{
    std::lock_guard<std::mutex> garde(verrou);
    // Un match FINI ne garde que le depart differe du gladiateur : il n'engage plus personne
    // (un gladiateur encore dans l'arene est de toute facon refuse par `file_ou_champ`).
    for (Match const& m : matchs)
        if (m.etat != Etat::FINI && (m.joueur == guid || m.gladiateur == guid))
            return true;
    return false;
}

// FIL MONDE. nullptr si le gladiateur est libre, sinon la raison.
char const* RefusGladiateur(uint32 guid, Player* b)
{
    if (!b)
        return "hors_ligne";
    if (!b->IsInWorld() || b->IsBeingTeleported())
        return "en_teleportation";
    PlayerbotAI* ia = GET_PLAYERBOT_AI(b);
    if (!ia || !sRandomPlayerbotMgr.IsRandomBot(b))
        return "pas_bot_aleatoire";
    if (ia->GetMaster())
        return "a_un_maitre";
    // Toute classe CoA (10/10 soir, demande du commanditaire), bornee par AiPlayerbot.Arene1v1.Classes.
    if (!IsAscensionClass(b->getClass()))
        return "pas_classe_coa";
    if (!ClasseAdmise(b->getClass()))
        return "classe_exclue";
    // Un mercenaire gagne de l'XP de victime JcJ et remonte de niveau
    // (docs/ENQUETE-niveau-reserve.md). Hachage du guid : PlayerbotAIConfig::IsMercenary.
    if (sPlayerbotAIConfig.IsMercenary(b->GetGUID().GetRawValue()))
        return "mercenaire";
    if (sPlayerbotAIConfig.EstGuildeDeReserve(b->GetGuildId()))
        return "reserve_directeur";
    if (CoaDirecteur::AffectationDe(guid, nullptr) || CoaDirecteur::ChasseurDe(guid) ||
        CoaDirecteur::CibleDeBanc(guid) || CoaDirecteur::CibleImmobile(guid))
        return "directeur";
    if (!b->IsAlive())
        return "mort";
    if (b->IsInCombat())
        return "combat";
    if (b->GetGroup())
        return "groupe";
    if (b->InBattlegroundQueue() || b->InBattleground() || b->InArena())
        return "file_ou_champ";
    if (b->IsInFlight())
        return "en_vol";
    if (b->GetMap() && b->GetMap()->Instanceable())
        return "instance";
    if (DejaEngage(guid))
        return "deja_engage";
    return nullptr;
}

// Copie de CoaDirecteur.cpp ScoreEquipement (espace anonyme la-bas : le factoriser
// imposait d'ouvrir ce fichier de 10 000 lignes, en cours d'ecriture par une autre
// session). Score mixte moyen, emplacement vide = 0, sans les emplacements que la
// fabrique ne remplit pas a ce niveau. Joueur et gladiateur se mesurent au meme niveau.
uint32 ScoreEquipement(Player* p, uint32 niveau)
{
    uint32 somme = 0, n = 0;
    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (i == EQUIPMENT_SLOT_TABARD || i == EQUIPMENT_SLOT_RANGED || i == EQUIPMENT_SLOT_OFFHAND ||
            i == EQUIPMENT_SLOT_BODY)
            continue;
        if (niveau < 50 && (i == EQUIPMENT_SLOT_TRINKET1 || i == EQUIPMENT_SLOT_TRINKET2))
            continue;
        if (niveau < 30 && (i == EQUIPMENT_SLOT_NECK || i == EQUIPMENT_SLOT_HEAD))
            continue;
        if (niveau < 20 && (i == EQUIPMENT_SLOT_FINGER1 || i == EQUIPMENT_SLOT_FINGER2))
            continue;
        ++n;
        if (Item* it = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            if (ItemTemplate const* proto = it->GetTemplate())
            {
                uint32 const ilvl = proto->Quality == ITEM_QUALITY_HEIRLOOM
                                        ? uint32(proto->GetItemLevelIncludingQuality(uint8(p->GetLevel())))
                                        : proto->ItemLevel;
                somme += PlayerbotFactory::CalcMixedGearScore(ilvl, proto->Quality);
            }
    }
    return n ? somme / n : 0;
}

// Copie de CoaDirecteur.cpp Reequiper : tout ote, puis l'equipement de la fabrique au
// niveau du bot, qualite au plus `qualite` (0 : celle de la conf), munitions refaites.
void Reequiper(Player* bot, uint32 qualite)
{
    PlayerbotFactory::DestroyEquippedGear(bot);
    PlayerbotFactory fabrique(bot, bot->GetLevel(), qualite, 0);
    fabrique.InitEquipment(false);
    fabrique.InitAmmo();
}

// FIL MONDE, AUCUN verrou tenu (Randomize -> ResetStrategies -> fabrique, qui prend le
// verrou partage du Directeur). La recette de la reserve du Directeur
// (CoaDirecteur.cpp, preparation `P-`) : PlayerbotFactory::Randomize(false) au niveau
// du joueur (niveau, sorts, talents, equipement), puis l'equipement rapproche de celui
// du joueur par la QUALITE, puis plein de vie et de ressource (CoaDuelCommandScript
// RemettreAPlein, meme ordre : recalcul avant remplissage).
// `reference` : celui dont l'equipement sert d'etalon (le joueur ; au banc, le bot A pour B, personne pour A :
// alors aucun reequipement). `niveau` : celui du joueur, ou celui du banc.
void Preparer(Player* bot, uint32 niveau, Player* reference, uint32 idMatch)
{
    uint32 const debutMs = getMSTime();
    uint32 const avant = bot->GetLevel();
    // Jamais vers le bas : Inscrire ne tire que des gladiateurs de niveau <= joueur, le banc refuse un bot au-dessus.
    if (avant < niveau)
    {
        if (bot->IsMounted())
            bot->Dismount();
        PlayerbotFactory fabrique(bot, niveau);
        fabrique.Randomize(false);
    }
    // Specialisation de DEGATS (et a distance pour le Ranger), comme la reserve du Directeur prend la sienne
    // (EnsureCoaSpecialization, embuscade) : un soigneur ou un tank n'a rien a faire seul en arene. APRES la
    // fabrique (Randomize recompose : sans gladiateur engage, la specialisation du hachage revient), AVANT
    // l'equipement.
    uint32 const specialisation = CoaSpecialiserGladiateur(bot);
    uint32 const scoreJoueur = reference ? ScoreEquipement(reference, bot->GetLevel()) : 0;
    uint32 const scoreAvant = ScoreEquipement(bot, bot->GetLevel());
    uint32 scoreApres = scoreAvant;
    uint32 reequipe = 0;
    if (reference && scoreAvant + PREPARATION_SCORE_ECART < scoreJoueur)
    {
        Reequiper(bot, 0);
        reequipe = static_cast<uint32>(sPlayerbotAIConfig.randomGearQualityLimit);
        scoreApres = ScoreEquipement(bot, bot->GetLevel());
    }
    for (uint32 qualite : {uint32(ITEM_QUALITY_UNCOMMON), uint32(ITEM_QUALITY_NORMAL)})
    {
        if (!reference || scoreApres <= scoreJoueur + PREPARATION_SCORE_ECART)
            break;
        Reequiper(bot, qualite);
        reequipe = qualite;
        scoreApres = ScoreEquipement(bot, bot->GetLevel());
    }
    // Pas de re-randomisation par RandomPlayerbotMgr (evenement « randomize ») entre la
    // preparation et l'arene : meme geste que `.coa niveau` (CoaDuelCommandScript.cpp).
    sRandomPlayerbotMgr.SetValue(bot, "randomize", 1);
    if (bot->IsMounted())
        bot->Dismount();
    bot->UpdateAllStats();
    bot->SetFullHealth();
    Powers const type = bot->getPowerType();
    if (type == POWER_RAGE || type == POWER_RUNIC_POWER)
        bot->SetPower(type, 0);
    else
        bot->SetPower(type, bot->GetMaxPower(type));
    if (type != POWER_MANA && bot->GetMaxPower(POWER_MANA) > 0)
        bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
    LOG_INFO("playerbots", "coa arene1v1: preparation id={} gladiateur={} niveau_avant={} niveau_apres={} niveau_joueur={}"
             " score_joueur={} score_avant={} score_apres={} reequipe={} classe={} specialisation={} spe_changee={}"
             " duree_ms={}", idMatch, bot->GetGUID().GetCounter(), avant, uint32(bot->GetLevel()), niveau, scoreJoueur,
             scoreAvant, scoreApres, reequipe, uint32(bot->getClass()), GetAscensionActiveSpecialization(bot),
             specialisation ? 1 : 0, GetMSTimeDiffToNow(debutMs));
}

// ------------------------------------------------------------------ defis CoA

// FIL MONDE. Vrai si le joueur porte un defi actif dont une mort EN ARENE couterait :
// a vies, sans resurrection ou a destin partage. mod-coa-challenges compte toute mort
// (HandlePlayerDeath, CoA.Challenges.Lifecycle.cpp:1936) sans exclure l'arene. Requete
// illisible : vrai (prudence), `lisible` faux.
bool DefiMortel(Player* p, bool& lisible)
{
    lisible = true;
    uint32 const guid = p->GetGUID().GetCounter();
    // MODES DE JEU (masque coa_character_gamemode.gameMode) : un mode actif SANS son defi
    // de base perd aussi des vies (Lifecycle.cpp:1983-2003, LivesTotal(base) =
    // coa_challenge_definition.lives, Spells.cpp:801). La base d'un bit est le defi dont
    // requiredGameMode vaut ce bit (GameModes.cpp:64-75) ; le premier trouve est retenu
    // par le module, ordre non garanti : on refuse si UN SEUL defi de ce bit est mortel.
    if (QueryResult modes = CharacterDatabase.Query("SELECT gameMode FROM coa_character_gamemode WHERE guid = {}", guid))
    {
        uint32 const masque = (*modes)[0].Get<uint32>();
        if (masque)
        {
            QueryResult dm = WorldDatabase.Query(
                "SELECT COUNT(*) FROM coa_challenge_definition WHERE requiredGameMode <> 0 AND (requiredGameMode & {}) <> 0"
                " AND (lives > 0 OR noResurrect = 1 OR sharedFate = 1)",
                masque);
            if (!dm)
            {
                lisible = false;
                return true;
            }
            if ((*dm)[0].Get<uint64>() > 0)
                return true;
        }
    }
    QueryResult actifs = CharacterDatabase.Query("SELECT challengeId FROM coa_character_challenge WHERE guid = {}", guid);
    if (!actifs)
        return false;
    std::string ids;
    do
    {
        ids += (ids.empty() ? "" : ",") + std::to_string((*actifs)[0].Get<uint32>());
    } while (actifs->NextRow());
    QueryResult d = WorldDatabase.Query(
        "SELECT COUNT(*) FROM coa_challenge_definition WHERE id IN ({}) AND (lives > 0 OR noResurrect = 1 OR sharedFate = 1)",
        ids);
    if (!d)
    {
        lisible = false;
        return true;
    }
    return (*d)[0].Get<uint64>() > 0;
}

// ------------------------------------------------------------------ files

// FIL MONDE. Sort `p` de la file 11 s'il y est (et n'est pas en arene). `prevenir` :
// paquet STATUS_NONE au client (jamais a un bot : il n'a pas d'interface).
void SortirDeFile(Player* p, bool prevenir)
{
    if (!p || p->InArena())
        return;
    uint32 const slot = p->GetBattlegroundQueueIndex(FILE_1V1);
    if (slot >= PLAYER_MAX_BATTLEGROUND_QUEUES)
        return;
    GroupQueueInfo gi;
    if (La1v1().GetPlayerGroupInfoData(p->GetGUID(), &gi))
    {
        // RemovePlayer(guid, true) cherche l'arene par GetBattleground(instance,
        // gi.BgTypeId = BATTLEGROUND_AA) et ne la trouve PAS : une arene est rangee sous
        // son type concret (BattlegroundMgr.cpp:292-322). Le siege invite resterait
        // compte (arene fantome). On decremente donc nous-memes, par TYPE_NONE.
        La1v1().RemovePlayer(p->GetGUID(), false);
        if (gi.IsInvitedToBGInstanceGUID)
            if (Battleground* bg = sBattlegroundMgr->GetBattleground(gi.IsInvitedToBGInstanceGUID, BATTLEGROUND_TYPE_NONE))
                bg->DecreaseInvitedCount(gi.teamId);
    }
    p->RemoveBattlegroundQueueId(FILE_1V1);
    if (prevenir)
        if (Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_AA))
        {
            WorldPacket data;
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bgt, slot, STATUS_NONE, 0, 0, 0, TEAM_NEUTRAL);
            p->SendDirectMessage(&data);
        }
}

void Dire(Player* p, std::string const& texte)
{
    // Jamais a un bot (le bot A d'un banc tient le role du joueur) : il n'a pas d'interface.
    if (p && p->GetSession() && !GET_PLAYERBOT_AI(p))
        ChatHandler(p->GetSession()).SendSysMessage(texte);
}

std::string Explication(std::string const& refus)
{
    if (refus == "desactive")
        return "L'arène 1 contre 1 est fermée pour l'instant.";
    if (refus == "niveau")
        return "Il faut être au moins niveau " + std::to_string(sPlayerbotAIConfig.arene1v1NiveauMin) +
               " pour l'arène 1 contre 1.";
    if (refus == "deja_inscrit" || refus == "deja_en_file")
        return "Vous êtes déjà inscrit à un champ de bataille ou à une arène.";
    if (refus == "champ_de_bataille")
        return "Vous êtes déjà sur un champ de bataille.";
    if (refus == "defi" || refus == "defi_mortel" || refus == "defi_illisible")
        return "Un de vos défis interdit l'arène, ou une mort en arène vous coûterait une vie.";
    if (refus == "aucun_gladiateur_au_niveau")
        return "Aucun gladiateur de votre niveau ou moins n'est libre : ils sont tous plus forts que vous. "
               "Réessayez plus tard.";
    if (refus == "aucun_gladiateur")
        return "Aucun gladiateur n'est libre pour l'instant. Réessayez dans un moment.";
    if (refus == "lfg")
        return "Vous êtes inscrit à la recherche de donjon.";
    if (refus == "classe_indisponible")
        return "Aucun gladiateur de cette classe n'est libre à votre niveau ou moins. Choisissez une autre classe.";
    return "Inscription à l'arène 1 contre 1 impossible pour l'instant.";
}

// Les gladiateurs libres pour `joueur` (niveau <= le sien), et pourquoi les autres ne le sont pas. FIL MONDE.
struct Recensement
{
    std::vector<Player*> libres;
    std::string raisons;
    uint32 trop = 0;    // libres mais de niveau superieur au joueur
    uint32 total = 0;   // gladiateurs de l'option
};

Recensement Recenser(Player* joueur)
{
    Recensement r;
    std::vector<uint32> const liste = Gladiateurs();
    r.total = uint32(liste.size());
    for (uint32 g : liste)
    {
        Player* b = ObjectAccessor::FindPlayerByLowGUID(g);
        if (char const* raison = RefusGladiateur(g, b))
            r.raisons += (r.raisons.empty() ? "" : ",") + std::to_string(g) + ":" + raison;
        else if (b->GetLevel() > joueur->GetLevel())
        {
            ++r.trop;
            r.raisons += (r.raisons.empty() ? "" : ",") + std::to_string(g) + ":niveau_" + std::to_string(b->GetLevel());
        }
        else
            r.libres.push_back(b);
    }
    return r;
}

// Rotation des classes adverses (10/10 soir) : par joueur humain (guid bas), classe -> Maintenant() de la derniere
// inscription contre elle. Memoire du processus (un redemarrage la vide : tout redevient « jamais affronte »).
// FIL MONDE SEUL (Inscrire), sans verrou. Borne : 21 entrees au plus par joueur inscrit au moins une fois.
std::unordered_map<uint32, std::unordered_map<uint8, uint32>> rencontres;

// La classe que `joueurGuid` a affrontee le MOINS recemment parmi celles de `libres` (jamais = 0), au sort entre
// ex aequo. 0 si `libres` est vide.
uint8 ClasseParRotation(uint32 joueurGuid, std::vector<Player*> const& libres)
{
    std::vector<uint8> classes;
    for (Player* b : libres)
        if (std::find(classes.begin(), classes.end(), b->getClass()) == classes.end())
            classes.push_back(b->getClass());
    if (classes.empty())
        return 0;
    auto const it = rencontres.find(joueurGuid);
    auto Derniere = [&](uint8 c) -> uint32
    {
        if (it == rencontres.end())
            return 0;
        auto const jt = it->second.find(c);
        return jt == it->second.end() ? 0 : jt->second;
    };
    uint32 plusAncienne = Derniere(classes.front());
    for (uint8 c : classes)
        plusAncienne = std::min(plusAncienne, Derniere(c));
    classes.erase(std::remove_if(classes.begin(), classes.end(), [&](uint8 c) { return Derniere(c) != plusAncienne; }),
                  classes.end());
    return classes[urand(0, uint32(classes.size()) - 1)];
}

/*
 * FIL MONDE, AUCUN verrou tenu (EndBattleground -> OnBattlegroundEnd prend `verrou`). Un humain manque de
 * gladiateur : le banc rend TOUS les siens. En file ou invites sans etre entres : sortis de la file, liberes, match
 * retire tout de suite (le recensement qui suit les retrouve libres). En arene : fin nulle ; ils sortent dans les
 * secondes qui suivent (suivi, etat FINI) et l'humain, prevenu « reessayez dans un moment », les retrouve.
 * Rend le nombre de matchs de banc cedes.
 */
uint32 CederLeBanc(char const* raison)
{
    std::vector<Match> bancs;
    {
        std::lock_guard<std::mutex> garde(verrou);
        for (Match const& m : matchs)
            if (m.banc && m.etat != Etat::FINI)
                bancs.push_back(m);
    }
    uint32 cedes = 0;
    uint32 const maintenant = Maintenant();
    for (Match const& m : bancs)
    {
        Player* a = ObjectAccessor::FindPlayerByLowGUID(m.joueur);
        Player* b = ObjectAccessor::FindPlayerByLowGUID(m.gladiateur);
        bool const dedans = (a && a->InBattleground()) || (b && b->InBattleground());
        if ((m.etat == Etat::EN_FILE || m.etat == Etat::INVITE) && !dedans)
        {
            SortirDeFile(a, false);
            SortirDeFile(b, false);
            Liberer(m.joueur, m.id, raison);
            Liberer(m.gladiateur, m.id, raison);
            LOG_INFO("playerbots", "coa arene1v1: abandon id={} joueur={} gladiateur={} etat={} raison={} duree_s={} banc=1",
                     m.id, m.joueur, m.gladiateur, NomEtat(m.etat), raison, maintenant - m.inscritS);
            {
                std::lock_guard<std::mutex> garde(verrou);
                matchs.erase(std::remove_if(matchs.begin(), matchs.end(), [&](Match const& x) { return x.id == m.id; }),
                             matchs.end());
                nbMatchs.store(uint32(matchs.size()), std::memory_order_release);
            }
            ++cedes;
            continue;
        }
        Battleground* bg = m.instance ? sBattlegroundMgr->GetBattleground(m.instance, BATTLEGROUND_TYPE_NONE) : nullptr;
        if (bg && EstArene1v1(bg) && (bg->GetStatus() == STATUS_WAIT_JOIN || bg->GetStatus() == STATUS_IN_PROGRESS))
        {
            LOG_INFO("playerbots", "coa arene1v1: banc_cede id={} instance={} raison={} statut={}", m.id, m.instance, raison,
                     uint32(bg->GetStatus()));
            bg->EndBattleground(TEAM_NEUTRAL);
            ++cedes;
        }
    }
    return cedes;
}

}  // namespace

// ------------------------------------------------------------------ inscription

bool CoaArene1v1::Inscrire(Player* joueur, std::string const& origine, std::string& refus, uint8 classeVoulue)
{
    refus.clear();
    if (!joueur)
    {
        refus = "joueur_absent";
        return false;
    }
    uint32 const jg = joueur->GetGUID().GetCounter();
    auto Refuser = [&](std::string const& code, std::string const& detail = "") {
        refus = code;
        LOG_INFO("playerbots", "coa arene1v1: inscription refus={} joueur={} nom={} niveau={} origine={}{}{}", code, jg,
                 joueur->GetName(), uint32(joueur->GetLevel()), origine, detail.empty() ? "" : " ", detail);
        Dire(joueur, Explication(code));
        return false;
    };

    if (!sPlayerbotAIConfig.arene1v1Enabled)
        return Refuser("desactive");
    if (GET_PLAYERBOT_AI(joueur) || !joueur->GetSession())
        return Refuser("pas_humain");
    // Un humain demande l'arene : le banc se tait BANC_PAUSE_HUMAIN_S (refus `occupe`), quelle que soit l'issue.
    dernierHumainS.store(std::max<uint32>(Maintenant(), 1), std::memory_order_release);
    if (joueur->GetLevel() < sPlayerbotAIConfig.arene1v1NiveauMin)
        return Refuser("niveau");
    if (!joueur->IsInWorld() || joueur->IsBeingTeleported())
        return Refuser("en_teleportation");
    if (!joueur->IsAlive())
        return Refuser("mort");
    if (joueur->InBattleground() || joueur->InArena())
        return Refuser("champ_de_bataille");
    if (DejaEngage(jg))
        return Refuser("deja_inscrit");
    // Comme HandleBattlemasterJoinArena : une seule file d'arene, et jamais avec une
    // file de champ de bataille (BattleGroundHandler.cpp:766-773).
    if (joueur->InBattlegroundQueue() || !joueur->HasFreeBattlegroundQueueId())
        return Refuser("deja_en_file");
    if (sLFGMgr->GetState(joueur->GetGUID()) > lfg::LFG_STATE_NONE)
        return Refuser("lfg");
    if (joueur->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
        return Refuser("temps_de_jeu");
    if (sDisableMgr->IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, BATTLEGROUND_AA, nullptr))
        return Refuser("arenes_desactivees");
    // Les defis CoA (NO_ARENAS, NO_ARENA_FINDER : CoA.Challenges.Scripts.cpp:1101-1104).
    // Ici le RETOUR FAUX bloque, quel que soit `err` (le coeur, lui, ne bloque que si
    // err <= 0, BattleGroundHandler.cpp:778).
    GroupJoinBattlegroundResult err = GroupJoinBattlegroundResult(BATTLEGROUND_AA);
    if (!sScriptMgr->OnPlayerCanJoinInArenaQueue(joueur, ObjectGuid::Empty, 0, BATTLEGROUND_AA, 0, 0, err))
        return Refuser("defi");
    bool lisible = true;
    if (DefiMortel(joueur, lisible))
        return Refuser(lisible ? "defi_mortel" : "defi_illisible");
    Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_AA);
    if (!bgt)
        return Refuser("sans_modele");
    PvPDifficultyEntry const* tranche = GetBattlegroundBracketByLevel(bgt->GetMapId(), joueur->GetLevel());
    if (!tranche)
        return Refuser("tranche");
    if (!PoserTables())
        return Refuser("file_indisponible");

    // Garde de l'ASSERT de BattlegroundQueue::AddGroup (m_QueuedPlayers.count == 0),
    // AVANT toute preparation.
    GroupQueueInfo gi;
    if (La1v1().GetPlayerGroupInfoData(joueur->GetGUID(), &gi))
        return Refuser("deja_en_file");

    // Le gladiateur. On ne fait JAMAIS descendre un bot de niveau (revue du 10/10 : la
    // descente n'est pas prouvee -- ClearSpells ne fait rien pour une classe CoA, et
    // SynchronizeProgression garde l'AscensionLiveBaseline). Parmi les libres de niveau <= celui
    // du joueur : d'abord la CLASSE (demandee au dialogue, sinon la rotation : celle que ce joueur
    // a affrontee le moins recemment), puis, dans cette classe, les PLUS HAUTS (au sort entre
    // egaux), montes au niveau du joueur. Les gladiateurs montent ainsi avec les joueurs : la
    // reserve se renouvelle un jour (gladiateurs.py --constituer).
    Recensement rec = Recenser(joueur);
    // L'humain est prioritaire sur le banc : s'il manque de gladiateur (de la classe demandee, le cas echeant),
    // le banc lui rend les siens, puis on recense de nouveau.
    {
        bool manque = rec.libres.empty();
        if (!manque && classeVoulue)
            manque = std::none_of(rec.libres.begin(), rec.libres.end(),
                                  [classeVoulue](Player* b) { return b->getClass() == classeVoulue; });
        if (manque)
            if (uint32 const cedes = CederLeBanc("humain_prioritaire"))
            {
                LOG_INFO("playerbots", "coa arene1v1: banc cede au joueur={} matchs={}", jg, cedes);
                rec = Recenser(joueur);
            }
    }
    std::vector<Player*>& libres = rec.libres;
    std::string const& raisons = rec.raisons;
    std::string const detailRecensement =
        "gladiateurs=" + std::to_string(rec.total) + " raisons=" + (raisons.empty() ? "-" : raisons);
    if (libres.empty())
        return Refuser(rec.trop ? "aucun_gladiateur_au_niveau" : "aucun_gladiateur", detailRecensement);
    uint32 const libresToutes = uint32(libres.size());
    uint8 const classe = classeVoulue ? classeVoulue : ClasseParRotation(jg, libres);
    libres.erase(std::remove_if(libres.begin(), libres.end(), [classe](Player* b) { return b->getClass() != classe; }),
                 libres.end());
    if (libres.empty())
        return Refuser("classe_indisponible", "classe=" + std::to_string(classe) + " " + detailRecensement);
    uint8 haut = 0;
    for (Player* b : libres)
        haut = std::max(haut, b->GetLevel());
    libres.erase(std::remove_if(libres.begin(), libres.end(), [haut](Player* b) { return b->GetLevel() != haut; }),
                 libres.end());
    Player* bot = libres[urand(0, uint32(libres.size()) - 1)];
    uint32 const bg = bot->GetGUID().GetCounter();
    if (La1v1().GetPlayerGroupInfoData(bot->GetGUID(), &gi))
        return Refuser("aucun_gladiateur", "gladiateur_deja_en_file=" + std::to_string(bg));

    uint32 idMatch;
    {
        std::lock_guard<std::mutex> garde(verrou);
        idMatch = prochainId++;
    }
    LOG_INFO("playerbots", "coa arene1v1: inscription id={} joueur={} nom={} niveau={} origine={} gladiateur={} nom_gladiateur={}"
             " classe={} choix={} libres={}/{} tranche={}", idMatch, jg, joueur->GetName(), uint32(joueur->GetLevel()),
             origine, bg, bot->GetName(), uint32(classe), classeVoulue ? "demande" : "rotation", libresToutes, rec.total,
             uint32(tranche->GetBracketId()));

    Preparer(bot, joueur->GetLevel(), joueur, idMatch);

    // Apres la fabrique : le gladiateur doit etre encore libre et dans la MEME tranche.
    PvPDifficultyEntry const* trancheBot = GetBattlegroundBracketByLevel(bgt->GetMapId(), bot->GetLevel());
    if (!bot->IsInWorld() || !bot->IsAlive() || bot->InBattlegroundQueue() || !trancheBot ||
        trancheBot->GetBracketId() != tranche->GetBracketId())
        return Refuser("gladiateur_non_pret", "id=" + std::to_string(idMatch) + " gladiateur=" + std::to_string(bg));

    // Re-garde de l'ASSERT apres la fabrique (aucun appel n'y met en file, mais rien ne coute).
    if (La1v1().GetPlayerGroupInfoData(joueur->GetGUID(), &gi) || La1v1().GetPlayerGroupInfoData(bot->GetGUID(), &gi))
        return Refuser("deja_en_file", "id=" + std::to_string(idMatch));

    // Le joueur, comme HandleBattlemasterJoinArena en solo (BattleGroundHandler.cpp:808-822).
    GroupQueueInfo* gj = La1v1().AddGroup(joueur, nullptr, BATTLEGROUND_AA, tranche, TYPE_1V1, false, false, 0, 0, 0, 0);
    uint32 const attente = La1v1().GetAverageQueueWaitTime(gj);
    uint32 const slotJ = joueur->AddBattlegroundQueueId(FILE_1V1);
    {
        WorldPacket data;
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bgt, slotJ, STATUS_WAIT_QUEUE, attente, 0, TYPE_1V1,
                                                        TEAM_NEUTRAL);
        joueur->SendDirectMessage(&data);
    }
    // Le gladiateur : la meme file, la meme tranche. Son action « bg status »
    // (BattleGroundJoinAction.cpp, BGStatusAction) acceptera l'invitation
    // (STATUS_WAIT_JOIN) en renvoyant CMSG_BATTLEFIELD_PORT avec le type 1.
    La1v1().AddGroup(bot, nullptr, BATTLEGROUND_AA, tranche, TYPE_1V1, false, false, 0, 0, 0, 0);
    bot->AddBattlegroundQueueId(FILE_1V1);
    Proteger(bot, idMatch);

    Match m;
    m.id = idMatch;
    m.joueur = jg;
    m.gladiateur = bg;
    m.nomJoueur = joueur->GetName();
    m.nomGladiateur = bot->GetName();
    m.classe = classe;
    m.tranche = tranche->GetBracketId();
    m.inscritS = Maintenant();
    m.relanceS = m.inscritS;
    {
        std::lock_guard<std::mutex> garde(verrou);
        matchs.push_back(m);
        nbMatchs.store(uint32(matchs.size()), std::memory_order_release);
    }
    sBattlegroundMgr->ScheduleQueueUpdate(0, TYPE_1V1, FILE_1V1, BATTLEGROUND_AA, tranche->GetBracketId());
    LOG_INFO("playerbots", "coa arene1v1: mise_en_file id={} joueur={} gladiateur={} file={} type={} tranche={} slot_joueur={}",
             idMatch, jg, bg, CoaArene1v1::FILE_ARENE, uint32(TYPE_1V1), uint32(m.tranche), slotJ);
    // Rotation : cette classe passe en dernier pour ce joueur (inscription faite, quelle qu'en soit l'issue).
    rencontres[jg][classe] = Maintenant();
    Dire(joueur, "Arène 1 contre 1 : votre adversaire : " + m.nomGladiateur + ", " + NomClasse(classe) + ", niveau " +
                     std::to_string(bot->GetLevel()) + ". Acceptez l'invitation quand elle s'affiche.");
    return true;
}

bool CoaArene1v1::GladiateurEngage(uint32 guidLow)
{
    if (!guidLow || !nbMatchs.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> garde(verrou);
    for (Match const& m : matchs)
        if (m.gladiateur == guidLow || (m.banc && m.joueur == guidLow))
            return true;
    return false;
}

bool CoaArene1v1::CacheurDeBanc(uint32 guidLow)
{
    if (!guidLow || !nbMatchs.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> garde(verrou);
    for (Match const& m : matchs)
        if (m.banc && m.etat != Etat::FINI &&
            ((m.cacheur == 1 && m.joueur == guidLow) || (m.cacheur == 2 && m.gladiateur == guidLow)))
            return true;
    return false;
}

bool CoaArene1v1::RetenirDepart(Player* bot)
{
    if (!bot || !nbMatchs.load(std::memory_order_acquire) || !sPlayerbotAIConfig.arene1v1DelaiDepartS)
        return false;
    // Lit l'arene du bot depuis son propre fil (celui de sa carte) ou le fil monde, comme
    // ArenaTactics et LeaveBG le font deja ; aucune ecriture.
    Battleground* bg = bot->GetBattleground();
    if (!EstArene1v1(bg) || bg->GetStatus() != STATUS_WAIT_LEAVE)
        return false;
    uint32 const guid = bot->GetGUID().GetCounter();
    uint32 const instance = bg->GetInstanceID();
    std::lock_guard<std::mutex> garde(verrou);
    for (Match const& m : matchs)
        if (m.gladiateur == guid && m.etat == Etat::FINI && m.instance == instance)
            return !m.departLibre;
    return false;
}

namespace
{
// ------------------------------------------------------------------ suivi (fil monde)

struct Verdict
{
    uint32 id = 0;
    bool retirer = false;
    Match maj;   // si !retirer : nouvel etat
};

// FIL MONDE. Invite, le gladiateur entre TOUT DE SUITE, sans attendre son action
// « bg status » ni le rappel de 20 s : meme paquet que BGStatusAction
// (BattleGroundJoinAction.cpp, type << unk2 << bgTypeId << unk << action), passe
// directement au gestionnaire du coeur, au fil monde comme tout CMSG_BATTLEFIELD_PORT
// (PROCESS_THREADUNSAFE, Opcodes.cpp:856). Le coeur refuse en silence un joueur en
// combat (BattleGroundHandler.cpp:420) : combat arrete juste avant, nouvel essai au tick
// suivant sinon.
// `estJoueur` : le bot A d'un banc (role « joueur »), entre de force comme le gladiateur.
void FaireEntrer(Player* g, Match& m, bool estJoueur = false)
{
    if (!g || !g->GetSession() || g->InBattleground() || !m.instance ||
        !g->IsInvitedForBattlegroundQueueType(FILE_1V1))
        return;
    Battleground* bg = sBattlegroundMgr->GetBattleground(m.instance, BATTLEGROUND_TYPE_NONE);
    if (!bg)
        return;
    if (g->IsInCombat())
    {
        g->AttackStop();
        g->CombatStop(true);
    }
    WorldPacket paquet(CMSG_BATTLEFIELD_PORT, 1 + 1 + 4 + 2 + 1);
    paquet << uint8(TYPE_1V1) << uint8(0) << uint32(bg->GetBgTypeID()) << uint16(0x1F90) << uint8(1);
    g->GetSession()->HandleBattleFieldPortOpcode(paquet);
    uint32& essais = estJoueur ? m.essaisEntreeJ : m.essaisEntree;
    ++essais;
    if (g->InBattleground() || essais == 1 || essais % 10 == 0)
        LOG_INFO("playerbots", "coa arene1v1: entree_forcee id={} gladiateur={} instance={} bg_type={} essai={} reussie={}"
                 " en_combat={}{}", m.id, g->GetGUID().GetCounter(), m.instance, uint32(bg->GetBgTypeID()), essais,
                 g->InBattleground() ? 1 : 0, g->IsInCombat() ? 1 : 0,
                 m.banc ? (estJoueur ? " banc=1 role=joueur" : " banc=1 role=gladiateur") : "");
}

// FIL MONDE, sans verrou. `m` est une COPIE.
Verdict Examiner(Match m, uint32 maintenant)
{
    Verdict v;
    v.id = m.id;
    Player* j = ObjectAccessor::FindPlayerByLowGUID(m.joueur);
    Player* g = ObjectAccessor::FindPlayerByLowGUID(m.gladiateur);
    bool const jFile = j && j->InBattlegroundQueueForBattlegroundQueueType(FILE_1V1);
    bool const gFile = g && g->InBattlegroundQueueForBattlegroundQueueType(FILE_1V1);
    bool const jArene = j && j->InArena();
    bool const gArene = g && g->InArena();

    auto Abandon = [&](char const* raison) {
        SortirDeFile(j, !m.banc);   // banc : A est un bot, pas de paquet
        SortirDeFile(g, false);
        Liberer(m.gladiateur, m.id, raison);
        if (m.banc)
            Liberer(m.joueur, m.id, raison);
        LOG_INFO("playerbots", "coa arene1v1: abandon id={} joueur={} gladiateur={} etat={} raison={} duree_s={}{}", m.id,
                 m.joueur, m.gladiateur, NomEtat(m.etat), raison, maintenant - m.inscritS, m.banc ? " banc=1" : "");
        v.retirer = true;
    };

    // Accepte (SetBattlegroundId pose a l'acceptation, avant la teleportation) : la
    // protection tombe, la composition ordinaire revient (avec `arena`).
    if (g && g->InBattleground())
        Liberer(m.gladiateur, m.id, "entree");
    if (m.banc && j && j->InBattleground())
        Liberer(m.joueur, m.id, "entree");

    if (m.etat == Etat::FINI)
    {
        Liberer(m.gladiateur, m.id, "fin");
        if (m.banc)
            Liberer(m.joueur, m.id, "fin");
        // Depart differe (AiPlayerbot.Arene1v1.DelaiDepartS) : le gladiateur reste visible (le
        // coeur a ressuscite tout le monde a la fin, Battleground.cpp:882-885) tant que le joueur
        // est dans l'arene et que le delai court : pas de disparition brutale. Le match reste dans
        // la table jusqu'a ce que le gladiateur soit sorti de CETTE arene.
        Battleground* bg = m.instance ? sBattlegroundMgr->GetBattleground(m.instance, BATTLEGROUND_TYPE_NONE) : nullptr;
        bool const gDedans = g && bg && g->GetBattleground() == bg;
        uint32 const delai = sPlayerbotAIConfig.arene1v1DelaiDepartS;
        uint32 const ecoule = m.finS && maintenant >= m.finS ? maintenant - m.finS : 0;
        // Filet : le coeur expulse a TIME_TO_AUTOREMOVE (120 s) ; au-dela du delai + 20 s, on oublie
        // le match sans plus rien retenir (RetenirDepart ne trouve plus rien).
        // Revue D5 : a l'oubli, un gladiateur ENCORE dans l'arene reste apaise (plus rien n'est retenu : departLibre)
        // jusqu'a sa sortie -- expulsion du coeur a 120 s ; filet a 150 s.
        if (gDedans && ecoule > delai + 20 && ecoule <= 150)
        {
            if ((ecoule - delai - 21) % 30 == 0)
                LOG_INFO("playerbots", "coa arene1v1: depart id={} gladiateur={} raison=oubli essais={} ecoule_s={}", m.id,
                         m.gladiateur, m.essaisDepart, ecoule);
            if (!m.departLibre)
            {
                std::lock_guard<std::mutex> garde(verrou);
                for (Match& x : matchs)
                    if (x.id == m.id)
                        x.departLibre = true;
                m.departLibre = true;
            }
            Apaiser(g, m.apaise);
            v.maj = m;
            return v;
        }
        if (!gDedans || ecoule > 150)
        {
            // Sortie, arene detruite, deconnexion (ou filet de 150 s) : drapeaux et strategies de la retenue rendus
            // (hors ligne : rien a lever).
            if (m.apaise)
                Desapaiser(m.gladiateur);
            v.retirer = true;
            return v;
        }
        bool const jDedans = j && j->GetBattleground() == bg;
        // Banc : personne a qui montrer le gladiateur, depart immediat.
        bool const echu = m.banc || !delai || ecoule >= delai;
        // Pacifique a chaque tick tant qu'il est dans l'arene terminee, depart compris : LeaveBG recompose ses
        // strategies (ResetStrategies) avant que la teleportation ne l'ait sorti ; les drapeaux tiennent jusqu'a la
        // sortie, constatee au tick suivant (!gDedans -> Desapaiser).
        if (bg->GetStatus() == STATUS_WAIT_LEAVE)
            Apaiser(g, m.apaise);
        if (bg->GetStatus() == STATUS_WAIT_LEAVE && (echu || !jDedans))
        {
            // D'abord rendre le depart libre (sous verrou : RetenirDepart le lit), PUIS appeler
            // LeaveBG, qui consulte RetenirDepart. Fil monde : CMSG_LEAVE_BATTLEFIELD est
            // PROCESS_THREADUNSAFE (Opcodes.cpp:868), c'est son fil ordinaire.
            {
                std::lock_guard<std::mutex> garde(verrou);
                for (Match& x : matchs)
                    if (x.id == m.id)
                        x.departLibre = true;
            }
            m.departLibre = true;
            ++m.essaisDepart;
            if (PlayerbotAI* ia = GET_PLAYERBOT_AI(g))
                BGStatusAction::LeaveBG(ia);
            // Banc : le bot A sort aussi (sa propre action « bg status » le ferait, plus tard).
            if (m.banc && jDedans)
                if (PlayerbotAI* iaJ = GET_PLAYERBOT_AI(j))
                    BGStatusAction::LeaveBG(iaJ);
            if (m.essaisDepart == 1 || m.essaisDepart % 10 == 0)
                LOG_INFO("playerbots", "coa arene1v1: depart id={} gladiateur={} raison={} vivant={} ecoule_s={} essai={}"
                         " sorti={}", m.id, m.gladiateur, echu ? "delai" : "joueur_parti", g->IsAlive() ? 1 : 0, ecoule,
                         m.essaisDepart, g->GetBattleground() == bg ? 0 : 1);
        }
        v.maj = m;
        return v;
    }

    if (m.etat == Etat::EN_FILE || m.etat == Etat::INVITE)
    {
        if (jArene || gArene)
            m.etat = Etat::EN_ARENE;   // l'entree a ete manquee par le crochet : rattrapee
        else if (!jFile)
        {
            // Le joueur a quitte la file (refus de l'invitation, deconnexion, invitation
            // expiree) : le gladiateur ne doit pas y rester seul.
            Abandon(j ? "joueur_hors_file" : "joueur_deconnecte");
            return v;
        }
        else if (!gFile)
        {
            Dire(j, "Arène 1 contre 1 : votre adversaire s'est retiré. Vous pouvez vous réinscrire.");
            Abandon(g ? "gladiateur_hors_file" : "gladiateur_deconnecte");
            return v;
        }
        else
        {
            GroupQueueInfo gi;
            if (m.etat == Etat::EN_FILE && La1v1().GetPlayerGroupInfoData(j->GetGUID(), &gi) &&
                gi.IsInvitedToBGInstanceGUID)
            {
                m.etat = Etat::INVITE;
                m.instance = gi.IsInvitedToBGInstanceGUID;
                LOG_INFO("playerbots", "coa arene1v1: invitation id={} joueur={} gladiateur={} instance={} attente_s={}{}",
                         m.id, m.joueur, m.gladiateur, m.instance, maintenant - m.inscritS, m.banc ? " banc=1" : "");
                FaireEntrer(g, m);
                if (m.banc)
                    FaireEntrer(j, m, true);
            }
            else if (m.etat == Etat::INVITE)
            {
                FaireEntrer(g, m);
                if (m.banc)
                    FaireEntrer(j, m, true);
            }
            else if (m.etat == Etat::EN_FILE)
            {
                if (maintenant - m.inscritS > ATTENTE_APPARIEMENT_S)
                {
                    Dire(j, "Arène 1 contre 1 : aucun appariement, inscription annulée.");
                    Abandon("sans_appariement");
                    return v;
                }
                if (maintenant - m.relanceS >= RELANCE_FILE_S)
                {
                    m.relanceS = maintenant;
                    sBattlegroundMgr->ScheduleQueueUpdate(0, TYPE_1V1, FILE_1V1, BATTLEGROUND_AA,
                                                          BattlegroundBracketId(m.tranche));
                }
            }
        }
    }

    if (m.etat == Etat::EN_ARENE)
    {
        // Le joueur est entre le premier : le gladiateur, encore invite, entre aussi.
        if (g && gFile && !gArene && !g->InBattleground())
            FaireEntrer(g, m);
        if (m.banc && j && jFile && !jArene && !j->InBattleground())
            FaireEntrer(j, m, true);
        // Banc : meme reposee de la strategie `arena` pour le bot A.
        if (m.banc && j && jArene && !m.strategieVueJ)
            if (PlayerbotAI* iaJ = GET_PLAYERBOT_AI(j))
            {
                if (!iaJ->HasStrategy("arena", BOT_STATE_COMBAT))
                {
                    iaJ->ResetStrategies(false);
                    LOG_INFO("playerbots", "coa arene1v1: strategie id={} gladiateur={} arena_reposee={} banc=1 role=joueur",
                             m.id, m.joueur, iaJ->HasStrategy("arena", BOT_STATE_COMBAT) ? 1 : 0);
                }
                m.strategieVueJ = iaJ->HasStrategy("arena", BOT_STATE_COMBAT);
            }
        // Banc : une arene ne se termine pas forcement (deux bots qui ne se trouvent pas, un cacheur) ; le coeur ne
        // borne qu'a 46 min. Fin NULLE a BANC_COMBAT_MAX_S de combat (ou BANC_SANS_DEBUT_S sans ouverture). Fil
        // monde, aucun verrou tenu (OnBattlegroundEnd le prend) ; une seule demande (limiteFaite).
        if (m.banc && !m.limiteFaite &&
            maintenant >= (m.debutCombatS ? m.debutCombatS + BANC_COMBAT_MAX_S : m.inscritS + BANC_SANS_DEBUT_S))
        {
            Battleground* bg = m.instance ? sBattlegroundMgr->GetBattleground(m.instance, BATTLEGROUND_TYPE_NONE) : nullptr;
            bool const finissable = bg && EstArene1v1(bg) &&
                                    (bg->GetStatus() == STATUS_WAIT_JOIN || bg->GetStatus() == STATUS_IN_PROGRESS);
            LOG_INFO("playerbots", "coa arene1v1: banc_limite id={} instance={} combat_s={} fin_demandee={}", m.id,
                     m.instance, m.debutCombatS ? maintenant - m.debutCombatS : 0, finissable ? 1 : 0);
            if (finissable)
            {
                m.limiteFaite = true;  // revue D6 : seulement si la fin est vraiment demandee (sinon redemandee)
                // Pose dans la table AVANT la fin : OnBattlegroundEnd l'ecrit sur la ligne `fin` (limite=1).
                {
                    std::lock_guard<std::mutex> garde(verrou);
                    for (Match& x : matchs)
                        if (x.id == m.id)
                            x.limiteFaite = true;
                }
                bg->EndBattleground(TEAM_NEUTRAL);
            }
        }
        // La strategie `arena` : AiFactory l'ajoute a ResetStrategies quand InArena()
        // (AiFactory.cpp:552-556, 872-876). BGStatusAction l'appelle AVANT la
        // teleportation ; on la relit ici, une fois entre.
        if (g && gArene && !m.strategieVue)
        {
            if (PlayerbotAI* ia = GET_PLAYERBOT_AI(g))
            {
                if (!ia->HasStrategy("arena", BOT_STATE_COMBAT))
                {
                    ia->ResetStrategies(false);
                    LOG_INFO("playerbots", "coa arene1v1: strategie id={} gladiateur={} arena_reposee={}", m.id,
                             m.gladiateur, ia->HasStrategy("arena", BOT_STATE_COMBAT) ? 1 : 0);
                }
                m.strategieVue = ia->HasStrategy("arena", BOT_STATE_COMBAT);
            }
        }
        if (!jArene && !gArene && maintenant - m.inscritS > OUBLI_S)
        {
            LOG_INFO("playerbots", "coa arene1v1: fin id={} raison=oubli duree_s={}", m.id, maintenant - m.inscritS);
            Liberer(m.gladiateur, m.id, "oubli");
            if (m.banc)
                Liberer(m.joueur, m.id, "oubli");
            v.retirer = true;
            return v;
        }
    }
    v.maj = m;
    return v;
}

// Toutes les 5 min : reconnecter un gladiateur hors ligne (comme `.coa enrole`).
void MaintenirEnLigne()
{
    for (uint32 g : Gladiateurs())
    {
        if (ObjectAccessor::FindPlayerByLowGUID(g))
            continue;
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(g);
        uint32 const compte = sCharacterCache->GetCharacterAccountIdByGuid(guid);
        if (!compte || !sPlayerbotAIConfig.IsInRandomAccountList(compte))
        {
            LOG_INFO("playerbots", "coa arene1v1: en_ligne gladiateur={} refus=pas_bot_aleatoire", g);
            continue;
        }
        bool const ok = sRandomPlayerbotMgr.CoaEnroler(guid);
        LOG_INFO("playerbots", "coa arene1v1: en_ligne gladiateur={} reconnecte={}", g, ok ? 1 : 0);
    }
}

class CoaArene1v1WorldScript : public WorldScript
{
public:
    CoaArene1v1WorldScript() : WorldScript("CoaArene1v1WorldScript", {WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE}) {}

    void OnStartup() override
    {
        if (sPlayerbotAIConfig.arene1v1Enabled)
            PoserTables();
    }

    void OnUpdate(uint32 diff) override
    {
        cumul += diff;
        if (cumul < 1000)
            return;
        cumul = 0;
        uint32 const maintenant = Maintenant();

        if (sPlayerbotAIConfig.arene1v1Enabled && sPlayerbotAIConfig.arene1v1MaintenirEnLigne &&
            maintenant - dernierMaintien >= MAINTIEN_EN_LIGNE_S)
        {
            dernierMaintien = maintenant;
            MaintenirEnLigne();
        }

        std::vector<Match> copie;
        {
            std::lock_guard<std::mutex> garde(verrou);
            if (matchs.empty())
                return;
            copie = matchs;
        }
        std::vector<Verdict> verdicts;
        verdicts.reserve(copie.size());
        for (Match const& m : copie)
            verdicts.push_back(Examiner(m, maintenant));

        std::lock_guard<std::mutex> garde(verrou);
        for (Verdict const& v : verdicts)
        {
            auto it = std::find_if(matchs.begin(), matchs.end(), [&](Match const& x) { return x.id == v.id; });
            if (it == matchs.end())
                continue;
            if (v.retirer)
            {
                matchs.erase(it);
                nbMatchs.store(uint32(matchs.size()), std::memory_order_release);
                continue;
            }
            // Ce que les crochets ont pu ecrire entre la copie et ici (entree, debut, fin)
            // l'emporte : on ne recopie que les champs du suivi.
            if (it->etat == Etat::FINI)
            {
                // Seuls les champs du depart differe (departLibre est deja pose sous verrou).
                if (v.maj.etat == Etat::FINI)
                {
                    it->essaisDepart = v.maj.essaisDepart;
                    it->apaise = v.maj.apaise;
                }
                continue;
            }
            if (v.maj.etat > it->etat)
                it->etat = v.maj.etat;
            if (!it->instance)
                it->instance = v.maj.instance;
            it->relanceS = v.maj.relanceS;
            it->strategieVue = it->strategieVue || v.maj.strategieVue;
            it->essaisEntree = v.maj.essaisEntree;
            it->strategieVueJ = it->strategieVueJ || v.maj.strategieVueJ;
            it->essaisEntreeJ = v.maj.essaisEntreeJ;
            it->limiteFaite = it->limiteFaite || v.maj.limiteFaite;
        }
    }

private:
    uint32 cumul = 0;
    uint32 dernierMaintien = 0;
};

// ------------------------------------------------------------------ crochets d'arene

class CoaArene1v1ArenaTeamScript : public ArenaTeamScript
{
public:
    CoaArene1v1ArenaTeamScript()
        : ArenaTeamScript("CoaArene1v1ArenaTeamScript",
                          {ARENATEAMHOOK_ON_TYPEID_TO_QUEUEID, ARENATEAMHOOK_ON_QUEUEID_TO_ARENA_TYPE,
                           ARENATEAMHOOK_ON_SET_ARENA_MAX_PLAYERS_PER_TEAM})
    {
    }

    void OnTypeIDToQueueID(const BattlegroundTypeId /*bgTypeId*/, const uint8 arenaType, uint32& queueTypeID) override
    {
        if (arenaType == TYPE_1V1 && tablesPosees.load(std::memory_order_acquire))
            queueTypeID = CoaArene1v1::FILE_ARENE;
    }

    void OnQueueIdToArenaType(const BattlegroundQueueTypeId bgQueueTypeId, uint8& arenaType) override
    {
        if (bgQueueTypeId == FILE_1V1 && tablesPosees.load(std::memory_order_acquire))
            arenaType = TYPE_1V1;
    }

    void OnSetArenaMaxPlayersPerTeam(const uint8 arenaType, uint32& maxPlayerPerTeam) override
    {
        if (arenaType == TYPE_1V1 && tablesPosees.load(std::memory_order_acquire))
            maxPlayerPerTeam = 1;
    }
};

class CoaArene1v1BGScript : public AllBattlegroundScript
{
public:
    CoaArene1v1BGScript()
        : AllBattlegroundScript("CoaArene1v1BGScript",
                                {ALLBATTLEGROUNDHOOK_CAN_FILL_PLAYERS_TO_BG, ALLBATTLEGROUNDHOOK_CAN_ADD_GROUP_TO_MATCHING_POOL,
                                 ALLBATTLEGROUNDHOOK_ON_BEFORE_SEND_JOIN_MESSAGE_ARENA_QUEUE,
                                 ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_ADD_PLAYER, ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END,
                                 ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_DESTROY})
    {
    }

    static bool EstLaFile(BattlegroundQueue* queue)
    {
        return queue && tablesPosees.load(std::memory_order_acquire) && queue == &La1v1();
    }

    // Jamais de complement dans une arene 1v1 deja creee (le siege d'un gladiateur qui
    // n'est pas venu ne se donne pas a un autre inscrit).
    bool CanFillPlayersToBG(BattlegroundQueue* queue, Battleground* /*bg*/, BattlegroundBracketId /*bracket*/) override
    {
        return !EstLaFile(queue);
    }

    // FIL MONDE (BattlegroundMgr::Update). La file 11 n'apparie que le match designe :
    // le plus ancien de la tranche dont les deux membres sont en file, non invites.
    bool CanAddGroupToMatchingPool(BattlegroundQueue* queue, GroupQueueInfo* group, uint32 /*poolPlayerCount*/,
                                   Battleground* bg, BattlegroundBracketId bracketId) override
    {
        if (!EstLaFile(queue))
            return true;
        if (!group || (bg && bg->GetInstanceID()))
            return false;
        std::vector<std::pair<uint32, uint32>> candidats;
        {
            std::lock_guard<std::mutex> garde(verrou);
            for (Match const& m : matchs)
                if (m.etat == Etat::EN_FILE && m.tranche == uint8(bracketId))
                    candidats.emplace_back(m.joueur, m.gladiateur);   // ordre d'inscription
        }
        for (auto const& [j, g] : candidats)
        {
            ObjectGuid const gj = ObjectGuid::Create<HighGuid::Player>(j);
            ObjectGuid const gg = ObjectGuid::Create<HighGuid::Player>(g);
            GroupQueueInfo a, b;
            if (!queue->GetPlayerGroupInfoData(gj, &a) || !queue->GetPlayerGroupInfoData(gg, &b) ||
                a.IsInvitedToBGInstanceGUID || b.IsInvitedToBGInstanceGUID)
                continue;
            return group->Players.count(gj) || group->Players.count(gg);
        }
        return false;
    }

    // Pas d'annonce au monde pour la file 11 (chaque inscription y met aussi un bot).
    bool OnBeforeSendJoinMessageArenaQueue(BattlegroundQueue* queue, Player* /*leader*/, GroupQueueInfo* /*ginfo*/,
                                           PvPDifficultyEntry const* /*bracketEntry*/, bool /*isRated*/) override
    {
        return !EstLaFile(queue);
    }

    void OnBattlegroundAddPlayer(Battleground* bg, Player* player) override
    {
        if (!EstArene1v1(bg) || !player)
            return;
        uint32 const p = player->GetGUID().GetCounter();
        std::lock_guard<std::mutex> garde(verrou);
        for (Match& m : matchs)
        {
            if (m.etat == Etat::FINI || (m.joueur != p && m.gladiateur != p))
                continue;
            m.instance = bg->GetInstanceID();
            m.etat = Etat::EN_ARENE;
            bool const estJoueur = m.joueur == p;
            (estJoueur ? m.joueurEntre : m.gladiateurEntre) = true;
            LOG_INFO("playerbots", "coa arene1v1: entree id={} guid={} role={} instance={} carte={} niveau={}", m.id, p,
                     estJoueur ? "joueur" : "gladiateur", m.instance, bg->GetMapId(), uint32(player->GetLevel()));
            return;
        }
    }

    // Peut tourner sur le fil de la carte d'arene : ne lit que l'arene et ses joueurs.
    void OnBattlegroundEnd(Battleground* bg, TeamId winnerTeam) override
    {
        if (!EstArene1v1(bg))
            return;
        uint32 vainqueur = 0;
        if (winnerTeam != TEAM_NEUTRAL)
            for (auto const& [guid, p] : bg->GetPlayers())
                if (p && p->GetBgTeamId() == winnerTeam)
                {
                    vainqueur = guid.GetCounter();
                    break;
                }
        uint32 const maintenant = Maintenant();
        std::lock_guard<std::mutex> garde(verrou);
        for (Match& m : matchs)
        {
            if (m.instance != bg->GetInstanceID() || m.etat == Etat::FINI)
                continue;
            char const* qui = winnerTeam == TEAM_NEUTRAL      ? "nul"
                              : vainqueur == m.joueur         ? "joueur"
                              : vainqueur == m.gladiateur     ? "gladiateur"
                                                              : "aucun";
            // Forfait : l'adversaire n'est jamais entre (CheckWinConditions a l'ouverture).
            bool const forfait = !m.joueurEntre || !m.gladiateurEntre;
            LOG_INFO("playerbots", "coa arene1v1: fin id={} instance={} vainqueur={} guid_vainqueur={} joueur={} nom_joueur={}"
                     " gladiateur={} nom_gladiateur={} classe_gladiateur={} forfait={} duree_combat_s={} duree_totale_s={}{}",
                     m.id, m.instance, qui, vainqueur, m.joueur, m.nomJoueur, m.gladiateur, m.nomGladiateur,
                     uint32(m.classe), forfait ? 1 : 0,
                     m.debutCombatS ? maintenant - m.debutCombatS : 0, maintenant - m.inscritS,
                     m.banc ? Acore::StringFormat(" banc=1 classe_joueur={} cacheur={} limite={}", uint32(m.classeJoueur),
                                                  m.cacheur == 1 ? "A" : m.cacheur == 2 ? "B" : "0", m.limiteFaite ? 1 : 0)
                            : std::string());
            m.etat = Etat::FINI;
            m.finS = maintenant;
        }
    }

    void OnBattlegroundDestroy(Battleground* bg) override
    {
        if (!EstArene1v1(bg))
            return;
        std::lock_guard<std::mutex> garde(verrou);
        for (Match& m : matchs)
            if (m.instance == bg->GetInstanceID() && m.etat != Etat::FINI)
            {
                LOG_INFO("playerbots", "coa arene1v1: fin id={} instance={} vainqueur=aucun raison=arene_detruite{}", m.id,
                         m.instance, m.banc ? " banc=1" : "");
                m.etat = Etat::FINI;
                m.finS = Maintenant();
            }
    }
};

// Ouverture des portes d'une ARENE : ArenaScript::OnArenaStart (Battleground.cpp:649),
// et non AllBattlegroundScript::OnBattlegroundStart, que le coeur n'appelle que pour
// un champ de bataille (branche `else`, Battleground.cpp:651-665). Fil monde
// (BattlegroundMgr::Update -> Battleground::Update).
class CoaArene1v1ArenaScript : public ArenaScript
{
public:
    CoaArene1v1ArenaScript()
        : ArenaScript("CoaArene1v1ArenaScript", {ARENAHOOK_ON_ARENA_START, ARENAHOOK_ON_BEFORE_CHECK_WIN_CONDITION})
    {
    }

    /*
     * Victoire a l'ouverture (match id=2 du 10/10, 20:00:02 UTC : Onyx contre Gnuklato, Guardian, « fin
     * vainqueur=joueur duree_combat_s=0 », aucune ligne `debut`). Arena::CheckWinConditions (Arena.cpp), appele a
     * l'ouverture des portes (Battleground.cpp:626), a chaque mort et a chaque depart, compte les vivants par
     * Battleground::GetAlivePlayersCountByTeam (Battleground.cpp:1831), qui ecarte tout joueur dont
     * HasByteFlag(UNIT_FIELD_BYTES_2, 3, FORM_SPIRITOFREDEMPTION) : un test de BIT (0x20) sur le numero de forme, pas
     * une egalite. Toute forme 32-63 de SpellShapeshiftForm.dbc y passe pour l'Esprit de redemption : Line, Tower,
     * Assault Formation (35-37, Guardian), Elude (51) et Stalker Form (52, Ranger), Lich Form, Draconic Form...
     * Gnuklato portait Assault Formation (803709, aura 36 forme 37 = 0x25) : son equipe comptait 0 vivant, l'autre
     * gagnait. La faction n'y est pour rien. Le coeur n'est pas touche : pour les seules arenes 1v1, la meme regle
     * avec une EGALITE de forme, puis on rend faux (le coeur saute sa propre verification).
     * Fil : celui de l'appelant (monde a l'ouverture, carte d'arene a une mort) ; ne lit que l'arene et ses
     * joueurs, aucun verrou (P-137).
     */
    bool OnBeforeArenaCheckWinConditions(Battleground* const bg) override
    {
        if (!EstArene1v1(bg))
            return true;
        uint32 vivants[PVP_TEAMS_COUNT] = {0, 0};
        uint32 ecartesParLeCoeur = 0;
        for (auto const& [guid, p] : bg->GetPlayers())
        {
            if (!p || !p->IsAlive())
                continue;
            TeamId const equipe = p->GetBgTeamId();
            if (equipe != TEAM_ALLIANCE && equipe != TEAM_HORDE)
                continue;
            ShapeshiftForm const forme = p->GetShapeshiftForm();
            if (forme == FORM_SPIRITOFREDEMPTION)
                continue;
            ++vivants[equipe];
            if (forme & FORM_SPIRITOFREDEMPTION)
                ++ecartesParLeCoeur;
        }
        TeamId vainqueur = TEAM_NEUTRAL;
        if (!vivants[TEAM_ALLIANCE] && bg->GetPlayersCountByTeam(TEAM_HORDE))
            vainqueur = TEAM_HORDE;
        else if (bg->GetPlayersCountByTeam(TEAM_ALLIANCE) && !vivants[TEAM_HORDE])
            vainqueur = TEAM_ALLIANCE;
        if (ecartesParLeCoeur)
            LOG_INFO("playerbots", "coa arene1v1: verif_victoire instance={} statut={} vivants_alliance={} vivants_horde={}"
                     " formes_0x20={} vainqueur={}", bg->GetInstanceID(), uint32(bg->GetStatus()), vivants[TEAM_ALLIANCE],
                     vivants[TEAM_HORDE], ecartesParLeCoeur, uint32(vainqueur));
        if (vainqueur != TEAM_NEUTRAL)
            bg->EndBattleground(vainqueur);
        return false;
    }

    void OnArenaStart(Battleground* bg) override
    {
        if (!EstArene1v1(bg))
            return;
        std::lock_guard<std::mutex> garde(verrou);
        for (Match& m : matchs)
            if (m.instance == bg->GetInstanceID() && m.etat != Etat::FINI)
            {
                m.debutCombatS = Maintenant();
                LOG_INFO("playerbots", "coa arene1v1: debut id={} instance={} joueur_present={} gladiateur_present={}{}",
                         m.id, m.instance, m.joueurEntre ? 1 : 0, m.gladiateurEntre ? 1 : 0, m.banc ? " banc=1" : "");
            }
    }
};

// ------------------------------------------------------------------ le bouton

class CoaArene1v1GossipScript : public AllCreatureScript
{
public:
    CoaArene1v1GossipScript() : AllCreatureScript("CoaArene1v1GossipScript") {}

    // Maitres de bataille d'ARENE seulement (battlemaster_entry -> BATTLEGROUND_AA). Le
    // menu ordinaire est prepare tel quel (PrepareGossipMenu, comme HandleGossipHelloOpcode,
    // NPCHandler.cpp:188-193), puis l'option est ajoutee. Les maitres sans drapeau
    // UNIT_NPC_FLAG_GOSSIP n'ouvrent pas de dialogue : le client leur envoie
    // CMSG_BATTLEMASTER_HELLO (4 sur 20 en base le 10/10).
    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        if (!sPlayerbotAIConfig.arene1v1Enabled || !player || !creature || !creature->IsBattleMaster())
            return false;
        if (player->GetLevel() < sPlayerbotAIConfig.arene1v1NiveauMin)
            return false;
        if (sBattlegroundMgr->GetBattleMasterBG(creature->GetEntry()) != BATTLEGROUND_AA)
            return false;
        // Un humain au maitre d'arene : le banc se tait (refus `occupe`). Atomique : ce crochet tourne au fil du paquet.
        if (!GET_PLAYERBOT_AI(player))
            dernierHumainS.store(std::max<uint32>(Maintenant(), 1), std::memory_order_release);
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Arène 1 contre 1 (contre un gladiateur à votre niveau)",
                         GOSSIP_EXPEDITEUR, GOSSIP_ACTION_INSCRIRE);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Arène 1 contre 1 : choisir la classe adverse",
                         GOSSIP_EXPEDITEUR, GOSSIP_ACTION_CHOISIR);
        player->SendPreparedGossip(creature);
        return true;
    }

    // FIL MONDE (CMSG_GOSSIP_SELECT_OPTION, PROCESS_THREADUNSAFE) : Inscrire et le recensement y lisent les
    // gladiateurs (autres joueurs) et les files.
    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (sender != GOSSIP_EXPEDITEUR || !player)
            return false;
        if (action == GOSSIP_ACTION_CHOISIR)
        {
            // Sous-menu : les classes pour lesquelles un gladiateur libre de niveau <= au joueur existe, maintenant.
            // Aucune : explication au joueur, menu ferme. Option eteinte entre-temps : idem (Inscrire refuserait).
            bool const actif = sPlayerbotAIConfig.arene1v1Enabled;
            Recensement const rec = actif ? Recenser(player) : Recensement();
            std::map<uint8, uint32> parClasse;
            for (Player* b : rec.libres)
                ++parClasse[b->getClass()];
            if (parClasse.empty() || !creature)
            {
                CloseGossipMenuFor(player);
                Dire(player, Explication(!actif ? "desactive" : rec.trop ? "aucun_gladiateur_au_niveau" : "aucun_gladiateur"));
                LOG_INFO("playerbots", "coa arene1v1: sous_menu joueur={} classes=0 actif={}", player->GetGUID().GetCounter(),
                         sPlayerbotAIConfig.arene1v1Enabled ? 1 : 0);
                return true;
            }
            ClearGossipMenuFor(player);
            for (auto const& [classe, n] : parClasse)
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                                 NomClasse(classe) + " (" + std::to_string(n) + (n > 1 ? " libres)" : " libre)"),
                                 GOSSIP_EXPEDITEUR, GOSSIP_ACTION_CLASSE + classe);
            SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature);
            return true;
        }
        CloseGossipMenuFor(player);
        std::string refus;
        if (action == GOSSIP_ACTION_INSCRIRE)
            CoaArene1v1::Inscrire(player, "dialogue", refus);
        else if (action > GOSSIP_ACTION_CLASSE && action - GOSSIP_ACTION_CLASSE < MAX_CLASSES &&
                 IsAscensionClass(uint8(action - GOSSIP_ACTION_CLASSE)))
            // Re-verifiee par Inscrire : le gladiateur a pu etre pris ou partir depuis l'affichage du sous-menu.
            CoaArene1v1::Inscrire(player, "dialogue_classe", refus, uint8(action - GOSSIP_ACTION_CLASSE));
        return true;
    }
};

// ------------------------------------------------------------------ reponse a l'invitation

// HandleBattleFieldPortOpcode appelle OnPlayerCanBattleFieldPort (BattleGroundHandler.cpp:
// 430) APRES ses refus « pas en file » et « en combat » (l.409-423, non vus ici), AVANT la
// lecture de la file : on y lit ce que le CLIENT renvoie (type d'arene, action 1 =
// entrer, 0 = quitter). Fil monde (PROCESS_THREADUNSAFE). Ne bloque jamais.
class CoaArene1v1PlayerScript : public PlayerScript
{
public:
    CoaArene1v1PlayerScript()
        : PlayerScript("CoaArene1v1PlayerScript",
                       {PLAYERHOOK_CAN_BATTLEFIELD_PORT,
                        PLAYERHOOK_ON_BEFORE_BATTLEMASTER_JOIN_ARENA})  // CoA (10/10/2026) arene 1v1 depuis l'interface JcJ
    {
    }

    // CoA (10/10/2026) arene 1v1 depuis l'interface JcJ
    // HandleBattlemasterJoinArena (BattleGroundHandler.cpp), juste apres la lecture du
    // paquet, AVANT la recherche du maitre de bataille : le bouton « 1v1 » de l'interface
    // Ascension (Skirmish1v1Button, bgIndex 3) appelle JoinBattlefield(3) sur la liste
    // d'arenes, que le coeur a envoyee avec un guid vide (HandleBattlefieldListOpcode) ;
    // le coeur jetterait donc la demande en silence. ArenaFrame.lua:57 du client :
    // « Ascension replaced 5v5 by 1v1 queue » -> slot 2. Fil monde
    // (CMSG_BATTLEMASTER_JOIN_ARENA est PROCESS_THREADUNSAFE, Opcodes.cpp:987), comme
    // l'option de dialogue : Inscrire peut y lire le gladiateur et les files.
    // Rend vrai (le coeur s'arrete) seulement pour un humain, Enabled, slot 2, solo, non
    // classe ; sinon faux et le coeur fait comme avant. Le prix : la file 5v5 solo non
    // classee n'est plus joignable par un humain quand Enabled = 1 (voulu : l'interface
    // Ascension n'a plus de 5v5). Les bots (BattleGroundJoinAction.cpp, slot 2 possible
    // en escarmouche solo) ne sont jamais pris.
    bool OnPlayerBeforeBattlemasterJoinArena(Player* player, ObjectGuid guid, uint8 arenaslot, uint8 asGroup,
                                             uint8 isRated) override
    {
        if (!player || GET_PLAYERBOT_AI(player))
            return false;
        bool const pris = sPlayerbotAIConfig.arene1v1Enabled && arenaslot == 2 && !asGroup && !isRated;
        // Revue (10/10) : un clic en rafale ne doit pas inonder CoaBots.log ni le chat. Un paquet
        // par joueur toutes les 2 s au plus ; le surplus est avale (pris) ou rendu au coeur
        // (non pris), sans ligne. Fil monde seul (PROCESS_THREADUNSAFE) : pas de verrou.
        static std::unordered_map<uint32, uint32> dernierPaquet;
        uint32 const maintenantMs = getMSTime();
        auto it = dernierPaquet.find(player->GetGUID().GetCounter());
        if (it != dernierPaquet.end() && getMSTimeDiff(it->second, maintenantMs) < 2000)
            return pris;
        dernierPaquet[player->GetGUID().GetCounter()] = maintenantMs;
        LOG_INFO("playerbots", "coa arene1v1: paquet_join_arena joueur={} nom={} guid={} slot={} groupe={} classe={}"
                 " pris={}", player->GetGUID().GetCounter(), player->GetName(), guid.ToString(), uint32(arenaslot),
                 uint32(asGroup), uint32(isRated), pris ? 1 : 0);
        if (!pris)
            return false;
        std::string refus;
        CoaArene1v1::Inscrire(player, "interface", refus);  // explique lui-meme tout refus au joueur
        return true;
    }

    bool OnPlayerCanBattleFieldPort(Player* player, uint8 arenaType, BattlegroundTypeId bgTypeId, uint8 action) override
    {
        if (!player || !nbMatchs.load(std::memory_order_acquire))
            return true;
        uint32 const p = player->GetGUID().GetCounter();
        uint32 id = 0;
        bool estJoueur = false;
        {
            std::lock_guard<std::mutex> garde(verrou);
            for (Match const& m : matchs)
                if (m.etat != Etat::FINI && (m.joueur == p || m.gladiateur == p))
                {
                    id = m.id;
                    estJoueur = m.joueur == p;
                    break;
                }
        }
        if (id)
            LOG_INFO("playerbots", "coa arene1v1: reponse id={} guid={} role={} action={} type_arene_recu={} bg_type={}"
                     " file_visee={} en_file_11={}", id, p, estJoueur ? "joueur" : "gladiateur",
                     action ? "entrer" : "quitter", uint32(arenaType), uint32(bgTypeId),
                     uint32(BattlegroundMgr::BGQueueTypeId(bgTypeId, arenaType)),
                     player->InBattlegroundQueueForBattlegroundQueueType(FILE_1V1) ? 1 : 0);
        return true;
    }
};

// ------------------------------------------------------------------ commande MJ

class CoaArene1v1CommandScript : public CommandScript
{
public:
    CoaArene1v1CommandScript() : CommandScript("CoaArene1v1CommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        // Banc bot contre bot (AiPlayerbot.Arene1v1.Banc) et etat lu par outils/banc-arene.py.
        static ChatCommandTable areneTable = {
            {"banc", HandleBancCommand, SEC_GAMEMASTER, Console::Yes},
            {"etat", HandleEtatCommand, SEC_GAMEMASTER, Console::Yes},
        };
        static ChatCommandTable coaTable = {
            // Console::Yes : pour les essais par la console RA (le nom designe le joueur).
            {"arene1v1", HandleArene1v1Command, SEC_GAMEMASTER, Console::Yes},
            {"arene", areneTable},
        };
        static ChatCommandTable commandTable = {
            {"coa", coaTable},
        };
        return commandTable;
    }

    // .coa arene1v1 <joueur>
    static bool HandleArene1v1Command(ChatHandler* handler, char const* args)
    {
        std::string nom = args ? args : "";
        while (!nom.empty() && (nom.back() == ' ' || nom.back() == '\r' || nom.back() == '\n'))
            nom.pop_back();
        while (!nom.empty() && nom.front() == ' ')
            nom.erase(nom.begin());
        if (nom.empty())
        {
            handler->PSendSysMessage("Usage: .coa arene1v1 <joueur>");
            return false;
        }
        Player* joueur = ObjectAccessor::FindPlayerByName(nom, true);
        if (!joueur)
        {
            handler->PSendSysMessage("coa arene1v1: {} est introuvable ou hors du monde.", nom);
            return false;
        }
        std::string refus;
        bool const ok = CoaArene1v1::Inscrire(joueur, "commande", refus);
        handler->PSendSysMessage("coa arene1v1: {} {}", joueur->GetName(), ok ? "inscrit" : "refus=" + refus);
        return ok;
    }

    // .coa arene etat -- lecture seule, fil monde (console RA). Ce que outils/banc-arene.py verifie avant chaque match.
    static bool HandleEtatCommand(ChatHandler* handler, char const* /*args*/)
    {
        uint32 humains = 0, bancs = 0;
        {
            std::lock_guard<std::mutex> garde(verrou);
            for (Match const& m : matchs)
                if (m.etat != Etat::FINI)
                    ++(m.banc ? bancs : humains);
        }
        // Les humains en arene (toute arene) ou en file d'arene, lus au fil monde comme WhoListCacheMgr.
        uint32 enArene = 0, enFile = 0;
        for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
        {
            if (!p || !p->GetSession() || GET_PLAYERBOT_AI(p))
                continue;
            if (p->InArena())
                ++enArene;
            else
                for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
                {
                    BattlegroundQueueTypeId const q = p->GetBattlegroundQueueTypeId(i);
                    if (q >= BATTLEGROUND_QUEUE_2v2 && q < MAX_BATTLEGROUND_QUEUE_TYPES)
                    {
                        ++enFile;
                        break;
                    }
                }
        }
        uint32 const dernier = dernierHumainS.load(std::memory_order_acquire);
        uint32 const maintenant = Maintenant();
        uint32 const pause = dernier && maintenant - dernier < BANC_PAUSE_HUMAIN_S ? BANC_PAUSE_HUMAIN_S - (maintenant - dernier) : 0;
        handler->PSendSysMessage("coa arene etat: banc_option={} ia_armee={} matchs_humains={} matchs_banc={} banc_max={}"
                                 " humains_en_arene={} humains_en_file_arene={} pause_humaine_s={}",
                                 sPlayerbotAIConfig.arene1v1Banc ? 1 : 0,
                                 (sPlayerbotAIConfig.coaAreneIaCommune || sPlayerbotAIConfig.coaAreneIaRanger) ? 1 : 0,
                                 humains, bancs, BANC_MAX, enArene, enFile, pause);
        return true;
    }

    /*
     * .coa arene banc <guidA> <guidB> <niveau> [cacheur=0|A|B] -- FIL MONDE (console RA : World::ProcessCliCommands).
     * Deux gladiateurs de AiPlayerbot.Arene1v1.Gladiateurs, prepares au niveau donne (jamais descendus), mis en file
     * 11 l'un contre l'autre : A tient le role « joueur », B celui du gladiateur. Meme table `matchs`, drapeau `banc`.
     * L'humain d'abord : refus `occupe` pendant un match humain, BANC_PAUSE_HUMAIN_S apres toute demande humaine, et
     * au-dela de BANC_MAX bancs ; un humain qui manque de gladiateur fait ceder le banc (CederLeBanc, Inscrire).
     * Monter un bot ne doit pas priver un humain de bas niveau : refus `reserve_niveau` si le bot est le seul de sa
     * classe a ce niveau ou en dessous (le plus bas niveau de chaque classe de gladiateurs ne monte jamais).
     */
    static bool HandleBancCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> mots;
        {
            std::istringstream flux(args ? args : "");
            for (std::string mot; flux >> mot;)
                mots.push_back(mot);
        }
        uint32 a = 0, b = 0, niveau = 0;
        uint8 cacheur = 0;
        auto Refus = [&](std::string const& code, std::string const& detail = "") {
            handler->PSendSysMessage("coa arene banc: refus={}{}{}", code, detail.empty() ? "" : " ", detail);
            LOG_INFO("playerbots", "coa arene1v1: banc refus={} a={} b={} niveau={}{}{}", code, a, b, niveau,
                     detail.empty() ? "" : " ", detail);
            handler->SetSentErrorMessage(true);
            return false;
        };
        if (!sPlayerbotAIConfig.arene1v1Banc)
            return Refus("desactive_banc");
        auto Entier = [](std::string const& t, uint32& v) {
            if (t.empty() || t.size() > 10 || !std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return false;
            unsigned long long const x = std::strtoull(t.c_str(), nullptr, 10);
            if (x == 0 || x > 0xFFFFFFFFull)
                return false;
            v = uint32(x);
            return true;
        };
        if (mots.size() < 3 || mots.size() > 4 || !Entier(mots[0], a) || !Entier(mots[1], b) || !Entier(mots[2], niveau))
        {
            handler->PSendSysMessage("Usage: .coa arene banc <guidA> <guidB> <niveau> [cacheur=0|A|B]");
            return Refus("syntaxe");
        }
        if (mots.size() == 4)
        {
            std::string c = mots[3];
            std::transform(c.begin(), c.end(), c.begin(), [](unsigned char x) { return char(std::tolower(x)); });
            if (c.rfind("cacheur=", 0) == 0)
                c = c.substr(8);
            if (c == "a")
                cacheur = 1;
            else if (c == "b")
                cacheur = 2;
            else if (c != "0" && c != "aucun")
                return Refus("syntaxe", "cacheur=" + mots[3]);
        }
        // L'IA d'arene (strategie « coa arene ») porte la trace par seconde et le mode cacheur : sans elle, le banc ne
        // mesurerait rien de ce qu'il pretend mesurer.
        if (!sPlayerbotAIConfig.coaAreneIaCommune && !sPlayerbotAIConfig.coaAreneIaRanger)
            return Refus("ia_eteinte");
        if (a == b)
            return Refus("meme_bot");
        if (niveau < 10 || niveau > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
            return Refus("niveau_hors_bornes");

        // L'humain d'abord.
        {
            uint32 humains = 0, bancs = 0;
            {
                std::lock_guard<std::mutex> garde(verrou);
                for (Match const& m : matchs)
                    if (m.etat != Etat::FINI)
                        ++(m.banc ? bancs : humains);
            }
            if (humains)
                return Refus("occupe", "raison=match_humain");
            uint32 const dernier = dernierHumainS.load(std::memory_order_acquire);
            uint32 const maintenant = Maintenant();
            if (dernier && maintenant - dernier < BANC_PAUSE_HUMAIN_S)
                return Refus("occupe", "raison=humain_recent attente_s=" +
                                           std::to_string(BANC_PAUSE_HUMAIN_S - (maintenant - dernier)));
            if (bancs >= BANC_MAX)
                return Refus("occupe", "raison=bancs_max");
        }

        std::vector<uint32> const liste = Gladiateurs();
        for (uint32 g : {a, b})
            if (std::find(liste.begin(), liste.end(), g) == liste.end())
                return Refus("pas_gladiateur", "guid=" + std::to_string(g));
        Player* pa = ObjectAccessor::FindPlayerByLowGUID(a);
        Player* pb = ObjectAccessor::FindPlayerByLowGUID(b);
        if (char const* r = RefusGladiateur(a, pa))
            return Refus(std::string("a_") + r);
        if (char const* r = RefusGladiateur(b, pb))
            return Refus(std::string("b_") + r);
        if (!pa->GetSession() || !pb->GetSession() || pa->GetSession()->GetAccountId() == pb->GetSession()->GetAccountId())
            return Refus("meme_compte");
        if (pa->GetLevel() > niveau)
            return Refus("a_niveau_superieur", "niveau_a=" + std::to_string(pa->GetLevel()));
        if (pb->GetLevel() > niveau)
            return Refus("b_niveau_superieur", "niveau_b=" + std::to_string(pb->GetLevel()));
        // La reserve des humains : un bot qu'il faudrait monter ne doit pas etre le seul gladiateur de sa classe a
        // son niveau ou en dessous (sinon un humain de ce niveau n'aurait plus cette classe). A et B sont exclus du
        // compte tous les deux (meme classe montee deux fois). Niveau d'un absent : cache des personnages.
        auto Reserve = [&](Player* bot) {
            if (bot->GetLevel() >= niveau)
                return true;
            for (uint32 g : liste)
            {
                if (g == a || g == b)
                    continue;
                uint8 classe = 0, niv = 0;
                if (Player* p = ObjectAccessor::FindPlayerByLowGUID(g))
                {
                    classe = p->getClass();
                    niv = p->GetLevel();
                }
                else if (CharacterCacheEntry const* c =
                             sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(g)))
                {
                    classe = c->Class;
                    niv = c->Level;
                }
                if (classe == bot->getClass() && niv && niv <= bot->GetLevel())
                    return true;
            }
            return false;
        };
        if (!Reserve(pa))
            return Refus("a_reserve_niveau", "classe=" + std::to_string(pa->getClass()) + " niveau_a=" +
                                                 std::to_string(pa->GetLevel()));
        if (!Reserve(pb))
            return Refus("b_reserve_niveau", "classe=" + std::to_string(pb->getClass()) + " niveau_b=" +
                                                 std::to_string(pb->GetLevel()));

        Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_AA);
        if (!bgt)
            return Refus("sans_modele");
        PvPDifficultyEntry const* tranche = GetBattlegroundBracketByLevel(bgt->GetMapId(), niveau);
        if (!tranche)
            return Refus("tranche");
        if (!PoserTables())
            return Refus("file_indisponible");
        GroupQueueInfo gi;
        if (La1v1().GetPlayerGroupInfoData(pa->GetGUID(), &gi) || La1v1().GetPlayerGroupInfoData(pb->GetGUID(), &gi))
            return Refus("deja_en_file");

        uint32 idMatch;
        {
            std::lock_guard<std::mutex> garde(verrou);
            idMatch = prochainId++;
        }
        LOG_INFO("playerbots", "coa arene1v1: inscription id={} joueur={} nom={} niveau={} origine=banc gladiateur={}"
                 " nom_gladiateur={} classe={} choix=banc libres=-/{} tranche={} banc=1 classe_joueur={} cacheur={}"
                 " niveau_banc={} faction_a={} faction_b={}", idMatch, a, pa->GetName(), uint32(pa->GetLevel()), b,
                 pb->GetName(), uint32(pb->getClass()), liste.size(), uint32(tranche->GetBracketId()),
                 uint32(pa->getClass()), cacheur == 1 ? "A" : cacheur == 2 ? "B" : "0", niveau,
                 uint32(pa->GetTeamId()), uint32(pb->GetTeamId()));

        // A d'abord, sans etalon ; B ensuite, son equipement rapproche de celui de A.
        Preparer(pa, niveau, nullptr, idMatch);
        Preparer(pb, niveau, pa, idMatch);

        for (Player* p : {pa, pb})
        {
            PvPDifficultyEntry const* t = GetBattlegroundBracketByLevel(bgt->GetMapId(), p->GetLevel());
            if (!p->IsInWorld() || !p->IsAlive() || p->InBattlegroundQueue() || !t ||
                t->GetBracketId() != tranche->GetBracketId())
                return Refus("non_pret", "id=" + std::to_string(idMatch) + " guid=" +
                                             std::to_string(p->GetGUID().GetCounter()));
        }
        if (La1v1().GetPlayerGroupInfoData(pa->GetGUID(), &gi) || La1v1().GetPlayerGroupInfoData(pb->GetGUID(), &gi))
            return Refus("deja_en_file", "id=" + std::to_string(idMatch));

        La1v1().AddGroup(pa, nullptr, BATTLEGROUND_AA, tranche, TYPE_1V1, false, false, 0, 0, 0, 0);
        uint32 const slotA = pa->AddBattlegroundQueueId(FILE_1V1);
        La1v1().AddGroup(pb, nullptr, BATTLEGROUND_AA, tranche, TYPE_1V1, false, false, 0, 0, 0, 0);
        pb->AddBattlegroundQueueId(FILE_1V1);
        Proteger(pa, idMatch);
        Proteger(pb, idMatch);

        Match m;
        m.id = idMatch;
        m.joueur = a;
        m.gladiateur = b;
        m.nomJoueur = pa->GetName();
        m.nomGladiateur = pb->GetName();
        m.classe = pb->getClass();
        m.classeJoueur = pa->getClass();
        m.tranche = tranche->GetBracketId();
        m.inscritS = Maintenant();
        m.relanceS = m.inscritS;
        m.banc = true;
        m.cacheur = cacheur;
        {
            std::lock_guard<std::mutex> garde(verrou);
            matchs.push_back(m);
            nbMatchs.store(uint32(matchs.size()), std::memory_order_release);
        }
        sBattlegroundMgr->ScheduleQueueUpdate(0, TYPE_1V1, FILE_1V1, BATTLEGROUND_AA, tranche->GetBracketId());
        LOG_INFO("playerbots", "coa arene1v1: mise_en_file id={} joueur={} gladiateur={} file={} type={} tranche={} slot_joueur={}"
                 " banc=1", idMatch, a, b, CoaArene1v1::FILE_ARENE, uint32(TYPE_1V1), uint32(m.tranche), slotA);
        handler->PSendSysMessage("coa arene banc: inscrit id={} a={} b={} classe_a={} classe_b={} niveau={} cacheur={}",
                                 idMatch, a, b, uint32(m.classeJoueur), uint32(m.classe), niveau,
                                 cacheur == 1 ? "A" : cacheur == 2 ? "B" : "0");
        return true;
    }
};
}  // namespace

void AddSC_coa_arene1v1()
{
    new CoaArene1v1WorldScript();
    new CoaArene1v1ArenaTeamScript();
    new CoaArene1v1BGScript();
    new CoaArene1v1ArenaScript();
    new CoaArene1v1PlayerScript();
    new CoaArene1v1GossipScript();
    new CoaArene1v1CommandScript();
}
