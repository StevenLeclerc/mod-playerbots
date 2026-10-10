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
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CoaDirecteur.h"
#include "Creature.h"
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
#include <string>
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

constexpr uint32 RELANCE_FILE_S = 5;            // ScheduleQueueUpdate tant que non apparie
constexpr uint32 ATTENTE_APPARIEMENT_S = 30;    // au-dela : abandon `sans_appariement`
constexpr uint32 OUBLI_S = 3600;                // filet : un match qu'aucune fin n'a clos
constexpr uint32 MAINTIEN_EN_LIGNE_S = 300;
constexpr uint32 PREPARATION_SCORE_ECART = 6;   // meme valeur que CoaDirecteur.cpp

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
};

std::mutex verrou;               // matchs, prochainId
std::vector<Match> matchs;
uint32 prochainId = 1;
std::atomic<bool> tablesPosees{false};
std::atomic<uint32> nbMatchs{0};   // = matchs.size(), pour GladiateurEngage (tout fil)

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
        bot->RemoveUnitFlag(UnitFlags(UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_NON_ATTACKABLE));
        if (PlayerbotAI* ia = GET_PLAYERBOT_AI(bot))
            ia->ResetStrategies(false);
    }
    LOG_INFO("playerbots", "coa arene1v1: protection id={} gladiateur={} levee=1 raison={} en_ligne={}", idMatch, guid,
             raison, bot ? 1 : 0);
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

bool DejaEngage(uint32 guid)
{
    std::lock_guard<std::mutex> garde(verrou);
    for (Match const& m : matchs)
        if (m.joueur == guid || m.gladiateur == guid)
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
    if (b->getClass() != CLASS_RANGER)
        return "pas_ranger";
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
void Preparer(Player* bot, Player* joueur, uint32 idMatch)
{
    uint32 const debutMs = getMSTime();
    uint32 const avant = bot->GetLevel();
    uint32 const niveau = joueur->GetLevel();
    // Jamais vers le bas : Inscrire ne tire que des gladiateurs de niveau <= joueur.
    if (avant < niveau)
    {
        if (bot->IsMounted())
            bot->Dismount();
        PlayerbotFactory fabrique(bot, niveau);
        fabrique.Randomize(false);
    }
    uint32 const scoreJoueur = ScoreEquipement(joueur, bot->GetLevel());
    uint32 const scoreAvant = ScoreEquipement(bot, bot->GetLevel());
    uint32 scoreApres = scoreAvant;
    uint32 reequipe = 0;
    if (scoreAvant + PREPARATION_SCORE_ECART < scoreJoueur)
    {
        Reequiper(bot, 0);
        reequipe = static_cast<uint32>(sPlayerbotAIConfig.randomGearQualityLimit);
        scoreApres = ScoreEquipement(bot, bot->GetLevel());
    }
    for (uint32 qualite : {uint32(ITEM_QUALITY_UNCOMMON), uint32(ITEM_QUALITY_NORMAL)})
    {
        if (scoreApres <= scoreJoueur + PREPARATION_SCORE_ECART)
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
             " score_joueur={} score_avant={} score_apres={} reequipe={} duree_ms={}", idMatch,
             bot->GetGUID().GetCounter(), avant, uint32(bot->GetLevel()), niveau, scoreJoueur, scoreAvant, scoreApres,
             reequipe, GetMSTimeDiffToNow(debutMs));
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
    if (p && p->GetSession())
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
    return "Inscription à l'arène 1 contre 1 impossible pour l'instant.";
}

}  // namespace

// ------------------------------------------------------------------ inscription

bool CoaArene1v1::Inscrire(Player* joueur, std::string const& origine, std::string& refus)
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
    // SynchronizeProgression garde l'AscensionLiveBaseline). On tire donc, parmi les
    // libres de niveau <= celui du joueur, ceux du PLUS HAUT niveau (au sort entre
    // egaux), et on le monte au niveau du joueur. Les gladiateurs montent ainsi avec
    // les joueurs : la reserve se renouvelle un jour (gladiateurs.py --constituer).
    std::vector<uint32> const liste = Gladiateurs();
    std::vector<Player*> libres;
    std::string raisons;
    uint32 trop = 0;
    for (uint32 g : liste)
    {
        Player* b = ObjectAccessor::FindPlayerByLowGUID(g);
        if (char const* r = RefusGladiateur(g, b))
            raisons += (raisons.empty() ? "" : ",") + std::to_string(g) + ":" + r;
        else if (b->GetLevel() > joueur->GetLevel())
        {
            ++trop;
            raisons += (raisons.empty() ? "" : ",") + std::to_string(g) + ":niveau_" + std::to_string(b->GetLevel());
        }
        else
            libres.push_back(b);
    }
    if (libres.empty())
        return Refuser(trop ? "aucun_gladiateur_au_niveau" : "aucun_gladiateur",
                       "gladiateurs=" + std::to_string(liste.size()) + " raisons=" + (raisons.empty() ? "-" : raisons));
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
             " libres={}/{} tranche={}", idMatch, jg, joueur->GetName(), uint32(joueur->GetLevel()), origine, bg,
             bot->GetName(), uint32(libres.size()), uint32(liste.size()), uint32(tranche->GetBracketId()));

    Preparer(bot, joueur, idMatch);

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
    Dire(joueur, "Arène 1 contre 1 : votre adversaire, " + m.nomGladiateur +
                     ", est prêt. Acceptez l'invitation quand elle s'affiche.");
    return true;
}

bool CoaArene1v1::GladiateurEngage(uint32 guidLow)
{
    if (!guidLow || !nbMatchs.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> garde(verrou);
    for (Match const& m : matchs)
        if (m.gladiateur == guidLow)
            return true;
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
void FaireEntrer(Player* g, Match& m)
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
    ++m.essaisEntree;
    if (g->InBattleground() || m.essaisEntree == 1 || m.essaisEntree % 10 == 0)
        LOG_INFO("playerbots", "coa arene1v1: entree_forcee id={} gladiateur={} instance={} bg_type={} essai={} reussie={}"
                 " en_combat={}", m.id, m.gladiateur, m.instance, uint32(bg->GetBgTypeID()), m.essaisEntree,
                 g->InBattleground() ? 1 : 0, g->IsInCombat() ? 1 : 0);
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
        SortirDeFile(j, true);
        SortirDeFile(g, false);
        Liberer(m.gladiateur, m.id, raison);
        LOG_INFO("playerbots", "coa arene1v1: abandon id={} joueur={} gladiateur={} etat={} raison={} duree_s={}", m.id,
                 m.joueur, m.gladiateur, NomEtat(m.etat), raison, maintenant - m.inscritS);
        v.retirer = true;
    };

    // Accepte (SetBattlegroundId pose a l'acceptation, avant la teleportation) : la
    // protection tombe, la composition ordinaire revient (avec `arena`).
    if (g && g->InBattleground())
        Liberer(m.gladiateur, m.id, "entree");

    if (m.etat == Etat::FINI)
    {
        Liberer(m.gladiateur, m.id, "fin");
        v.retirer = true;
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
                LOG_INFO("playerbots", "coa arene1v1: invitation id={} joueur={} gladiateur={} instance={} attente_s={}",
                         m.id, m.joueur, m.gladiateur, m.instance, maintenant - m.inscritS);
                FaireEntrer(g, m);
            }
            else if (m.etat == Etat::INVITE)
                FaireEntrer(g, m);
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
                continue;
            if (v.maj.etat > it->etat)
                it->etat = v.maj.etat;
            if (!it->instance)
                it->instance = v.maj.instance;
            it->relanceS = v.maj.relanceS;
            it->strategieVue = it->strategieVue || v.maj.strategieVue;
            it->essaisEntree = v.maj.essaisEntree;
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
                     " gladiateur={} nom_gladiateur={} forfait={} duree_combat_s={} duree_totale_s={}", m.id, m.instance,
                     qui, vainqueur, m.joueur, m.nomJoueur, m.gladiateur, m.nomGladiateur, forfait ? 1 : 0,
                     m.debutCombatS ? maintenant - m.debutCombatS : 0, maintenant - m.inscritS);
            m.etat = Etat::FINI;
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
                LOG_INFO("playerbots", "coa arene1v1: fin id={} instance={} vainqueur=aucun raison=arene_detruite", m.id,
                         m.instance);
                m.etat = Etat::FINI;
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
    CoaArene1v1ArenaScript() : ArenaScript("CoaArene1v1ArenaScript", {ARENAHOOK_ON_ARENA_START}) {}

    void OnArenaStart(Battleground* bg) override
    {
        if (!EstArene1v1(bg))
            return;
        std::lock_guard<std::mutex> garde(verrou);
        for (Match& m : matchs)
            if (m.instance == bg->GetInstanceID() && m.etat != Etat::FINI)
            {
                m.debutCombatS = Maintenant();
                LOG_INFO("playerbots", "coa arene1v1: debut id={} instance={} joueur_present={} gladiateur_present={}",
                         m.id, m.instance, m.joueurEntre ? 1 : 0, m.gladiateurEntre ? 1 : 0);
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
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Arène 1 contre 1 (contre un gladiateur à votre niveau)",
                         GOSSIP_EXPEDITEUR, GOSSIP_ACTION_INSCRIRE);
        player->SendPreparedGossip(creature);
        return true;
    }

    // FIL MONDE (CMSG_GOSSIP_SELECT_OPTION, PROCESS_THREADUNSAFE).
    bool CanCreatureGossipSelect(Player* player, Creature* /*creature*/, uint32 sender, uint32 action) override
    {
        if (sender != GOSSIP_EXPEDITEUR)
            return false;
        CloseGossipMenuFor(player);
        if (action == GOSSIP_ACTION_INSCRIRE)
        {
            std::string refus;
            CoaArene1v1::Inscrire(player, "dialogue", refus);
        }
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
        static ChatCommandTable coaTable = {
            // Console::Yes : pour les essais par la console RA (le nom designe le joueur).
            {"arene1v1", HandleArene1v1Command, SEC_GAMEMASTER, Console::Yes},
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
