/*
 * CoA -- le Directeur, lot D1. Voir CoaDirecteur.h pour le pourquoi.
 */

#include "CoaDirecteur.h"
#include "CoaArene1v1.h"   // Arene1v1 : GladiateurEngage, garde du bus
#include "CoaDirecteurSource.h"
#include "CheckMountStateAction.h"   // lot T6 : MountData (CollectMountData)

#include "Channel.h"
#include "ChannelMgr.h"
#include "CellImpl.h"   // lot D5 : creatures autour du joueur (attirer)
#include "CharacterCache.h"
#include "Chat.h"
#include "Corpse.h"
#include "CreatureTextMgr.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GridDefines.h"
#include "GridNotifiers.h"       // lot D5
#include "GridNotifiersImpl.h"   // lot D5
#include "GridTerrainData.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LastMovementValue.h"
#include "PositionValue.h"
#include "LFGMgr.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "MapMgr.h"
#include "MercenaryRewards.h"
#include "MiscPackets.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PathGenerator.h"   // lot D5 : chemin de la creature attiree
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "Weather.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <future>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Lot T6 (13/10/2026) : les sorts de monture que connait un bot, lus comme l'action « check mount state »
// (definie sans `static` dans Ai/Base/Actions/CheckMountStateAction.cpp, non declaree dans son en-tete).
MountData CollectMountData(Player const* bot);

namespace
{
// ------------------------------------------------------------------ file 8888

constexpr size_t FILE_MAX = 256;           // requetes en attente, au-dela : "busy"
constexpr size_t PAR_TICK = 64;            // executees par tick monde
constexpr auto DELAI_REPONSE = std::chrono::milliseconds(2000);

struct Requete
{
    std::string texte;
    std::promise<std::string> reponse;
};

std::mutex verrouFile;
std::deque<std::shared_ptr<Requete>> file;

// Correctif C5. Pose par WorldScript::OnShutdown (coa_directeur_worldscript,
// plus bas), apres la boucle monde et avant la destruction des statiques : un
// fil du 8888 qui le lit vrai rend "timeout" SANS prendre verrouFile ni toucher
// `file`. Il REDUIT la fenetre, il ne la ferme pas (un fil peut l'avoir lu faux
// juste avant) : SUPPOSE sans consequence, OnShutdown precedant de loin la
// destruction des statiques (Main.cpp:419).
std::atomic<bool> arretServeur{false};

/*
 * `monde` : une ligne par personnage en jeu, champs separes par ',', lignes
 * par une tabulation, pour tenir dans le protocole a une ligne du port 8888.
 *
 *   guid,nom,type,carte,zone,aire,x,y,z,niveau,pv%,combat,etat,ffa,pvp,victime,vol
 *
 * vol (26/09) : 0 au sol, 1 en taxi (IsInFlight), 2 drapeaux de vol (IsFlying) --
 * ATTENTION, 2 vaut aussi pour un bot au sol sur monture volante : playerbots
 * pose FLYING|DISABLE_GRAVITY des qu'il porte l'aura (MovementActions.cpp).
 * Les lecteurs tolerent son absence (observateur.py, ecoute.py : `len(c) < 16`).
 * type : R (bot aleatoire), B (autre bot), H (humain). etat : A vivant,
 * D corps (esprit non libere), G fantome -- meme distinction que
 * LogPlayerLocation. Le niveau est ecrit comme un NOMBRE : le CSV de playerbots
 * l'ecrit comme un octet brut (P-139), on ne reproduit pas l'erreur.
 *
 * Execute par le fil monde : ObjectAccessor::GetPlayers() et chaque Player* y
 * sont stables, les cartes ayant fini leur mise a jour.
 */
std::string Monde()
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    bool premier = true;
    for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
    {
        if (!p || !p->IsInWorld())
            continue;
        if (!premier)
            out << '\t';
        premier = false;

        char const type = sRandomPlayerbotMgr.IsRandomBot(p) ? 'R' : (GET_PLAYERBOT_AI(p) ? 'B' : 'H');
        // Correctif C1 : GetCorpse() ne cherche que sur la carte COURANTE
        // (Player.cpp:4876-4878) ; un fantome dont le corps est reste dans une
        // instance sortait 'D'. Le drapeau de fantome, lui, est sur le joueur.
        char const etat = p->isDead() ? (p->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? 'G' : 'D') : 'A';
        Unit const* victime = p->GetVictim();

        out << p->GetGUID().GetCounter() << ',' << p->GetName() << ',' << type << ','
            << p->GetMapId() << ',' << p->GetZoneId() << ',' << p->GetAreaId() << ','
            << p->GetPositionX() << ',' << p->GetPositionY() << ',' << p->GetPositionZ() << ','
            << static_cast<uint32>(p->GetLevel()) << ','
            << static_cast<uint32>(p->GetHealthPct()) << ','
            << (p->IsInCombat() ? 1 : 0) << ',' << etat << ','
            << (p->IsFFAPvP() ? 1 : 0) << ',' << (p->IsPvP() ? 1 : 0) << ','
            << (victime ? victime->GetGUID().GetCounter() : 0) << ','
            << (p->IsInFlight() ? 1 : (p->IsFlying() ? 2 : 0));
    }
    return out.str();
}

// `monde` coute ~533 personnages et ~50 Ko : sans plafond, un client qui
// l'enchainerait ferait jusqu'a PAR_TICK photos par tick, sur le fil monde.
// On sert donc la meme photo pendant une seconde. Seul le fil monde touche
// ces deux variables (ViderRequetes), aucun verrou n'est necessaire.
constexpr uint32 CACHE_MONDE_MS = 1000;
std::string photoMonde;
uint32 photoMondeMs = 0;
bool photoMondeValide = false;

std::string Consignes();                      // plus bas, avec le magasin
std::string Lieux(std::string const& texte);  // plus bas
std::string Joueur(std::string const& texte); // plus bas (lot D3, § 3.4)

std::string Executer(std::string const& texte)
{
    if (texte == "consignes")
        return Consignes();
    if (texte.rfind("lieux,", 0) == 0)
        return Lieux(texte);
    // Lot D3 : intercepte AVANT HandleRemoteCommand, qui rendait `invalid guid`
    // pour un humain (binaire D2).
    if (texte.rfind("joueur,", 0) == 0)
        return Joueur(texte);
    if (texte == "monde" || texte.rfind("monde,", 0) == 0)
    {
        uint32 const maintenant = getMSTime();
        if (!photoMondeValide || getMSTimeDiff(photoMondeMs, maintenant) >= CACHE_MONDE_MS)
        {
            photoMonde = Monde();
            photoMondeMs = maintenant;
            photoMondeValide = true;
        }
        return photoMonde;
    }
    return RandomPlayerbotMgr::instance().HandleRemoteCommand(texte);
}

// ------------------------------------------------------------------ arrets

std::mutex verrouArrets;
std::unordered_set<uint32> arrets;

// ------------------------------------------------------------------ commandes

constexpr size_t TEXTE_MAX = 500;
constexpr uint32 INTERVALLE_MIN_MS = 3000;   // une commande toutes les 3 s par personnage

std::mutex verrouCadence;
std::unordered_map<uint32, uint32> derniereCommande;

bool TropTot(uint32 guidLow)
{
    std::lock_guard<std::mutex> garde(verrouCadence);
    uint32 const maintenant = getMSTime();
    auto it = derniereCommande.find(guidLow);
    if (it != derniereCommande.end() && getMSTimeDiff(it->second, maintenant) < INTERVALLE_MIN_MS)
        return true;
    derniereCommande[guidLow] = maintenant;
    return false;
}

std::string Nettoyer(char const* args)
{
    std::string t = args ? args : "";
    while (!t.empty() && (t.back() == ' ' || t.back() == '\r' || t.back() == '\n'))
        t.pop_back();
    while (!t.empty() && t.front() == ' ')
        t.erase(t.begin());
    if (t.size() > TEXTE_MAX)
    {
        // Lot D3 (SPEC-directeur-D3.md § 3.3) : la coupe est en OCTETS, et elle
        // pouvait tomber au milieu d'un caractere accentue -- la ligne partait
        // en base avec un octet de tete orphelin. On remonte au debut du dernier
        // caractere ; s'il annonce plus d'octets qu'il n'en reste, il est retire.
        t.resize(TEXTE_MAX);
        size_t debut = t.size() - 1;
        while (debut > 0 && (static_cast<unsigned char>(t[debut]) & 0xC0) == 0x80)
            --debut;
        unsigned char const tete = static_cast<unsigned char>(t[debut]);
        size_t const longueur = tete < 0x80 ? 1 : (tete & 0xE0) == 0xC0 ? 2 : (tete & 0xF0) == 0xE0 ? 3
                                : (tete & 0xF8) == 0xF0 ? 4 : 1;
        if ((tete & 0xC0) == 0x80 || debut + longueur > t.size())
            t.resize(debut);
    }
    return t;
}

// La demande part en base de facon ASYNCHRONE (DatabaseWorkerPool::Execute) :
// le fil qui execute la commande n'attend jamais MySQL.
void Enregistrer(Player* joueur, char const* type, std::string texte)
{
    CharacterDatabase.EscapeString(texte);
    CharacterDatabase.Execute(
        "INSERT INTO coa_dir_demande (guid, compte, type, texte, carte, zone, aire, x, y, z, niveau, statut, cree_le) "
        "VALUES ({}, {}, '{}', '{}', {}, {}, {}, {}, {}, {}, {}, 'nouveau', NOW())",
        joueur->GetGUID().GetCounter(), joueur->GetSession()->GetAccountId(), type, texte,
        joueur->GetMapId(), joueur->GetZoneId(), joueur->GetAreaId(), joueur->GetPositionX(),
        joueur->GetPositionY(), joueur->GetPositionZ(), static_cast<uint32>(joueur->GetLevel()));
}

Player* JoueurDe(ChatHandler* handler)
{
    WorldSession* session = handler->GetSession();
    return session ? session->GetPlayer() : nullptr;
}

// ------------------------------------------------------------------ arrets en base (C2)
//
// Correctif C2. Le drapeau d'arret est pose EN MEMOIRE tout de suite (arrets,
// plus haut) ; la base, elle, ne recoit que le DERNIER etat de chaque joueur,
// au plus une fois par seconde et par joueur. Un stop puis une reprise dans la
// seconde n'ecrivent donc que l'etat final, jamais l'intermediaire -- une
// cadence naive aurait pu perdre la reprise et laisser le joueur arrete apres
// un redemarrage.
//
// HandleStop et HandleReprise tournent sur le fil monde (CMSG_MESSAGECHAT est
// PROCESS_THREADUNSAFE) comme Persister ; le verrou n'est qu'une ceinture, pour
// le cas ou une commande serait un jour executee depuis un autre fil.

constexpr uint32 PERSISTANCE_MIN_MS = 1000;
constexpr uint32 DEMANDE_ARRET_MIN_MS = 3000;   // ligne coa_dir_demande de stop/reprise

std::mutex verrouPersistance;
std::unordered_map<uint32, bool> aPersister;          // guid -> arrete ?
std::unordered_map<uint32, uint32> dernierePersistance; // guid -> getMSTime
std::unordered_map<uint32, std::pair<uint32, bool>> derniereDemandeArret; // guid -> (getMSTime, arret ?)

void NoterAPersister(uint32 guidLow, bool arrete)
{
    std::lock_guard<std::mutex> garde(verrouPersistance);
    aPersister[guidLow] = arrete;
}

// Cadence propre de la ligne de demande de stop/reprise : 3 s par joueur, pour
// les REPETITIONS seulement. L'ARRET lui-meme n'est jamais freine ; seule sa
// trace dans coa_dir_demande l'est, pour qu'un joueur qui martele la meme
// commande n'inonde pas la table. Un CHANGEMENT d'etat (stop -> reprise, ou
// l'inverse) est toujours trace : sinon le dernier constat de l'observateur
// dirait STOP alors que le jeu et coa_dir_arret disent reprise (revue D2).
bool DemandeArretTropTot(uint32 guidLow, bool arret)
{
    std::lock_guard<std::mutex> garde(verrouPersistance);
    uint32 const maintenant = getMSTime();
    auto it = derniereDemandeArret.find(guidLow);
    if (it != derniereDemandeArret.end() && it->second.second == arret &&
        getMSTimeDiff(it->second.first, maintenant) < DEMANDE_ARRET_MIN_MS)
        return true;
    derniereDemandeArret[guidLow] = { maintenant, arret };
    return false;
}

// `force` : a l'arret du serveur (OnShutdown), tout ce qui attend est ecrit,
// sans la cadence d'une seconde. Toujours par la file ASYNCHRONE : StopDB
// n'est appele qu'apres OnShutdown (Main.cpp) et DatabaseWorkerPool::Close
// vide la file avant de fermer (PCQueue::Shutdown, arret en douceur). Une
// ecriture SYNCHRONE ici pourrait au contraire passer AVANT une ecriture
// asynchrone plus ancienne du meme joueur encore en file, et l'etat
// intermediaire gagnerait en base (le travers que C2 evite). L'ordre des
// ecritures d'un meme joueur tient a CharacterDatabase.WorkerThreads = 1
// (valeur de worldserver.conf) ; avec plusieurs fils, seule la seconde
// d'ecart entre deux ecritures d'un joueur le protege.
void PersisterArrets(bool force)
{
    std::vector<std::pair<uint32, bool>> lot;
    {
        std::lock_guard<std::mutex> garde(verrouPersistance);
        if (aPersister.empty())
            return;
        uint32 const maintenant = getMSTime();
        for (auto it = aPersister.begin(); it != aPersister.end();)
        {
            auto d = dernierePersistance.find(it->first);
            if (!force && d != dernierePersistance.end() && getMSTimeDiff(d->second, maintenant) < PERSISTANCE_MIN_MS)
            {
                ++it;
                continue;
            }
            lot.emplace_back(it->first, it->second);
            dernierePersistance[it->first] = maintenant;
            it = aPersister.erase(it);
        }
    }
    if (lot.empty())
        return;
    for (auto const& [guidLow, arrete] : lot)
    {
        if (arrete)
            CharacterDatabase.Execute("REPLACE INTO coa_dir_arret (guid, depuis) VALUES ({}, NOW())", guidLow);
        else
            CharacterDatabase.Execute("DELETE FROM coa_dir_arret WHERE guid = {}", guidLow);
    }
    LOG_INFO("playerbots", "coa directeur : arrets persistes ({} joueur(s){})", lot.size(), force ? ", arret du serveur" : "");
}

// ------------------------------------------------------------------ compteurs

std::array<std::atomic<uint64>, CoaDirecteur::COA_DIR_NB_COMPTEURS> compteurs{};

uint64 Lire(CoaDirecteur::CoaDirCompteur c)
{
    return compteurs[c].load(std::memory_order_relaxed);
}

// ------------------------------------------------------------------ magasin de consignes
//
// UN SEUL ECRIVAIN : le fil monde (drainage du bus, levee sur stop, echeances,
// purge). LECTEURS : tous les fils, dont les fils de carte a chaque candidat de
// cible (PossibleTargetsValue::AcceptUnit), le chemin le plus chaud du serveur.
// D'ou le compteur atomique lu AVANT tout verrou : zero treve, zero verrou. Il
// est tenu egal a treves.size() SOUS le verrou exclusif, donc il baisse a la
// levee, pas a la lecture (defaut decrit en tete de MercenaryRewards.h).

struct Treve
{
    uint32 id;       // commande qui l'a posee
    uint32 cible;
    uint32 joueur;   // humain autour duquel elle a ete posee, 0 = aucun
    uint32 finS;     // GameTime::GetGameTime(), en secondes
};

std::shared_mutex verrouConsignes;
std::unordered_map<uint32, Treve> treves;   // cle : guid bas de la cible
std::atomic<uint32> nbTreves{0};

// Paix de zone (19/10/2026, CoaDirecteur.h). `paix` n'est lu et ecrit QUE par le fil monde (bus, levees, echeances,
// Consignes et Bilan, qui y tournent tous) : aucun verrou. Les fils de carte ne lisent QUE casesPaix et nbPaix,
// atomiques : PAIX_MAX mots `zone << 32 | finS` (0 = case libre), recopies par le fil monde a chaque pose et levee.
// nbPaix est tenu egal a paix.size() ; il est ecrit APRES les cases a la pose et APRES les avoir videes a la levee :
// un lecteur qui le voit a 0 sort sans lire les cases.
struct Paix
{
    uint32 id;       // commande qui l'a posee
    uint32 zone;
    uint32 carte;
    uint32 finS;     // GameTime::GetGameTime(), en secondes
    uint32 kase;     // indice dans casesPaix
};
std::unordered_map<uint32, Paix> paix;      // cle : zone ; fil monde seul
std::array<std::atomic<uint64>, CoaDirecteur::PAIX_MAX> casesPaix{};
std::atomic<uint32> nbPaix{0};

// Lot D2c : suspensions de mercenaires, meme verrou, meme patron (un compteur
// atomique tenu egal a suspensions.size() sous le verrou exclusif).
struct Suspension
{
    uint32 id;              // commande qui l'a posee
    uint32 guid;            // bot mercenaire suspendu
    uint32 joueur;          // humain autour duquel elle a ete posee, 0 = aucun
    uint32 finS;            // GameTime::GetGameTime(), en secondes
    bool rpg;               // mode fige a la pose (SuspensionModeRpg)
    bool reconstruit;       // composition RPG reconstruite au moins une fois
    // Pour la ligne de fin (mesure du § 5.5). `ia` n'est jamais dereference :
    // il ne sert qu'a reconnaitre une reconnexion (autre moteur, compteurs
    // repartis de zero) et a rendre alors NA plutot qu'un ecart faux.
    PlayerbotAI const* ia;
    uint32 quetesAcceptees0;
    uint32 quetesRendues0;
};

std::unordered_map<uint32, Suspension> suspensions;   // cle : guid bas du bot
std::atomic<uint32> nbSuspensions{0};

// Lot D2b : affectations, meme verrou, meme patron. La partie publique (lue
// par AffectationDe) est `a` ; le reste ne sert qu'au suivi du fil monde.
struct AffectationInterne
{
    CoaDirecteur::Affectation a;
    CoaDirVerbe verbe;       // reaffecter, alleger ou peupler (audit, `consignes`)
    // Lot D3 (§ 3.6) : `origine` de la commande qui l'a posee (regle, ask, arc,
    // manuel), figee a la pose. Avec `joueur`, c'est le PROPRIETAIRE du bot
    // affecte : un autre proprietaire est refuse `occupe`.
    std::string origine;
    uint32 guid;             // bot affecte
    uint32 joueur;           // humain autour duquel elle a ete posee, 0 = aucun
    uint32 poseS;            // pour TrajetMaxSecondes
    uint8 niveau;            // niveau a la pose, suivi d'une montee de 1 ; autre ecart : levee `niveau`
    bool mercenaireRpg;      // mercenaire suspendu en mode RPG a la pose
    // Mesure du § 6.5, relevee a l'ARRIVEE (fil monde). `ia` n'est jamais
    // dereference : il reconnait une reconnexion (compteurs repartis de zero).
    PlayerbotAI const* ia;
    uint32 quetesAcceptees0;
    uint32 quetesRendues0;
    // Releve a T + RELEVE_SEJOUR_S apres l'arrivee (§ 6.5, revue D2) : une
    // ligne `coa directeur sejour: ... motif=arrivee_5min`, une seule fois.
    uint32 arriveeS = 0;
    bool releveFait = false;
    // Etape d'itineraire (a.etape) : son disque a ete atteint. Elle reste en
    // TRAJET ; ce drapeau ne sert qu'a ne journaliser l'arrivee qu'une fois, a
    // la publier dans `consignes`, et a ne plus l'abandonner pour
    // TrajetMaxSecondes (elle est arrivee).
    bool etapeArrivee = false;
};

std::unordered_map<uint32, AffectationInterne> affectations;   // cle : guid bas du bot
std::atomic<uint32> nbAffectations{0};

// Lot D2b, suite : etat `mort_en_trajet` (option Directeur.ReleveCimetiere).
// Meme verrou, meme patron. UN SEUL ECRIVAIN, le fil monde : pose par
// SuivreAffectations quand il leve une affectation EN TRAJET pour `mort`,
// retire par SuivreMortsEnTrajet (resurrection, deconnexion, echeance) et par
// la purge du bus eteint. Lu par FindCorpseAction et ReviveFromCorpseAction
// (MortEnTrajet), qui envoient alors le fantome a l'esprit-guerisseur.
struct MortTrajet
{
    uint32 id;       // affectation levee
    uint32 guid;     // bot
    uint32 carte;    // lieu de la mort : le cadavre s'il existe deja, sinon le bot
    float x, y;
    uint32 mortS;    // Maintenant() a la pose
    uint32 finS;     // mortS + ReleveSecondes
};
std::unordered_map<uint32, MortTrajet> mortsEnTrajet;   // cle : guid bas du bot
std::atomic<uint32> nbMortsEnTrajet{0};

// Correctif du 26/09 (bot 195, E-1 Orgrimmar) : TRAJETS LEVES. Meme verrou,
// meme patron. UN SEUL ECRIVAIN, le fil monde : pose par FinirAffectation pour
// toute affectation qui finit EN TRAJET, retire a l'echeance par
// SuivreTrajetsLeves (bus allume ou eteint). Lu par l'IA du bot
// (TrajetLeveVers), qui remet elle-meme son GO_GRIND a IDLE : l'etat rpgInfo
// n'a qu'un proprietaire (P-134). Sans ce relais, un bot dont l'affectation
// tombe garde le point tire pour elle, y marche sans fin (GO_GRIND n'a pas
// d'echeance) et finit par y sauter (NewRpgBaseAction::MoveFarTo, enlisement).
struct TrajetLeve
{
    uint32 id;       // affectation levee
    uint32 carte;
    float x, y, rayon;
    uint32 finS;     // Maintenant() + TRAJET_LEVE_S
    uint32 leveMs;   // getMSTime() a la levee, meme horloge que rpgInfo.startT
};
// Borne le temps pendant lequel un bot mort, en combat ou hors new rpg a la
// levee peut encore appliquer la remise a zero en revenant a son parcours.
constexpr uint32 TRAJET_LEVE_S = 900;
std::unordered_map<uint32, TrajetLeve> trajetsLeves;   // cle : guid bas du bot
std::atomic<uint32> nbTrajetsLeves{0};

// Compagnons (27/09, option Directeur.Compagnons) : PACTES. Meme verrou, meme
// patron. UN SEUL ECRIVAIN, le fil monde : pose par ExecuterAffectation a la
// pose de toute presence (etape, finale, `C-`), dans la section exclusive de
// l'affectation ; prolonge ou retire par SuivrePactes (groupe du joueur,
// echeance, joueur deconnecte, option eteinte) et par la purge du bus eteint. Lu par PossibleTargetsValue::AcceptUnit (Pacte), depuis
// les fils de carte. Il SURVIT a la levee de l'affectation : un compagnon que
// le joueur a invite (levee `maitre_humain`), puis renvoye du groupe, se
// retrouve FFA a 15-30 yd d'un joueur FFA ; calmer et stop le laissent au meme
// endroit. Le pacte n'ajoute aucune action : il retire une cible.
struct PacteCompagnon
{
    uint32 id;       // commande (presence) qui l'a pose
    uint32 joueur;   // humain protege
    uint32 finS;     // fin de l'affectation + PACTE_MARGE_S
};
constexpr uint32 PACTE_MARGE_S = 600;
// Compagnons : le point du service (sa photo du joueur) a au plus 150 yd de la
// position reelle du joueur a la pose (garde de coherence : le centre posé est
// la position reelle, revue m1).
constexpr float COMPAGNON_CENTRE_MAX_YD = 150.0f;
// Compagnons : un joueur qui a franchi plus que cela en une seconde s'est
// teleporte sur la meme carte (pierre de foyer, portail) : la presence est
// levee `joueur_saut` (revue m9), sans quoi le bot traverserait le continent.
constexpr float COMPAGNON_SAUT_YD = 300.0f;
// Le suivi ne recale le centre que si le joueur s'en est ecarte d'autant.
constexpr float COMPAGNON_RECALAGE_YD = 3.0f;
// Missions (30/09) : un suiveur de bande recale son centre sur le chef des que
// celui-ci s'en est eloigne de plus de BANDE_RECALAGE_YD ; degrade en Chasse,
// il retrouve un disque de BANDE_DEGRADEE_RAYON_YD.
constexpr float BANDE_RECALAGE_YD = 4.0f;
constexpr float BANDE_DEGRADEE_RAYON_YD = 35.0f;

// Missions de l'animation (30/09, option Directeur.Missions). Le nom de chaque
// type est celui de la colonne `texte` (contrat avec emetteur.py, _valider_d3).
char const* NomMission(CoaDirecteur::CoaDirMission m)
{
    switch (m)
    {
        case CoaDirecteur::CoaDirMission::Quete:   return "quete";
        case CoaDirecteur::CoaDirMission::Chasse:  return "chasse";
        case CoaDirecteur::CoaDirMission::Recolte: return "recolte";
        case CoaDirecteur::CoaDirMission::Passage: return "passage";
        case CoaDirecteur::CoaDirMission::Bande:   return "bande";
        case CoaDirecteur::CoaDirMission::Evenement: return "evenement";   // lot F des evenements (06/10)
        default:                                   return "aucune";
    }
}

std::unordered_map<uint32, PacteCompagnon> pactes;   // cle : guid bas du bot
std::atomic<uint32> nbPactes{0};

// Lot D4c (SPEC-directeur-D4.md § 6.2) : PRIMES. Meme verrou, meme patron. UN
// SEUL ECRIVAIN, le fil monde : pose par ExecuterPrime, retiree par
// SuivrePrimes (echeance, mort, regle, defi, deconnexion), la levee (`lever`,
// stop, bus eteint). Lue par les fils de carte (ProieImposee, sous verrou
// partage) apres le compteur atomique : sans prime, rien ne se paie.
struct Prime
{
    uint32 id;          // commande qui l'a posee
    uint32 joueur;      // guid du joueur prime (la cible)
    uint32 finS;        // Maintenant()
    uint32 plafond;     // chasseurs au plus (colonne `valeur`)
    std::string origine;  // proprietaire (regle, ask, arc, manuel) : seul le meme, ou `manuel`, remplace
    // Reserve de chasseurs (02/10) : le joueur est AFK (PLAYER_FLAGS_AFK). Lu sur
    // le JOUEUR par le fil monde seul (pose, puis SuivrePrimes chaque seconde),
    // ecrit sous le verrou exclusif ; les fils de carte ne lisent que cette
    // copie (ChasseurDe, ProieImposee). Un prime AFK n'est la proie de personne.
    bool afk = false;
    // Embuscade (02/10, option Directeur.Embuscade). Fil monde seul
    // (SuivreEmbuscades), sous le verrou exclusif ; les fils de carte ne lisent
    // que `assaut`, `raisonAssaut` et `places` (Embuscade, verrou partage).
    bool assaut = false;
    char raisonAssaut = '-';
    uint32 premierEnPositionS = 0;   // Maintenant() du premier chasseur en position, 0 : aucun
    uint32 calmeDepuisS = 0;         // assaut donne, aucun chasseur actif depuis (0 : il en reste)
    uint32 procheDepuisS = 0;        // sans assaut, un chasseur a EMB_PROCHE_YD depuis (raison e), 0 : aucun
    // Revue M3 : id de la PREMIERE prime d'une suite de reposes de meme origine
    // (relevement du plafond) -- borne des replis, qui ne repart pas a zero.
    uint32 idOrigine = 0;
    std::array<uint32, CoaDirecteur::EMBUSCADE_PLACES> places{};   // guid bas du chasseur par place, 0 libre
    // Retraite (03/10/2026) : les chasseurs de la RESERVE qui ont recu une affectation
    // `chasser` pour cette prime (guid bas, sans doublon, PRIME_SERVIS_MAX au plus).
    // Fil monde seul (ExecuterAffectation, sous le verrou exclusif) ; garde a un
    // remplacement ; relu a la fin de la prime (CommencerRetraites).
    std::vector<uint32> servis;
};
constexpr size_t PRIME_SERVIS_MAX = 48;
std::unordered_map<uint32, Prime> primes;   // cle : guid bas du joueur
std::atomic<uint32> nbPrimes{0};
// Chasseurs actifs dans le magasin des affectations, tenu SOUS le verrou
// exclusif avec nbAffectations (MajCompteursAffectations) : ProieImposee
// commence par lui, un seul chargement atomique hors de toute prime.
std::atomic<uint32> nbChasseurs{0};

// Banc d'essai de prime (03/10/2026, option Directeur.PrimeBanc). La COPIE de la
// liste de conf, ecrite par le fil monde seul (SuivreBancs, chaque seconde) sous
// son PROPRE verrou, jamais tenu en meme temps qu'un autre : les crochets de mort
// (fils de carte, PousserMort) la lisent sans toucher le magasin des consignes
// (P-137), comme chasseursSuivis. `bancTexte` : la chaine deja lue (fil monde seul).
std::shared_mutex verrouBancs;
std::unordered_set<uint32> bancs;
std::atomic<uint32> nbBancs{0};
std::string bancTexte;

// Retraite des chasseurs de reserve a la fin d'une prime (03/10/2026). Magasin des
// consignes (verrouConsignes) : ecrit par le fil monde seul (CommencerRetraites,
// SuivreRetraites, LeverRetraite), lu par les fils de carte (EnRetraite) apres son
// compteur atomique.
struct Retraite
{
    uint32 bot = 0;
    uint32 prime = 0;        // id de la prime servie
    uint32 joueur = 0;       // guid du prime
    uint32 debutS = 0;       // Maintenant() au debut
    uint32 fuirDe = 0;       // humain le plus proche a eviter (0 : aucun), recale chaque seconde
    uint32 essaiS = 0;       // Maintenant() de la derniere recherche de point
    uint8 essais = 0;        // recherches de point sans resultat
    // Banc du 03/10 (arcs 12 a 14) : le LIEU DU COMBAT, fige au debut (position du chasseur,
    // ou de son cadavre). Le prime, lui, bouge : fantome au cimetiere, ou bot releve ailleurs
    // en 20 s -- mesure depuis lui, le chasseur concluait `deja_loin` sans quitter le corps.
    uint32 carte = 0;
    float sx = 0.0f, sy = 0.0f;
    bool scene = false;
};
std::unordered_map<uint32, Retraite> retraites;   // cle : guid bas du bot
std::atomic<uint32> nbRetraites{0};
// Definies plus bas (section « retraite »), apres le repos de la reserve et la
// recherche de point sur.
void CommencerRetraite(uint32 bot, uint32 prime, uint32 joueur, char const* cause);
bool LeverRetraite(uint32 bot, char const* motif);
void SuivreRetraites();
void SuivreImmobiles();   // 04/10 : cibles de contrat immobiles (section « repos de la reserve »)
void SuivreReactions();   // 12/10 (lots T4-T5) : reactions des cibles de contrat (apres le repos de la reserve)

// Lot D4c : auras des DEFIS BLOQUANTS (PVE_ONLY, a vies, sans resurrection),
// lues UNE fois au demarrage (DemarrerBus, requete synchrone sur acore_world),
// puis lues par le fil monde seul (ExecuterPrime, SuivrePrimes) : aucun verrou.
// `defisLus` faux (table absente, requete en echec) : toute prime est refusee
// `defi` -- on ne pose pas une menace qu'on ne sait pas borner.
std::unordered_set<uint32> aurasDefiBloquant;
bool defisLus = false;

// Lot D5 (15/10/2026) : onglet CoABounty, ligne `^A|audace|optin`. GARDE DE PRESENCE des deux tables, lue UNE fois
// au demarrage (DemarrerBus, requete synchrone sur information_schema, comme la garde de colonnes) : aucune requete
// n'est jamais envoyee vers une table absente. Fil monde seul a l'ecriture ; relue au prochain demarrage.
bool tableAudaceLue = false;   // acore_characters.coa_dir_audace (migration D4a, 2026_09_27_01_coa_dir_memoire.sql)
bool tableOptinLue = false;    // acore_characters.coa_dir_imprevu_optin (2026_10_15_01_coa_dir_imprevu_optin.sql)

// Lot D4c (SPEC-directeur-D4.md § 3.3) : MORTS D'HUMAINS. Les crochets de mort
// tournent sur un FIL DE CARTE (Unit::Kill) : ils n'ecrivent que cette file, par
// valeur, sous un verrou court (P-137 : jamais le magasin des consignes). Le fil
// monde seul la vide (DrainerMorts), ecrit la ligne `coa directeur mort:` et
// garde les evenements des dernieres secondes (mortsRecentes, fil monde seul)
// pour nommer le tueur d'un prime tombe.
struct EvenementMort
{
    uint32 victime = 0;
    bool victimeHumaine = false;
    uint32 tueur = 0;        // guid du joueur tueur (maitre d'un familier) ; 0 : creature ou environnement
    char tueurType = 'E';    // H humain, J bot joueur (R ou B au fil monde), C creature, E environnement
    uint32 entree = 0;       // creature : son entree
    uint8 niveauV = 0, niveauT = 0;
    uint32 carte = 0, zone = 0, aire = 0;
    float x = 0.0f, y = 0.0f;
    uint32 t = 0;            // Maintenant() a la mort
    // Lot R3 (03/10/2026) : la victime est un CHASSEUR DE PRIME (affectation `chasser`
    // active), tue par n'importe qui -- un compagnon bot du prime, une creature, un garde.
    bool victimeChasseur = false;
    // Banc d'essai de prime (03/10/2026) : la victime est un BOT de Directeur.PrimeBanc,
    // traite comme un humain prime pour sa ligne `mort:` (champ `banc=1`, EN FIN).
    bool victimeBanc = false;
};
constexpr size_t MORTS_FILE_MAX = 256;
std::mutex verrouMorts;
std::vector<EvenementMort> fileMorts;             // sous verrouMorts
// Lot R3 (03/10/2026) : guids bas des chasseurs de prime actifs, SOUS verrouMorts.
// Copie du magasin des affectations, refaite par MajCompteursAffectations (fil monde,
// sous le verrou exclusif des consignes, puis verrouMorts : toujours dans cet ordre ;
// DrainerMorts relache verrouMorts avant de prendre les consignes, PousserMort ne
// prend que verrouMorts). Les crochets de mort (fils de carte) la lisent sans jamais
// toucher le magasin (P-137).
std::unordered_set<uint32> chasseursSuivis;
struct MortRecente
{
    EvenementMort e;
    bool chasseur;           // le tueur etait un chasseur designe de la victime
};
std::deque<MortRecente> mortsRecentes;            // fil monde seul
constexpr uint32 MORT_RECENTE_S = 5;              // un prime tombe est rapproche des morts de 5 s
uint64 nMorts = 0;                                // fil monde seul : `n=` de la ligne

// A appeler SOUS le verrou exclusif, apres toute modification du magasin des
// affectations : nbAffectations et nbChasseurs restent egaux a ce qu'ils
// comptent (au plus MaxAffectations entrees : le decompte est borne).
void MajCompteursAffectations()
{
    uint32 chasseurs = 0;
    for (auto const& [guid, f] : affectations)
        if (f.a.chasseur)
            ++chasseurs;
    nbAffectations.store(static_cast<uint32>(affectations.size()), std::memory_order_relaxed);
    nbChasseurs.store(chasseurs, std::memory_order_relaxed);
    std::lock_guard<std::mutex> garde(verrouMorts);
    chasseursSuivis.clear();
    for (auto const& [guid, f] : affectations)
        if (f.a.chasseur)
            chasseursSuivis.insert(guid);
}

uint32 Maintenant()
{
    return static_cast<uint32>(GameTime::GetGameTime().count());
}

uint32 EpoqueMonde()
{
    return static_cast<uint32>(GameTime::GetStartTime().count());
}

char const* NomVerbe(CoaDirVerbe v)
{
    switch (v)
    {
        case CoaDirVerbe::Treve:               return "treve";
        case CoaDirVerbe::Reaffecter:          return "reaffecter";
        case CoaDirVerbe::Alleger:             return "alleger";
        case CoaDirVerbe::SuspendreMercenaire: return "suspendre_mercenaire";
        case CoaDirVerbe::Lever:               return "lever";
        case CoaDirVerbe::Peupler:             return "peupler";
        case CoaDirVerbe::FaireParler:         return "faire_parler";
        case CoaDirVerbe::Repondre:            return "repondre";
        case CoaDirVerbe::HeureDuJour:         return "heure_du_jour";
        case CoaDirVerbe::Meteo:               return "meteo";
        case CoaDirVerbe::Prime:               return "prime";
        case CoaDirVerbe::Chasser:             return "chasser";
        case CoaDirVerbe::Recompenser:         return "recompenser";
        case CoaDirVerbe::Evenement:           return "evenement";
        case CoaDirVerbe::Attirer:             return "attirer";
        case CoaDirVerbe::Soigner:             return "soigner";
        case CoaDirVerbe::Paix:                return "paix";
        default:                               return "inconnu";
    }
}

CoaDirVerbe VerbeDe(std::string const& nom)
{
    if (nom == "treve")                return CoaDirVerbe::Treve;
    if (nom == "reaffecter")           return CoaDirVerbe::Reaffecter;
    if (nom == "alleger")              return CoaDirVerbe::Alleger;
    if (nom == "suspendre_mercenaire") return CoaDirVerbe::SuspendreMercenaire;
    if (nom == "lever")                return CoaDirVerbe::Lever;
    if (nom == "peupler")              return CoaDirVerbe::Peupler;
    if (nom == "faire_parler")         return CoaDirVerbe::FaireParler;
    if (nom == "repondre")             return CoaDirVerbe::Repondre;
    if (nom == "heure_du_jour")        return CoaDirVerbe::HeureDuJour;
    if (nom == "meteo")                return CoaDirVerbe::Meteo;
    if (nom == "prime")                return CoaDirVerbe::Prime;
    if (nom == "chasser")              return CoaDirVerbe::Chasser;
    if (nom == "recompenser")          return CoaDirVerbe::Recompenser;
    if (nom == "evenement")            return CoaDirVerbe::Evenement;
    if (nom == "attirer")              return CoaDirVerbe::Attirer;
    if (nom == "soigner")              return CoaDirVerbe::Soigner;
    if (nom == "paix")                 return CoaDirVerbe::Paix;
    return CoaDirVerbe::Inconnu;
}

// Verbes du lot D3 : ils exigent les colonnes de la migration D3 (§ 3.2).
bool VerbeD3(CoaDirVerbe v)
{
    return v == CoaDirVerbe::Peupler || v == CoaDirVerbe::FaireParler || v == CoaDirVerbe::Repondre ||
           v == CoaDirVerbe::HeureDuJour || v == CoaDirVerbe::Meteo;
}

// Verbes du lot D4c : ils exigent l'ENUM de la migration D4 (§ 3.2). `prime`
// lit `valeur` (colonne D3) : ColonnesD4 implique ColonnesD3.
bool VerbeD4(CoaDirVerbe v)
{
    return v == CoaDirVerbe::Prime || v == CoaDirVerbe::Chasser;
}

// Lot R3 (03/10/2026) : `recompenser` exige l'ENUM de la migration
// 2026_10_03_00_coa_directeur_recompense.sql (D4 + `recompenser`) ; il lit
// `valeur` et `texte` (colonnes D3) : ColonnesRecompense implique ColonnesD4.
bool VerbeR(CoaDirVerbe v)
{
    return v == CoaDirVerbe::Recompenser;
}

// Lot C des evenements (04/10/2026, docs/EVENEMENTS-grimoire-directeur.md § 5.1) :
// `evenement` exige l'ENUM R3 en tete ET la valeur 'evenement' (migration
// 2026_10_04_00_coa_directeur_evenement.sql) ; il lit `texte` et `valeur` (colonnes D3).
bool VerbeEv(CoaDirVerbe v)
{
    return v == CoaDirVerbe::Evenement;
}

// Lot D5 « l'imprevu » (15/10/2026) : `attirer` et `soigner` exigent l'ENUM R3 en tete ET les valeurs
// 'attirer' et 'soigner' (controle « contient », ColonnesImprevu) ; ils lisent `texte` et `valeur` (colonnes D3).
bool VerbeImprevu(CoaDirVerbe v)
{
    return v == CoaDirVerbe::Attirer || v == CoaDirVerbe::Soigner;
}

// Paix de zone (19/10/2026) : `paix` exige l'ENUM R3 en tete ET la valeur 'paix' (controle « contient »,
// ColonnesPaix ; migration 2026_10_19_00_coa_directeur_paix.sql). Il ne lit aucune colonne D3.
bool VerbePaix(CoaDirVerbe v)
{
    return v == CoaDirVerbe::Paix;
}

CoaDirCanal CanalDe(std::string const& nom)
{
    if (nom.empty())       return CoaDirCanal::Aucun;
    if (nom == "systeme")  return CoaDirCanal::Systeme;
    if (nom == "dire")     return CoaDirCanal::Dire;
    if (nom == "crier")    return CoaDirCanal::Crier;
    if (nom == "chuchoter") return CoaDirCanal::Chuchoter;
    if (nom == "zone")     return CoaDirCanal::Zone;
    return CoaDirCanal::Inconnu;
}

char const* NomCanal(CoaDirCanal c)
{
    switch (c)
    {
        case CoaDirCanal::Systeme:   return "systeme";
        case CoaDirCanal::Dire:      return "dire";
        case CoaDirCanal::Crier:     return "crier";
        case CoaDirCanal::Chuchoter: return "chuchoter";
        case CoaDirCanal::Zone:      return "zone";
        case CoaDirCanal::Aucun:     return "-";
        default:                     return "inconnu";
    }
}

// Verbes dont CE binaire porte l'effet : treve (D2a), suspendre_mercenaire
// (D2c), reaffecter et alleger (D2b), et les cinq verbes du lot D3. Un verbe
// allume dans la conf mais sans effet ici est refuse `verbe_eteint`.
bool VerbeImplemente(CoaDirVerbe v)
{
    return v == CoaDirVerbe::Treve || v == CoaDirVerbe::SuspendreMercenaire || v == CoaDirVerbe::Reaffecter ||
           v == CoaDirVerbe::Alleger || VerbeD3(v) || VerbeD4(v) || VerbeR(v) || VerbeEv(v) || VerbeImprevu(v) ||
           VerbePaix(v);
}

// `AiPlayerbot.Directeur.Verbes` : liste separee par des virgules (espaces
// toleres). Relue a chaque commande (60 par minute au plus) : un rechargement
// de conf prend effet a la commande suivante.
bool VerbeAllume(CoaDirVerbe v)
{
    std::string const& liste = sPlayerbotAIConfig.directeurVerbes;
    std::string const nom = NomVerbe(v);
    size_t debut = 0;
    while (debut <= liste.size())
    {
        size_t fin = liste.find(',', debut);
        if (fin == std::string::npos)
            fin = liste.size();
        size_t a = debut, b = fin;
        while (a < b && (liste[a] == ' ' || liste[a] == '\t'))
            ++a;
        while (b > a && (liste[b - 1] == ' ' || liste[b - 1] == '\t'))
            --b;
        if (liste.compare(a, b - a, nom) == 0 && b - a == nom.size())
            return true;
        debut = fin + 1;
    }
    return false;
}

// ------------------------------------------------------------------ source MySQL
//
// Transport retenu pour D2 (SPEC § 3). Lecture ASYNCHRONE : le fil monde
// n'attend jamais MySQL ; il pose une requete toutes les BusIntervalleMs, et
// seulement si aucune n'est en vol, puis en recueille le resultat aux ticks
// suivants par son propre QueryCallbackProcessor (World::ProcessQueryCallbacks
// a un processeur prive, inutilisable ici).
//
// IDEMPOTENCE. `dernierId` en memoire : une ligne n'est livree qu'une fois dans
// la vie du processus -- tant qu'il n'y a qu'un emetteur, en serie (§ 3.4) ;
// sinon un AUTO_INCREMENT commite en retard serait saute, et le service compte
// ces `perdues`. Les accuses portent en plus leur propre garde de statut.
//
// LIGNES SAUTEES (revue D2). Meme avec le verrou d'emission, un INSERT dont le
// client a ete tue (delai, signal) peut etre commite APRES la liberation du
// verrou, donc apres un id plus grand deja lu. Sans rien de plus, cette ligne
// resterait `en_attente` pour toujours. Toutes les BALAYAGE_MS, le jeu accuse
// donc `expire` (raison `sautee`) les lignes `en_attente` d'id <= dernierId
// dont l'echeance est passee depuis plus de BALAYAGE_MARGE_S : une ligne livree
// est accusee au tick meme de sa livraison, bien avant cette marge. Le jeu reste
// seul ecrivain de `statut` (P-134). La ligne sautee n'est jamais executee.

class CoaDirSourceMysql : public CoaDirSource
{
public:
    bool Demarrer(uint32 epoqueMonde, bool bus) override
    {
        _dernierId = 0;
        _attenteMs = 0;
        _balayageMs = 0;
        _enVol = false;
        _pretes.clear();

        // Requete SYNCHRONE, admise au demarrage (modele : CoaPostes.h).
        QueryResult existe = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()"
            " AND TABLE_NAME IN ('coa_dir_commande', 'coa_dir_instance')");
        if (!existe || (*existe)[0].Get<uint64>() != 2)
        {
            LOG_INFO("playerbots", "coa directeur : table coa_dir_commande absente, bus inerte");
            return false;
        }
        // GARDE DE COLONNES (lot D3, SPEC-directeur-D3.md § 3.2, P-131). Les cinq
        // colonnes ET l'ENUM exact de `verbe` : tester `texte` seul laisserait
        // passer une migration a moitie appliquee, et l'egalite stricte de l'ENUM
        // refuse aussi une migration future que ce binaire ne connait pas (elle
        // se relit). Sans cette garde, un binaire D3 installe avant la migration
        // ferait echouer CHAQUE lecture du bus (colonne inconnue, rappel sans
        // ligne), treves comprises, et rien au journal du module. Requete
        // SYNCHRONE, au demarrage seulement ; relue au prochain demarrage.
        _colonnesD3 = false;
        _colonnesD4 = false;
        _colonnesR = false;
        _colonnesEv = false;
        _colonnesImprevu = false;
        _colonnesPaix = false;
        QueryResult colonnes = CharacterDatabase.Query(
            "SELECT (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()"
            " AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME IN ('texte','canal','valeur','intensite','delai_s')),"
            " IFNULL((SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()"
            " AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'verbe'), '')");
        uint64 nbColonnes = 0;
        std::string typeVerbe;
        if (colonnes)
        {
            nbColonnes = (*colonnes)[0].Get<uint64>();
            typeVerbe = (*colonnes)[1].Get<std::string>();
        }
        // Lot D4c (SPEC-directeur-D4.md § 3.1-3.2) : LE PIEGE DE L'EGALITE STRICTE.
        // La migration D4 ajoute `prime` et `chasser` a l'ENUM ; un binaire D3,
        // qui exige l'ENUM D3 exact, eteindrait alors tous ses verbes D3 a son
        // prochain demarrage. Ce binaire accepte donc les DEUX formes : ENUM D3
        // (D3 vrai, D4 faux) ; ENUM D4, ou tout ENUM qui COMMENCE par les douze
        // valeurs D4 dans l'ordre (migration future, D5 : sans quoi elle
        // referait le piege) -> D3 et D4 vrais. Il demarre sans risque avant
        // comme apres la migration.
        bool const d4 = nbColonnes == 5 && CommencePar(typeVerbe, ENUM_VERBE_D4);
        // Revue D4c (m3) : meme regle que le service (emetteur.enum_prolonge) --
        // l'ENUM D3 exact ou tout ENUM qui le prolonge.
        _colonnesD3 = nbColonnes == 5 && (CommencePar(typeVerbe, ENUM_VERBE_D3) || d4);
        _colonnesD4 = d4;
        // Lot R3 (03/10/2026) : meme regle, l'ENUM D4 prolonge de `recompenser`
        // (ou d'autres valeurs apres lui). L'ENUM D4 seul : `recompenser` inerte.
        _colonnesR = d4 && CommencePar(typeVerbe, ENUM_VERBE_R);
        // Lot C des evenements (04/10/2026) : l'ENUM R3 en tete ET la valeur 'evenement'
        // quelque part apres (« contient », ordre commun recompenser, pister, attirer,
        // soigner, evenement : un chantier livre dans le desordre n'eteint pas les autres).
        // Entre apostrophes : 'evenement_x' ne la contient pas.
        _colonnesEv = _colonnesR && typeVerbe.find(",'evenement'") != std::string::npos;
        // Lot D5 (15/10/2026) : meme regle « contient », les deux valeurs (lues en base le 15/10 :
        // ...,'recompenser','pister','attirer','soigner','evenement').
        _colonnesImprevu = _colonnesR && typeVerbe.find(",'attirer'") != std::string::npos &&
                           typeVerbe.find(",'soigner'") != std::string::npos;
        // Paix de zone (19/10/2026) : meme regle « contient », apres l'ENUM R3. Entre apostrophes : 'paix_x' ne la
        // contient pas.
        _colonnesPaix = _colonnesR && typeVerbe.find(",'paix'") != std::string::npos;
        if (_colonnesD3)
            LOG_INFO("playerbots", "coa directeur : colonnes D3 presentes, verbes D3 lisibles");
        else
            LOG_INFO("playerbots", "coa directeur : colonnes D3 absentes, verbes D3 inertes (colonnes={}/5, verbe={})",
                     nbColonnes, typeVerbe.empty() ? std::string("?") : typeVerbe);
        if (_colonnesD4)
            LOG_INFO("playerbots", "coa directeur : colonnes D4 presentes, verbes D4 lisibles");
        else
            LOG_INFO("playerbots", "coa directeur : colonnes D4 absentes, verbes D4 inertes (verbe={})",
                     typeVerbe.empty() ? std::string("?") : typeVerbe);
        if (_colonnesR)
            LOG_INFO("playerbots", "coa directeur : colonnes recompense presentes, verbe recompenser lisible");
        else
            LOG_INFO("playerbots", "coa directeur : colonnes recompense absentes, verbe recompenser inerte");
        if (_colonnesEv)
            LOG_INFO("playerbots", "coa directeur : colonnes evenement presentes, verbe evenement lisible");
        else
            LOG_INFO("playerbots", "coa directeur : colonnes evenement absentes, verbe evenement inerte");
        if (_colonnesImprevu)
            LOG_INFO("playerbots", "coa directeur : colonnes imprevu presentes, verbes attirer et soigner lisibles");
        else
            LOG_INFO("playerbots", "coa directeur : colonnes imprevu absentes, verbes attirer et soigner inertes");
        if (_colonnesPaix)
            LOG_INFO("playerbots", "coa directeur : colonnes paix presentes, verbe paix lisible");
        else
            LOG_INFO("playerbots", "coa directeur : colonnes paix absentes, verbe paix inerte");
        CharacterDatabase.Execute(
            "REPLACE INTO coa_dir_instance (id, epoque, demarre_le, bus) VALUES (1, {}, NOW(), {})",
            epoqueMonde, bus ? 1 : 0);
        return true;
    }

    void Vidanger(std::function<void(CoaDirCommande const&)> const& recevoir) override
    {
        _queries.ProcessReadyCallbacks();
        if (_pretes.empty())
            return;
        std::vector<CoaDirCommande> lot;
        lot.swap(_pretes);
        for (CoaDirCommande const& c : lot)
            recevoir(c);
    }

    void Sonder(uint32 diff, std::function<void(CoaDirCommande const&)> const& recevoir) override
    {
        Vidanger(recevoir);

        _balayageMs += diff;
        if (_balayageMs >= BALAYAGE_MS)
        {
            _balayageMs = 0;
            if (_dernierId)
                CharacterDatabase.Execute(
                    "UPDATE coa_dir_commande SET statut = 'expire', raison = 'sautee', vu_le = NOW(3)"
                    " WHERE statut = 'en_attente' AND id <= {} AND expire_le < NOW() - INTERVAL {} SECOND",
                    _dernierId, BALAYAGE_MARGE_S);
        }

        _attenteMs += diff;
        if (_enVol || _attenteMs < sPlayerbotAIConfig.directeurBusIntervalleMs)
            return;
        _attenteMs = 0;
        _enVol = true;

        // `origine` (colonne D2, NOT NULL) dans les DEUX formes : le refus
        // `occupe` en a besoin (§ 3.6). Les colonnes D3 seulement si la garde
        // les a vues : sinon, la forme D2 inchangee.
        std::string const sql = Acore::StringFormat(
            "SELECT id, verbe, cible, IFNULL(joueur, 0), duree_s, IFNULL(carte, 0), IFNULL(x, 0), IFNULL(y, 0),"
            " IFNULL(z, 0), IFNULL(rayon, 0), epoque_monde, IF(expire_le < NOW(), 1, 0), IFNULL(action, ''),"
            " origine{}"
            " FROM coa_dir_commande WHERE statut = 'en_attente' AND id > {} ORDER BY id LIMIT 32",
            _colonnesD3 ? ", IFNULL(texte, ''), IFNULL(canal, ''), IFNULL(valeur, 0), IFNULL(intensite, 0), delai_s" : "",
            _dernierId);
        bool const d3 = _colonnesD3;

        // `this` : l'objet est une statique de ce fichier, qui vit autant que
        // son processeur de rappels (membre).
        _queries.AddCallback(CharacterDatabase.AsyncQuery(sql).WithCallback(
            [this, d3](QueryResult r)
            {
                _enVol = false;
                if (!r)
                    return;
                do
                {
                    Field* f = r->Fetch();
                    CoaDirCommande c;
                    c.id = f[0].Get<uint32>();
                    c.verbe = VerbeDe(f[1].Get<std::string>());
                    c.cible = f[2].Get<uint32>();
                    c.joueur = f[3].Get<uint32>();
                    c.dureeS = f[4].Get<uint32>();
                    c.carte = f[5].Get<uint32>();
                    c.x = f[6].Get<float>();
                    c.y = f[7].Get<float>();
                    c.z = f[8].Get<float>();
                    c.rayon = f[9].Get<float>();
                    c.epoqueMonde = f[10].Get<uint32>();
                    c.expiree = f[11].Get<uint32>() != 0;
                    c.action = f[12].Get<std::string>();
                    c.origine = f[13].Get<std::string>();
                    if (d3)
                    {
                        c.texte = f[14].Get<std::string>();
                        c.canal = CanalDe(f[15].Get<std::string>());
                        c.valeur = f[16].Get<int32>();
                        c.intensite = f[17].Get<float>();
                        c.delaiS = f[18].Get<uint32>();
                    }
                    // Avance ICI, avant toute nouvelle requete : la suivante ne
                    // part qu'une fois _enVol retombe, donc apres ce rappel.
                    _dernierId = std::max(_dernierId, c.id);
                    _pretes.push_back(c);
                } while (r->NextRow());
            }));
    }

    void Acquitter(uint32 id, char const* statut, char const* raison) override
    {
        CharacterDatabase.Execute(
            "UPDATE coa_dir_commande SET statut = '{}', raison = {}, vu_le = NOW(3) WHERE id = {} AND statut = 'en_attente'",
            statut, Litteral(raison), id);
    }

    // Garde sur les DEUX statuts non finaux : avec plusieurs fils d'ecriture
    // asynchrone, cette ligne pourrait passer avant l'accuse `execute` du meme
    // id ; elle l'emporte alors quand meme (l'accuse ne trouve plus
    // `en_attente`). Un statut final n'est jamais ecrase.
    void Clore(uint32 id, char const* statut, char const* raison) override
    {
        CharacterDatabase.Execute(
            "UPDATE coa_dir_commande SET statut = '{}', raison = {}, vu_le = NOW(3) WHERE id = {}"
            " AND statut IN ('en_attente', 'execute')",
            statut, Litteral(raison), id);
    }

    void SignalerBus(bool bus) override
    {
        CharacterDatabase.Execute("UPDATE coa_dir_instance SET bus = {} WHERE id = 1", bus ? 1 : 0);
    }

    bool ColonnesD3() const override { return _colonnesD3; }
    bool ColonnesD4() const override { return _colonnesD4; }
    bool ColonnesRecompense() const override { return _colonnesR; }
    bool ColonnesEvenement() const override { return _colonnesEv; }
    bool ColonnesImprevu() const override { return _colonnesImprevu; }
    bool ColonnesPaix() const override { return _colonnesPaix; }

private:
    // Les raisons sont des constantes de ce fichier, jamais une saisie : pas
    // d'echappement a faire, seulement NULL ou une chaine entre apostrophes.
    static std::string Litteral(char const* raison)
    {
        return raison ? Acore::StringFormat("'{}'", raison) : std::string("NULL");
    }

    static constexpr uint32 BALAYAGE_MS = 60000;
    static constexpr uint32 BALAYAGE_MARGE_S = 60;
    // COLUMN_TYPE attendu, forme MESUREE pour l'ENUM D2 le 26/09 (SPEC § 3.2) :
    // minuscules, apostrophes, sans espace.
    static constexpr char const* ENUM_VERBE_D3 =
        "enum('treve','reaffecter','alleger','suspendre_mercenaire','lever','peupler','faire_parler','repondre',"
        "'heure_du_jour','meteo')";
    // Lot D4c : l'ENUM de la migration 2026_09_28_00_coa_directeur_d4.sql, meme
    // forme (minuscules, apostrophes, sans espace : celle que MySQL rend pour
    // l'ENUM D3, MESUREE le 28/09 dans information_schema).
    static constexpr char const* ENUM_VERBE_D4 =
        "enum('treve','reaffecter','alleger','suspendre_mercenaire','lever','peupler','faire_parler','repondre',"
        "'heure_du_jour','meteo','prime','chasser')";
    // Lot R3 (03/10/2026) : l'ENUM de la migration 2026_10_03_00_coa_directeur_recompense.sql.
    static constexpr char const* ENUM_VERBE_R =
        "enum('treve','reaffecter','alleger','suspendre_mercenaire','lever','peupler','faire_parler','repondre',"
        "'heure_du_jour','meteo','prime','chasser','recompenser')";

    // `type` est-il `attendu`, ou `attendu` prolonge de valeurs de plus ? Pour un
    // ENUM : meme debut sans la parenthese finale, puis `)` ou `,'`.
    static bool CommencePar(std::string const& type, char const* attendu)
    {
        std::string const tete(attendu, std::char_traits<char>::length(attendu) - 1);   // sans ')'
        if (type.size() < tete.size() + 1 || type.compare(0, tete.size(), tete) != 0)
            return false;
        if (type.compare(tete.size(), std::string::npos, ")") == 0)
            return true;
        return type.compare(tete.size(), 2, ",'") == 0 && type.back() == ')';
    }

    QueryCallbackProcessor _queries;
    std::vector<CoaDirCommande> _pretes;
    uint32 _dernierId = 0;
    uint32 _attenteMs = 0;
    uint32 _balayageMs = 0;
    bool _enVol = false;
    bool _colonnesD3 = false;
    bool _colonnesD4 = false;
    bool _colonnesR = false;
    bool _colonnesEv = false;
    bool _colonnesImprevu = false;
    bool _colonnesPaix = false;
};

// ------------------------------------------------------------------ bus

CoaDirSourceMysql sourceMysql;
CoaDirSource* source = nullptr;          // nul : bus inerte (table absente, ou pas demarre)
bool dernierEtatBus = false;
uint32 secondeMs = 0;
uint32 bilanMs = 0;
constexpr uint32 BILAN_MS = 5 * MINUTE * IN_MILLISECONDS;

// Seau a jetons, MaxCommandesParMinute. Fil monde seul.
double jetons = -1.0;
uint32 jetonsMs = 0;
// Revue 16/10 : seau PROPRE a `recompenser`, de meme capacite. Une part gagnee ne dispute plus ses jetons aux verbes de
// mise en scene (un gros evenement vidait le seau commun : la part etait refusee `budget` puis reemise des minutes plus
// tard), mais une boucle d'emission du service reste bornee (pas de paiement sans limite de debit).
double jetonsRecompense = -1.0;
uint32 jetonsRecompenseMs = 0;

bool PrendreJetonDe(double& seau, uint32& seauMs)
{
    double const capacite = sPlayerbotAIConfig.directeurMaxCommandesParMinute;
    uint32 const maintenant = getMSTime();
    if (seau < 0.0)
        seau = capacite;
    else
        seau = std::min(capacite, seau + getMSTimeDiff(seauMs, maintenant) * capacite / 60000.0);
    seauMs = maintenant;
    if (seau < 1.0)
        return false;
    seau -= 1.0;
    return true;
}

bool PrendreJeton()
{
    return PrendreJetonDe(jetons, jetonsMs);
}

void Journaliser(CoaDirCommande const& c, char const* statut, char const* raison)
{
    LOG_INFO("playerbots", "coa directeur cmd: id={} verbe={} cible={} joueur={} statut={} raison={}",
             c.id, NomVerbe(c.verbe), c.cible, c.joueur, statut, raison ? raison : "-");
}

void Accuser(CoaDirCommande const& c, char const* statut, char const* raison)
{
    Journaliser(c, statut, raison);
    if (source)
        source->Acquitter(c.id, statut, raison);
}

void Refuser(CoaDirCommande const& c, char const* raison)
{
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REFUSEES);
    Accuser(c, "refuse", raison);
}

// Statut final d'une consigne deja executee. Hors verrou : appele apres que
// le magasin a ete modifie.
void CloreConsigne(Treve const& t, char const* statut, char const* raison)
{
    LOG_INFO("playerbots", "coa directeur cmd: id={} verbe=treve cible={} joueur={} statut={} raison={}",
             t.id, t.cible, t.joueur, statut, raison ? raison : "-");
    if (source)
        source->Clore(t.id, statut, raison);
}

// Retire du magasin toute treve pour laquelle `lever` rend vrai, puis ecrit
// leur levee (statut `leve`). Rend le nombre de consignes levees.
template <typename Predicat>
size_t LeverSi(Predicat lever, char const* raison)
{
    if (!nbTreves.load(std::memory_order_relaxed))
        return 0;
    std::vector<Treve> levees;
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto it = treves.begin(); it != treves.end();)
        {
            if (lever(it->second))
            {
                levees.push_back(it->second);
                it = treves.erase(it);
            }
            else
                ++it;
        }
        nbTreves.store(static_cast<uint32>(treves.size()), std::memory_order_relaxed);
    }
    for (Treve const& t : levees)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        CloreConsigne(t, "leve", raison);
    }
    return levees.size();
}

// Fin NATURELLE : rien n'est ecrit en base, le service la connait
// (vu_le + duree_s) ; le bilan la compte (`echues`).
void LeverEchues()
{
    if (!nbTreves.load(std::memory_order_relaxed))
        return;
    uint32 const maintenant = Maintenant();
    std::unique_lock<std::shared_mutex> garde(verrouConsignes);
    for (auto it = treves.begin(); it != treves.end();)
    {
        if (it->second.finS <= maintenant)
        {
            it = treves.erase(it);
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ECHUES);
        }
        else
            ++it;
    }
    nbTreves.store(static_cast<uint32>(treves.size()), std::memory_order_relaxed);
}

// ------------------------------------------------------------------ paix de zone (19/10/2026)
//
// Fil monde seul (voir le magasin `paix`, plus haut). Une case est ECRITE avant que nbPaix ne la compte, et VIDEE avant
// que nbPaix ne la decompte ; un lecteur qui la verrait encore pleine apres la levee lirait une paix de plus pendant
// quelques instructions, jamais une case a moitie ecrite.

void PublierNbPaix()
{
    nbPaix.store(static_cast<uint32>(paix.size()), std::memory_order_release);
}

void CloreConsigne(Paix const& z, char const* statut, char const* raison)
{
    LOG_INFO("playerbots", "coa directeur cmd: id={} verbe=paix cible={} joueur=0 statut={} raison={}",
             z.id, z.zone, statut, raison ? raison : "-");
    if (source)
        source->Clore(z.id, statut, raison);
}

// Retire du magasin toute paix pour laquelle `lever` rend vrai, puis ecrit leur levee (statut `leve`). -> nombre.
template <typename Predicat>
size_t LeverPaixSi(Predicat lever, char const* raison)
{
    if (paix.empty())
        return 0;
    std::vector<Paix> levees;
    for (auto it = paix.begin(); it != paix.end();)
    {
        if (lever(it->second))
        {
            casesPaix[it->second.kase].store(0, std::memory_order_release);
            levees.push_back(it->second);
            it = paix.erase(it);
        }
        else
            ++it;
    }
    PublierNbPaix();
    for (Paix const& z : levees)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        CloreConsigne(z, "leve", raison);
        LOG_INFO("playerbots", "coa directeur paix: fin zone={} carte={} id={} raison={}", z.zone, z.carte, z.id, raison);
    }
    return levees.size();
}

// Fin NATURELLE : rien n'est ecrit en base (le service la connait : vu_le + duree_s) ; une ligne au journal du module
// et le bilan la compte (`paix_echues`).
void LeverPaixEchues()
{
    if (paix.empty())
        return;
    uint32 const maintenant = Maintenant();
    bool change = false;
    for (auto it = paix.begin(); it != paix.end();)
    {
        if (it->second.finS <= maintenant)
        {
            casesPaix[it->second.kase].store(0, std::memory_order_release);
            LOG_INFO("playerbots", "coa directeur paix: fin zone={} carte={} id={} raison=echue", it->second.zone,
                     it->second.carte, it->second.id);
            it = paix.erase(it);
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PAIX_ECHUES);
            change = true;
        }
        else
            ++it;
    }
    if (change)
        PublierNbPaix();
}

// `paix=<actives> paix_posees= paix_echues= ecartes_paix= ffa_retires_paix=`, EN FIN de la ligne bilan.
std::string BilanPaix()
{
    return Acore::StringFormat(" paix={} paix_posees={} paix_echues={} ecartes_paix={} ffa_retires_paix={}",
                               paix.size(), Lire(CoaDirecteur::COA_DIR_PAIX_POSEES),
                               Lire(CoaDirecteur::COA_DIR_PAIX_ECHUES), Lire(CoaDirecteur::COA_DIR_ECARTE_PAIX),
                               Lire(CoaDirecteur::COA_DIR_FFA_RETIRES_PAIX));
}

// ------------------------------------------------------------------ suspensions (D2c)
//
// SPEC-directeur-D2.md § 5. Le magasin (suspensions, plus haut) n'a qu'un
// ecrivain, le fil monde. La file de reconstruction et la surveillance du
// filet ne sont touchees QUE par le fil monde : aucun verrou.
//
// LA REGLE QUI TIENT TOUT : la composition des strategies n'appartient qu'a la
// fabrique (AiFactory), qui LIT MercenaireEnRpg. Ce fichier ne fait qu'appeler
// ResetStrategies, l'API de reconstruction du proprietaire, deja employee par
// RandomPlayerbotMgr::Refresh depuis ce meme fil. Jamais addStrategy par nom
// (P-130), jamais le drapeau FFA, jamais IsMercenary.
//
// PIEGE DE VERROU : ResetStrategies rappelle la fabrique, qui prend le verrou
// PARTAGE de verrouConsignes (MercenaireEnRpg). Ne JAMAIS l'appeler en tenant
// le verrou exclusif : interblocage sur le fil monde, donc serveur fige.

// Le bot est-il suspendu EN MODE RPG par `suspendre_mercenaire` ? C'est l'ancien
// MercenaireEnRpg, avant le lot D4c : la question que posent les controles et le
// suivi des AFFECTATIONS (fin `fin_suspension`, refus `mercenaire` de peupler,
// drapeau mercenaireRpg). La fabrique, elle, lit CoaDirecteur::MercenaireEnRpg,
// qui y ajoute le chasseur en trajet : confondre les deux ferait lever
// `fin_suspension` un chasseur a son arrivee.
bool SuspenduEnRpg(uint32 guidLow)
{
    if (!nbSuspensions.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = suspensions.find(guidLow);
    return it != suspensions.end() && it->second.rpg && it->second.finS > Maintenant();
}

// Lot D4c : le bot porte-t-il une affectation `chasser` ACTIVE et EN TRAJET ?
// C'est ce qui le fait composer en mode RPG (CoaDirecteur::MercenaireEnRpg).
// Sans chasseur : un chargement atomique.
bool ChasseurEnTrajet(uint32 guidLow)
{
    if (!nbChasseurs.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = affectations.find(guidLow);
    // Reserve de chasseurs (02/10) : un chasseur de RESERVE n'est jamais compose
    // en mode RPG -- il garde sa chasse en trajet, pour traquer des
    // l'atterrissage de son saut (CONCEPTION-reserve-chasseurs.md § 2.3).
    return it != affectations.end() && it->second.a.chasseur && !it->second.a.reserve &&
           it->second.a.phase == CoaDirecteur::CoaDirPhase::Trajet && it->second.a.finS > Maintenant();
}

constexpr uint32 FILET_MS = 30 * IN_MILLISECONDS;
constexpr uint32 FILET_LEVEE_S = 5 * MINUTE;       // surveillance apres une levee
constexpr size_t RECONSTRUCTIONS_PAR_SECONDE = 32;

std::unordered_set<uint32> aReconstruire;            // guid bas
std::unordered_map<uint32, uint32> surveillesJusqua; // guid -> fin de surveillance (s)
std::unordered_set<uint32> filetFait;                // le filet a deja relance ce guid
uint32 filetMs = 0;

// Lot D4c : la composition d'un CHASSEUR change (pose en trajet : mode RPG ;
// arrivee ou fin en trajet : retour a la chasse). La fabrique la relit par
// ResetStrategies, depuis la file (Reconstruire, fil monde, jamais sous le
// verrou exclusif), et le filet surveille la bascule 5 min. Fil monde seul.
void BasculerComposition(uint32 guidLow)
{
    aReconstruire.insert(guidLow);
    surveillesJusqua[guidLow] = Maintenant() + FILET_LEVEE_S;
    filetFait.erase(guidLow);
}

// Cartes ou un bot aleatoire joue : continents et Outreterre, Norfendre. Les
// cartes de banc (§ 6.3) et les instances sont exclues.
bool CarteDeSuspension(uint32 carte)
{
    return carte == 0 || carte == 1 || carte == 530 || carte == 571;
}

Player* JoueurConnecte(uint32 guidLow)
{
    // FindConnectedPlayer et non FindPlayer : un bot en teleportation lointaine
    // n'est pas IsInWorld, mais il n'est pas hors ligne pour autant. Le
    // prendre pour hors ligne le retirerait de la file sans reconstruction, et
    // aucune connexion ne viendrait la refaire.
    return ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guidLow));
}

// Ligne de fin, mesure du § 5.5 : `coa directeur suspension:`. Les compteurs
// de rpgStatistic sont lus sur le fil monde, cartes a l'arret (P-137). NA si
// le bot est hors ligne ou s'est reconnecte (autre moteur).
void JournaliserFinSuspension(Suspension const& s, char const* motif)
{
    Player* bot = JoueurConnecte(s.guid);
    PlayerbotAI* ia = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    std::string acceptees = "NA", rendues = "NA";
    if (ia && ia == s.ia && ia->rpgStatistic.questAccepted >= s.quetesAcceptees0 &&
        ia->rpgStatistic.questRewarded >= s.quetesRendues0)
    {
        acceptees = std::to_string(ia->rpgStatistic.questAccepted - s.quetesAcceptees0);
        rendues = std::to_string(ia->rpgStatistic.questRewarded - s.quetesRendues0);
    }
    LOG_INFO("playerbots",
             "coa directeur suspension: id={} guid={} mode={} reconstruit={} quetes_acceptees={} quetes_rendues={}"
             " motif={}",
             s.id, s.guid, s.rpg ? 1 : 0, s.reconstruit ? 1 : 0, acceptees, rendues, motif);
}

// Fin d'une suspension retiree du magasin (hors verrou). `statut` nul : fin
// naturelle, rien n'est ecrit en base (le service la connait). `reconstruire` :
// la composition doit revenir a celle de mercenaire.
void FinirSuspension(Suspension const& s, char const* statut, char const* raison, char const* motif,
                     bool reconstruire)
{
    JournaliserFinSuspension(s, motif);
    if (statut)
    {
        LOG_INFO("playerbots",
                 "coa directeur cmd: id={} verbe=suspendre_mercenaire cible={} joueur={} statut={} raison={}", s.id,
                 s.guid, s.joueur, statut, raison ? raison : "-");
        if (source)
            source->Clore(s.id, statut, raison);
    }
    if (reconstruire)
    {
        aReconstruire.insert(s.guid);
        surveillesJusqua[s.guid] = Maintenant() + FILET_LEVEE_S;
        filetFait.erase(s.guid);   // nouvelle attente : le filet a de nouveau droit a un essai
    }
}

// Retire du magasin toute suspension pour laquelle `lever` rend vrai, puis
// ecrit leur levee (statut `leve`). Rend le nombre de consignes levees.
template <typename Predicat>
size_t LeverSuspensionsSi(Predicat lever, char const* raison)
{
    if (!nbSuspensions.load(std::memory_order_relaxed))
        return 0;
    std::vector<Suspension> levees;
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto it = suspensions.begin(); it != suspensions.end();)
        {
            if (lever(it->second))
            {
                levees.push_back(it->second);
                it = suspensions.erase(it);
            }
            else
                ++it;
        }
        nbSuspensions.store(static_cast<uint32>(suspensions.size()), std::memory_order_relaxed);
    }
    for (Suspension const& s : levees)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        FinirSuspension(s, "leve", raison, raison, s.rpg);
    }
    return levees.size();
}

// Fin NATURELLE (echeance) : rien en base, compte `echues`.
void LeverSuspensionsEchues()
{
    if (!nbSuspensions.load(std::memory_order_relaxed))
        return;
    uint32 const maintenant = Maintenant();
    std::vector<Suspension> echues;
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto it = suspensions.begin(); it != suspensions.end();)
        {
            if (it->second.finS <= maintenant)
            {
                echues.push_back(it->second);
                it = suspensions.erase(it);
            }
            else
                ++it;
        }
        nbSuspensions.store(static_cast<uint32>(suspensions.size()), std::memory_order_relaxed);
    }
    for (Suspension const& s : echues)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ECHUES);
        FinirSuspension(s, nullptr, nullptr, "echue", s.rpg);
    }
}

// Un humain a pris le bot APRES la pose (invitation) : la consigne est levee,
// raison `maitre_humain`, le controle de la pose (§ 5.4) valant pour toute la
// duree. Sans cela, la garde de PossibleTargetsValue continuerait jusqu'a
// 30 min de lui interdire toute convoitise de joueur alors qu'il suit un
// maitre. Chaque seconde, fil monde, cartes a l'arret (P-137) : au plus
// MaxSuspensions recherches. Un bot hors ligne est laisse tel quel (§ 5.2 : sa
// reconnexion rend le mode RPG, sa consigne garde donc sa place au plafond
// jusqu'a l'echeance, c'est voulu). La reconstruction que FinirSuspension met
// en file pour un mode RPG en sort aussitot (Reconstruire : maitre humain),
// sans ecraser les strategies du maitre.
void LeverSuspensionsMaitreHumain()
{
    if (!nbSuspensions.load(std::memory_order_relaxed))
        return;
    std::vector<uint32> guids;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        guids.reserve(suspensions.size());
        for (auto const& [guid, s] : suspensions)
            guids.push_back(guid);
    }
    std::unordered_set<uint32> pris;
    for (uint32 guid : guids)
    {
        Player* bot = JoueurConnecte(guid);
        PlayerbotAI* ia = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (ia && ia->HasGameClientMaster())
            pris.insert(guid);
    }
    if (!pris.empty())
        LeverSuspensionsSi([&pris](Suspension const& s) { return pris.count(s.guid) != 0; }, "maitre_humain");
}

// La composition RPG de ce guid est etablie : on le note sur la consigne
// active, s'il y en a une en mode RPG (ligne de fin, `reconstruit=`). Deux
// appelants : la file (Reconstruire), et le filet quand il CONSTATE « new rpg »
// sur un suspendu RPG -- cas d'une reconnexion pendant la suspension, ou la
// fabrique a compose le mode RPG a la connexion sans passer par la file. Une
// ligne `reconstruit=1` avec des quetes `NA` signale donc une reconnexion : les
// compteurs rpgStatistic du nouveau moteur repartent de zero, l'ecart n'est pas
// mesurable. `reconstruit=0` en mode 1 : composition RPG jamais etablie ni
// constatee (bot hors ligne, differe jusqu'a l'echeance, hors branche
// mercenaire, ou suspension trop courte pour que le filet de 30 s passe).
void MarquerReconstruit(uint32 guidLow)
{
    std::unique_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = suspensions.find(guidLow);
    if (it != suspensions.end() && it->second.rpg)
        it->second.reconstruit = true;
}

// File de reconstruction, chaque seconde (§ 5.3). Un bot hors ligne en sort
// sans reconstruction (la connexion relit la consigne) ; un mort, un bot en
// combat, en vol, en teleportation ou en champ de bataille y RESTE : un bot
// qui se releve par ses propres actions (retour au corps) ne reconstruit
// rien, c'est donc a nous de le faire quand il redevient disponible.
void Reconstruire()
{
    size_t faites = 0;
    for (auto it = aReconstruire.begin(); it != aReconstruire.end();)
    {
        uint32 const guid = *it;
        Player* bot = JoueurConnecte(guid);
        PlayerbotAI* ia = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!bot || !ia)
        {
            it = aReconstruire.erase(it);
            continue;
        }
        // Un maitre humain a pris le bot entre-temps : ses strategies sont les
        // siennes (co/nc poses a la main), on ne les ecrase pas. L'invitation
        // a deja reconstruit (AcceptInvitationAction), en lisant la consigne.
        if (ia->HasGameClientMaster())
        {
            it = aReconstruire.erase(it);
            continue;
        }
        if (faites >= RECONSTRUCTIONS_PAR_SECONDE || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() ||
            bot->IsInFlight() || bot->IsBeingTeleported() || bot->InBattleground())
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_RECONSTRUCTIONS_DIFFEREES);
            ++it;
            continue;
        }
        // Hors de tout verrou (voir PIEGE DE VERROU plus haut).
        ia->ResetStrategies();
        ++faites;
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_RECONSTRUCTIONS);
        it = aReconstruire.erase(it);
        MarquerReconstruit(guid);
    }
}

// La fabrique rend-elle, pour ce bot, la branche ou le mode RPG change quelque
// chose ? Recopie des conditions d'AiFactory::AddDefaultNonCombatStrategies
// (bot aleatoire hors champ de bataille, seul ou chef de groupe, puis le
// predicat mercenaireSansPve sans son terme D2c, puis new rpg allume). Hors de
// cette branche, « new rpg » ne dit rien du mode : on ne compare pas.
bool BrancheMercenaire(Player* bot)
{
    if (!sPlayerbotAIConfig.enableNewRpgStrategy || !sPlayerbotAIConfig.wildPvpEnabled ||
        !sPlayerbotAIConfig.wildPvpMercenariesSkipPve || bot->GetLevel() < sPlayerbotAIConfig.wildPvpMinLevel ||
        !sPlayerbotAIConfig.IsMercenary(bot->GetGUID().GetRawValue()) || !sRandomPlayerbotMgr.IsRandomBot(bot) ||
        bot->InBattleground())
        return false;
    Group* groupe = bot->GetGroup();
    return !groupe || groupe->GetLeaderGUID() == bot->GetGUID();
}

// Filet, toutes les 30 s (§ 5.3). Pour chaque suspension active en mode RPG et
// chaque guid leve depuis moins de 5 min : la composition doit porter « new
// rpg » si et seulement si MercenaireEnRpg. Divergence : une reconstruction de
// plus, UNE SEULE par attente (filetFait), pour qu'une divergence que la
// reconstruction ne corrige pas -- une conf qui ajoute ou retire « new rpg »
// par RandomBotNonCombatStrategies, par exemple -- ne relance pas
// ResetStrategies toutes les 30 s a vie.
void Filet()
{
    uint32 const maintenant = Maintenant();
    std::vector<uint32> candidats;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto const& [guid, s] : suspensions)
            if (s.rpg)
                candidats.push_back(guid);
    }
    for (auto it = surveillesJusqua.begin(); it != surveillesJusqua.end();)
    {
        if (it->second <= maintenant)
        {
            filetFait.erase(it->first);
            it = surveillesJusqua.erase(it);
        }
        else
        {
            candidats.push_back(it->first);
            ++it;
        }
    }
    std::sort(candidats.begin(), candidats.end());
    candidats.erase(std::unique(candidats.begin(), candidats.end()), candidats.end());

    for (uint32 guid : candidats)
    {
        // En file : la divergence est attendue, la reconstruction arrive.
        if (aReconstruire.count(guid) || filetFait.count(guid))
            continue;
        Player* bot = ObjectAccessor::FindPlayerByLowGUID(guid);
        PlayerbotAI* ia = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!ia || !bot->IsAlive() || bot->IsInCombat() || ia->HasGameClientMaster() || !BrancheMercenaire(bot))
            continue;
        bool const attendu = CoaDirecteur::MercenaireEnRpg(guid);
        bool const present = ia->HasStrategy("new rpg", BOT_STATE_NON_COMBAT);
        if (attendu == present)
        {
            // Composition RPG constatee sur un suspendu (reconnexion : la
            // fabrique l'a composee sans la file). Sans effet si deja notee.
            if (attendu)
                MarquerReconstruit(guid);
            continue;
        }
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_DIVERGENCES);
        LOG_INFO("playerbots", "coa directeur : divergence de composition guid={} attendu_rpg={} new_rpg={}", guid,
                 attendu ? 1 : 0, present ? 1 : 0);
        aReconstruire.insert(guid);
        filetFait.insert(guid);
    }
    // Plus aucun candidat ne justifie une marque qui ne serait plus relue.
    if (candidats.empty())
        filetFait.clear();
}

// ------------------------------------------------------------------ affectations (D2b)
//
// SPEC-directeur-D2.md § 6. Le magasin (affectations, plus haut) n'a qu'un
// ecrivain, le fil monde : la reception (Recevoir), le suivi de chaque seconde
// (SuivreAffectations), les levees (lever, stop, purge). Les decideurs
// existants -- teleportation aleatoire, new rpg, chasse -- LISENT la consigne
// par AffectationDe ; rien de playerbots ni du coeur n'est ecrit d'ici (P-134),
// aucune strategie n'est ajoutee ni retiree (P-130).

constexpr uint32 AFFECTATION_DUREE_MIN_S = 60;
constexpr uint32 AFFECTATION_DUREE_MAX_S = 3600;
constexpr float AFFECTATION_RAYON_MIN = 40.0f;
constexpr float AFFECTATION_RAYON_MAX = 300.0f;
// Sous CoaChasseAction::DistanceMax (1 500, CoaChasse.h) : c'est le CENTRE qui
// reste « encore valable » vu du bot au depart, pas tout le disque. Un point du
// disque peut etre a 1 400 + rayon (jusqu'a 1 700 yd) : la chasse ne tire donc
// que les points a DistanceMax du bot (TirerDansLeDisque, argument `depuis`),
// et le centre en repli.
constexpr float AFFECTATION_DISTANCE_MAX = 1400.0f;
constexpr float AFFECTATION_HUMAIN_YD = 200.0f;
constexpr int TIRAGES_DISQUE = 6;

// Sol sec au point (x, y), regle de P-132 : partir d'au-dessus du terrain brut.
// PIEGE : GetGridHeight cree la grille si elle n'existe pas (Map.cpp:1108) --
// c'est la meme lecture que `lieux` et que CoaChasse, bornee par les appelants.
bool SolSec(Map* map, float x, float y, float z, float& sol)
{
    if (!map || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !Acore::IsValidMapCoord(x, y))
        return false;
    float const depart = std::max(z, map->GetGridHeight(x, y));
    float const h = map->GetHeight(x, y, depart);
    if (!(h > INVALID_HEIGHT))
        return false;
    if (map->IsInWater(PHASEMASK_NORMAL, x, y, h, DEFAULT_COLLISION_HEIGHT))
        return false;
    sol = h;
    return true;
}

// Laisse de hauteur (P-145), partagee par la pose (ExecuterAffectation) et le
// tirage (TirerDansLeDisque) : voir le commentaire de TirerDansLeDisque.
constexpr float TIRAGE_ECART_Z = 6.0f;
constexpr float TIRAGE_PENTE_MAX = 1.0f;       // 45 degres, en moyenne depuis le centre
constexpr float TIRAGE_SONDE_HAUTE = DEFAULT_HEIGHT_SEARCH;   // 50 : la portee du rayon vmap
constexpr float TIRAGE_TOIT_YD = 2.0f;
// Convention du service : `z` inconnu = -5000 (Z_INCONNU, destinations.py et
// itineraire.py, destinations de quete). Aucun sol du monde ouvert n'est si bas.
constexpr float POSE_Z_INCONNU_MAX = -4000.0f;

// Sonde de toit (P-145) : une surface existe-t-elle au-dessus de `sol`, a plus
// de TIRAGE_TOIT_YD ? Le rayon part de max(sol, zRef) + 50 et descend jusqu'a
// `sol` : un plancher plus haut que zRef (le centre) est vu meme si `sol` est
// le terrain brut 33 yd plus bas (Orgrimmar), ce que sol + 50 ratait au-dela de
// 50 yd. Sans aucune surface lue (ni grille ni vmap) : toit suppose. PIEGE :
// tout modele a collision compte, un arbre compris -- c'est le prix du refus.
bool ToitAuDessus(Map* map, float x, float y, float sol, float zRef)
{
    float const depart = std::max(sol, zRef) + TIRAGE_SONDE_HAUTE;
    float const haut = map->GetHeight(x, y, depart, true, depart - sol);
    return !(haut > INVALID_HEIGHT) || haut > sol + TIRAGE_TOIT_YD;
}

char const* NomPhase(CoaDirecteur::CoaDirPhase p)
{
    return p == CoaDirecteur::CoaDirPhase::Sejour ? "sejour" : "trajet";
}

// Ligne de mesure du § 6.5 : `coa directeur sejour:`, a la fin de l'affectation
// ET une fois RELEVE_SEJOUR_S apres l'arrivee (motif `arrivee_5min`, l'affectation
// continue). Ecarts de quetes DEPUIS L'ARRIVEE ; NA si le bot n'est jamais
// arrive, est hors ligne ou s'est reconnecte (autre moteur, compteurs repartis
// de zero). Fil monde (P-137).
constexpr uint32 RELEVE_SEJOUR_S = 300;

void JournaliserFinAffectation(AffectationInterne const& f, char const* motif)
{
    Player* bot = JoueurConnecte(f.guid);
    PlayerbotAI* ia = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    std::string acceptees = "NA", rendues = "NA";
    if (f.a.phase == CoaDirecteur::CoaDirPhase::Sejour && ia && ia == f.ia &&
        ia->rpgStatistic.questAccepted >= f.quetesAcceptees0 && ia->rpgStatistic.questRewarded >= f.quetesRendues0)
    {
        acceptees = std::to_string(ia->rpgStatistic.questAccepted - f.quetesAcceptees0);
        rendues = std::to_string(ia->rpgStatistic.questRewarded - f.quetesRendues0);
    }
    LOG_INFO("playerbots",
             "coa directeur sejour: id={} guid={} phase={} quetes_acceptees={} quetes_rendues={} motif={}", f.a.id,
             f.guid, NomPhase(f.a.phase), acceptees, rendues, motif);
}

// Fin d'une affectation retiree du magasin (hors verrou). `statut` nul : fin
// naturelle, rien n'est ecrit en base (le service la connait).
void FinirAffectation(AffectationInterne const& f, char const* statut, char const* raison, char const* motif)
{
    JournaliserFinAffectation(f, motif);
    // Toute fin EN TRAJET (lever, remplace, echue, abandon, mort, stop, bus
    // eteint...) : l'IA du bot abandonnera le point tire pour ce disque. Fil
    // monde, verrou non tenu par les appelants (hors verrou, voir plus haut).
    if (f.a.phase == CoaDirecteur::CoaDirPhase::Trajet)
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        trajetsLeves[f.guid] = TrajetLeve{f.a.id, f.a.carte, f.a.x, f.a.y, f.a.rayon, Maintenant() + TRAJET_LEVE_S,
                                     getMSTime()};
        nbTrajetsLeves.store(static_cast<uint32>(trajetsLeves.size()), std::memory_order_relaxed);
    }
    // Lot D4c : un chasseur qui finit EN TRAJET etait compose en mode RPG. S'il
    // n'en porte plus d'autre en trajet (la continuation du meme parcours vient
    // d'etre ecrite AVANT cet appel), la fabrique doit lui rendre sa chasse.
    if (f.a.chasseur && f.a.phase == CoaDirecteur::CoaDirPhase::Trajet && !ChasseurEnTrajet(f.guid))
        BasculerComposition(f.guid);
    // Reserve de chasseurs (02/10, revue M2) : un chasseur qui n'en est plus un
    // (aucune autre affectation `chasser` active ecrite avant cet appel) perd sa
    // strategie de combat « coa chasseur prime » a la recomposition. Sans elle,
    // rien de grave : ses actions par defaut relisent ChasseurDe et se taisent.
    else if (f.a.chasseur && !CoaDirecteur::ChasseurDe(f.guid))
        BasculerComposition(f.guid);
    // Retraite (03/10/2026) : un chasseur de RESERVE dont l'affectation tombe a la fin
    // de sa prime (`fin_prime`) ou a sa mort (`mort` : il se relevera pres du prime)
    // quitte la scene. Les autres fins (remplace, lever du service, echeance...) : rien
    // de neuf ; la fin de la prime le reprendra (Prime::servis).
    if (f.a.chasseur && f.a.reserve && motif &&
        (std::strcmp(motif, "fin_prime") == 0 || std::strcmp(motif, "mort") == 0))
    {
        uint32 prime = 0;
        {
            std::shared_lock<std::shared_mutex> garde(verrouConsignes);
            auto it = primes.find(f.joueur);
            if (it != primes.end())
                prime = it->second.id;
        }
        CommencerRetraite(f.guid, prime, f.joueur, motif);
    }
    if (statut)
    {
        LOG_INFO("playerbots", "coa directeur cmd: id={} verbe={} cible={} joueur={} statut={} raison={}", f.a.id,
                 NomVerbe(f.verbe), f.guid, f.joueur, statut, raison ? raison : "-");
        if (source)
            source->Clore(f.a.id, statut, raison);
    }
}

// Retire du magasin toute affectation pour laquelle `lever` rend vrai, puis
// ecrit leur levee (statut `leve`). Rend le nombre de consignes levees.
template <typename Predicat>
size_t LeverAffectationsSi(Predicat lever, char const* raison)
{
    if (!nbAffectations.load(std::memory_order_relaxed))
        return 0;
    std::vector<AffectationInterne> levees;
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto it = affectations.begin(); it != affectations.end();)
        {
            if (lever(it->second))
            {
                levees.push_back(it->second);
                it = affectations.erase(it);
            }
            else
                ++it;
        }
        MajCompteursAffectations();
    }
    for (AffectationInterne const& f : levees)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        FinirAffectation(f, "leve", raison, raison);
    }
    return levees.size();
}

// La prime du joueur `joueurLow` court-elle ? Tout fil (verrou partage).
bool PrimeCourt(uint32 joueurLow, uint32 maintenant)
{
    if (!joueurLow || !nbPrimes.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = primes.find(joueurLow);
    return it != primes.end() && it->second.finS > maintenant;
}

// Vrai groupe (09/10/2026, CoaDirecteurGroupe.inc) : le bot est-il dans le groupe pose pour l'evenement de ce joueur ?
// Fil monde seul. Declaree ici : SuivreAffectations la lit, le fragment la definit plus bas.
bool GroupeDEvenementPour(uint32 botLow, uint32 joueurLow);

// Suivi des phases, chaque seconde, fil monde, cartes a l'arret (§ 6.2). Au plus
// MaxAffectations recherches. Trois temps : copie sous verrou partage, examen
// des joueurs HORS verrou (MercenaireEnRpg prend lui-meme le verrou partage),
// application sous verrou exclusif, puis journal et accuses hors verrou.
void SuivreAffectations()
{
    if (!nbAffectations.load(std::memory_order_relaxed))
        return;
    uint32 const maintenant = Maintenant();

    std::vector<AffectationInterne> copies;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        copies.reserve(affectations.size());
        for (auto const& [guid, f] : affectations)
            copies.push_back(f);
    }

    enum class Issue : uint8 { Arrivee, Levee, Echue, Niveau, Releve, EtapeArrivee, Centre, Mission };
    struct Decision
    {
        uint32 guid, id;
        Issue issue;
        char const* raison;
        CoaDirecteur::CoaDirCompteur compteur;   // COA_DIR_NB_COMPTEURS : aucun de plus
        PlayerbotAI const* ia;
        uint32 acceptees, rendues;
        uint8 niveau = 0;                        // Issue::Niveau : le niveau a retenir
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;   // Issue::Centre : la position du joueur
    };
    std::vector<Decision> decisions;
    auto lever = [&decisions](AffectationInterne const& f, char const* raison, CoaDirecteur::CoaDirCompteur compteur)
    { decisions.push_back(Decision{f.guid, f.a.id, Issue::Levee, raison, compteur, nullptr, 0, 0}); };

    uint32 const trajetMax = sPlayerbotAIConfig.directeurTrajetMaxSecondes;
    bool const releveCimetiere = sPlayerbotAIConfig.directeurReleveCimetiere;
    uint32 const releveS = sPlayerbotAIConfig.directeurReleveSecondes;
    std::vector<MortTrajet> morts;
    for (AffectationInterne const& f : copies)
    {
        if (f.a.finS <= maintenant)
        {
            decisions.push_back(Decision{f.guid, f.a.id, Issue::Echue, nullptr, CoaDirecteur::COA_DIR_NB_COMPTEURS,
                                         nullptr, 0, 0});
            continue;
        }
        // FindConnectedPlayer : un bot en teleportation lointaine n'est pas
        // IsInWorld, mais il n'est pas hors ligne (voir JoueurConnecte).
        Player* bot = JoueurConnecte(f.guid);
        PlayerbotAI* ia = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!bot || !ia)
        {
            lever(f, "hors_ligne", CoaDirecteur::COA_DIR_AFFECTATIONS_LEVEES_HORS_LIGNE);
            continue;
        }
        if (!bot->IsInWorld() || bot->IsBeingTeleported())
            continue;   // on repassera dans une seconde
        // Mort, en trajet comme en sejour : sans cette levee la garde de
        // RandomTeleport le garderait au cimetiere apres Revive (P-139).
        if (!bot->IsAlive())
        {
            lever(f, "mort", CoaDirecteur::COA_DIR_AFFECTATIONS_LEVEES_MORT);
            // Relève au cimetiere (lot D2b, suite) : SEULEMENT une mort en
            // TRAJET (etape comprise), sur la carte du disque, hors champ de
            // bataille et arene (leur relachement a ses propres regles). Le
            // lieu retenu est celui du cadavre s'il existe deja (le fantome a
            // pu etre teleporte au cimetiere avant ce passage), sinon le bot,
            // qui n'a pas encore relache. Fil monde, cartes a l'arret (P-137).
            if (releveCimetiere && f.a.phase == CoaDirecteur::CoaDirPhase::Trajet &&
                bot->GetMapId() == f.a.carte && !bot->InBattleground() && !bot->InArena())
            {
                MortTrajet m{f.a.id, f.guid, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), maintenant,
                             maintenant + releveS};
                if (Corpse* corps = bot->GetCorpse())
                {
                    m.carte = corps->GetMapId();
                    m.x = corps->GetPositionX();
                    m.y = corps->GetPositionY();
                }
                morts.push_back(m);
            }
            continue;
        }
        // Un humain l'a pris apres la pose : c'est son maitre qui le mene.
        // Vrai groupe (09/10) : SAUF l'inscrit que le Directeur a mis dans le groupe du joueur servi, et dont ce
        // joueur est devenu le maitre (UpdateAIGroupMaster) : il sert toujours l'evenement, et son INSCRIPTION
        // (l'affectation : assaillants d'evenement, soins, accuses du service) doit tenir jusqu'a la fin. Son
        // COMPORTEMENT, lui, change (AiFactory, LU) : un bot aleatoire groupe sans etre chef n'a plus « grind » ni
        // « new rpg » -- ni CibleDEvenement, ni laisse, ni ralliement ; il suit le joueur (« follow ») et engage par
        // « dps assist » / « tank assist », qui lisent AssaillantsDEvenement (memes bornes que CibleDEvenement).
        // EVENEMENTS-groupe.md § 4.
        if (ia->HasGameClientMaster() &&
            !(f.a.presence && f.a.mission == CoaDirecteur::CoaDirMission::Evenement &&
              GroupeDEvenementPour(f.guid, f.a.missionParam) && ia->GetMaster() &&
              ia->GetMaster()->GetGUID().GetCounter() == f.a.missionParam))
        {
            lever(f, "maitre_humain", CoaDirecteur::COA_DIR_NB_COMPTEURS);
            continue;
        }
        // Lot D3 : une presence n'a de sens qu'autour du joueur qui l'a
        // demandee. Deconnecte (FindConnectedPlayer : une teleportation
        // lointaine n'est pas une deconnexion), la presence tombe.
        // Lot D4c : un chasseur aussi (sa prime tombe `hors_ligne` a la meme
        // seconde, SuivrePrimes ; ceinture si l'ordre changeait).
        if ((f.a.presence || f.a.chasseur) && (!f.joueur || !JoueurConnecte(f.joueur)))
        {
            lever(f, "joueur_hors_ligne", CoaDirecteur::COA_DIR_NB_COMPTEURS);
            continue;
        }
        // Rerandomisation (AdjustBotToRange) : la destination n'est plus de
        // sa tranche, et la teleportation periodique doit reprendre. Regle (b)
        // de la SPEC § 6.2 : seule une BAISSE ou un saut de plus d'un niveau
        // la signe. Une montee de 1 est la progression naturelle d'un bot qui
        // fait ses quetes en SEJOUR (decision 4) : l'affectation tient, et le
        // niveau retenu suit, pour que la montee suivante soit jugee de meme.
        // Un tirage d'AdjustBotToRange au meme niveau ou a +1 passe inapercu :
        // la destination reste alors de sa tranche, rien a lever.
        {
            uint8 const niveau = bot->GetLevel();
            if (niveau < f.niveau || niveau > f.niveau + 1)
            {
                lever(f, "niveau", CoaDirecteur::COA_DIR_NB_COMPTEURS);
                continue;
            }
            if (niveau != f.niveau)
                decisions.push_back(Decision{f.guid, f.a.id, Issue::Niveau, nullptr,
                                             CoaDirecteur::COA_DIR_NB_COMPTEURS, nullptr, 0, 0, niveau});
        }
        // L'affectation ne survit pas a la suspension en mode RPG (§ 6.3) :
        // levee, echeance, stop, purge ou remplacement par un mode 0. Le suivi
        // passe dans Entretien AVANT la file de reconstruction : l'affectation
        // tombe au plus tard au tick ou la chasse est rendue.
        if (f.mercenaireRpg && !SuspenduEnRpg(f.guid))
        {
            lever(f, "fin_suspension", CoaDirecteur::COA_DIR_AFFECTATIONS_LEVEES_FIN_SUSPENSION);
            continue;
        }
        // Compagnons (27/09) : le centre SUIT le joueur, lu ici (fil monde,
        // cartes a l'arret, P-137) et ecrit sous le verrou exclusif plus bas
        // (seul ecrivain de l'affectation, P-134). FIGE si le joueur est mort
        // (le bot l'attend sur place, il ne suit pas un fantome au cimetiere),
        // en vol (taxi ou monture volante), en teleportation ou sur une autre
        // carte (le service leve alors : `joueur_autre_carte`).
        if (f.a.compagnon)
        {
            Player* joueur = JoueurConnecte(f.joueur);   // non nul : teste plus haut
            if (joueur && joueur->IsInWorld() && !joueur->IsBeingTeleported() && joueur->GetMapId() == f.a.carte &&
                joueur->IsAlive() && !joueur->IsInFlight() && !joueur->IsFlying())
            {
                float const ex = joueur->GetPositionX() - f.a.x;
                float const ey = joueur->GetPositionY() - f.a.y;
                if (ex * ex + ey * ey > COMPAGNON_SAUT_YD * COMPAGNON_SAUT_YD)
                {
                    lever(f, "joueur_saut", CoaDirecteur::COA_DIR_NB_COMPTEURS);
                    continue;
                }
                if (ex * ex + ey * ey > COMPAGNON_RECALAGE_YD * COMPAGNON_RECALAGE_YD)
                {
                    Decision d{f.guid, f.a.id, Issue::Centre, nullptr, CoaDirecteur::COA_DIR_NB_COMPTEURS, nullptr, 0,
                               0};
                    d.cx = joueur->GetPositionX();
                    d.cy = joueur->GetPositionY();
                    d.cz = joueur->GetPositionZ();
                    decisions.push_back(d);
                }
            }
        }
        // Missions (30/09). Une mission CADUQUE est degradee en Chasse (le bot
        // reste au lieu, il ne repart pas) : quete plus incomplete (finie, rendue,
        // abandonnee), chef de bande parti (fin, mort, hors ligne, autre carte).
        // Un suiveur dont le chef est ARRIVE voit son centre recale sur lui des
        // qu'il a bouge de plus de BANDE_RECALAGE_YD. Lu ici, fil monde, cartes a
        // l'arret (P-137) ; ecrit sous le verrou exclusif plus bas (P-134).
        if (f.a.mission == CoaDirecteur::CoaDirMission::Quete && f.a.phase == CoaDirecteur::CoaDirPhase::Sejour &&
            bot->GetQuestStatus(f.a.missionParam) != QUEST_STATUS_INCOMPLETE)
        {
            Decision d{f.guid, f.a.id, Issue::Mission, "quete_finie", CoaDirecteur::COA_DIR_NB_COMPTEURS, nullptr, 0,
                       0};
            decisions.push_back(d);
        }
        else if (f.a.mission == CoaDirecteur::CoaDirMission::Bande)
        {
            AffectationInterne const* chefAff = nullptr;
            for (AffectationInterne const& autre : copies)
                if (autre.guid == f.a.missionParam)
                    chefAff = &autre;
            Player* chef = chefAff ? JoueurConnecte(chefAff->guid) : nullptr;
            if (!chefAff || !chefAff->a.presence || chefAff->a.compagnon || chefAff->joueur != f.joueur ||
                chefAff->a.mission == CoaDirecteur::CoaDirMission::Bande || chefAff->a.finS <= maintenant || !chef ||
                !chef->IsAlive() || chef->GetMapId() != f.a.carte)
            {
                decisions.push_back(Decision{f.guid, f.a.id, Issue::Mission, "chef_parti",
                                             CoaDirecteur::COA_DIR_NB_COMPTEURS, nullptr, 0, 0});
            }
            else if (chef->IsInWorld() && !chef->IsBeingTeleported() && !chefAff->a.etape &&
                     chefAff->a.phase == CoaDirecteur::CoaDirPhase::Sejour)
            {
                float const ex = chef->GetPositionX() - f.a.x;
                float const ey = chef->GetPositionY() - f.a.y;
                if (ex * ex + ey * ey > BANDE_RECALAGE_YD * BANDE_RECALAGE_YD)
                {
                    Decision d{f.guid, f.a.id, Issue::Centre, nullptr, CoaDirecteur::COA_DIR_BANDES_RECALEES, nullptr,
                               0, 0};
                    d.cx = chef->GetPositionX();
                    d.cy = chef->GetPositionY();
                    d.cz = chef->GetPositionZ();
                    decisions.push_back(d);
                }
            }
        }
        if (f.a.phase != CoaDirecteur::CoaDirPhase::Trajet)
        {
            // Sejour : releve unique a T + 5 min (§ 6.5). `maintenant` et
            // `arriveeS` sont la meme horloge (Maintenant()).
            if (!f.releveFait && maintenant - f.arriveeS >= RELEVE_SEJOUR_S)
                decisions.push_back(Decision{f.guid, f.a.id, Issue::Releve, nullptr,
                                             CoaDirecteur::COA_DIR_NB_COMPTEURS, nullptr, 0, 0});
            continue;
        }
        if (CoaDirecteur::DansLeDisque(f.a, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY()))
        {
            // Etape d'itineraire : pas de SEJOUR. Elle reste en TRAJET
            // jusqu'a ce que l'affectation suivante la remplace ou que
            // l'echeance tombe ; l'arrivee n'est constatee qu'une fois.
            if (f.a.etape)
            {
                if (!f.etapeArrivee)
                    decisions.push_back(Decision{f.guid, f.a.id, Issue::EtapeArrivee, nullptr,
                                                 CoaDirecteur::COA_DIR_NB_COMPTEURS, nullptr, 0, 0});
                continue;
            }
            decisions.push_back(Decision{f.guid, f.a.id, Issue::Arrivee, nullptr, CoaDirecteur::COA_DIR_NB_COMPTEURS,
                                         ia, ia->rpgStatistic.questAccepted, ia->rpgStatistic.questRewarded});
            continue;
        }
        // Une etape deja atteinte n'est pas un trajet qui n'arrive pas : sortie
        // du disque (combat, errance), la laisse l'y ramene.
        // Reserve de chasseurs (02/10, revue M3) : un chasseur de RESERVE traque au
        // lieu de rejoindre le disque de sa finale ; il n'est pas leve pour ce
        // motif tant que sa prime court (sa fin le leve `fin_prime`).
        if (!f.etapeArrivee && maintenant - f.poseS >= trajetMax &&
            !(f.a.chasseur && f.a.reserve && PrimeCourt(f.joueur, maintenant)))
            lever(f, "trajet_abandonne", CoaDirecteur::COA_DIR_TRAJETS_ABANDONNES);
    }
    if (decisions.empty())
        return;   // `morts` est alors vide : toute mort passe par une decision `mort`

    struct Fin
    {
        AffectationInterne f;
        Decision d;
    };
    std::vector<Fin> fins;
    std::vector<AffectationInterne> releves;
    std::vector<AffectationInterne> etapes;
    std::vector<MortTrajet> mortsPosees;
    uint32 arrivees = 0;
    uint32 arriveesPresence = 0;
    uint32 bandesRecalees = 0;
    struct MissionDegradee
    {
        uint32 guid, id;
        char const* avant;
        char const* raison;
    };
    std::vector<MissionDegradee> missionsDegradees;
    std::vector<AffectationInterne> chasseursArrives;   // lot D4c : retour a la chasse
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        for (Decision const& d : decisions)
        {
            auto it = affectations.find(d.guid);
            // Seul le fil monde ecrit : rien n'a pu changer depuis la copie.
            // Ceinture tout de meme : on n'applique qu'a la MEME consigne.
            if (it == affectations.end() || it->second.a.id != d.id)
                continue;
            if (d.issue == Issue::Arrivee)
            {
                it->second.a.phase = CoaDirecteur::CoaDirPhase::Sejour;
                it->second.ia = d.ia;
                it->second.quetesAcceptees0 = d.acceptees;
                it->second.quetesRendues0 = d.rendues;
                it->second.arriveeS = maintenant;
                ++arrivees;
                if (it->second.a.presence)
                    ++arriveesPresence;
                if (it->second.a.chasseur)
                    chasseursArrives.push_back(it->second);
                continue;
            }
            if (d.issue == Issue::Releve)
            {
                it->second.releveFait = true;
                releves.push_back(it->second);
                continue;
            }
            if (d.issue == Issue::Niveau)
            {
                it->second.niveau = d.niveau;
                continue;
            }
            if (d.issue == Issue::Centre)
            {
                it->second.a.x = d.cx;
                it->second.a.y = d.cy;
                it->second.a.z = d.cz;
                if (d.compteur != CoaDirecteur::COA_DIR_NB_COMPTEURS)
                    ++bandesRecalees;
                continue;
            }
            if (d.issue == Issue::Mission)
            {
                // Degradee en Chasse, au meme lieu. Un suiveur retrouve un disque
                // de chasse (le sien etait de BANDE_MAX_YD autour du chef).
                missionsDegradees.push_back(
                    MissionDegradee{d.guid, d.id, NomMission(it->second.a.mission), d.raison});
                if (it->second.a.mission == CoaDirecteur::CoaDirMission::Bande)
                    it->second.a.rayon = std::max(it->second.a.rayon, BANDE_DEGRADEE_RAYON_YD);
                it->second.a.mission = CoaDirecteur::CoaDirMission::Chasse;
                it->second.a.missionParam = 0;
                continue;
            }
            if (d.issue == Issue::EtapeArrivee)
            {
                it->second.etapeArrivee = true;
                etapes.push_back(it->second);
                continue;
            }
            fins.push_back(Fin{it->second, d});
            affectations.erase(it);
        }
        MajCompteursAffectations();
        // L'etat mort_en_trajet n'est pose que pour une affectation
        // effectivement levee ici (meme id) : la ceinture ci-dessus vaut aussi
        // pour lui. Un etat deja pose pour ce bot est remplace (nouvelle mort).
        for (MortTrajet const& m : morts)
        {
            bool levee = false;
            for (Fin const& fin : fins)
                if (fin.f.guid == m.guid && fin.f.a.id == m.id)
                    levee = true;
            if (!levee)
                continue;
            mortsEnTrajet[m.guid] = m;
            mortsPosees.push_back(m);
        }
        nbMortsEnTrajet.store(static_cast<uint32>(mortsEnTrajet.size()), std::memory_order_relaxed);
    }
    for (uint32 i = 0; i < arrivees; ++i)
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_TRAJETS_ARRIVES);
    for (uint32 i = 0; i < arriveesPresence; ++i)
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ARRIVEES_PRESENCE);
    for (uint32 i = 0; i < bandesRecalees; ++i)
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_BANDES_RECALEES);
    for (MissionDegradee const& m : missionsDegradees)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_MISSIONS_DEGRADEES);
        LOG_INFO("playerbots", "coa directeur mission: id={} guid={} type=chasse avant={} etat=degradee raison={}", m.id,
                 m.guid, m.avant, m.raison);
    }
    for (AffectationInterne const& r : releves)
        JournaliserFinAffectation(r, "arrivee_5min");
    // Lot D4c : le chasseur est dans son disque (SEJOUR) ; MercenaireEnRpg rend
    // faux des maintenant, la fabrique lui rend sa chasse.
    for (AffectationInterne const& ch : chasseursArrives)
    {
        BasculerComposition(ch.guid);
        LOG_INFO("playerbots", "coa directeur chasse: bot={} joueur={} id={} motif=arrivee", ch.guid, ch.joueur,
                 ch.a.id);
    }
    for (AffectationInterne const& e : etapes)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ETAPES_ARRIVEES);
        LOG_INFO("playerbots", "coa directeur etape: guid={} id={} arrivee=1 fin_s={}", e.guid, e.a.id, e.a.finS);
    }
    for (MortTrajet const& m : mortsPosees)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_MORTS_EN_TRAJET);
        LOG_INFO("playerbots", "coa directeur mort_en_trajet: id={} guid={} carte={} x={:.1f} y={:.1f} fin_s={}", m.id,
                 m.guid, m.carte, m.x, m.y, m.finS);
    }
    for (Fin const& fin : fins)
    {
        if (fin.d.issue == Issue::Echue)
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ECHUES);
            FinirAffectation(fin.f, nullptr, nullptr, "echue");
            continue;
        }
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        if (fin.d.compteur < CoaDirecteur::COA_DIR_NB_COMPTEURS)
            CoaDirecteur::Compter(fin.d.compteur);
        if (fin.d.compteur == CoaDirecteur::COA_DIR_TRAJETS_ABANDONNES)
            LOG_INFO("playerbots", "coa directeur trajet abandonne: id={} guid={}", fin.f.a.id, fin.f.guid);
        FinirAffectation(fin.f, "leve", fin.d.raison, fin.d.raison);
    }
}

// Etat mort_en_trajet (lot D2b, suite), chaque seconde, fil monde, cartes a
// l'arret. Il tombe : bot de nouveau en vie (`ressuscite`, avec la distance au
// lieu de la mort : au-dela de RELEVE_LOIN_YD, il ne s'est pas releve a son
// cadavre), hors ligne, echeance, ou option eteinte. Meme patron que le suivi
// des affectations : copie sous verrou partage, examen hors verrou, retrait
// sous verrou exclusif, journal hors verrou.
constexpr float RELEVE_LOIN_YD = 60.0f;   // > CORPSE_RECLAIM_RADIUS (39) : pas repris au cadavre

void SuivreMortsEnTrajet()
{
    if (!nbMortsEnTrajet.load(std::memory_order_relaxed))
        return;
    uint32 const maintenant = Maintenant();
    bool const option = sPlayerbotAIConfig.directeurReleveCimetiere;

    std::vector<MortTrajet> copies;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        copies.reserve(mortsEnTrajet.size());
        for (auto const& [guid, m] : mortsEnTrajet)
            copies.push_back(m);
    }

    struct Fin
    {
        MortTrajet m;
        char const* motif;
        float distance;   // < 0 : autre carte, ou sans objet
        bool ressuscite = false;
    };
    std::vector<Fin> fins;
    for (MortTrajet const& m : copies)
    {
        if (!option)
        {
            fins.push_back(Fin{m, "option_eteinte", -1.0f});
            continue;
        }
        Player* bot = JoueurConnecte(m.guid);
        if (!bot)
        {
            fins.push_back(Fin{m, "hors_ligne", -1.0f});
            continue;
        }
        if (bot->IsInWorld() && !bot->IsBeingTeleported() && bot->IsAlive())
        {
            float distance = -1.0f;
            if (bot->GetMapId() == m.carte)
            {
                float const dx = bot->GetPositionX() - m.x;
                float const dy = bot->GetPositionY() - m.y;
                distance = std::sqrt(dx * dx + dy * dy);
            }
            fins.push_back(Fin{m, "ressuscite", distance, true});
            continue;
        }
        if (m.finS <= maintenant)
            fins.push_back(Fin{m, "echue", -1.0f});
    }
    if (fins.empty())
        return;
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        for (Fin const& fin : fins)
        {
            auto it = mortsEnTrajet.find(fin.m.guid);
            if (it != mortsEnTrajet.end() && it->second.mortS == fin.m.mortS && it->second.id == fin.m.id)
                mortsEnTrajet.erase(it);
        }
        nbMortsEnTrajet.store(static_cast<uint32>(mortsEnTrajet.size()), std::memory_order_relaxed);
    }
    for (Fin const& fin : fins)
    {
        if (fin.ressuscite)
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_RELEVES_RESSUSCITES);
            if (fin.distance < 0.0f || fin.distance > RELEVE_LOIN_YD)
                CoaDirecteur::Compter(CoaDirecteur::COA_DIR_RELEVES_LOIN_DU_CORPS);
        }
        LOG_INFO("playerbots", "coa directeur releve: id={} guid={} motif={} duree_s={} distance={}", fin.m.id,
                 fin.m.guid, fin.motif, maintenant - fin.m.mortS,
                 fin.distance < 0.0f ? std::string("NA") : Acore::StringFormat("{:.0f}", fin.distance));
    }
}

// Trajets leves (correctif du 26/09), chaque seconde, bus allume ou eteint :
// seule l'echeance les retire. PAS de purge au bus eteint : c'est justement
// la levee `bus_eteint` qui en pose, et les bots doivent encore les lire.
void SuivreTrajetsLeves()
{
    if (!nbTrajetsLeves.load(std::memory_order_relaxed))
        return;
    uint32 const maintenant = Maintenant();
    std::unique_lock<std::shared_mutex> garde(verrouConsignes);
    for (auto it = trajetsLeves.begin(); it != trajetsLeves.end();)
    {
        if (it->second.finS <= maintenant)
            it = trajetsLeves.erase(it);
        else
            ++it;
    }
    nbTrajetsLeves.store(static_cast<uint32>(trajetsLeves.size()), std::memory_order_relaxed);
}

// Compagnons : pactes, chaque seconde, fil monde, cartes a l'arret. Ils
// tombent a l'echeance, a la deconnexion du joueur protege, ou tous si
// l'option est eteinte. Meme patron : copie des cles sous verrou partage,
// lecture des joueurs hors verrou, retrait sous verrou exclusif.
void SuivrePactes()
{
    if (!nbPactes.load(std::memory_order_relaxed))
        return;
    uint32 const maintenant = Maintenant();
    bool const option = sPlayerbotAIConfig.directeurCompagnons;
    std::vector<std::pair<uint32, PacteCompagnon>> copies;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        copies.assign(pactes.begin(), pactes.end());
    }
    std::vector<std::pair<uint32, uint32>> retraits;       // (bot, id)
    std::vector<std::pair<uint32, uint32>> prolongations;  // (bot, id)
    for (auto const& [bot, p] : copies)
    {
        Player* joueur = option ? JoueurConnecte(p.joueur) : nullptr;
        if (!option || !joueur)
        {
            retraits.emplace_back(bot, p.id);
            continue;
        }
        // Dans le groupe du joueur (il l'a invite) : le pacte court tant qu'il
        // y reste, puis PACTE_MARGE_S. Sans quoi un compagnon renvoye du groupe
        // apres la fin du pacte redeviendrait FFA a quelques pas d'un joueur FFA.
        Player* compagnon = JoueurConnecte(bot);
        Group* groupe = joueur->GetGroup();
        if (compagnon && groupe && compagnon->GetGroup() == groupe)
        {
            // Revue m3 : prolonge a mi-marge seulement (pas un verrou exclusif
            // par seconde et par compagnon groupe).
            if (p.finS < maintenant + PACTE_MARGE_S / 2)
                prolongations.emplace_back(bot, p.id);
            continue;
        }
        if (p.finS <= maintenant)
            retraits.emplace_back(bot, p.id);
    }
    if (retraits.empty() && prolongations.empty())
        return;
    std::unique_lock<std::shared_mutex> garde(verrouConsignes);
    for (auto const& [bot, id] : retraits)
    {
        auto it = pactes.find(bot);
        if (it != pactes.end() && it->second.id == id)
            pactes.erase(it);
    }
    for (auto const& [bot, id] : prolongations)
    {
        auto it = pactes.find(bot);
        if (it != pactes.end() && it->second.id == id)
            it->second.finS = maintenant + PACTE_MARGE_S;
    }
    nbPactes.store(static_cast<uint32>(pactes.size()), std::memory_order_relaxed);
}

void PurgerPactes()
{
    if (!nbPactes.load(std::memory_order_relaxed))
        return;
    std::unique_lock<std::shared_mutex> garde(verrouConsignes);
    pactes.clear();
    nbPactes.store(0, std::memory_order_relaxed);
}

// Purge sans condition (bus eteint) : plus aucun etat, les lecteurs retombent
// sur leur chargement atomique.
void PurgerMortsEnTrajet()
{
    if (!nbMortsEnTrajet.load(std::memory_order_relaxed))
        return;
    std::unique_lock<std::shared_mutex> garde(verrouConsignes);
    mortsEnTrajet.clear();
    nbMortsEnTrajet.store(0, std::memory_order_relaxed);
}

// ------------------------------------------------------------------ lot D3 : aides communes

// Un joueur HUMAIN : pas de moteur playerbots (meme forme que Monde(), type H),
// et une session (ChatHandler l'exige).
bool Humain(Player* p)
{
    return p && !GET_PLAYERBOT_AI(p) && p->GetSession();
}

// Autonomie des evenements (11/10/2026, coa-project/docs/CONCEPTION-autonomie-evenements.md § 5) : l'ANCRE d'un
// evenement (le joueur de reference des commandes `evenement` point/haut, invoquer, ordre, et de la presence F-/T-/L-/Q-).
// Un humain, toujours ; un BOT en monde et non mercenaire, seulement si AiPlayerbot.Directeur.EvenementAncreBot vaut 1
// (meme regle que le service : un mercenaire ne joue pas l'evenement, calibrage.recenser). Option eteinte : Humain(p),
// a l'identique. Fil monde seul (EstMercenaire lit RandomPlayerbotMgr::currentBots, sans verrou : MercenaryRewards.h).
// Ne remplace Humain NULLE PART ailleurs : ce qui s'adresse a l'ecran d'un client (ciel, meteo, paroles, recompense,
// coffre, portail, objectifs, lieu `joueur`) reste reserve aux humains.
// 19/10 : seule exception hors du .cpp, ExecuterGisement (CoaDirecteurGardien.inc) : le gisement est pour les humains du
// PUBLIC, une ancre bot le pose pour eux.
bool AncreDEvenement(Player* p)
{
    if (Humain(p))
        return true;
    // Revue du 11/10 : bot ALEATOIRE seul (jamais l'altbot d'un compte humain, que le bus ne doit pas pouvoir viser).
    return sPlayerbotAIConfig.directeurEvenementAncreBot && p && GET_PLAYERBOT_AI(p) && p->IsInWorld() &&
           sRandomPlayerbotMgr.IsRandomBot(p) && !MercenaryRewards::EstMercenaire(p);
}

// ------------------------------------------------------------------ banc d'essai de prime (03/10/2026)
//
// Option Directeur.PrimeBanc. EstBanc : fil monde, le personnage est-il un BOT de la
// liste ? Jamais un humain (la liste ne peut pas soustraire un humain a la regle).
bool EstBanc(Player* p)
{
    return p && !Humain(p) && CoaDirecteur::CibleDeBanc(p->GetGUID().GetCounter());
}

// Chaque seconde, fil monde (Entretien, bus allume ou eteint) : la liste de conf est
// relue (une comparaison de chaine quand rien n'a change), la copie des fils de carte
// remplacee, et chaque bot ajoute ou retire est RECOMPOSE (AiFactory lit
// CibleDeBanc : composition immobile, ou celle d'avant). Liste mal formee : AUCUN
// bot (meme regle que ReserveGuildes et que le service).
void SuivreBancs()
{
    std::string const& texte = sPlayerbotAIConfig.directeurPrimeBanc;
    if (texte == bancTexte)
        return;
    bancTexte = texte;
    std::vector<uint32> const lus = sPlayerbotAIConfig.PrimeBancGuids();
    std::unordered_set<uint32> nouveaux(lus.begin(), lus.end());
    std::vector<uint32> changes;
    {
        std::unique_lock<std::shared_mutex> garde(verrouBancs);
        for (uint32 g : bancs)
            if (!nouveaux.count(g))
                changes.push_back(g);
        for (uint32 g : nouveaux)
            if (!bancs.count(g))
                changes.push_back(g);
        bancs.swap(nouveaux);
        nbBancs.store(static_cast<uint32>(bancs.size()), std::memory_order_relaxed);
    }
    uint32 recomposes = 0;
    for (uint32 g : changes)
        if (JoueurConnecte(g))
        {
            BasculerComposition(g);
            ++recomposes;
        }
    bool const malFormee = lus.empty() && texte.find_first_not_of(" \t") != std::string::npos;
    LOG_INFO("playerbots", "coa directeur banc: cibles={} changes={} recomposes={} mal_formee={}", lus.size(),
             changes.size(), recomposes, malFormee ? 1 : 0);
}

// La regle d'une prime : High Risk pour un humain ; pour une cible de banc, le FFA
// que playerbots pose sur tout bot sans maitre (PlayerbotAI::UpdateAI) hors lieu sans
// PvP. Aucune aura n'est posee ni lue sur le bot, aucun drapeau ecrit (P-134).
// `pose` : a la pose, le FFA est exige ; ensuite (SuivrePrimes) seul le lieu compte
// (le coeur retire le drapeau un tick a chaque changement d'aire, P-126).
bool HighRisk(Player* p);
bool RegleDePrime(Player* p, bool pose)
{
    if (Humain(p))
        return HighRisk(p);
    if (!EstBanc(p) || sPlayerbotAIConfig.CoaLieuSansPvp(p->GetZoneId(), p->GetAreaId()))
        return false;
    return !pose || p->IsFFAPvP();
}

// Proprietaire d'une affectation (SPEC-directeur-D3.md § 3.6, decision n° 15) :
// son `origine` ; pour `ask` et `arc`, `origine` ET `joueur` (le joueur qui a
// demande). Pour `regle`, `joueur` n'est que l'humain le plus proche, pose pour
// que son `stop` leve la consigne : il ne dit pas a qui est le bot.
bool MemeProprietaire(AffectationInterne const& f, CoaDirCommande const& c)
{
    if (f.origine != c.origine)
        return false;
    if (c.origine == "ask" || c.origine == "arc")
        return f.joueur == c.joueur;
    return true;
}

// `peupler` : un humain AUTRE que `exclu` a moins de `rayon` du centre ? Meme
// boucle que PlayerbotAI::HasPlayerNearby (Bot/PlayerbotAI.cpp), qui ne sait
// exempter personne : MJ invisibles ignores, point de vue compris.
bool HumainProcheSauf(WorldPosition centre, uint32 carte, float rayon, uint32 exclu)
{
    float const carre = rayon * rayon;
    for (Player* humain : sRandomPlayerbotMgr.GetPlayers())
    {
        if (!humain || humain->GetGUID().GetCounter() == exclu)
            continue;
        if (humain->IsGameMaster() && !humain->isGMVisible())
            continue;
        if (humain->GetMapId() != carte)
            continue;
        if (centre.sqDistance(WorldPosition(humain)) < carre)
            return true;
        WorldObject* vue = humain->GetViewpoint();
        if (vue && vue != humain && centre.sqDistance(WorldPosition(vue)) < carre)
            return true;
    }
    return false;
}

constexpr uint32 AURA_REGLE_PVE = 9931032;        // mod-ascension-compat, AscensionRulesets.cpp
constexpr uint32 AURA_REGLE_HIGH_RISK = 1004019;
constexpr uint32 AURA_REGLE_WAR_MODE = 1004119;

constexpr float PRESENCE_A_VUE_YD = 150.0f;       // visibilite 100 yd (worldserver.conf) + marge
constexpr float PRESENCE_CENTRE_MIN_YD = 40.0f;   // centre du disque, depuis le joueur a la pose
constexpr float PRESENCE_CENTRE_MAX_YD = 300.0f;
// Lot D4c (`chasser`, SPEC-directeur-D4.md § 6.3) : le disque final d'un
// chasseur, depuis le joueur prime a la pose.
constexpr float CHASSEUR_CENTRE_MIN_YD = 120.0f;
constexpr float CHASSEUR_CENTRE_MAX_YD = 300.0f;

// ------------------------------------------------------------------ teleportation cachee (27/09)
//
// Option Directeur.TeleportCache. Une presence `peupler` d'action `T-<n>` est
// la PREMIERE etape d'un parcours long : a la pose, le fil monde (cartes a
// l'arret, P-137) teleporte le bot sur un point de son disque, puis pose
// l'affectation (une etape). Le bot marche ensuite les etapes suivantes. UN
// ETAT NEUF, `sautsFaits` (guid -> Maintenant() du saut), fil monde seul, sans
// verrou : ecrit et lu par ExecuterAffectation seulement (anti double saut).
//
// Ce qui garantit qu'aucun humain ne voit le bot apparaitre ou disparaitre :
//  - la DISTANCE, d'abord : Visibility.Distance.Continents = 100 yd
//    (worldserver.conf en service, lu le 27/09) ; le client ne recoit pas un
//    objet au-dela. Le point est a >= SAUT_JOUEUR_MIN_YD du demandeur et a
//    >= SAUT_HUMAIN_YD de TOUT humain (point de vue compris) ; au depart, aucun
//    humain a SAUT_HUMAIN_YD du bot (HasPlayerNearby, la garde de RandomTeleport) ;
//  - la LIGNE DE VUE, ensuite : ni les pieds ni la tete du bot au point ne sont
//    visibles de l'oeil du demandeur. Map::isInLineOfSight ne lit QUE les
//    modeles (vmap, objets de jeu) -- lu dans Map.cpp, pas le relief --, d'ou
//    une seconde sonde sur la grille de terrain (ReliefCoupe) ;
//    La ligne de vue part de la TETE du personnage (+ 1,5 yd), pas de la
//    camera (plus haute, en arriere) : c'est un COMPLEMENT de la distance, qui
//    seule garantit (revue du 27/09, m3) ;
//  - le CHAMP, si possible : un point derriere le joueur (hors de l'arc de
//    SAUT_CHAMP_RAD devant lui) est prefere. La camera du client tourne
//    librement : ce n'est qu'une preference, le journal dit `dans_le_champ`.
constexpr float SAUT_JOUEUR_MIN_YD = 150.0f;
constexpr float SAUT_JOUEUR_MAX_YD = 260.0f;      // service : 150-250 yd, + 10 de marge (le joueur bouge)
constexpr float SAUT_HUMAIN_YD = 150.0f;
constexpr float SAUT_DEPART_MIN_YD = 300.0f;      // plus pres, il n'y a rien a gagner a sauter
constexpr uint32 SAUT_REPOS_S = 900;              // un bot ne saute pas deux fois en 15 min
constexpr float SAUT_OEIL_YD = 1.5f;              // au-dessus de la tete du joueur : la camera
constexpr float SAUT_CORPS_BAS_YD = 0.5f;
constexpr float SAUT_CORPS_HAUT_YD = 2.5f;
constexpr float SAUT_PAS_RELIEF_YD = 4.0f;
constexpr float SAUT_CHAMP_RAD = 2.0f * float(M_PI) / 3.0f;   // 120 degres devant le joueur
std::unordered_map<uint32, uint32> sautsFaits;    // guid bas du bot -> Maintenant() du saut
constexpr float SAUT_WINTERGRASP_MARGE_YD = 300.0f;

// Saut LOINTAIN (30/09, option AiPlayerbot.Directeur.TeleportLointain, defaut 0) :
// une presence d'action `L-<n>`, que le service n'emet qu'apres un `sans_cache`
// (plaine nue, demande 47 du 30/09 aux Tarides : 4 refus sur 4). Le point est a
// SAUT_LOINTAIN_MIN_YD-SAUT_LOINTAIN_MAX_YD du demandeur et n'a PAS a etre cache :
// seule la distance garantit. Visibility.Distance.Continents = 100 yd (lu dans
// worldserver.conf en service le 30/09) : le client ne recoit aucun objet au-dela
// (WorldObject::CanSeeOrDetect -> IsWithinSightRange(GetSightRange), Object.cpp),
// et 250 yd laissent 150 yd de marge au joueur qui avance vers le point. Toutes
// les autres gardes du saut cache restent : sol sec, ciel ouvert (ToitAuDessus,
// P-145), point relie au centre, aucun humain ni point de vue (HumainProcheSauf,
// oeil de lynx compris) a SAUT_HUMAIN_YD, depart hors de vue, Wintergrasp, un saut
// par bot en SAUT_REPOS_S.
constexpr float SAUT_LOINTAIN_MIN_YD = 250.0f;
constexpr float SAUT_LOINTAIN_MAX_YD = 350.0f;

// Revue du 27/09 (M1) : a Wintergrasp un joueur voit a 175 yd
// (VISIBILITY_DIST_WINTERGRASP, Object.cpp), pas a 100 : la marge de 150 yd ne
// tient plus. Meme boite que WorldObject::IsInWintergrasp (Object.h), elargie
// de 300 yd : ni le bot, ni le joueur (donc aucun point, a <= 260 yd de lui).
bool PresDeWintergrasp(uint32 carte, float x, float y)
{
    float const m = SAUT_WINTERGRASP_MARGE_YD;
    return carte == 571 && x > 3733.33331f - m && x < 5866.66663f + m && y > 1599.99999f - m &&
           y < 4799.99997f + m;
}

// Le relief coupe-t-il le segment (x1, y1, z1) -> (x2, y2, z2) ? La grille de
// terrain est lue tous les SAUT_PAS_RELIEF_YD, bouts exclus ; un sol plus haut
// que le segment (+ 0,5 yd) le coupe. Sans donnee de grille : pas coupe (la
// sonde ne cache alors rien : prudent).
bool ReliefCoupe(Map* map, float x1, float y1, float z1, float x2, float y2, float z2)
{
    float const dx = x2 - x1, dy = y2 - y1;
    float const d = std::sqrt(dx * dx + dy * dy);
    int const n = static_cast<int>(d / SAUT_PAS_RELIEF_YD);
    for (int i = 1; i < n; ++i)
    {
        float const t = static_cast<float>(i) / static_cast<float>(n);
        float const h = map->GetGridHeight(x1 + t * dx, y1 + t * dy);
        if (h > INVALID_HEIGHT && h > z1 + t * (z2 - z1) + 0.5f)
            return true;
    }
    return false;
}

// Le point (x, y, sol) est-il cache a l'oeil du joueur ? Oui si NI les pieds
// NI la tete ne sont en vue : un modele (Map::isInLineOfSight, toutes les
// verifications, aucun modele ignore) ou le relief coupe chacun des deux rayons.
bool CacheAuJoueur(Map* map, Player* joueur, float x, float y, float sol)
{
    float const ox = joueur->GetPositionX(), oy = joueur->GetPositionY();
    float const oz = joueur->GetPositionZ() + joueur->GetCollisionHeight() + SAUT_OEIL_YD;
    for (float haut : {SAUT_CORPS_BAS_YD, SAUT_CORPS_HAUT_YD})
    {
        float const tz = sol + haut;
        if (map->isInLineOfSight(ox, oy, oz, x, y, tz, joueur->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                 VMAP::ModelIgnoreFlags::Nothing) &&
            !ReliefCoupe(map, ox, oy, oz, x, y, tz))
            return false;
    }
    return true;
}

// Le point du saut : dans le disque (centre, puis 8 caps a mi-rayon, puis 8 a
// 0,9 rayon), a SAUT_JOUEUR_MIN_YD-SAUT_JOUEUR_MAX_YD du joueur, sol sec a
// +-6 yd du sol du centre (P-132, fenetre de P-145), sous le ciel ouvert
// (ToitAuDessus, P-145 : jamais sous une ville), aucun humain a SAUT_HUMAIN_YD,
// cache au joueur. Deux passes : hors de son champ d'abord, puis partout.
// `lointain` (saut `L-`, 30/09) : a SAUT_LOINTAIN_MIN_YD-SAUT_LOINTAIN_MAX_YD du
// joueur, et la ligne de vue n'est PAS jugee (la distance seule garantit) ; tout
// le reste est identique.
// -> essais faits ; `trouve` faux : aucun point.
struct PointSaut
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool trouve = false;
    bool dansLeChamp = false;
    uint8 essais = 0;
};
PointSaut ChercherPointCache(Map* map, Player* joueur, float cx, float cy, float solCentre, float rayon,
                             bool lointain = false)
{
    float const dMin = lointain ? SAUT_LOINTAIN_MIN_YD : SAUT_JOUEUR_MIN_YD;
    float const dMax = lointain ? SAUT_LOINTAIN_MAX_YD : SAUT_JOUEUR_MAX_YD;
    PointSaut r;
    std::vector<std::pair<float, float>> pts{{cx, cy}};
    for (float f : {0.5f, 0.9f})
        for (int k = 0; k < 8; ++k)
        {
            float const a = float(M_PI) * k / 4.0f + (f > 0.6f ? float(M_PI) / 8.0f : 0.0f);
            pts.emplace_back(cx + f * rayon * std::cos(a), cy + f * rayon * std::sin(a));
        }
    for (int passe = 0; passe < 2 && !r.trouve; ++passe)
    {
        for (auto const& [px, py] : pts)
        {
            float const jx = px - joueur->GetPositionX(), jy = py - joueur->GetPositionY();
            float const dj = std::sqrt(jx * jx + jy * jy);
            if (dj < dMin || dj > dMax)
                continue;
            // Revue du 30/09 (m3) : un point lointain peut etre jusqu'a 350 yd du joueur,
            // donc dans la boite de Wintergrasp (visibilite 175 yd > SAUT_HUMAIN_YD) meme si
            // bot et joueur en sont a plus de 300 yd : le point lui-meme est teste.
            if (lointain && PresDeWintergrasp(map->GetId(), px, py))
                continue;
            Position const p(px, py, solCentre);
            bool const champ = joueur->HasInArc(SAUT_CHAMP_RAD, &p);
            if ((passe == 0) == champ)   // passe 0 : hors du champ ; passe 1 : le reste
                continue;
            ++r.essais;
            float sol = 0.0f;
            if (!SolSec(map, px, py, solCentre + Z_OFFSET_FIND_HEIGHT, sol) || std::fabs(sol - solCentre) > TIRAGE_ECART_Z)
                continue;
            if (ToitAuDessus(map, px, py, sol, sol))
                continue;
            // Revue du 27/09 (m1) : le service a trace la route depuis le CENTRE.
            // Un point qui en serait separe (corniche, autre cote d'un mur) y
            // laisserait le bot coince (aucun saut d'enlisement en presence).
            // Approximation peu couteuse de la connexite : pente d'au plus
            // TIRAGE_PENTE_MAX et aucun modele entre le point et le centre.
            if (px != cx || py != cy)
            {
                float const ex = px - cx, ey = py - cy;
                float const plan = std::sqrt(ex * ex + ey * ey);
                if (std::fabs(sol - solCentre) > TIRAGE_PENTE_MAX * plan ||
                    !map->isInLineOfSight(cx, cy, solCentre + SAUT_OEIL_YD, px, py, sol + SAUT_OEIL_YD,
                                          PHASEMASK_NORMAL, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                    continue;
            }
            if (HumainProcheSauf(WorldPosition(map->GetId(), px, py, sol), map->GetId(), SAUT_HUMAIN_YD, 0))
                continue;
            if (!lointain && !CacheAuJoueur(map, joueur, px, py, sol))
                continue;
            r.x = px;
            r.y = py;
            r.z = sol;
            r.trouve = true;
            r.dansLeChamp = champ;
            break;
        }
    }
    return r;
}

// ------------------------------------------------------------------ lot D3 : repondre, paroles, ciel, meteo
//
// SPEC-directeur-D3.md § 2.8, § 5.2, § 6.2, § 6.3. TROIS ETATS NEUFS, TOUS AU
// FIL MONDE SEUL, sans verrou : un seul ecrivain (Recevoir, Entretien,
// HandleStop, la purge du bus eteint) et un seul lecteur (le meme fil :
// Consignes et `joueur,<guid>` du 8888 y sont executes, P-137). Aucun fil de
// carte ne les lit. Aucun etat du coeur n'est ecrit (P-134) : ni la meteo de
// zone (WeatherMgr en reste seul proprietaire), ni l'heure du monde -- des
// PAQUETS au seul joueur, rien d'autre. Aucune strategie touchee (P-130).

// Prefixe de `repondre` (decision n° 18) : la SEULE chaine a codes `|` que le
// Directeur envoie. Le texte venu du service ne peut en porter aucun
// (TexteBusValide) : SendSysMessage ne les echappe pas (Chat.cpp, l'echappement
// calcule `msg` puis envoie `str`, l'original).
constexpr char const* PREFIXE_DIRECTEUR = "|cffc8a2ff[Directeur]|r ";
constexpr size_t TEXTE_BUS_MAX = 255;             // octets, colonne `texte` VARCHAR(255)

// Texte d'une commande : non vide, <= 255 octets, structure UTF-8 correcte
// (octets de tete et de suite ; formes trop longues et surrogates NON testes :
// l'encodeur du service n'en produit pas), caracteres de 1 a 3 octets seulement
// (la connexion MySQL du coeur est en `utf8`, 3 octets : un caractere de 4 octets
// -- emoji -- reviendrait en `?`), ni `|` ni caractere de controle (C0, 0x7F,
// C1). Tout autre texte : refus `texte`.
bool TexteBusValide(std::string const& t)
{
    if (t.empty() || t.size() > TEXTE_BUS_MAX)
        return false;
    size_t i = 0;
    while (i < t.size())
    {
        unsigned char const o = static_cast<unsigned char>(t[i]);
        if (o < 0x20 || o == 0x7F || o == '|')
            return false;
        size_t const n = o < 0x80 ? 1 : (o >= 0xC2 && o <= 0xDF) ? 2 : (o >= 0xE0 && o <= 0xEF) ? 3 : 0;
        if (!n || i + n > t.size())
            return false;
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(t[i + k]) & 0xC0) != 0x80)
                return false;
        // Controles C1 (U+0080 a U+009F : C2 80 a C2 9F) : des controles aussi.
        if (n == 2 && o == 0xC2 && static_cast<unsigned char>(t[i + 1]) < 0xA0)
            return false;
        i += n;
    }
    return true;
}

// Commande qui vise UN joueur (repondre, heure_du_jour, meteo) : `cible` est
// son guid, `joueur` vaut 0 ou le meme guid. Toute autre forme : `bornes`.
bool CibleJoueurValide(CoaDirCommande const& c)
{
    return c.cible != 0 && (c.joueur == 0 || c.joueur == c.cible);
}

// Verbe `repondre` (§ 2.8). Une ligne systeme, prefixee, au seul joueur. Pas de
// consigne, pas de levee : accusee `execute`, puis oubliee. Exemptee de l'arret
// (decision n° 4) : c'est Recevoir qui l'en exempte.
void ExecuterRepondre(CoaDirCommande const& c)
{
    if (!CibleJoueurValide(c))
    {
        Refuser(c, "bornes");
        return;
    }
    Player* p = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!p)
    {
        Refuser(c, "hors_ligne");
        return;
    }
    if (!Humain(p))
    {
        Refuser(c, "pas_humain");
        return;
    }
    if (!TexteBusValide(c.texte))
    {
        Refuser(c, "texte");
        return;
    }
    ChatHandler(p->GetSession()).SendSysMessage(std::string(PREFIXE_DIRECTEUR) + c.texte);
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REPONSES);
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots", "coa directeur reponse: id={} joueur={} octets={}", c.id, c.cible, c.texte.size());
    Accuser(c, "execute", nullptr);
}

// ---- recompense (lot R3, 03/10/2026)
//
// Verbe `recompenser` (coa-project/docs/CONCEPTION-recompenses-primes.md § 1.5, § 5) :
// verse au joueur `cible` (`joueur` 0 ou le meme), en ligne OU NON,
// `valeur` cuivres et les objets de `texte` (« id:quantite[,id:quantite] », 4 au plus),
// chacun de la liste blanche AiPlayerbot.Directeur.RecompenseObjets. `valeur` au plus
// AiPlayerbot.Directeur.RecompenseMaxCuivre. Le montant est CALCULE par le service
// (exploitation/directeur/recompenses.py, primes.py), jamais par le LLM : le jeu borne.
// Refus (accuse `refuse`, ligne `coa directeur recompense: … statut=refuse`) :
//   bornes (cible, `valeur` negative, rien a verser), plafond, objet (forme, liste
//   blanche, gabarit, quantite), inconnu (personnage absent du cache), pas_humain.
// 08/10 : joueur dans le monde -> argent et objets DIRECTEMENT dans la bourse et les sacs,
// seul ce qui ne tient pas part par courrier ; hors du monde -> UN courrier, comme avant
// (VerserRecompense, « remise directe » plus haut).
// IDEMPOTENCE. La remise directe (SaveInventoryAndGoldToDB), le courrier (mail, mail_items,
// item_instance) ET l'accuse `execute` de la commande partent dans la MEME transaction :
// apres un plantage, soit tout est en base, soit rien, et une commande encore `en_attente`
// n'a rien verse. Dans la vie du processus, une commande n'est livree qu'une fois
// (dernierId de la source).
// Joueur connecte (JoueurConnecte : en chargement compris) : MailDraft l'ajoute aussi a
// sa liste en memoire (Mail.cpp, SendMailTo), sinon le courrier n'existerait qu'en base
// et le serveur pourrait l'ecraser (conception § 1.5).
constexpr size_t RECOMPENSE_OBJETS_MAX = 4;
constexpr uint32 RECOMPENSE_QUANTITE_MAX = 20;

// Un entier decimal de 1 a 10 chiffres, sans signe ni espace. -> faux sinon.
bool EntierRecompense(std::string const& t, uint32& v)
{
    if (t.empty() || t.size() > 10 || !std::all_of(t.begin(), t.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        return false;
    unsigned long long const x = std::strtoull(t.c_str(), nullptr, 10);
    if (x > 0xFFFFFFFFull)
        return false;
    v = static_cast<uint32>(x);
    return true;
}

// L'objet `id` est-il dans AiPlayerbot.Directeur.RecompenseObjets (« 1397885,… ») ?
// Lecture STRICTE : un morceau mal forme, et AUCUN objet n'est permis.
bool ObjetRecompensePermis(uint32 id)
{
    std::string const& liste = sPlayerbotAIConfig.directeurRecompenseObjets;
    if (!id || liste.find_first_not_of(" \t") == std::string::npos)
        return false;
    bool trouve = false;
    size_t debut = 0;
    while (debut <= liste.size())
    {
        size_t fin = liste.find(',', debut);
        if (fin == std::string::npos)
            fin = liste.size();
        std::string morceau = liste.substr(debut, fin - debut);
        morceau.erase(std::remove_if(morceau.begin(), morceau.end(), [](char ch) { return ch == ' ' || ch == '\t'; }),
                      morceau.end());
        uint32 v = 0;
        if (!EntierRecompense(morceau, v) || !v)
            return false;
        trouve = trouve || v == id;
        debut = fin + 1;
    }
    return trouve;
}

// `texte` -> [(objet, quantite)]. Vide : aucun objet (vrai). Faux : forme illisible, plus
// de RECOMPENSE_OBJETS_MAX objets, quantite nulle ou au-dela de RECOMPENSE_QUANTITE_MAX.
bool LireObjetsRecompense(std::string const& texte, std::vector<std::pair<uint32, uint32>>& objets)
{
    objets.clear();
    if (texte.empty())
        return true;
    size_t debut = 0;
    while (debut <= texte.size())
    {
        size_t fin = texte.find(',', debut);
        if (fin == std::string::npos)
            fin = texte.size();
        std::string const morceau = texte.substr(debut, fin - debut);
        size_t const deux = morceau.find(':');
        uint32 id = 0, n = 0;
        if (deux == std::string::npos || !EntierRecompense(morceau.substr(0, deux), id) ||
            !EntierRecompense(morceau.substr(deux + 1), n) || !id || !n || n > RECOMPENSE_QUANTITE_MAX)
            return false;
        objets.emplace_back(id, n);
        if (objets.size() > RECOMPENSE_OBJETS_MAX)
            return false;
        debut = fin + 1;
    }
    return !objets.empty();
}

void RefuserRecompense(CoaDirCommande const& c, char const* raison)
{
    LOG_INFO("playerbots",
             "coa directeur recompense: id={} joueur={} cuivre={} objets={} action={} statut=refuse raison={}", c.id,
             c.cible, c.valeur, c.texte.empty() ? std::string("-") : c.texte,
             c.action.empty() ? std::string("-") : c.action, raison);
    Refuser(c, raison);
}

// ---- remise directe (08/10/2026)
//
// Demande du commanditaire (08/10) : « il faut donner les objets en direct et envoyer par courrier que ce qui ne
// rentre pas dans les sacs ». Partage par `recompenser` (ExecuterRecompense) et la fermeture du portail
// (RefermerPortail, CoaDirecteurPortail.inc).
//
// Joueur DANS LE MONDE (JoueurPourRemise : en jeu, ni en chargement, ni en teleportation, ni en deconnexion) :
//   - l'argent est credite par Player::ModifyMoney, au plus jusqu'au plafond : ModifyMoney refuse TOUT montant qui
//     ne laisse pas GetMoney() + montant < MAX_MONEY_AMOUNT (Player.cpp), donc on ne lui propose que la marge ; le
//     credit retenu est l'ecart MESURE de GetMoney() (un compte d'essai plafonne, CONFIG_TRIAL_MONEY_CAP, est
//     refuse en bloc : tout part par courrier) ;
//   - chaque objet est pose dans les sacs (CanStoreNewItem + StoreNewItem + SendNewItem, forme de Player::AddItem
//     et de .additem) : la quantite qui tient (sacs pleins, objet unique deja possede, categorie limitee :
//     no_space_count de CanStoreItem) est posee, le reste part par courrier ;
//   - ce qui ne tient pas part par COURRIER, comme avant.
// Hors ligne, ou connecte mais pas dans le monde : tout par courrier, comme avant.
//
// ORDRE ET IDEMPOTENCE. Les objets du courrier sont crees pour la quantite ENTIERE AVANT toute ecriture (un echec
// de creation refuse sans rien verser, comme avant) ; la remise directe ne fait ensuite plus rien qui puisse
// echouer : un objet pose en entier est detruit (jamais sauve), un objet pose en partie voit sa quantite reduite
// (Item::SetCount). La remise directe touche la MEMOIRE du joueur, puis Player::SaveInventoryAndGoldToDB(trans)
// l'ecrit DANS LA TRANSACTION DU COURRIER ET DE L'ACCUSE de la commande (forme de la prise d'un courrier et de
// l'echange, cote coeur). CharacterDatabase n'a qu'UN fil asynchrone (CharacterDatabase.WorkerThreads = 1,
// worldserver.conf en service) : les transactions passent dans l'ordre ou elles sont remises, et toute sauvegarde
// du joueur posterieure (periodique, deconnexion) l'est APRES celle-ci. Donc :
//   - plantage avant la validation : ni le credit, ni le courrier, ni l'accuse ne sont en base (la memoire meurt
//     avec le processus) ; la commande, encore `en_attente`, sera rejouee et versera UNE fois ;
//   - apres la validation : les trois sont en base, la commande n'est plus `en_attente` et n'est pas rejouee ;
//   - dans la vie du processus, une commande n'est livree qu'une fois (dernierId de la source ; le portail quitte
//     le registre avant la transaction) : un credit direct reussi n'est jamais refait.
// Risque residuel, le meme que celui du coeur pour la prise d'un courrier : une transaction qui ECHOUE (erreur SQL
// autre qu'un interblocage, que TransactionTask rejoue) laisse le credit en memoire sans l'accuse ; l'argent serait
// ecrit par la sauvegarde suivante du joueur et la commande rejouee apres un redemarrage. Aucune ecriture ne peut
// le prevenir sans attendre le resultat de la transaction, ce que le fil monde ne fait pas.
// FIL MONDE SEUL : la deconnexion d'un joueur (World::UpdateSessions) et les mises a jour de cartes
// (MapMgr::Update, attendu) ne tournent pas pendant cette fonction ; le joueur ne peut pas partir entre deux etapes.
struct RemiseRecompense
{
    bool direct = false;                                      // joueur dans le monde : remise directe tentee
    uint32 cuivreDirect = 0;
    uint32 cuivreCourrier = 0;
    std::vector<std::pair<uint32, uint32>> objetsDirect;      // (objet, quantite posee dans les sacs)
    std::vector<std::pair<uint32, uint32>> objetsCourrier;    // (objet, quantite partie par courrier)
    bool courrier = false;                                    // un courrier est parti
};

// Le joueur peut-il recevoir dans ses sacs ? -> lui-meme, ou nullptr (tout par courrier).
Player* JoueurPourRemise(Player* p)
{
    if (!p || !p->IsInWorld() || p->IsBeingTeleported())
        return nullptr;
    WorldSession* s = p->GetSession();
    if (!s || s->PlayerLoading() || s->PlayerLogout())
        return nullptr;
    return p;
}

std::string TexteObjetsRemise(std::vector<std::pair<uint32, uint32>> const& objets)
{
    std::string s;
    for (auto const& [id, n] : objets)
        s += (s.empty() ? "" : ",") + std::to_string(id) + ":" + std::to_string(n);
    return s.empty() ? std::string("-") : s;
}

// Remise directe puis courrier du reste, dans `trans` (l'appelant y ajoute l'accuse et la valide). `p` : le joueur
// connecte (JoueurConnecte, nullptr hors ligne) ; `crees` : un objet par entree de `objets`, quantite entiere, cree
// avant l'appel ; la fonction en prend possession (detruit, reduit, ou confie au courrier). `directPermis` faux : tout
// par courrier (cible de banc : outils/banc-prime.py prouve le paiement par le courrier recu).
RemiseRecompense VerserRecompense(CharacterDatabaseTransaction trans, Player* p, uint32 guidLow, uint32 cuivre,
                                  std::vector<std::pair<uint32, uint32>> const& objets, std::vector<Item*>& crees,
                                  char const* sujet, char const* corps, bool directPermis = true)
{
    RemiseRecompense r;
    Player* d = directPermis ? JoueurPourRemise(p) : nullptr;
    r.direct = d != nullptr;
    for (size_t i = 0; i < objets.size() && i < crees.size(); ++i)
    {
        uint32 const id = objets[i].first, n = objets[i].second;
        uint32 pose = 0;
        if (d)
        {
            uint32 manque = 0;
            ItemPosCountVec dest;
            InventoryResult const res = d->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, id, n, &manque);
            uint32 place = 0;
            for (ItemPosCount const& x : dest)
                place += x.count;
            // Seules deux formes sont prises : tout tient (OK), ou une partie tient et `manque` dit exactement le
            // reste (sacs pleins, unique, categorie limitee). Toute autre forme : rien dans les sacs, tout au courrier.
            bool const coherent = res == EQUIP_ERR_OK ? place == n : (manque < n && place == n - manque);
            if (place && coherent)
                if (Item* pose_ = d->StoreNewItem(dest, id, true))
                {
                    d->SendNewItem(pose_, place, true, false);
                    pose = place;
                }
        }
        if (pose)
            r.objetsDirect.emplace_back(id, pose);
        if (pose >= n)
        {
            delete crees[i];   // jamais sauve ni ajoute a quoi que ce soit
            crees[i] = nullptr;
            continue;
        }
        if (pose)
            crees[i]->SetCount(n - pose);
        r.objetsCourrier.emplace_back(id, n - pose);
    }
    if (cuivre && d)
    {
        uint32 const avant = d->GetMoney();
        // ModifyMoney n'accepte `m` que si avant < MAX_MONEY_AMOUNT - m : la marge est MAX - avant - 1.
        uint32 const marge = avant < MAX_MONEY_AMOUNT ? uint32(MAX_MONEY_AMOUNT) - avant - 1 : 0;
        uint32 const tente = std::min(cuivre, marge);   // <= MAX_MONEY_AMOUNT < 2^31 : tient dans un int32
        if (tente)
            d->ModifyMoney(static_cast<int32>(tente), false);
        uint32 const apres = d->GetMoney();
        r.cuivreDirect = apres > avant ? std::min(apres - avant, cuivre) : 0;
    }
    r.cuivreCourrier = cuivre - r.cuivreDirect;
    if (d && (r.cuivreDirect || !r.objetsDirect.empty()))
        d->SaveInventoryAndGoldToDB(trans);
    bool resteObjet = false;
    for (Item* objet : crees)
        resteObjet = resteObjet || objet;
    if (r.cuivreCourrier || resteObjet)
    {
        MailDraft brouillon(sujet, corps);
        if (r.cuivreCourrier)
            brouillon.AddMoney(r.cuivreCourrier);
        for (Item* objet : crees)
            if (objet)
            {
                objet->SaveToDB(trans);
                brouillon.AddItem(objet);
            }
        brouillon.SendMailTo(trans, p ? MailReceiver(p, guidLow) : MailReceiver(guidLow),
                             MailSender(MAIL_NORMAL, guidLow, MAIL_STATIONERY_GM));
        r.courrier = true;
    }
    crees.clear();   // tout est detruit ou confie au courrier
    return r;
}

std::string TexteCuivre(uint32 cuivre)
{
    uint32 const po = cuivre / 10000, pa = (cuivre / 100) % 100, pc = cuivre % 100;
    std::string s;
    if (po)
        s += std::to_string(po) + " po";
    if (pa)
        s += (s.empty() ? "" : " ") + std::to_string(pa) + " pa";
    if (pc || s.empty())
        s += (s.empty() ? "" : " ") + std::to_string(pc) + " pc";
    return s;
}

std::string TexteRemise(uint32 cuivre, std::vector<std::pair<uint32, uint32>> const& objets)
{
    std::string s = cuivre ? TexteCuivre(cuivre) : std::string();
    for (auto const& [id, n] : objets)
    {
        ItemTemplate const* gabarit = sObjectMgr->GetItemTemplate(id);
        std::string nom = "[" + (gabarit ? gabarit->Name1 : std::to_string(id)) + "]";
        if (n > 1)
            nom += " x" + std::to_string(n);
        s += (s.empty() ? "" : ", ") + nom;
    }
    return s;
}

// Message au joueur (apres la remise de la transaction ; `quoi` = « Votre prime », …). Rien hors du monde : le
// courrier s'annonce seul a la connexion.
void DireRemise(Player* p, RemiseRecompense const& r, char const* quoi)
{
    Player* d = r.direct ? JoueurPourRemise(p) : nullptr;
    if (!d)
        return;
    bool const direct = r.cuivreDirect || !r.objetsDirect.empty();
    bool const courrier = r.cuivreCourrier || !r.objetsCourrier.empty();
    if (direct && courrier)
        ChatHandler(d->GetSession()).PSendSysMessage("{}{} : {}, remis directement ; par courrier (ce qui n'a pas"
                                                     " pu vous etre remis) : {}.", PREFIXE_DIRECTEUR, quoi,
                                                     TexteRemise(r.cuivreDirect, r.objetsDirect),
                                                     TexteRemise(r.cuivreCourrier, r.objetsCourrier));
    else if (direct)
        ChatHandler(d->GetSession()).PSendSysMessage("{}{} : {}, remis directement.", PREFIXE_DIRECTEUR, quoi,
                                                     TexteRemise(r.cuivreDirect, r.objetsDirect));
    else if (courrier)
        ChatHandler(d->GetSession()).PSendSysMessage("{}{} : rien n'a pu vous être remis directement, tout part"
                                                     " par courrier : {}.",
                                                     PREFIXE_DIRECTEUR, quoi,
                                                     TexteRemise(r.cuivreCourrier, r.objetsCourrier));
}

void ExecuterRecompense(CoaDirCommande const& c)
{
    if (!CibleJoueurValide(c) || c.valeur < 0 || (c.valeur == 0 && c.texte.empty()))
    {
        RefuserRecompense(c, "bornes");
        return;
    }
    uint32 const cuivre = static_cast<uint32>(c.valeur);
    if (cuivre > sPlayerbotAIConfig.directeurRecompenseMaxCuivre)
    {
        RefuserRecompense(c, "plafond");
        return;
    }
    std::vector<std::pair<uint32, uint32>> objets;
    if (!LireObjetsRecompense(c.texte, objets))
    {
        RefuserRecompense(c, "objet");
        return;
    }
    for (auto const& [id, n] : objets)
    {
        ItemTemplate const* gabarit = sObjectMgr->GetItemTemplate(id);
        if (!ObjetRecompensePermis(id) || !gabarit || n > gabarit->GetMaxStackSize() ||
            (gabarit->MaxCount > 0 && n > static_cast<uint32>(gabarit->MaxCount)))
        {
            RefuserRecompense(c, "objet");
            return;
        }
    }
    CharacterCacheEntry const* fiche =
        sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(c.cible));
    if (!fiche)
    {
        RefuserRecompense(c, "inconnu");
        return;
    }
    Player* p = JoueurConnecte(c.cible);
    // Banc d'essai de prime (03/10/2026) : le courrier d'une cible de banc (un bot de
    // Directeur.PrimeBanc, en ligne ou non) est accepte ; un humain l'est toujours.
    bool const banc = !(p && Humain(p)) && CoaDirecteur::CibleDeBanc(c.cible);
    if (!banc && ((p && !Humain(p)) || (!p && sPlayerbotAIConfig.IsInRandomAccountList(fiche->AccountId))))
    {
        RefuserRecompense(c, "pas_humain");
        return;
    }
    // Objets crees AVANT la transaction : un echec n'ecrit rien (rien n'est encore ajoute).
    std::vector<Item*> crees;
    for (auto const& [id, n] : objets)
    {
        Item* objet = Item::CreateItem(id, n, p);
        if (!objet)
        {
            for (Item* x : crees)
                delete x;
            RefuserRecompense(c, "objet");
            return;
        }
        crees.push_back(objet);
    }
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    // 06/10 : `intensite` = 1 marque une part de la Faille d'orage (service : recompenses.MARQUE_FAILLE).
    // 09/10 : `intensite` = 2, une part de la Nuit des morts (recompenses.MARQUE_NUIT ; versee a la victoire, sans
    // objet a cliquer). Toute autre valeur : la prime, comme avant.
    // 12/10 (lot T2, coa-project/docs/CONCEPTION-primes-traqueur-bots.md) : `intensite` = 3, la prime d'un CONTRAT
    // sur un bot rempli (service : contrats.py, source `contrat_bot`). Aucun service ne l'envoie avant T2.
    bool const faille = c.intensite == 1.0f;
    bool const nuit = c.intensite == 2.0f;
    bool const contrat = c.intensite == 3.0f;
    // 13/10 (lot V, CoaDirecteurConvoi.inc) : `intensite` = 4, une part du CONVOI de bandits (recompenses.MARQUE_CONVOI).
    bool const convoi = c.intensite == 4.0f;
    // 13/10 (lot T6, coa-project/docs/CONCEPTION-primes-traqueur-bots.md § 10) : `intensite` = 5, un POT-DE-VIN accepte
    // (service : contrats.py, source `contrat_bot`, motif `pot_de_vin` ; recompenses.MARQUE_SOUDOIEMENT).
    bool const soudoiement = c.intensite == 5.0f;
    char const* const titre = faille ? "La faille d'orage" : nuit ? "La nuit des morts" : contrat ? "Contrat rempli"
                              : convoi ? "Le convoi des Sept Routes" : soudoiement ? "Un arrangement discret"
                              : "Votre prime";
    char const* const corps = faille ? "Vous avez aidé à refermer la faille. Voici votre part. Le Directeur."
                              : nuit ? "Vous avez tenu jusqu'à l'aube. Voici votre part. Le Directeur."
                              : contrat ? "Votre cible est tombée. Voici la prime du contrat. Le Directeur."
                              : convoi ? "Le convoi est arrivé. Voici votre part. La Compagnie des Sept Routes."
                              : soudoiement ? "Comme convenu. Vous ne m'avez jamais vu."
                                     : "Le contrat sur votre tête est soldé. Le Directeur.";
    // 08/10 : en jeu, dans les sacs et la bourse ; le reste (et tout, hors ligne) par courrier (VerserRecompense).
    RemiseRecompense const r =
        VerserRecompense(trans, p, c.cible, cuivre, objets, crees, titre, corps,
                         !banc);   // cible de banc (un bot) : tout par courrier, comme avant
    // L'accuse dans la MEME transaction (voir plus haut). Meme garde de statut que
    // CoaDirSourceMysql::Acquitter ; c'est la seule source en service.
    if (source)
        trans->Append(Acore::StringFormat(
            "UPDATE coa_dir_commande SET statut = 'execute', raison = NULL, vu_le = NOW(3) WHERE id = {}"
            " AND statut = 'en_attente'",
            c.id));
    CharacterDatabase.CommitTransaction(trans);
    DireRemise(p, r, faille ? "Votre part de la faille d'orage"
                     : nuit ? "Votre part de la nuit des morts"
                     : contrat ? "La prime de votre contrat"
                     : soudoiement ? "Le prix de votre silence"
                            : "Votre prime");
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    Journaliser(c, "execute", nullptr);
    LOG_INFO("playerbots",
             "coa directeur recompense: id={} joueur={} cuivre={} objets={} action={} statut=execute en_ligne={}"
             " direct={} direct_cuivre={} direct_objets={} courrier={} courrier_cuivre={} courrier_objets={}{}",
             c.id, c.cible, cuivre, c.texte.empty() ? std::string("-") : c.texte,
             c.action.empty() ? std::string("-") : c.action, p ? 1 : 0, r.direct ? 1 : 0, r.cuivreDirect,
             TexteObjetsRemise(r.objetsDirect), r.courrier ? 1 : 0, r.cuivreCourrier,
             TexteObjetsRemise(r.objetsCourrier), banc ? " banc=1" : "");
}

// ---- paroles (§ 5.2)

constexpr uint32 PAROLE_DELAI_MAX_S = 120;
constexpr uint32 PAROLE_GRACE_MS = 60 * IN_MILLISECONDS;   // echue sans livraison : perdue
constexpr uint32 PAROLE_PAR_BOT_MS = 30 * IN_MILLISECONDS; // une parole par bot et par 30 s
constexpr size_t PAROLES_EN_FILE_MAX = 64;
constexpr float PAROLE_DIRE_YD = 40.0f;                    // ListenRange.Say
constexpr float PAROLE_CRIER_YD = 300.0f;                  // ListenRange.Yell

struct Parole
{
    uint32 id;
    uint32 bot;
    uint32 joueur;
    CoaDirCanal canal;
    std::string texte;
    uint32 poseMs;     // getMSTime() a l'execution de la commande
    uint32 delaiMs;    // livraison au plus tot a poseMs + delaiMs
    bool horsVue = false;   // ParoleAVue : deja differee une fois (compteur paroles_hors_vue)
};
std::vector<Parole> paroles;                           // fil monde seul
std::deque<uint32> parolesAcceptees;                   // getMSTime des paroles acceptees, 60 s glissantes
std::unordered_map<uint32, uint32> derniereParoleBot;  // guid du bot -> getMSTime de sa derniere acceptee

void OublierParolesAnciennes(uint32 maintenantMs)
{
    while (!parolesAcceptees.empty() && getMSTimeDiff(parolesAcceptees.front(), maintenantMs) >= MINUTE * IN_MILLISECONDS)
        parolesAcceptees.pop_front();
    for (auto it = derniereParoleBot.begin(); it != derniereParoleBot.end();)
    {
        if (getMSTimeDiff(it->second, maintenantMs) >= PAROLE_PAR_BOT_MS)
            it = derniereParoleBot.erase(it);
        else
            ++it;
    }
}

// Verbe `faire_parler` : la commande s'accuse `execute` et la parole entre en
// file ; la livraison (LivrerParoles, chaque seconde) dit `livree` ou `perdue`
// dans la ligne `coa directeur parole:`, jamais en base (la commande est deja
// accusee). Les refus de cette fonction, eux, sont des accuses ordinaires.
void ExecuterParole(CoaDirCommande const& c)
{
    // 6. bornes
    if (c.cible == 0 || c.joueur == 0 || c.delaiS > PAROLE_DELAI_MAX_S ||
        !(c.canal == CoaDirCanal::Dire || c.canal == CoaDirCanal::Crier || c.canal == CoaDirCanal::Chuchoter ||
          c.canal == CoaDirCanal::Zone))
    {
        Refuser(c, "bornes");
        return;
    }
    if (!TexteBusValide(c.texte))
    {
        Refuser(c, "texte");
        return;
    }
    Player* bot = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!bot)
    {
        Refuser(c, "hors_ligne");
        return;
    }
    PlayerbotAI* ia = GET_PLAYERBOT_AI(bot);
    if (!ia || !sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        Refuser(c, "pas_un_bot");
        return;
    }
    if (ia->HasGameClientMaster())
    {
        Refuser(c, "maitre_humain");
        return;
    }
    Player* joueur = ObjectAccessor::FindPlayerByLowGUID(c.joueur);
    if (!joueur)
    {
        Refuser(c, "hors_ligne");
        return;
    }
    if (!Humain(joueur))
    {
        Refuser(c, "pas_humain");
        return;
    }
    if (bot->GetTeamId() != joueur->GetTeamId())
    {
        Refuser(c, "faction");
        return;
    }
    // 7. plafonds : `plafond` = AiPlayerbot.Directeur.MaxParolesParMinute, tous
    // bots confondus (et la file bornee) ; `cadence` = une parole par bot et par
    // 30 s (pas `budget`, deja pris par le jeton du bus, controle 5). Comptes a
    // l'ACCEPTATION : une parole perdue a la livraison compte.
    uint32 const maintenantMs = getMSTime();
    OublierParolesAnciennes(maintenantMs);
    if (parolesAcceptees.size() >= sPlayerbotAIConfig.directeurMaxParolesParMinute ||
        paroles.size() >= PAROLES_EN_FILE_MAX)
    {
        Refuser(c, "plafond");
        return;
    }
    if (derniereParoleBot.count(c.cible))
    {
        Refuser(c, "cadence");
        return;
    }
    parolesAcceptees.push_back(maintenantMs);
    derniereParoleBot[c.cible] = maintenantMs;
    paroles.push_back(Parole{c.id, c.cible, c.joueur, c.canal, c.texte, maintenantMs, c.delaiS * IN_MILLISECONDS, false});
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    Accuser(c, "execute", nullptr);
}

// `distance` : du locuteur au joueur a la livraison, -1 si inconnue (l'un des
// deux absent, ou autre carte). `vu` : le joueur a-t-il le locuteur a son client
// (Player::HaveAtClient) ? Say et Yell ne livrent qu'a ceux-la
// (MessageDistDeliverer::SendPacket, GridNotifiers.h), donc au plus a la
// visibilite (Visibility.Distance.Continents = 100 yd en service, lu le 30/09),
// quel que soit ListenRange.Yell (300) : `livree vu=0` = dite, pas entendue.
void JournaliserParole(Parole const& p, char const* statut, char const* raison, float distance = -1.0f,
                       int vu = -1)
{
    LOG_INFO("playerbots", "coa directeur parole: id={} guid={} joueur={} canal={} statut={} raison={} distance={:.0f}"
             " vu={}", p.id, p.bot, p.joueur, NomCanal(p.canal), statut, raison ? raison : "-", distance, vu);
}

// Canal de zone du bot, cherche COMME PlayerbotAI::SayToChannel (id
// ZoneChannelId, nom qui contient la zone ou l'aire), mais sans parler : on
// veut savoir QUEL canal, et si le bot en est membre (Channel::Say se tait
// sinon, et SayToChannel rend vrai quand meme).
Channel* CanalDeZone(Player* bot, PlayerbotAI* ia)
{
    ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId());
    AreaTableEntry const* zone = ia->GetCurrentZone();
    if (!cMgr || !zone)
        return nullptr;
    std::string const nomZone = PlayerbotAI::GetLocalizedAreaName(zone);
    AreaTableEntry const* aire = ia->GetCurrentArea();
    std::string const nomAire = aire ? PlayerbotAI::GetLocalizedAreaName(aire) : "";
    for (auto const& [cle, canal] : cMgr->GetChannels())
    {
        if (!canal || canal->GetChannelId() != sPlayerbotAIConfig.zoneChannelId || canal->GetName().empty())
            continue;
        if ((!nomZone.empty() && canal->GetName().find(nomZone) != std::string::npos) ||
            (!nomAire.empty() && canal->GetName().find(nomAire) != std::string::npos))
            return canal;
    }
    return nullptr;
}

// Livraison, chaque seconde, fil monde, cartes a l'arret : Say et Yell visitent
// les voisins de la grille sans course (P-137). Une parole echue est livree ou
// perdue a ce passage ; seul un bot mort ou en combat la fait attendre, au plus
// PAROLE_GRACE_MS apres son heure.
void LivrerParoles()
{
    if (paroles.empty())
        return;
    uint32 const maintenantMs = getMSTime();
    std::vector<Parole> restantes;
    restantes.reserve(paroles.size());
    std::vector<Parole> lot;
    lot.swap(paroles);
    for (Parole& p : lot)
    {
        uint32 const ecoule = getMSTimeDiff(p.poseMs, maintenantMs);
        if (ecoule < p.delaiMs)
        {
            restantes.push_back(std::move(p));
            continue;
        }
        char const* perdue = nullptr;
        char const* reserve = nullptr;   // livree, mais sans preuve qu'elle a ete entendue
        Player* bot = ObjectAccessor::FindPlayerByLowGUID(p.bot);
        Player* joueur = ObjectAccessor::FindPlayerByLowGUID(p.joueur);
        PlayerbotAI* ia = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        bool const memeCarte = bot && joueur && bot->GetMapId() == joueur->GetMapId();
        float const distance = memeCarte ? bot->GetExactDist(joueur) : -1.0f;
        int const vu = (bot && joueur) ? (memeCarte && joueur->HaveAtClient(bot) ? 1 : 0) : -1;
        if (!bot || !ia)
            perdue = "locuteur_absent";
        else if (!joueur)
            perdue = "joueur_absent";
        else if (CoaDirecteur::EstArrete(p.joueur))
            perdue = "arret";
        else if (bot->GetTeamId() != joueur->GetTeamId())
            perdue = "faction";
        else if (ia->HasGameClientMaster())
            perdue = "maitre_humain";
        else if (!bot->IsAlive() || bot->IsInCombat())
        {
            if (ecoule < p.delaiMs + PAROLE_GRACE_MS)
            {
                restantes.push_back(std::move(p));
                continue;
            }
            perdue = "indisponible";
        }
        // 30/09 (demande 43) : `dire` et `crier` ne sont entendus que d'un joueur qui a le
        // locuteur a son client (vu, plus haut). Option ParoleAVue : sinon la parole
        // ATTEND (le locuteur ou le joueur peut encore s'approcher), au plus
        // PAROLE_GRACE_MS apres son heure, puis `perdue raison=hors_vue`. Option a 0 :
        // comme avant (dite, `livree vu=0`).
        bool const aPortee = memeCarte && distance <= (p.canal == CoaDirCanal::Dire ? PAROLE_DIRE_YD : PAROLE_CRIER_YD);
        if (!perdue && sPlayerbotAIConfig.directeurParoleAVue && aPortee && vu != 1 &&
            (p.canal == CoaDirCanal::Dire || p.canal == CoaDirCanal::Crier))
        {
            if (!p.horsVue)
            {
                p.horsVue = true;
                CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PAROLES_HORS_VUE);
            }
            if (ecoule < p.delaiMs + PAROLE_GRACE_MS)
            {
                restantes.push_back(std::move(p));
                continue;
            }
            perdue = "hors_vue";
        }
        if (!perdue)
        {
            switch (p.canal)
            {
                case CoaDirCanal::Dire:
                    if (!memeCarte || distance > PAROLE_DIRE_YD)
                        perdue = "loin";
                    else
                        ia->Say(p.texte);
                    break;
                case CoaDirCanal::Crier:
                    if (!memeCarte || distance > PAROLE_CRIER_YD)
                        perdue = "loin";
                    else
                        ia->Yell(p.texte);
                    break;
                case CoaDirCanal::Chuchoter:
                    if (!ia->Whisper(p.texte, joueur->GetName()))
                        perdue = "joueur_absent";
                    break;
                case CoaDirCanal::Zone:
                {
                    Channel* canal = bot->GetZoneId() == joueur->GetZoneId() ? CanalDeZone(bot, ia) : nullptr;
                    if (bot->GetZoneId() != joueur->GetZoneId())
                        perdue = "autre_zone";
                    else if (!canal || !bot->IsInChannel(canal))
                        perdue = "hors_canal";
                    else
                    {
                        // LIMITE (revue D3) : Player::IsInChannel ne compare que
                        // l'id du canal, commun a tous les « General » ; et
                        // Channel::IsOn est prive. Si le bot n'est pas membre DE
                        // CE canal, Channel::Say se tait (NotMember). La ligne dit
                        // donc `livree raison=non_prouve` : seul le joueur, a
                        // l'ecran, prouve la livraison (epreuve du § 5.5).
                        canal->Say(bot->GetGUID(), p.texte, LANG_UNIVERSAL);
                        ia->LogChat("tells channel " + canal->GetName(), p.texte);
                        reserve = "non_prouve";
                    }
                    break;
                }
                default:
                    perdue = "canal";
                    break;
            }
        }
        if (perdue)
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PAROLES_PERDUES);
            JournaliserParole(p, "perdue", perdue, distance, vu);
        }
        else
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PAROLES_LIVREES);
            JournaliserParole(p, "livree", reserve, distance, vu);
        }
    }
    // Une commande executee pendant la boucle ? Impossible : on est dans
    // Entretien, pas dans Recevoir. Ceinture tout de meme : on ajoute.
    for (Parole& p : paroles)
        restantes.push_back(std::move(p));
    paroles.swap(restantes);
}

// Levee de paroles en file (lever, stop, bus eteint) : statut `leve` en base,
// ligne `parole:` perdue.
template <typename Predicat>
size_t LeverParolesSi(Predicat lever, char const* raison)
{
    if (paroles.empty())
        return 0;
    size_t n = 0;
    for (auto it = paroles.begin(); it != paroles.end();)
    {
        if (!lever(*it))
        {
            ++it;
            continue;
        }
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PAROLES_PERDUES);
        JournaliserParole(*it, "perdue", raison);
        LOG_INFO("playerbots", "coa directeur cmd: id={} verbe=faire_parler cible={} joueur={} statut=leve raison={}",
                 it->id, it->bot, it->joueur, raison);
        if (source)
            source->Clore(it->id, "leve", raison);
        it = paroles.erase(it);
        ++n;
    }
    return n;
}

// ---- ciel (§ 6.2)

constexpr int32 MINUTES_PAR_JOUR = 24 * 60;

struct Ciel
{
    uint32 id;
    uint32 joueur;    // guid du joueur qui le voit (la cible)
    uint32 minute;    // minute du ciel voulue, heure de la MACHINE (UTC ici, decision n° 13)
    uint32 finS;      // Maintenant()
    uint32 carte;     // carte a la pose : un changement de carte renvoie l'heure vraie
};
std::unordered_map<uint32, Ciel> ciels;   // cle : guid du joueur ; fil monde seul

// Meme paquet que Player::SendInitialPacketsBeforeAddToMap (SMSG_LOGIN_SETTIMESPEED :
// heure empaquetee, vitesse, uint32 0), au seul joueur. SUPPOSE, a eprouver
// (§ 6.2) : que le client 3.3.5a l'accepte hors connexion.
void EnvoyerHeure(Player* p, time_t t)
{
    WorldPacket data(SMSG_LOGIN_SETTIMESPEED, 4 + 4 + 4);
    data.AppendPackedTime(t);
    data << float(0.01666667f);
    data << uint32(0);
    p->SendDirectMessage(&data);
}

// L'instant dont l'heure LOCALE de la machine est `minute` aujourd'hui.
time_t InstantDeMinute(uint32 minute)
{
    time_t const maintenant = GameTime::GetGameTime().count();
    std::tm const lt = Acore::Time::TimeBreakdown(maintenant);
    return maintenant - (lt.tm_hour * HOUR + lt.tm_min * MINUTE + lt.tm_sec) + static_cast<time_t>(minute) * MINUTE;
}

// Fin d'un ciel retire (hors de toute iteration). `renvoyer` : l'heure vraie
// est renvoyee si le joueur est encore la, sur la carte de la pose.
void FinirCiel(Ciel const& ciel, char const* statut, char const* raison, char const* motif, bool renvoyer)
{
    if (renvoyer)
        if (Player* p = ObjectAccessor::FindPlayerByLowGUID(ciel.joueur))
            if (p->GetMapId() == ciel.carte)
                EnvoyerHeure(p, GameTime::GetGameTime().count());
    LOG_INFO("playerbots", "coa directeur ciel: id={} guid={} minute={} motif={}", ciel.id, ciel.joueur, ciel.minute,
             motif);
    if (statut)
    {
        LOG_INFO("playerbots", "coa directeur cmd: id={} verbe=heure_du_jour cible={} joueur={} statut={} raison={}",
                 ciel.id, ciel.joueur, ciel.joueur, statut, raison ? raison : "-");
        if (source)
            source->Clore(ciel.id, statut, raison);
    }
}

void ExecuterCiel(CoaDirCommande const& c)
{
    if (!CibleJoueurValide(c) || c.valeur < 0 || c.valeur >= MINUTES_PAR_JOUR ||
        c.dureeS < AFFECTATION_DUREE_MIN_S || c.dureeS > AFFECTATION_DUREE_MAX_S)
    {
        Refuser(c, "bornes");
        return;
    }
    Player* p = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!p)
    {
        Refuser(c, "hors_ligne");
        return;
    }
    if (!Humain(p))
    {
        Refuser(c, "pas_humain");
        return;
    }
    Ciel const nouveau{c.id, c.cible, static_cast<uint32>(c.valeur), Maintenant() + c.dureeS, p->GetMapId()};
    auto it = ciels.find(c.cible);
    if (it != ciels.end())
    {
        // Un ciel par joueur, le dernier remplace ; pas de renvoi de l'heure
        // vraie entre les deux (le nouveau paquet part juste apres).
        Ciel const ancien = it->second;
        ciels.erase(it);
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REMPLACEES);
        std::string const par = Acore::StringFormat("par_{}", c.id);
        FinirCiel(ancien, "remplace", par.c_str(), "remplace", false);
    }
    ciels[c.cible] = nouveau;
    EnvoyerHeure(p, InstantDeMinute(nouveau.minute));
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_CIELS);
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots", "coa directeur ciel: id={} guid={} minute={} motif=pose fin_s={}", c.id, c.cible,
             nouveau.minute, nouveau.finS);
    Accuser(c, "execute", nullptr);
}

template <typename Predicat>
size_t LeverCielsSi(Predicat lever, char const* raison)
{
    if (ciels.empty())
        return 0;
    std::vector<Ciel> leves;
    for (auto it = ciels.begin(); it != ciels.end();)
    {
        if (lever(it->second))
        {
            leves.push_back(it->second);
            it = ciels.erase(it);
        }
        else
            ++it;
    }
    for (Ciel const& ciel : leves)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        FinirCiel(ciel, "leve", raison, raison, true);
    }
    return leves.size();
}

// Chaque seconde : echeance (heure vraie renvoyee), deconnexion (l'etat tombe,
// la reconnexion renvoie l'heure vraie), changement de carte (le coeur a deja
// renvoye l'heure vraie, MovementHandler -> SendInitialPacketsBeforeAddToMap :
// on ne renvoie rien).
void SuivreCiels()
{
    if (ciels.empty())
        return;
    uint32 const maintenant = Maintenant();
    struct Fin { Ciel ciel; char const* motif; bool renvoyer; };
    std::vector<Fin> fins;
    for (auto it = ciels.begin(); it != ciels.end();)
    {
        Player* p = JoueurConnecte(it->first);
        char const* motif = nullptr;
        bool renvoyer = false;
        if (!p)
            motif = "hors_ligne";
        else if (!p->IsInWorld() || p->IsBeingTeleported())
        {
            ++it;
            continue;
        }
        else if (p->GetMapId() != it->second.carte)
            motif = "carte";
        else if (it->second.finS <= maintenant)
        {
            motif = "echue";
            renvoyer = true;
        }
        if (!motif)
        {
            ++it;
            continue;
        }
        fins.push_back(Fin{it->second, motif, renvoyer});
        it = ciels.erase(it);
    }
    for (Fin const& f : fins)
    {
        if (f.renvoyer)
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ECHUES);
        FinirCiel(f.ciel, nullptr, nullptr, f.motif, f.renvoyer);
    }
}

// ---- meteo (§ 6.3)

struct MeteoJoueur
{
    uint32 id;
    uint32 joueur;
    uint32 zone;           // zone a la pose : en sortir met fin a la meteo
    uint32 etat;           // WeatherState
    float intensite;       // ]0,0001 ; 0,9999[
    uint32 finS;           // Maintenant()
    uint32 dernierEnvoiMs; // getMSTime() du dernier paquet
};
std::unordered_map<uint32, MeteoJoueur> meteos;   // cle : guid du joueur ; fil monde seul

// Etats permis : ceux de l'enum WeatherState (Weather/Weather.h).
bool EtatMeteoPermis(int32 v)
{
    switch (v)
    {
        case WEATHER_STATE_FINE: case WEATHER_STATE_FOG: case WEATHER_STATE_LIGHT_RAIN:
        case WEATHER_STATE_MEDIUM_RAIN: case WEATHER_STATE_HEAVY_RAIN: case WEATHER_STATE_LIGHT_SNOW:
        case WEATHER_STATE_MEDIUM_SNOW: case WEATHER_STATE_HEAVY_SNOW: case WEATHER_STATE_LIGHT_SANDSTORM:
        case WEATHER_STATE_MEDIUM_SANDSTORM: case WEATHER_STATE_HEAVY_SANDSTORM: case WEATHER_STATE_THUNDERS:
        case WEATHER_STATE_BLACKRAIN: case WEATHER_STATE_BLACKSNOW:
            return true;
        default:
            return false;
    }
}

void EnvoyerMeteo(Player* p, MeteoJoueur const& m)
{
    WorldPackets::Misc::Weather paquet(static_cast<WeatherState>(m.etat), m.intensite);
    p->SendDirectMessage(paquet.Write());
}

// Retablissement : le temps de la zone ou se trouve le joueur, par les voies du
// coeur. Map::SendZoneWeather ne dit RIEN pour une zone sans info dynamique (35
// zones seulement ont des donnees) ; on envoie donc d'abord le beau temps, que
// SendZoneWeather remplace aussitot quand la zone a son temps. Sans cela, la
// pluie du Directeur suivrait le joueur. (L'ordre inverse de la spec, qui
// supposait qu'on sache si la zone a une info : Map::_zoneDynamicInfo est prive.)
void RetablirMeteo(Player* p)
{
    Weather::SendFineWeatherUpdateToPlayer(p);
    if (Map* map = p->GetMap())
        map->SendZoneWeather(p->GetZoneId(), p);
}

void FinirMeteo(MeteoJoueur const& m, char const* statut, char const* raison, char const* motif, bool retablir)
{
    if (retablir)
        if (Player* p = ObjectAccessor::FindPlayerByLowGUID(m.joueur))
            RetablirMeteo(p);
    LOG_INFO("playerbots", "coa directeur meteo: id={} guid={} etat={} zone={} motif={}", m.id, m.joueur, m.etat, m.zone,
             motif);
    if (statut)
    {
        LOG_INFO("playerbots", "coa directeur cmd: id={} verbe=meteo cible={} joueur={} statut={} raison={}", m.id,
                 m.joueur, m.joueur, statut, raison ? raison : "-");
        if (source)
            source->Clore(m.id, statut, raison);
    }
}

void ExecuterMeteo(CoaDirCommande const& c)
{
    if (!CibleJoueurValide(c) || !EtatMeteoPermis(c.valeur) || !std::isfinite(c.intensite) || c.intensite <= 0.0f ||
        c.intensite >= 1.0f || c.dureeS < AFFECTATION_DUREE_MIN_S || c.dureeS > AFFECTATION_DUREE_MAX_S)
    {
        Refuser(c, "bornes");
        return;
    }
    Player* p = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!p)
    {
        Refuser(c, "hors_ligne");
        return;
    }
    if (!Humain(p))
    {
        Refuser(c, "pas_humain");
        return;
    }
    // Bornes du coeur (Weather::UpdateWeather) : ]0,0001 ; 0,9999[.
    float const intensite = std::min(0.9999f, std::max(0.0001f, c.intensite));
    MeteoJoueur const nouvelle{c.id, c.cible, p->GetZoneId(), static_cast<uint32>(c.valeur), intensite,
                               Maintenant() + c.dureeS, getMSTime()};
    auto it = meteos.find(c.cible);
    if (it != meteos.end())
    {
        // La nouvelle remplace ; pas de retablissement entre les deux.
        MeteoJoueur const ancienne = it->second;
        meteos.erase(it);
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REMPLACEES);
        std::string const par = Acore::StringFormat("par_{}", c.id);
        FinirMeteo(ancienne, "remplace", par.c_str(), "remplace", false);
    }
    meteos[c.cible] = nouvelle;
    EnvoyerMeteo(p, nouvelle);
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_METEOS);
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots", "coa directeur meteo: id={} guid={} etat={} zone={} motif=pose intensite={:.2f} fin_s={}",
             c.id, c.cible, nouvelle.etat, nouvelle.zone, nouvelle.intensite, nouvelle.finS);
    Accuser(c, "execute", nullptr);
}

template <typename Predicat>
size_t LeverMeteosSi(Predicat lever, char const* raison)
{
    if (meteos.empty())
        return 0;
    std::vector<MeteoJoueur> levees;
    for (auto it = meteos.begin(); it != meteos.end();)
    {
        if (lever(it->second))
        {
            levees.push_back(it->second);
            it = meteos.erase(it);
        }
        else
            ++it;
    }
    for (MeteoJoueur const& m : levees)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        FinirMeteo(m, "leve", raison, raison, true);
    }
    return levees.size();
}

// Chaque seconde : echeance et changement de zone (fin + retablissement),
// deconnexion (l'etat tombe), sinon reaffirmation toutes les
// MeteoReaffirmationSecondes -- une regeneration de WeatherMgr, qui part du fil
// de CARTE (Map::UpdateWeather), peut ecraser le paquet ; le Directeur n'y
// ecrit rien, il renvoie le sien depuis le fil monde.
void SuivreMeteos()
{
    if (meteos.empty())
        return;
    uint32 const maintenant = Maintenant();
    uint32 const maintenantMs = getMSTime();
    uint32 const periodeMs = sPlayerbotAIConfig.directeurMeteoReaffirmationSecondes * IN_MILLISECONDS;
    struct Fin { MeteoJoueur m; char const* motif; bool retablir; };
    std::vector<Fin> fins;
    for (auto it = meteos.begin(); it != meteos.end();)
    {
        MeteoJoueur& m = it->second;
        Player* p = JoueurConnecte(it->first);
        char const* motif = nullptr;
        if (!p)
            motif = "hors_ligne";
        else if (!p->IsInWorld() || p->IsBeingTeleported())
        {
            ++it;
            continue;
        }
        else if (p->GetZoneId() != m.zone)
            motif = "zone";
        else if (m.finS <= maintenant)
            motif = "echue";
        if (motif)
        {
            fins.push_back(Fin{m, motif, p != nullptr});
            it = meteos.erase(it);
            continue;
        }
        if (getMSTimeDiff(m.dernierEnvoiMs, maintenantMs) >= periodeMs)
        {
            EnvoyerMeteo(p, m);
            m.dernierEnvoiMs = maintenantMs;
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REAFFIRMATIONS_METEO);
            LOG_INFO("playerbots", "coa directeur meteo: id={} guid={} etat={} zone={} motif=reaffirme", m.id, m.joueur,
                     m.etat, m.zone);
        }
        ++it;
    }
    for (Fin const& f : fins)
    {
        if (f.retablir && std::string_view(f.motif) == "echue")
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ECHUES);
        FinirMeteo(f.m, nullptr, nullptr, f.motif, f.retablir);
    }
}

// ------------------------------------------------------------------ lot C : verbe `evenement` (04/10/2026)
//
// coa-project/docs/EVENEMENTS-grimoire-directeur.md § 2.1-2.3, § 3.5, § 5.1-5.2 (lot C) ;
// rendu a l'ecran : docs/EVENEMENTS-banc-rendu-verdicts.md (banc du 04/10, 21:14-22:12 Paris).
// UN verbe de bus, `evenement`, dont `texte` porte la SOUS-ACTION dans une grammaire fermee :
//   ambiance                  aure de filtre d'ecran posee sur le joueur, reaffirmee, retiree
//   son | son:point           PlayDirectSound au joueur | PlayDistanceSound d'un declencheur (son
//                             POSITIONNEL : liste a part EvenementSonsPoint, coupure DBC >= 100 yd)
//   musique                   PlayDirectMusic au joueur (valeur 0 : arret, SUPPOSE)
//   effet:<lieu>:<forme>      lieu joueur | point | haut ; forme kit | sort :
//                             kit = SMSG_PLAY_SPELL_VISUAL (le KIT, jamais CastSpell : 9 sur 10
//                             visibles au banc) ; sort = aure cosmetique durable posee par un
//                             declencheur invisible sur lui-meme (`.aura` du banc) ; le
//                             declencheur ne vit pas plus que l'aure (duree du sort, Spell.dbc)
//   annoncer:<message>        emote de boss (CHAT_MSG_RAID_BOSS_EMOTE) au seul joueur
// `cible` = le joueur humain (`joueur` 0 ou le meme), `valeur` = l'id RESOLU par le service,
// `duree_s` = la duree (ambiance, musique, sort), `carte/x/y/z` = le point (lieux point, haut).
// LISTES BLANCHES FERMEES (AiPlayerbot.Directeur.Evenement*, lecture stricte : un morceau mal
// forme et AUCUN id n'est permis) ; defauts = les ids notes ✔ au banc du 04/10 (10008 retire :
// « rien » au banc, volume 0,12 dans SoundEntries.dbc).
// Envoye AU SEUL JOUEUR (R7, P-134) : paquet direct, declencheur visible de son seul invocateur
// (TempSummon::SetVisibleBySummonerOnly, Object.cpp:1865). UNE EXCEPTION : l'aure d'ambiance est
// une vraie aure du joueur ; sa mise a jour (SMSG_AURA_UPDATE, AuraApplication::ClientUpdate,
// SpellAuras.cpp:240) part a tout l'entourage, qui voit donc son ICONE en le ciblant. Le filtre
// d'ecran ne devrait s'appliquer que chez le porteur (SUPPOSE, a eprouver en jeu).
// Registre au fil monde seul ; `Lever…Si` sur lever, stop (arret), bus eteint.
// Journal : `coa directeur evenement: id= sous_action= lieu= forme= joueur= valeur= motif=`.

constexpr uint32 EV_AMBIANCE_DUREE_MIN_S = 30;     // schema (scenario_evenement.schema.json : 30-1800)
constexpr uint32 EV_DUREE_MAX_S = 3600;
constexpr uint32 EV_MUSIQUE_DUREE_MIN_S = 10;
constexpr uint32 EV_SORT_DUREE_MIN_S = 5;
constexpr uint32 EV_DECLENCHEUR_KIT_MS = 10 * IN_MILLISECONDS;   // vie d'un declencheur de kit ou de son
constexpr uint32 EV_ENVOI_DIFFERE_MS = 1000;       // le client doit connaitre le declencheur avant le paquet
constexpr float EV_SOL_ECART_MAX_YD = 15.0f;       // z du service et sol lu : au-dela, refus `sol`
constexpr size_t EV_DECLENCHEURS_MAX = 256;         // tout le royaume (64 jusqu'au 18/10 : 8 evenements simultanes)
constexpr size_t EV_ENVOIS_MAX = 512;               // 128 jusqu'au 18/10

enum class EvSous : uint8 { Aucune, Ambiance, Son, Musique, Effet, Annoncer };
enum class EvLieu : uint8 { Joueur, Point, Haut };
enum class EvForme : uint8 { Aucune, Kit, Sort };

struct EvLu
{
    EvSous sous = EvSous::Aucune;
    EvLieu lieu = EvLieu::Joueur;
    EvForme forme = EvForme::Aucune;
    std::string message;   // annoncer
};

char const* NomEvSous(EvSous s)
{
    switch (s)
    {
        case EvSous::Ambiance: return "ambiance";
        case EvSous::Son:      return "son";
        case EvSous::Musique:  return "musique";
        case EvSous::Effet:    return "effet";
        case EvSous::Annoncer: return "annoncer";
        default:               return "inconnue";
    }
}

char const* NomEvLieu(EvLieu l)
{
    return l == EvLieu::Point ? "point" : l == EvLieu::Haut ? "haut" : "joueur";
}

char const* NomEvForme(EvForme f)
{
    return f == EvForme::Kit ? "kit" : f == EvForme::Sort ? "sort" : "-";
}

// `texte` -> sous-action. Grammaire FERMEE, casse exacte ; tout le reste : faux (refus `texte`).
bool LireSousAction(std::string const& t, EvLu& lu)
{
    lu = EvLu();
    if (t == "ambiance")      { lu.sous = EvSous::Ambiance; return true; }
    if (t == "son")           { lu.sous = EvSous::Son; return true; }
    if (t == "son:point")     { lu.sous = EvSous::Son; lu.lieu = EvLieu::Point; return true; }
    if (t == "musique")       { lu.sous = EvSous::Musique; return true; }
    if (t == "effet:joueur:kit") { lu.sous = EvSous::Effet; lu.forme = EvForme::Kit; return true; }
    if (t == "effet:point:kit")  { lu.sous = EvSous::Effet; lu.lieu = EvLieu::Point; lu.forme = EvForme::Kit; return true; }
    if (t == "effet:haut:kit")   { lu.sous = EvSous::Effet; lu.lieu = EvLieu::Haut; lu.forme = EvForme::Kit; return true; }
    if (t == "effet:point:sort") { lu.sous = EvSous::Effet; lu.lieu = EvLieu::Point; lu.forme = EvForme::Sort; return true; }
    if (t == "effet:haut:sort")  { lu.sous = EvSous::Effet; lu.lieu = EvLieu::Haut; lu.forme = EvForme::Sort; return true; }
    static constexpr char const PREFIXE_ANNONCE[] = "annoncer:";
    size_t const n = sizeof(PREFIXE_ANNONCE) - 1;
    if (t.size() > n && t.compare(0, n, PREFIXE_ANNONCE) == 0)
    {
        lu.message = t.substr(n);
        // `%` : le cadre d'emote de boss du client formate le texte avec le nom de
        // l'emetteur (%s) ; un `%` seul y serait interprete. Refuse, comme au service.
        if (lu.message.find('%') != std::string::npos ||
            lu.message.find_first_not_of(' ') == std::string::npos)
            return false;
        lu.sous = EvSous::Annoncer;
        return true;
    }
    return false;
}

// L'id `id` est-il dans la liste `liste` (« 800325,92883 ») ? Lecture STRICTE, comme
// ObjetRecompensePermis : un morceau mal forme (vide compris) et AUCUN id n'est permis.
bool IdDeListe(std::string const& liste, uint32 id)
{
    if (!id || liste.find_first_not_of(" \t") == std::string::npos)
        return false;
    bool trouve = false;
    size_t debut = 0;
    while (debut <= liste.size())
    {
        size_t fin = liste.find(',', debut);
        if (fin == std::string::npos)
            fin = liste.size();
        std::string morceau = liste.substr(debut, fin - debut);
        morceau.erase(std::remove_if(morceau.begin(), morceau.end(), [](char ch) { return ch == ' ' || ch == '\t'; }),
                      morceau.end());
        uint32 v = 0;
        if (!EntierRecompense(morceau, v) || !v)
            return false;
        trouve = trouve || v == id;
        debut = fin + 1;
    }
    return trouve;
}

struct EvAmbiance
{
    uint32 id;
    uint32 joueur;
    uint32 sort;
    uint32 finS;           // Maintenant()
    uint32 carte;          // carte a la pose : en changer met fin a l'ambiance
    uint32 dernierMs;      // getMSTime() de la derniere (re)pose
};
std::unordered_map<uint32, EvAmbiance> evAmbiances;   // cle : guid du joueur ; fil monde seul

struct EvMusique
{
    uint32 id;
    uint32 joueur;
    uint32 musique;
    uint32 finS;
};
std::unordered_map<uint32, EvMusique> evMusiques;     // cle : guid du joueur ; fil monde seul

struct EvDeclencheur
{
    uint32 id;
    uint32 joueur;
    ObjectGuid guid;
    uint32 carte;
    uint32 finS;           // Maintenant() ; le TempSummon s'efface seul a son terme
    EvSous sous;
    EvForme forme;
    uint32 valeur;
};
std::vector<EvDeclencheur> evDeclencheurs;            // fil monde seul

struct EvEnvoi
{
    uint32 id;
    uint32 joueur;
    ObjectGuid guid;       // le declencheur
    bool kit;              // vrai : kit visuel ; faux : son
    uint32 valeur;
    uint32 poseMs;         // getMSTime() de l'invocation
};
std::vector<EvEnvoi> evEnvois;                         // fil monde seul

// SMSG_PLAY_SPELL_VISUAL, meme forme que Unit::SendPlaySpellVisual(guid, id) (Unit.cpp),
// mais au SEUL joueur (SendDirectMessage) au lieu de SendMessageToSet.
void EnvoyerKit(Player* p, ObjectGuid guid, uint32 kit)
{
    WorldPacket data(SMSG_PLAY_SPELL_VISUAL, 8 + 4);
    data << guid;
    data << uint32(kit);   // SpellVisualKit.dbc
    p->SendDirectMessage(&data);
}

// Duree bornee de l'aure d'ambiance : jamais plus que la periode de reaffirmation + 10 s,
// ni que l'echeance. Une aure sauvee avec le personnage (deconnexion, sauvegarde
// automatique, plantage) s'eteint donc d'elle-meme peu apres la reconnexion -- 800325 est
// INFINIE dans Spell.dbc : posee telle quelle, elle survivrait a tout.
int32 DureeAureAmbianceMs(EvAmbiance const& a)
{
    uint32 const periodeS = sPlayerbotAIConfig.directeurEvenementReaffirmationSecondes;
    uint32 const maintenant = Maintenant();
    uint32 const resteS = a.finS > maintenant ? a.finS - maintenant : 1;
    return static_cast<int32>(std::min(periodeS + 10, resteS) * IN_MILLISECONDS);
}

bool PoserAureAmbiance(Player* p, EvAmbiance const& a)
{
    Aura* aure = p->AddAura(a.sort, p);
    if (!aure)
        return false;
    int32 const ms = DureeAureAmbianceMs(a);
    aure->SetMaxDuration(ms);
    aure->SetDuration(ms);
    aure->SetNeedClientUpdateForTargets();
    return true;
}

void RetirerAureAmbiance(Player* p, EvAmbiance const& a)
{
    // Seule l'aure posee par le joueur sur lui-meme (AddAura : lanceur = p).
    p->RemoveAurasDueToSpell(a.sort, p->GetGUID());
}

void JournaliserEvenement(uint32 id, EvSous sous, EvLieu lieu, EvForme forme, uint32 joueur, uint32 valeur,
                          char const* motif)
{
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action={} lieu={} forme={} joueur={} valeur={} motif={}",
             id, NomEvSous(sous), NomEvLieu(lieu), NomEvForme(forme), joueur, valeur, motif);
}

void CloreEvenement(uint32 id, uint32 joueur, char const* statut, char const* raison)
{
    if (!statut)
        return;
    LOG_INFO("playerbots", "coa directeur cmd: id={} verbe=evenement cible={} joueur={} statut={} raison={}", id,
             joueur, joueur, statut, raison ? raison : "-");
    if (source)
        source->Clore(id, statut, raison);
}

void FinirAmbiance(EvAmbiance const& a, char const* statut, char const* raison, char const* motif, bool retirer)
{
    if (retirer)
        if (Player* p = ObjectAccessor::FindPlayerByLowGUID(a.joueur))
            RetirerAureAmbiance(p, a);
    JournaliserEvenement(a.id, EvSous::Ambiance, EvLieu::Joueur, EvForme::Aucune, a.joueur, a.sort, motif);
    CloreEvenement(a.id, a.joueur, statut, raison);
}

// Arret de la musique : SMSG_PLAY_MUSIC d'id 0. SUPPOSE (non lu dans le client) : que le
// client 3.3.5a coupe alors la musique en cours ; a eprouver en jeu. Sans effet, la musique
// va a son terme (banc du 04/10 : « la musique va jusqu'au bout »).
void FinirMusique(EvMusique const& m, char const* statut, char const* raison, char const* motif, bool couper)
{
    if (couper)
        if (Player* p = ObjectAccessor::FindPlayerByLowGUID(m.joueur))
            p->PlayDirectMusic(0, p);
    JournaliserEvenement(m.id, EvSous::Musique, EvLieu::Joueur, EvForme::Aucune, m.joueur, m.musique, motif);
    CloreEvenement(m.id, m.joueur, statut, raison);
}

void FinirDeclencheur(EvDeclencheur const& d, char const* statut, char const* raison, char const* motif, bool effacer)
{
    if (effacer)
        if (Map* map = sMapMgr->FindBaseNonInstanceMap(d.carte))
            if (Creature* cr = map->GetCreature(d.guid))
                cr->DespawnOrUnsummon();
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action={} lieu=declencheur forme={} joueur={} valeur={}"
             " motif={}", d.id, NomEvSous(d.sous), NomEvForme(d.forme), d.joueur, d.valeur, motif);
    // Un kit ou un son ponctuel a deja ete accuse `execute` : seule sa levee explicite
    // (lever, arret, bus eteint) est ecrite, jamais son terme naturel.
    CloreEvenement(d.id, d.joueur, statut, raison);
}

// Un declencheur invisible, visible de son SEUL invocateur, pose a (x, y, z), vivant `vieMs`.
// Player::SummonCreature (WorldObject::SummonCreature, Object.cpp:2383) avec
// visibleBySummonerOnly ; WORLD_TRIGGER (Unit.h:38, 12999 « World Invisible Trigger »,
// flags_extra 130 : declencheur, non selectionnable). Nul : invocation refusee par le coeur.
Creature* PoserDeclencheur(Player* p, float x, float y, float z, uint32 vieMs, bool enHauteur)
{
    TempSummon* s = p->SummonCreature(WORLD_TRIGGER, x, y, z, 0.0f, TEMPSUMMON_TIMED_DESPAWN, vieMs, nullptr, true);
    if (!s)
        return nullptr;
    if (enHauteur)
        s->SetDisableGravity(true);   // SUPPOSE utile : rien ne le fait tomber sans mouvement
    return s;
}

void RefuserEvenement(CoaDirCommande const& c, EvLu const& lu, char const* raison)
{
    JournaliserEvenement(c.id, lu.sous, lu.lieu, lu.forme, c.cible, static_cast<uint32>(std::max(c.valeur, 0)),
                         raison);
    Refuser(c, raison);
}

// Lot D des evenements (05/10/2026) : definis plus bas, APRES le lot C (bloc « lot D des evenements »).
bool EstApparition(std::string const& t);
void ExecuterApparition(CoaDirCommande const& c);
template <typename Predicat>
size_t LeverInvoquesSi(Predicat lever, char const* raison);
// Lot P des evenements (06/10/2026) : definis dans CoaDirecteurPortail.inc, inclus apres le lot D.
bool EstPortail(std::string const& t);
void ExecuterPortail(CoaDirCommande const& c);
template <typename Predicat>
size_t LeverPortailsSi(Predicat lever, char const* raison);
// Lot G des evenements (08/10/2026) : definis dans CoaDirecteurGardien.inc, inclus apres le lot P.
bool EstCoffre(std::string const& t);
void ExecuterCoffre(CoaDirCommande const& c);
template <typename Predicat>
size_t LeverCoffresSi(Predicat lever, char const* raison);
// Gisement d'apres-evenement (19/10/2026) : defini dans CoaDirecteurGardien.inc, apres le coffre.
bool EstGisement(std::string const& t);
void ExecuterGisement(CoaDirCommande const& c);
// Objectifs d'evenement (10/10/2026) : definis dans CoaDirecteurObjectifs.inc, inclus apres le lot G.
bool EstObjectif(std::string const& t);
void ExecuterObjectif(CoaDirCommande const& c);
template <typename Predicat>
size_t LeverObjectifsSi(Predicat lever, char const* raison);
// Convoi v2 (13/10/2026) : la barricade, definie dans CoaDirecteurConvoi.inc.
bool EstBarricade(std::string const& t);
void ExecuterBarricade(CoaDirCommande const& c);
template <typename Predicat>
size_t LeverBarricadesSi(Predicat lever, char const* raison);
// Lot D5 (15/10/2026) : creatures attirees et soigneurs, definis dans CoaDirecteurImprevu.inc. Leves avec les
// evenements (memes trois appelants : `lever`, `.directeur stop`, bus eteint), predicat (id, joueur).
template <typename Predicat>
size_t LeverImprevuSi(Predicat lever, char const* raison);

void ExecuterEvenement(CoaDirCommande const& c)
{
    // Lot D (05/10/2026) : invoquer, ordre, retirer ont leur propre grammaire et leurs gardes.
    if (EstApparition(c.texte))
    {
        ExecuterApparition(c);
        return;
    }
    if (EstPortail(c.texte))   // lot P (06/10/2026) : le portail a refermer
    {
        ExecuterPortail(c);
        return;
    }
    if (EstCoffre(c.texte))    // lot G (08/10/2026) : le coffre de la faille
    {
        ExecuterCoffre(c);
        return;
    }
    if (EstGisement(c.texte))  // 19/10/2026 : le gisement d'apres-evenement (CoaDirecteurGardien.inc)
    {
        ExecuterGisement(c);
        return;
    }
    if (EstObjectif(c.texte))  // 10/10/2026 : objectifs d'evenement (etats du monde)
    {
        ExecuterObjectif(c);
        return;
    }
    if (EstBarricade(c.texte))  // 13/10/2026 : convoi v2, la barricade (CoaDirecteurConvoi.inc)
    {
        ExecuterBarricade(c);
        return;
    }
    EvLu lu;
    if (!TexteBusValide(c.texte) || !LireSousAction(c.texte, lu))
    {
        RefuserEvenement(c, lu, "texte");
        return;
    }
    if (!CibleJoueurValide(c) || c.valeur < 0)
    {
        RefuserEvenement(c, lu, "bornes");
        return;
    }
    uint32 const valeur = static_cast<uint32>(c.valeur);
    // Bornes et liste blanche, par sous-action.
    bool ok = false;
    switch (lu.sous)
    {
        case EvSous::Ambiance:
            ok = IdDeListe(sPlayerbotAIConfig.directeurEvenementAmbiances, valeur) &&
                 c.dureeS >= EV_AMBIANCE_DUREE_MIN_S && c.dureeS <= EV_DUREE_MAX_S;
            break;
        case EvSous::Son:
            // Au point, un son est POSITIONNEL (SMSG_PLAY_OBJECT_SOUND) : le client l'attenue selon
            // MinDistance/DistanceCutoff de SoundEntries.dbc, que le coeur ne charge pas (seul l'ID).
            // D'ou une liste a part, des seuls sons dont la coupure est >= 100 yd (relevee au DBC).
            ok = IdDeListe(lu.lieu == EvLieu::Point ? sPlayerbotAIConfig.directeurEvenementSonsPoint
                                                    : sPlayerbotAIConfig.directeurEvenementSons,
                           valeur) &&
                 c.dureeS == 0;
            break;
        case EvSous::Musique:
            // Une musique jouee est TOUJOURS enregistree (une echeance), sinon ni `lever`, ni
            // `.directeur stop`, ni le bus eteint ne pourraient la couper.
            ok = valeur == 0 ? c.dureeS == 0
                             : IdDeListe(sPlayerbotAIConfig.directeurEvenementMusiques, valeur) &&
                                   c.dureeS >= EV_MUSIQUE_DUREE_MIN_S && c.dureeS <= EV_DUREE_MAX_S;
            break;
        case EvSous::Effet:
            // Lieu `haut` : EvenementSorts OU EvenementSortsHaut (le nuage d'orage 45213, ✘ pose au
            // sol au banc) ; lieu `point` : EvenementSorts seul.
            ok = lu.forme == EvForme::Kit
                     ? IdDeListe(sPlayerbotAIConfig.directeurEvenementKits, valeur) && c.dureeS == 0
                     : (IdDeListe(sPlayerbotAIConfig.directeurEvenementSorts, valeur) ||
                        (lu.lieu == EvLieu::Haut && IdDeListe(sPlayerbotAIConfig.directeurEvenementSortsHaut, valeur))) &&
                           c.dureeS >= EV_SORT_DUREE_MIN_S && c.dureeS <= EV_DUREE_MAX_S;
            break;
        case EvSous::Annoncer:
            ok = valeur == 0 && c.dureeS == 0;
            break;
        default:
            break;
    }
    if (!ok)
    {
        RefuserEvenement(c, lu, "liste_blanche_ou_bornes");
        return;
    }
    Player* p = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!p)
    {
        RefuserEvenement(c, lu, "hors_ligne");
        return;
    }
    // Ancre bot (11/10) : aux seuls lieux `point` et `haut` ; au lieu `joueur` (aure, musique, son ou kit personnel,
    // annonce), un client est requis : humain seul. Option eteinte : Humain(p) partout, comme avant.
    if (lu.lieu == EvLieu::Joueur ? !Humain(p) : !AncreDEvenement(p))
    {
        RefuserEvenement(c, lu, "pas_humain");
        return;
    }
    if (!p->IsInWorld() || p->IsBeingTeleported() || !CarteDeSuspension(p->GetMapId()))
    {
        RefuserEvenement(c, lu, "carte");
        return;
    }

    // Point (son:point, effet:point, effet:haut) : sur la carte du joueur, a portee de vue,
    // sur un sol sec proche du z du service.
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    bool const auPoint = lu.lieu != EvLieu::Joueur;
    if (auPoint)
    {
        Map* map = p->GetMap();
        float const portee = float(sPlayerbotAIConfig.directeurEvenementDistanceYd);
        // Distance AVANT le sol : SolSec lit GetGridHeight, qui CREE la grille si elle
        // n'existe pas (voir SolSec) ; un point lointain ne doit rien charger.
        if (c.carte != p->GetMapId() || !std::isfinite(c.x) || !std::isfinite(c.y) ||
            p->GetExactDist2d(c.x, c.y) > portee)
        {
            RefuserEvenement(c, lu, "distance");
            return;
        }
        float sol = 0.0f;
        if (c.z < POSE_Z_INCONNU_MAX || !SolSec(map, c.x, c.y, c.z, sol) || std::fabs(sol - c.z) > EV_SOL_ECART_MAX_YD)
        {
            RefuserEvenement(c, lu, "sol");
            return;
        }
        px = c.x;
        py = c.y;
        pz = lu.lieu == EvLieu::Haut ? sol + float(sPlayerbotAIConfig.directeurEvenementHauteurYd) : sol;
        // Distance au DECLENCHEUR (sol + hauteur pour `haut`) : au-dela de la portee, il serait
        // hors de vue (P-165) et le kit ou le son perdu sans refus.
        if (p->GetExactDist(px, py, pz) > portee)
        {
            RefuserEvenement(c, lu, "distance");
            return;
        }
        uint32 parJoueur = 0;
        for (EvDeclencheur const& d : evDeclencheurs)
            if (d.joueur == c.cible)
                ++parJoueur;
        if (evDeclencheurs.size() >= EV_DECLENCHEURS_MAX || evEnvois.size() >= EV_ENVOIS_MAX ||
            parJoueur >= sPlayerbotAIConfig.directeurEvenementDeclencheursParJoueur)
        {
            RefuserEvenement(c, lu, "plein");
            return;
        }
    }

    uint32 const maintenant = Maintenant();
    switch (lu.sous)
    {
        case EvSous::Ambiance:
        {
            if (!p->IsAlive())
            {
                RefuserEvenement(c, lu, "mort");
                return;
            }
            EvAmbiance const nouvelle{c.id, c.cible, valeur, maintenant + c.dureeS, p->GetMapId(), getMSTime()};
            auto it = evAmbiances.find(c.cible);
            if (it != evAmbiances.end())
            {
                // Une ambiance par joueur, la derniere remplace. L'ancienne aure n'est
                // retiree que si le sort change (sinon la nouvelle pose la rafraichit).
                EvAmbiance const ancienne = it->second;
                evAmbiances.erase(it);
                CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REMPLACEES);
                std::string const par = Acore::StringFormat("par_{}", c.id);
                FinirAmbiance(ancienne, "remplace", par.c_str(), "remplace", ancienne.sort != valeur);
            }
            if (!PoserAureAmbiance(p, nouvelle))
            {
                RefuserEvenement(c, lu, "sort");
                return;
            }
            evAmbiances[c.cible] = nouvelle;
            break;
        }
        case EvSous::Son:
            if (!auPoint)
                p->PlayDirectSound(valeur, p);
            else
            {
                Creature* d = PoserDeclencheur(p, px, py, pz, EV_DECLENCHEUR_KIT_MS, false);
                if (!d)
                {
                    RefuserEvenement(c, lu, "declencheur");
                    return;
                }
                evDeclencheurs.push_back(EvDeclencheur{c.id, c.cible, d->GetGUID(), p->GetMapId(),
                                                       maintenant + EV_DECLENCHEUR_KIT_MS / IN_MILLISECONDS,
                                                       lu.sous, lu.forme, valeur});
                evEnvois.push_back(EvEnvoi{c.id, c.cible, d->GetGUID(), false, valeur, getMSTime()});
            }
            break;
        case EvSous::Musique:
        {
            auto it = evMusiques.find(c.cible);
            if (it != evMusiques.end())
            {
                EvMusique const ancienne = it->second;
                evMusiques.erase(it);
                CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REMPLACEES);
                std::string const par = Acore::StringFormat("par_{}", c.id);
                FinirMusique(ancienne, "remplace", par.c_str(), "remplace", false);
            }
            p->PlayDirectMusic(valeur, p);   // 0 : arret (SUPPOSE, voir FinirMusique)
            if (valeur)                      // duree exigee plus haut : toujours une echeance
                evMusiques[c.cible] = EvMusique{c.id, c.cible, valeur, maintenant + c.dureeS};
            break;
        }
        case EvSous::Effet:
            if (lu.forme == EvForme::Kit && !auPoint)
                EnvoyerKit(p, p->GetGUID(), valeur);
            else
            {
                uint32 vieMs = lu.forme == EvForme::Kit ? EV_DECLENCHEUR_KIT_MS : c.dureeS * IN_MILLISECONDS;
                if (lu.forme == EvForme::Sort)
                {
                    // Le declencheur ne survit pas a son aure : un sort a duree finie (42427 : 10 s,
                    // 43467 : 12 s, 61236 : 25 s, 42345 : 35 s ; SpellDuration.dbc) s'eteint seul, et
                    // un declencheur restant occuperait le quota (`plein`) pour rien. -1 : infini.
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(valeur);
                    if (!info)
                    {
                        RefuserEvenement(c, lu, "sort");
                        return;
                    }
                    int32 const sortMs = info->GetMaxDuration();
                    if (sortMs > 0)
                        vieMs = std::min<uint32>(vieMs, std::max<uint32>(uint32(sortMs), IN_MILLISECONDS));
                }
                Creature* d = PoserDeclencheur(p, px, py, pz, vieMs, lu.lieu == EvLieu::Haut);
                if (!d)
                {
                    RefuserEvenement(c, lu, "declencheur");
                    return;
                }
                if (lu.forme == EvForme::Sort && !d->AddAura(valeur, d))
                {
                    d->DespawnOrUnsummon();
                    RefuserEvenement(c, lu, "sort");
                    return;
                }
                evDeclencheurs.push_back(EvDeclencheur{c.id, c.cible, d->GetGUID(), p->GetMapId(),
                                                       maintenant + (vieMs + IN_MILLISECONDS - 1) / IN_MILLISECONDS,
                                                       lu.sous, lu.forme, valeur});
                if (lu.forme == EvForme::Kit)
                    evEnvois.push_back(EvEnvoi{c.id, c.cible, d->GetGUID(), true, valeur, getMSTime()});
            }
            break;
        case EvSous::Annoncer:
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_BOSS_EMOTE, LANG_UNIVERSAL, ObjectGuid::Empty,
                                         p->GetGUID(), lu.message, 0);
            p->SendDirectMessage(&data);
            break;
        }
        default:
            break;
    }
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots",
             "coa directeur evenement: id={} sous_action={} lieu={} forme={} joueur={} valeur={} motif=pose duree_s={}"
             " octets={}",
             c.id, NomEvSous(lu.sous), NomEvLieu(lu.lieu), NomEvForme(lu.forme), c.cible, valeur, c.dureeS,
             lu.message.size());
    Accuser(c, "execute", nullptr);
}

// Leve tout ce que l'evenement a pose pour les commandes ou `lever(id, joueur)` est vrai :
// aure retiree, musique coupee (SUPPOSE), declencheurs effaces, envois differes oublies.
template <typename Predicat>
size_t LeverEvenementsSi(Predicat lever, char const* raison)
{
    // Lot D : les invoques d'abord (retrait immediat, ligne `tues=` par lot).
    size_t const invoques = LeverInvoquesSi(lever, raison) + LeverPortailsSi(lever, raison) +   // + lot P
                            LeverCoffresSi(lever, raison) +                                      // + lot G
                            LeverObjectifsSi(lever, raison) +                                    // + objectifs
                            LeverBarricadesSi(lever, raison) +                                   // + convoi v2
                            LeverImprevuSi(lever, raison);                                       // + lot D5
    if (evAmbiances.empty() && evMusiques.empty() && evDeclencheurs.empty() && evEnvois.empty())
        return invoques;
    std::vector<EvAmbiance> ambiances;
    for (auto it = evAmbiances.begin(); it != evAmbiances.end();)
    {
        if (lever(it->second.id, it->second.joueur))
        {
            ambiances.push_back(it->second);
            it = evAmbiances.erase(it);
        }
        else
            ++it;
    }
    std::vector<EvMusique> musiques;
    for (auto it = evMusiques.begin(); it != evMusiques.end();)
    {
        if (lever(it->second.id, it->second.joueur))
        {
            musiques.push_back(it->second);
            it = evMusiques.erase(it);
        }
        else
            ++it;
    }
    std::vector<EvDeclencheur> declencheurs;
    for (auto it = evDeclencheurs.begin(); it != evDeclencheurs.end();)
    {
        if (lever(it->id, it->joueur))
        {
            declencheurs.push_back(*it);
            it = evDeclencheurs.erase(it);
        }
        else
            ++it;
    }
    evEnvois.erase(std::remove_if(evEnvois.begin(), evEnvois.end(),
                                  [&lever](EvEnvoi const& e) { return lever(e.id, e.joueur); }),
                   evEnvois.end());
    for (EvAmbiance const& a : ambiances)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        FinirAmbiance(a, "leve", raison, raison, true);
    }
    for (EvMusique const& m : musiques)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        FinirMusique(m, "leve", raison, raison, true);
    }
    for (EvDeclencheur const& d : declencheurs)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        FinirDeclencheur(d, "leve", raison, raison, true);
    }
    return invoques + ambiances.size() + musiques.size() + declencheurs.size();
}

// Chaque seconde (Entretien, bus allume ou eteint) : envois differes dus, reaffirmation des
// ambiances, echeances ; deconnexion (ambiances, musiques, declencheurs) et changement de carte
// (ambiances, declencheurs).
void SuivreEvenements()
{
    if (evAmbiances.empty() && evMusiques.empty() && evDeclencheurs.empty() && evEnvois.empty())
        return;
    uint32 const maintenant = Maintenant();
    uint32 const maintenantMs = getMSTime();

    // 1. Envois differes : le declencheur existe depuis au moins EV_ENVOI_DIFFERE_MS.
    for (auto it = evEnvois.begin(); it != evEnvois.end();)
    {
        if (getMSTimeDiff(it->poseMs, maintenantMs) < EV_ENVOI_DIFFERE_MS)
        {
            ++it;
            continue;
        }
        Player* p = ObjectAccessor::FindPlayerByLowGUID(it->joueur);
        Creature* d = p && p->IsInWorld() ? ObjectAccessor::GetCreature(*p, it->guid) : nullptr;
        if (d)
        {
            if (it->kit)
                EnvoyerKit(p, it->guid, it->valeur);
            else
                d->PlayDistanceSound(it->valeur, p);
        }
        LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action={} lieu=declencheur joueur={} valeur={}"
                 " motif={}", it->id, it->kit ? "effet" : "son", it->joueur, it->valeur, d ? "envoye" : "perdu");
        it = evEnvois.erase(it);
    }

    // 2. Ambiances.
    uint32 const periodeMs = sPlayerbotAIConfig.directeurEvenementReaffirmationSecondes * IN_MILLISECONDS;
    struct FinA { EvAmbiance a; char const* motif; bool retirer; };
    std::vector<FinA> finsA;
    for (auto it = evAmbiances.begin(); it != evAmbiances.end();)
    {
        EvAmbiance& a = it->second;
        Player* p = JoueurConnecte(it->first);
        char const* motif = nullptr;
        if (!p)
            motif = "hors_ligne";
        else if (!p->IsInWorld() || p->IsBeingTeleported())
        {
            ++it;
            continue;
        }
        else if (p->GetMapId() != a.carte)
            motif = "carte";
        else if (a.finS <= maintenant)
            motif = "echue";
        if (motif)
        {
            finsA.push_back(FinA{a, motif, p != nullptr});
            it = evAmbiances.erase(it);
            continue;
        }
        if (getMSTimeDiff(a.dernierMs, maintenantMs) >= periodeMs)
        {
            // Mort : AddAura refuse (Unit.cpp) ; la pose suivante la rendra au vivant.
            if (p->IsAlive())
                PoserAureAmbiance(p, a);
            a.dernierMs = maintenantMs;
        }
        ++it;
    }
    for (FinA const& f : finsA)
    {
        if (std::string_view(f.motif) == "echue")
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ECHUES);
        FinirAmbiance(f.a, nullptr, nullptr, f.motif, f.retirer);
    }

    // 3. Musiques et declencheurs echus : rien a envoyer (la musique va a son terme, le
    // TempSummon s'efface seul) ; le registre les oublie. Un declencheur dont le joueur est
    // parti (hors ligne, autre carte) est EFFACE aussitot : il ne compte plus dans les quotas.
    for (auto it = evMusiques.begin(); it != evMusiques.end();)
    {
        bool const parti = !JoueurConnecte(it->first);
        if (parti || it->second.finS <= maintenant)
        {
            EvMusique const m = it->second;
            it = evMusiques.erase(it);
            FinirMusique(m, nullptr, nullptr, parti ? "hors_ligne" : "echue", false);
        }
        else
            ++it;
    }
    for (auto it = evDeclencheurs.begin(); it != evDeclencheurs.end();)
    {
        Player* p = JoueurConnecte(it->joueur);
        char const* motif = !p                                                    ? "hors_ligne"
                            : (p->IsInWorld() && !p->IsBeingTeleported() &&
                               p->GetMapId() != it->carte)                        ? "carte"
                            : it->finS <= maintenant                              ? "echue"
                                                                                  : nullptr;
        if (motif)
        {
            EvDeclencheur const d = *it;
            it = evDeclencheurs.erase(it);
            FinirDeclencheur(d, nullptr, nullptr, motif, std::string_view(motif) != "echue");
        }
        else
            ++it;
    }
}

// ------------------------------------------------------------------ lot D des evenements (05/10/2026) : apparitions
//
// coa-project/docs/EVENEMENTS-grimoire-directeur.md § 2.5 (monstres), § 2.6 (ordres), § 2.10 (retirer),
// § 5.2 (lot D reduit a E4 pour la Faille d'orage) ; compte rendu : docs/EVENEMENTS-lot-D.md.
// Trois sous-actions de plus au verbe `evenement`, ETEINTES tant que Directeur.EvenementApparitions = 0
// (`retirer` excepte : il ne pose rien) :
//   invoquer:<n>:<d>:<vue>:<arrivee>
//        n monstres (1-6) de l'entree `valeur` -- liste blanche EvenementCreatures ET plage
//        9100000-9109999 imposee (exclue de l'echelle de niveau par mod-ascension-compat, CanScale) --
//        au point carte/x/y/z (sol controle, a EvenementInvocationDistanceYd du joueur au plus),
//        disperses dans `rayon` (0-15 yd) ; niveau = niveau du joueur + d (d : -1, 0, +1), pose par le
//        crochet OnBeforeCreatureSelectLevel de ce fichier ; vie `duree_s` (60-3600 s) en
//        TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT (4, grimoire § 2.5 ; la seule forme permise) ; vue
//        `scene` (mise en scene) ou `hors_vue` (refus `vue` si un humain est a moins de 110 yd + la
//        dispersion du point, regle V6) ; point (et chaque point disperse) hors sanctuaire, capitale et
//        lieu sans JcJ (CoaLieuSansPvp, refus `lieu`) ; arrivee `aucune` ou `naissance` (7398 lance par l'invoque sur lui-meme, si
//        EvenementSortsArrivee le permet) ou `ciel` (10/10 : pose a EV_CIEL_HAUTEUR_YD au-dessus de son sol,
//        DESCEND au sol par la spline de MoveLand, point de retour remis au sol ; le coeur rend la gravite seul ; option
//        EvenementArriveeCiel, eteinte : pose au sol comme `aucune`, journal `arrivee=ciel_eteint`). Jamais visibleBySummonerOnly ni phase : les bots le voient.
//        Map::SummonCreature SANS invocateur : aucun lien au joueur (ni m_CreatedByPlayer, ni crochet
//        OnPlayerBeforeTempSummonInitStats). Tout ou rien : un echec en cours retire ceux deja poses.
//   ordre:attaquer | ordre:tenir | ordre:errer | ordre:charger_vers
//        `valeur` = id de la commande `invoquer` visee, 0 = tous les invoques vivants du joueur.
//        attaquer : AttackStart sur le joueur `cible` (sauf invoque deja aux prises avec un autre : il
//        garde son combat, compte `occupes`) ; tenir : reste sur place (foyer = position, mouvement de
//        repos, reaction DEFENSIVE jusqu'au prochain attaquer/errer/charger_vers) ; errer : erre dans `rayon` (3-30 yd) autour de sa position ;
//        charger_vers : COURT au point carte/x/y/z (foyer = ce point ; FORCED_MOVEMENT_RUN depuis le 10/10 :
//        « les geants c'est mou », la spline prenait le pas de marche). Hors combat seulement, sauf
//        attaquer. Liste fermee : `tenir` et `charger_vers` sont ceux du grimoire (§ 2.6) ; `attaquer`
//        et `errer` sont le minimum demande au lot ; fuir_*, etat et faction ne sont pas faits.
//   retirer | retirer:immediat
//        `valeur` = id de la commande `invoquer`, 0 = tous ceux du joueur. `retirer` attend qu'aucun
//        humain ne voie l'invoque (portee de vue de la carte + 10 yd), EV_RETRAIT_HORS_VUE_MAX_S au
//        plus ; `retirer:immediat` : tout de suite (issues `immediat`, grimoire § 2.10). Un `retirer`
//        est du MENAGE : exempt du budget, de l'arret et de l'interrupteur du verbe (Recevoir).
// Registre au FIL MONDE seul (P-137) ; morts relevees chaque seconde par lecture (aucun crochet de
// mort : la creature est lue sur le fil monde, cartes a l'arret). Levee immediate sur lever, stop
// (arret), bus eteint, deconnexion et changement de carte du joueur. RENFORTS : une creature de la
// plage invoquee par un invoque inscrit (SmartAI du Gardien, lot E) est signalee par le crochet
// OnCreatureAddWorld (fil de carte : file sous verrou, le seul verrou du lot) et inscrite au fil monde
// sous le lot de son invocateur : elle est levee avec lui. Niveau relu chaque seconde : un niveau
// reecrit (coeur sans l'exclusion CanScale) RETIRE l'invoque (`motif=echelle`, compte `retires`).
// Journal : `coa directeur evenement: id= sous_action=invoquer joueur= valeur= motif= posees= tues=
// tues_humain= tues_bot= disparus= retires= renforts=` a la levee et a la fin ; `tues_humain`/`tues_bot` :
// le joueur qui a marque la creature (GetLootRecipient, premier frappeur) ; le reste des tues n'a aucun
// joueur marqueur.

constexpr uint32 EV_ENTREE_MIN = 9100000;           // plage des gabarits d'evenement (grimoire § 2.5, § 5.5)
constexpr uint32 EV_ENTREE_MAX = 9109999;
constexpr uint32 EV_INVOQUER_NOMBRE_MAX = 6;         // grimoire § 2.5 : 6 monstres par groupe et par etape
constexpr float EV_INVOQUER_DISPERSION_MAX_YD = 15.0f;
constexpr float EV_INVOQUER_DISPERSION_MIN_YD = 2.0f;   // plusieurs monstres : jamais empiles
constexpr uint32 EV_INVOQUER_VIE_MIN_S = 60;
constexpr float EV_HORS_VUE_YD = 110.0f;             // regle V6
constexpr float EV_ERRER_MIN_YD = 3.0f;
constexpr float EV_ERRER_MAX_YD = 30.0f;
constexpr uint32 EV_RETRAIT_HORS_VUE_MAX_S = 300;    // au-dela, retire meme vu
constexpr float EV_VU_MARGE_YD = 10.0f;
constexpr size_t EV_INVOQUES_MAX = 256;              // tout le royaume (64 jusqu'au 18/10 : 8 evenements simultanes)
constexpr uint32 EV_SORT_NAISSANCE = 7398;           // « Birth », effet 46, le monstre sur lui-meme
// Arrivee `ciel` (10/10/2026, retours du 09/10 : « je pensais les voir surgir d'en haut ») : hauteur de pose au-dessus
// du sol du point, et duree au bout de laquelle la gravite revient meme si la descente n'est pas finie.
constexpr float EV_CIEL_HAUTEUR_YD = 18.0f;
constexpr uint32 EV_CIEL_MAX_S = 6;
constexpr uint32 EV_POINT_CIEL = 0xC1E1;             // id du mouvement MoveLand (EffectMovementGenerator)
// Egares (10/10/2026, retours du 09/10 : « certains etaient vraiment partis, ils stagnaient loin de la zone » --
// MESURE : deux nagas aux prises avec les Braves de Bloodhoof, 4 min loin du joueur). Un invoque qui a recu
// `attaquer` et qui reste plus de EV_EGARE_S secondes aux prises avec un PNJ (cible non controlee par un joueur), ou HORS
// COMBAT a plus de EV_EGARE_YD du joueur, est RELANCE sur le joueur (combat rompu, AttackStart), EV_EGARE_RELANCES fois,
// puis RETIRE (compte au lot : `retires` et `egares`). Un invoque en combat contre un joueur ou un bot (cible controlee
// par un joueur) n'est JAMAIS egare, meme loin : il chasse, ou un bot inscrit le tient (relecture : sans cela, un chef
// qui poursuit un joueur qui recule aurait ete retire, et la victoire avec lui). Jamais pendant un `tenir`, un `errer`, un `charger_vers` (seul
// `attaquer` arme la garde), ni joueur mort, ni retrait hors de vue demande.
// Relecture du 10/10 : un invoque FORT (chef, Mere-wyrm, Seigneur, elite : rang non nul, PV du gabarit >= EV_EGARE_FORT_PV,
// forme etendue du lot G ou renfort a mi-vie arme) n'est JAMAIS retire par cette garde : il est relance a chaque
// EV_EGARE_S, sans limite. Sans cela, un chef sorti de combat (joueur recule de plus de 60 yd pour se soigner, laisse)
// etait retire avec tues = 0, et la Faille finissait sans victoire ni coffre (faille_orage.py : la victoire exige un
// gardien tue, la retentative exige qu'aucun gardien ne soit pose).
constexpr float EV_EGARE_YD = 60.0f;
constexpr uint32 EV_EGARE_S = 20;
constexpr uint8 EV_EGARE_RELANCES = 1;
constexpr float EV_EGARE_FORT_PV = 3.0f;             // CreatureTemplate::ModHealth ; les chefs sont a 6-8, les elites a 3-3,5

// Niveau impose au monstre en cours de creation (Map::SummonCreature -> Creature::Create ->
// UpdateEntry -> SelectLevel -> OnBeforeCreatureSelectLevel). Pose par le fil monde autour de l'appel
// seulement ; thread_local : un fil de carte qui cree une creature au meme instant lit toujours 0.
thread_local uint8 evNiveauForce = 0;

// Renforts (relecture du 05/10) : une creature de la plage 9100000-9109999 INVOQUEE PAR UNE AUTRE (SmartAI
// du Gardien a 50 % PV, lot E) n'est pas posee par ExecuterInvoquer. Le crochet OnCreatureAddWorld la
// signale ici depuis le fil de sa carte (la creature elle-meme seulement : entree, invocateur, carte) ;
// le fil monde vide la file (DrainerRenforts) et l'inscrit sous le lot de son invocateur s'il est au
// registre. Sinon, elle survivrait a lever, stop, bus eteint, deconnexion et changement de carte.
constexpr size_t EV_RENFORTS_FILE_MAX = 256;
struct EvRenfortVu
{
    ObjectGuid guid;
    ObjectGuid invocateur;
    uint32 carte;
};
std::mutex evRenfortsVerrou;                          // le SEUL verrou du lot D : la file ci-dessous
std::vector<EvRenfortVu> evRenfortsVus;               // sous evRenfortsVerrou
uint32 evRenfortsPerdus = 0;                          // sous evRenfortsVerrou : file pleine

enum class EvApp : uint8 { Aucune, Invoquer, Ordre, Retirer };
// 13/10 (lot V, convoi de bandits, CoaDirecteurConvoi.inc) : Escorter et Assaillir.
enum class EvOrdre : uint8 { Aucun, Attaquer, Tenir, Errer, ChargerVers, Escorter, Assaillir };

struct EvAppLu
{
    EvApp sous = EvApp::Aucune;
    uint32 nombre = 0;
    int32 delta = 0;
    bool horsVue = false;
    bool naissance = false;
    bool ciel = false;          // 10/10 : arrivee `ciel`
    EvOrdre ordre = EvOrdre::Aucun;
    bool immediat = false;
    // Lot G (08/10/2026, CoaDirecteurGardien.inc) : forme etendue d'invoquer, 7 morceaux. 0 : absent.
    bool etendue = false;
    uint8 pv = 0;
    uint8 degats = 0;
    bool renfort = false;
    // Chefs a mecaniques (10/10/2026, CoaDirecteurBoss.inc) : 7e morceau `meca1` | `meca2` | `meca3` (palier du public,
    // seul / 2 a 5 / 5 et plus). 0 : absent.
    uint8 meca = 0;
    // Lot V (13/10/2026, CoaDirecteurConvoi.inc) : `ordre:assaillir:<protege>` -- la commande `invoquer` du protege.
    uint32 protege = 0;
    // Convoi v2 (13/10/2026) : `ordre:escorter:<vitesse>` -- % de la course de base ; 0 : la marche du lot V.
    uint16 vitesse = 0;
};

// Lot G des evenements (08/10/2026) : definis dans CoaDirecteurGardien.inc, inclus apres le lot P.
bool LireMorceauxGardien(std::string const& pv, std::string const& degats, std::string const& renfort, EvAppLu& lu);
// Chefs a mecaniques (10/10/2026) : definis dans CoaDirecteurBoss.inc, inclus apres le lot G.
bool LireMeca(std::string const& m, EvAppLu& lu);
// Convoi de bandits (13/10/2026) : definie dans CoaDirecteurConvoi.inc, incluse apres les chefs a mecaniques. Grammaire
// A PART de celle du lot D (son epreuve la compare au service : SOUS_ACTIONS_EV_D).
bool LireOrdreConvoi(std::string const& t, EvAppLu& lu);

char const* NomEvOrdre(EvOrdre o)
{
    switch (o)
    {
        case EvOrdre::Attaquer:    return "attaquer";
        case EvOrdre::Tenir:       return "tenir";
        case EvOrdre::Errer:       return "errer";
        case EvOrdre::ChargerVers: return "charger_vers";
        case EvOrdre::Escorter:    return "escorter";
        case EvOrdre::Assaillir:   return "assaillir";
        default:                   return "-";
    }
}

char const* NomEvApp(EvApp s)
{
    return s == EvApp::Invoquer ? "invoquer" : s == EvApp::Ordre ? "ordre" : s == EvApp::Retirer ? "retirer" : "inconnue";
}

// Le `texte` releve-t-il du lot D ? (prefixe seul : la grammaire est lue par LireApparition)
bool EstApparition(std::string const& t)
{
    return t == "retirer" || t.rfind("retirer:", 0) == 0 || t.rfind("ordre:", 0) == 0 ||
           t.rfind("invoquer:", 0) == 0;
}

// Grammaire FERMEE, casse exacte ; tout le reste : faux (refus `texte`).
bool LireApparition(std::string const& t, EvAppLu& lu)
{
    lu = EvAppLu();
    if (t == "retirer")          { lu.sous = EvApp::Retirer; return true; }
    if (t == "retirer:immediat") { lu.sous = EvApp::Retirer; lu.immediat = true; return true; }
    if (t == "ordre:attaquer")     { lu.sous = EvApp::Ordre; lu.ordre = EvOrdre::Attaquer; return true; }
    if (t == "ordre:tenir")        { lu.sous = EvApp::Ordre; lu.ordre = EvOrdre::Tenir; return true; }
    if (t == "ordre:errer")        { lu.sous = EvApp::Ordre; lu.ordre = EvOrdre::Errer; return true; }
    if (t == "ordre:charger_vers") { lu.sous = EvApp::Ordre; lu.ordre = EvOrdre::ChargerVers; return true; }
    if (LireOrdreConvoi(t, lu))      // 13/10 : lot V (escorter, assaillir:<protege>)
        return true;
    static constexpr char const PREFIXE[] = "invoquer:";
    size_t const n = sizeof(PREFIXE) - 1;
    if (t.size() <= n || t.compare(0, n, PREFIXE) != 0)
        return false;
    std::vector<std::string> morceaux;
    size_t debut = n;
    while (true)
    {
        size_t const fin = t.find(':', debut);
        morceaux.push_back(t.substr(debut, fin == std::string::npos ? std::string::npos : fin - debut));
        if (fin == std::string::npos)
            break;
        debut = fin + 1;
    }
    if (morceaux.size() != 4 && morceaux.size() != 7)   // 7 : forme etendue du lot G
        return false;
    if (morceaux.size() == 7 && !LireMorceauxGardien(morceaux[4], morceaux[5], morceaux[6], lu))
        return false;
    // n : un seul chiffre, 1 a 6.
    if (morceaux[0].size() != 1 || morceaux[0][0] < '1' || morceaux[0][0] > char('0' + EV_INVOQUER_NOMBRE_MAX))
        return false;
    lu.nombre = uint32(morceaux[0][0] - '0');
    if (morceaux[1] == "-1")
        lu.delta = -1;
    else if (morceaux[1] == "0")
        lu.delta = 0;
    else if (morceaux[1] == "+1")
        lu.delta = 1;
    else
        return false;
    if (morceaux[2] == "hors_vue")
        lu.horsVue = true;
    else if (morceaux[2] != "scene")
        return false;
    if (morceaux[3] == "naissance")
        lu.naissance = true;
    else if (morceaux[3] == "ciel")
        lu.ciel = true;
    else if (morceaux[3] != "aucune")
        return false;
    lu.sous = EvApp::Invoquer;
    return true;
}

struct EvInvoque
{
    uint32 id;               // commande `invoquer`
    uint32 joueur;
    ObjectGuid guid;
    uint32 carte;
    uint32 entree;
    uint8 niveau;            // niveau pose (renfort : lu a l'inscription) ; un autre lu plus tard : retire
    bool mort = false;
    bool renfort = false;    // invoque par un invoque inscrit (SmartAI), pas par ExecuterInvoquer
    uint32 retraitDepuisS = 0;   // Maintenant() de la demande de retrait hors de vue ; 0 : aucune
    uint8 pv = 100;              // lot G : dose (%) de ses PV et de ses degats, 100 : pleine
    uint8 degats = 100;
    bool renfortArme = false;    // lot G : fera sortir ses Etincelles sous 50 % PV...
    bool renfortFait = false;    // ... une seule fois
    uint32 cielDepuisS = 0;      // 10/10 : Maintenant() de la pose en hauteur (arrivee `ciel`) ; 0 : au sol
    uint32 attaqueS = 0;         // 10/10 : Maintenant() du dernier `attaquer` ; 0 : garde des egares desarmee
    uint32 egareDepuisS = 0;     // 10/10 : debut de l'egarement en cours ; 0 : aucun
    uint8 relances = 0;          // 10/10 : relances faites sur cet invoque
    bool fort = false;           // 10/10 (relecture) : chef ou elite -- la garde des egares le relance, ne le retire jamais
    // Chefs a mecaniques (10/10/2026, CoaDirecteurBoss.inc).
    uint8 meca = 0;              // chef arme `meca<k>` : palier du public k (1-3) ; 0 : non arme
    uint8 paliersFaits = 0;      // chef : bits 0/1/2 = paliers 75/50/25 deja declenches (une fois chacun, meme refuses)
    bool bouclierActif = false;  // chef : bouclier lie aux renforts du palier 50 pose par le C++, pas encore juge fini
    uint32 bouclierDepuisS = 0;  // chef : Maintenant() de la pose du bouclier
    ObjectGuid chefGuid;         // renfort de palier : son chef
    // Convoi de bandits (13/10/2026, CoaDirecteurConvoi.inc).
    bool protege = false;        // gabarit de EV_CONVOI_PROTEGES : passif, jamais cible publiee, jamais egare
    uint32 etatConvoiS = 0;      // protege : Maintenant() de la derniere ligne `sous_action=convoi motif=etat`
    bool rapide = false;         // convoi v2 : protege lance au trot (`escorter:<vitesse>`) : etat toutes les 2 s
    uint8 palier = 0;            // renfort de palier : 75, 50 ou 25 ; 0 : autre
    // Revue 16/10 : assaillant du convoi (ordre `assaillir`) -- le protege vise. IMMUNE_TO_NPC lui est rendu quand ce
    // protege est mort ou parti, ou au retrait (SuivreAssaillant, CoaDirecteurConvoi.inc). Vide : pas assaillant.
    ObjectGuid assailli;
};
std::vector<EvInvoque> evInvoques;                    // fil monde seul

struct EvLot
{
    uint32 id;
    uint32 joueur;
    uint32 entree;
    uint32 posees = 0;
    uint32 tues = 0;
    uint32 tuesHumain = 0;
    uint32 tuesBot = 0;
    uint32 disparus = 0;     // disparus sans qu'on les ait vus morts ni retires (echeance, grille dechargee)
    uint32 retires = 0;
    uint32 renforts = 0;     // creatures de la plage invoquees par un invoque du lot, inscrites au lot
    uint32 egares = 0;       // 10/10 : invoques retires par la garde des egares (comptes aussi dans `retires`)
    bool retraitDemande = false;
};
std::unordered_map<uint32, EvLot> evLots;            // cle : id de la commande `invoquer` ; fil monde seul

Creature* CreatureInvoquee(EvInvoque const& v)
{
    Map* map = sMapMgr->FindBaseNonInstanceMap(v.carte);
    return map ? map->GetCreature(v.guid) : nullptr;
}

// Un humain en vue de la creature (portee de vue de la carte + marge) ?
bool VueDUnHumain(Creature* cr)
{
    Map* map = cr->GetMap();
    float const portee = map->GetVisibilityRange() + EV_VU_MARGE_YD;
    for (auto const& ref : map->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (p && p->IsInWorld() && Humain(p) && cr->IsWithinDist(p, portee))
            return true;
    }
    return false;
}

bool HumainAMoinsDe(Map* map, float x, float y, float z, float yd)
{
    for (auto const& ref : map->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (p && p->IsInWorld() && Humain(p) && p->GetExactDist(x, y, z) <= yd)
            return true;
    }
    return false;
}

uint32 InvoquesVivantsDe(uint32 joueur)
{
    uint32 n = 0;
    for (EvInvoque const& v : evInvoques)
        if (v.joueur == joueur && !v.mort)
            ++n;
    return n;
}

void JournaliserLot(EvLot const& l, char const* motif)
{
    LOG_INFO("playerbots",
             "coa directeur evenement: id={} sous_action=invoquer joueur={} valeur={} motif={} posees={} tues={}"
             " tues_humain={} tues_bot={} disparus={} retires={} renforts={} egares={}",
             l.id, l.joueur, l.entree, motif, l.posees, l.tues, l.tuesHumain, l.tuesBot, l.disparus, l.retires,
             l.renforts, l.egares);
}

// Premiere lecture d'un invoque mort : compte au lot, avec le joueur qui l'avait marque.
void NoterMort(EvInvoque& v, Creature* cr)
{
    v.mort = true;
    auto it = evLots.find(v.id);
    if (it == evLots.end())
        return;
    ++it->second.tues;
    if (Player* marqueur = cr->GetLootRecipient())
    {
        if (Humain(marqueur))
            ++it->second.tuesHumain;
        else
            ++it->second.tuesBot;
    }
}

// Retire l'invoque (s'il existe encore) et tient les comptes du lot. Le lot n'est pas efface ici.
void EffacerInvoque(EvInvoque& v)
{
    auto it = evLots.find(v.id);
    Creature* cr = CreatureInvoquee(v);
    if (!cr)
    {
        if (!v.mort && it != evLots.end())
            ++it->second.disparus;
        return;
    }
    if (!v.mort && !cr->IsAlive())
        NoterMort(v, cr);
    cr->DespawnOrUnsummon();
    if (it != evLots.end())
        ++it->second.retires;
}

// Fil monde : vide la file des renforts signales par OnCreatureAddWorld et inscrit chacun sous le lot
// de son invocateur, si celui-ci est au registre (vivant ou mort, tant que son corps y est) ; les
// autres sont ignores (creature de la plage posee hors du Directeur, invocateur deja leve). Appele en
// tete de SuivreInvoques ET de LeverInvoquesSi : un renfort ne de la meme seconde qu'une levee part avec
// elle. Le registre n'est jamais lu hors du fil monde ; la file ne contient que des guids.
void DrainerRenforts()
{
    std::vector<EvRenfortVu> vus;
    uint32 perdus = 0;
    {
        std::lock_guard<std::mutex> garde(evRenfortsVerrou);
        vus.swap(evRenfortsVus);
        std::swap(perdus, evRenfortsPerdus);
    }
    if (perdus)
        LOG_INFO("playerbots", "coa directeur evenement: sous_action=invoquer motif=renforts_perdus perdus={}", perdus);
    for (EvRenfortVu const& r : vus)
    {
        // Dans l'ordre d'arrivee : un renfort de renfort trouve son invocateur deja inscrit.
        auto parent = std::find_if(evInvoques.begin(), evInvoques.end(),
                                   [&r](EvInvoque const& v) { return v.guid == r.invocateur; });
        if (parent == evInvoques.end())
            continue;
        if (std::any_of(evInvoques.begin(), evInvoques.end(), [&r](EvInvoque const& v) { return v.guid == r.guid; }))
            continue;
        auto lot = evLots.find(parent->id);
        Map* map = sMapMgr->FindBaseNonInstanceMap(r.carte);
        Creature* cr = map ? map->GetCreature(r.guid) : nullptr;
        if (lot == evLots.end() || !cr)
            continue;   // deja parti : rien a lever
        EvInvoque v;
        v.id = parent->id;
        v.joueur = parent->joueur;
        v.guid = r.guid;
        v.carte = r.carte;
        v.entree = cr->GetEntry();
        v.niveau = cr->GetLevel();
        v.renfort = true;   // mort deja ? NoterMort a la prochaine lecture (SuivreInvoques, EffacerInvoque)
        v.retraitDepuisS = parent->retraitDepuisS;   // un `retirer` hors de vue deja demande vaut pour lui
        evInvoques.push_back(v);
        ++lot->second.renforts;
        LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=invoquer joueur={} valeur={} motif=renfort"
                 " entree={} niveau={}", v.id, v.joueur, lot->second.entree, v.entree, v.niveau);
    }
}

// Retrait IMMEDIAT de tout invoque et de tout lot pour lesquels `lever(id, joueur)` est vrai ;
// chaque lot leve est journalise (tues=) et clos `leve`. Rend le nombre de lots leves.
template <typename Predicat>
size_t LeverInvoquesSi(Predicat lever, char const* raison)
{
    DrainerRenforts();
    if (evInvoques.empty() && evLots.empty())
        return 0;
    std::vector<EvInvoque> partis;
    for (auto it = evInvoques.begin(); it != evInvoques.end();)
    {
        if (lever(it->id, it->joueur))
        {
            partis.push_back(*it);
            it = evInvoques.erase(it);
        }
        else
            ++it;
    }
    for (EvInvoque& v : partis)
        EffacerInvoque(v);
    std::vector<EvLot> lots;
    for (auto it = evLots.begin(); it != evLots.end();)
    {
        if (lever(it->first, it->second.joueur))
        {
            lots.push_back(it->second);
            it = evLots.erase(it);
        }
        else
            ++it;
    }
    for (EvLot const& l : lots)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        JournaliserLot(l, raison);
        CloreEvenement(l.id, l.joueur, "leve", raison);
    }
    return lots.size();
}

void RefuserApparition(CoaDirCommande const& c, EvAppLu const& lu, char const* raison)
{
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action={} ordre={} joueur={} valeur={} motif={}",
             c.id, NomEvApp(lu.sous), NomEvOrdre(lu.ordre), c.cible, c.valeur, raison);
    Refuser(c, raison);
}

// Le point est-il dans un lieu sans JcJ -- aire ou zone sanctuaire, zone capitale, zone ou aire
// interdite de JcJ par la conf (CoaLieuSansPvp, le predicat des mercenaires) ? Jamais de monstres
// hostiles a tous la (docs/EVENEMENT-1-orage-faille.md : « hors sanctuaire ou capitale »). Lu sur la
// grille du point (deja chargee par SolSec), fil monde.
bool PointSansPvp(Map* map, float x, float y, float z)
{
    uint32 zone = 0, aire = 0;
    map->GetZoneAndAreaId(PHASEMASK_NORMAL, zone, aire, x, y, z);
    return sPlayerbotAIConfig.CoaLieuSansPvp(zone, aire);
}

// Point d'apparition ou de course : sur la carte du joueur, a la portee d'invocation, sur un sol sec
// proche du z du service, hors lieu sans JcJ. Rend le sol dans `sol` ; faux : `raison` dit pourquoi.
bool PointApparition(CoaDirCommande const& c, Player* p, float& sol, char const*& raison)
{
    float const portee = float(sPlayerbotAIConfig.directeurEvenementInvocationDistanceYd);
    // Distance AVANT le sol : SolSec cree la grille (voir SolSec).
    if (c.carte != p->GetMapId() || !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) ||
        p->GetExactDist2d(c.x, c.y) > portee)
    {
        raison = "distance";
        return false;
    }
    if (c.z < POSE_Z_INCONNU_MAX || !SolSec(p->GetMap(), c.x, c.y, c.z, sol) || std::fabs(sol - c.z) > EV_SOL_ECART_MAX_YD)
    {
        raison = "sol";
        return false;
    }
    if (PointSansPvp(p->GetMap(), c.x, c.y, sol))
    {
        raison = "lieu";
        return false;
    }
    return true;
}

// Lot G (CoaDirecteurGardien.inc) : gardes de la forme etendue, dose, renfort a mi-vie.
char const* RefusGardien(EvAppLu const& lu, uint32 entree);
void DoserInvoque(Creature* cr, uint8 pv, uint8 degats);
void DeclencherRenforts();
// Chefs a mecaniques (CoaDirecteurBoss.inc) : gardes de `meca<k>`, armement, paliers, bouclier lie, degats en %.
char const* RefusMeca(EvAppLu const& lu, uint32 entree);
void ArmerMeca(Creature* cr, uint8 public_);
void ReprendrePaliers();
void DeclencherPaliers();
void SuivreBoucliers();
bool PctMeca(Unit const* attaquant, uint32 sort, bool tic, uint8& pct);
bool Insensible(Unit const* u);
// Convoi de bandits (CoaDirecteurConvoi.inc) : garde de la pose d'un protege, armement, ordres escorter/assaillir,
// etat du protege, et l'exemption de la garde des egares.
char const* RefusConvoi(EvAppLu const& lu, uint32 entree);
void ArmerProtege(Creature* cr, EvInvoque& v);
void ExecuterOrdreConvoi(CoaDirCommande const& c, EvAppLu const& lu, Player* p);
void JournaliserProtege(EvInvoque const& v, Creature* cr, char const* motif);
void SuivreProtege(EvInvoque& v, Creature* cr, uint32 maintenant);
void SuivreAssaillant(EvInvoque& v, Creature* cr);
bool EstProtegeDe(Unit const* u, uint32 joueur);

void ExecuterInvoquer(CoaDirCommande const& c, EvAppLu const& lu, Player* p)
{
    uint32 const entree = static_cast<uint32>(c.valeur);
    if (entree < EV_ENTREE_MIN || entree > EV_ENTREE_MAX ||
        !IdDeListe(sPlayerbotAIConfig.directeurEvenementCreatures, entree) || c.dureeS < EV_INVOQUER_VIE_MIN_S ||
        c.dureeS > EV_DUREE_MAX_S || !std::isfinite(c.rayon) || c.rayon < 0.0f ||
        c.rayon > EV_INVOQUER_DISPERSION_MAX_YD ||
        (lu.naissance && !IdDeListe(sPlayerbotAIConfig.directeurEvenementSortsArrivee, EV_SORT_NAISSANCE)))
    {
        RefuserApparition(c, lu, "liste_blanche_ou_bornes");
        return;
    }
    if (!sObjectMgr->GetCreatureTemplate(entree))
    {
        RefuserApparition(c, lu, "gabarit");   // lot E (donnees) absent
        return;
    }
    if (char const* refusG = RefusGardien(lu, entree))   // lot G : forme etendue (option, renfort du Gardien)
    {
        RefuserApparition(c, lu, refusG);
        return;
    }
    if (char const* refusM = RefusMeca(lu, entree))      // 10/10 : `meca<k>` (option, chef de la table, nombre 1)
    {
        RefuserApparition(c, lu, refusM);
        return;
    }
    if (char const* refusV = RefusConvoi(lu, entree))    // 13/10 : protege du convoi (option, nombre 1)
    {
        RefuserApparition(c, lu, refusV);
        return;
    }
    float sol = 0.0f;
    char const* raison = nullptr;
    if (!PointApparition(c, p, sol, raison))
    {
        RefuserApparition(c, lu, raison);
        return;
    }
    Map* map = p->GetMap();
    float const dispersion = lu.nombre > 1 ? std::max(c.rayon, EV_INVOQUER_DISPERSION_MIN_YD) : c.rayon;
    // hors_vue : aucun humain a 110 yd d'AUCUN point possible -- un point disperse est a `dispersion` du
    // centre au plus (relecture du 05/10 : le centre seul laissait un monstre a 95 yd d'un humain).
    if (lu.horsVue && HumainAMoinsDe(map, c.x, c.y, sol, EV_HORS_VUE_YD + dispersion))
    {
        RefuserApparition(c, lu, "vue");
        return;
    }
    if (evInvoques.size() + lu.nombre > EV_INVOQUES_MAX ||
        InvoquesVivantsDe(c.cible) + lu.nombre > sPlayerbotAIConfig.directeurEvenementInvoquesParJoueur)
    {
        RefuserApparition(c, lu, "plein");
        return;
    }
    if (evLots.count(c.id))
    {
        RefuserApparition(c, lu, "deja_pose");   // inatteignable : une commande n'est livree qu'une fois
        return;
    }
    uint32 const niveauMax = std::min<uint32>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL), 80u);
    uint8 const niveau = static_cast<uint8>(std::clamp<int32>(int32(p->GetLevel()) + lu.delta, 1, int32(niveauMax)));
    uint32 const vieMs = c.dureeS * IN_MILLISECONDS;
    bool const ciel = lu.ciel && sPlayerbotAIConfig.directeurEvenementArriveeCiel;

    std::vector<EvInvoque> poses;
    char const* echec = nullptr;
    for (uint32 i = 0; i < lu.nombre && !echec; ++i)
    {
        float x = c.x, y = c.y, z = sol;
        if (dispersion > 0.0f)
        {
            float const part = 2.0f * float(M_PI) / float(lu.nombre);
            float const angle = part * float(i) + frand(0.0f, part);
            float const d = dispersion * frand(0.5f, 1.0f);
            float const tx = c.x + d * std::cos(angle);
            float const ty = c.y + d * std::sin(angle);
            float solI = 0.0f;
            // Sol du point disperse proche du sol du centre, et hors lieu sans JcJ ; sinon, le centre
            // (deja controle).
            if (SolSec(map, tx, ty, sol, solI) && std::fabs(solI - sol) <= EV_SOL_ECART_MAX_YD &&
                !PointSansPvp(map, tx, ty, solI))
            {
                x = tx;
                y = ty;
                z = solI;
            }
        }
        float const face = std::atan2(p->GetPositionY() - y, p->GetPositionX() - x);
        // Arrivee `ciel` (10/10) : posee en hauteur au-dessus du sol lu du point ; elle descend ensuite (plus bas).
        float const zPose = ciel ? z + EV_CIEL_HAUTEUR_YD : z;
        evNiveauForce = niveau;
        TempSummon* s = map->SummonCreature(entree, Position(x, y, zPose, face), nullptr, vieMs, nullptr);
        evNiveauForce = 0;
        if (!s)
        {
            echec = "invocation";
            break;
        }
        // Forme du coeur (WorldObject::SummonCreature) : le type se pose apres InitStats.
        s->SetTempSummonType(TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT);
        EvInvoque v;
        v.id = c.id;
        v.joueur = c.cible;
        v.guid = s->GetGUID();
        v.carte = p->GetMapId();
        v.entree = entree;
        v.niveau = niveau;
        v.pv = lu.pv ? lu.pv : 100;          // lot G
        v.degats = lu.degats ? lu.degats : 100;
        v.renfortArme = lu.renfort;
        v.meca = lu.meca;                     // 10/10 : chef a mecaniques (paliers, bouclier lie, degats en %)
        if (CreatureTemplate const* gabarit = s->GetCreatureTemplate())
            v.fort = lu.etendue || lu.renfort || gabarit->rank != 0 || gabarit->ModHealth >= EV_EGARE_FORT_PV;
        if (ciel)
        {
            // La DESCENTE tient a la spline de MoveLand, pas au drapeau de gravite : Creature::UpdateMovementFlags
            // (appelee a chaque mise a jour de position, Unit::ProcessTerrainStatusUpdate) rend la gravite d'elle-meme
            // a toute creature dont le gabarit interdit le vol (creature_template_movement absent : Flight None, cas
            // des 37 gabarits). SetDisableGravity ici ne vaut que pour l'envoi de la pose ; rendu de la descente
            // SUPPOSE, a verifier au banc avant EvenementArriveeCiel = 1.
            s->SetDisableGravity(true);
            s->GetMotionMaster()->MoveLand(EV_POINT_CIEL, x, y, z);
            // Relecture du 10/10 : Map::SummonCreature a pose le point de retour au point de POSE (sol + 18 yd) ;
            // sans cela, toute laisse ou tout retour (MoveTargetedHome) la renverrait en l'air, hors de portee.
            s->SetHomePosition(x, y, z, face);
            v.cielDepuisS = std::max<uint32>(Maintenant(), 1);
        }
        poses.push_back(v);
        // Niveau relu a chaque pose : un autre niveau veut dire qu'un crochet de creation passe APRES le
        // notre l'a reecrit. Ce n'est PAS la garde de l'echelle d'Ascension : ses scripts s'enregistrent
        // AVANT ceux de mod-playerbots (ModulesLoader.cpp), donc a la creation c'est notre niveau qui
        // gagne meme sans l'exclusion CanScale ; c'est OnAllCreatureUpdate qui le reecrirait ensuite, et
        // SuivreInvoques retire alors l'invoque (`motif=echelle`).
        if (s->GetLevel() != niveau)
        {
            LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=invoquer joueur={} valeur={} motif=echelle"
                     " niveau_voulu={} niveau_lu={}", c.id, c.cible, entree, niveau, s->GetLevel());
            echec = "echelle";
            break;
        }
        DoserInvoque(s, lu.pv, lu.degats);   // lot G : rien sans la forme etendue
        ArmerProtege(s, poses.back());       // 13/10 : protege du convoi (passif, au pas, sans IMMUNE_TO_NPC)
        if (lu.meca)
            ArmerMeca(s, lu.meca);           // 10/10 : marque des degats en % et palier du public au SmartAI (SetData)
        if (lu.naissance)
            s->CastSpell(s, EV_SORT_NAISSANCE, true);   // rendu SUPPOSE (banc du 04/10 : « on voit rien » par .aura)
    }
    if (echec)
    {
        for (EvInvoque const& v : poses)
            if (Creature* cr = CreatureInvoquee(v))
                cr->DespawnOrUnsummon();
        RefuserApparition(c, lu, echec);
        return;
    }
    EvLot l;
    l.id = c.id;
    l.joueur = c.cible;
    l.entree = entree;
    l.posees = uint32(poses.size());
    evLots[c.id] = l;
    evInvoques.insert(evInvoques.end(), poses.begin(), poses.end());
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots",
             "coa directeur evenement: id={} sous_action=invoquer joueur={} valeur={} motif=pose posees={} niveau={}"
             " duree_s={} dispersion={:.1f} vue={} arrivee={} x={:.1f} y={:.1f} z={:.1f}",
             c.id, c.cible, entree, poses.size(), niveau, c.dureeS, dispersion, lu.horsVue ? "hors_vue" : "scene",
             lu.naissance ? "naissance" : ciel ? "ciel" : lu.ciel ? "ciel_eteint" : "aucune", c.x, c.y, sol);
    Accuser(c, "execute", nullptr);
}

void ExecuterOrdre(CoaDirCommande const& c, EvAppLu const& lu, Player* p)
{
    if (lu.ordre == EvOrdre::Escorter || lu.ordre == EvOrdre::Assaillir)
    {
        ExecuterOrdreConvoi(c, lu, p);   // 13/10 : lot V (CoaDirecteurConvoi.inc)
        return;
    }
    uint32 const vise = static_cast<uint32>(c.valeur);   // 0 : tous les invoques du joueur
    float sol = 0.0f;
    if (lu.ordre == EvOrdre::Errer &&
        (!std::isfinite(c.rayon) || c.rayon < EV_ERRER_MIN_YD || c.rayon > EV_ERRER_MAX_YD))
    {
        RefuserApparition(c, lu, "liste_blanche_ou_bornes");
        return;
    }
    if (lu.ordre == EvOrdre::ChargerVers)
    {
        char const* raison = nullptr;
        if (!PointApparition(c, p, sol, raison))
        {
            RefuserApparition(c, lu, raison);
            return;
        }
    }
    if (lu.ordre == EvOrdre::Attaquer && !p->IsAlive())
    {
        RefuserApparition(c, lu, "mort");
        return;
    }
    uint32 vises = 0, appliques = 0, occupes = 0;
    uint32 const maintenant = std::max<uint32>(Maintenant(), 1);
    for (EvInvoque& v : evInvoques)
    {
        // 13/10 : un protege du convoi n'obeit qu'a `escorter` (jamais attaquer, tenir, errer, charger_vers).
        if (v.joueur != c.cible || v.mort || v.protege || (vise && v.id != vise))
            continue;
        ++vises;
        Creature* cr = CreatureInvoquee(v);
        if (!cr || !cr->IsAlive())
            continue;
        if (lu.ordre == EvOrdre::Attaquer)
        {
            // Bots actifs, relecture du 07/10 : un invoque deja aux prises avec un AUTRE que le joueur (un bot inscrit
            // qui le defendait, un autre humain) garde son combat. Sans cela, l'`attaquer` renvoye au retour d'un
            // joueur absent (faille_orage.py, joueur_de_retour) jetait sur lui, d'un coup, tout ce que les bots
            // tenaient occupe.
            // 10/10 : `attaquer` arme la garde des egares (SuivreInvoques), occupe ou non.
            v.attaqueS = maintenant;
            v.egareDepuisS = 0;
            Unit* victime = cr->GetVictim();
            if (cr->IsInCombat() && victime && victime != p)
            {
                cr->SetReactState(REACT_AGGRESSIVE);   // sans changer sa cible : il ne reste pas defensif apres
                ++occupes;
                continue;
            }
            if (cr->AI() && cr->GetMapId() == p->GetMapId() && cr->IsValidAttackTarget(p))
            {
                cr->SetReactState(REACT_AGGRESSIVE);   // leve la reaction defensive d'un `tenir`
                cr->AI()->AttackStart(p);
                ++appliques;
            }
            continue;
        }
        if (cr->IsInCombat())
            continue;   // un combat en cours n'est jamais interrompu
        // 10/10 : tenir, errer, charger_vers desarment la garde des egares (le monstre doit rester ou aller ailleurs).
        v.attaqueS = 0;
        v.egareDepuisS = 0;
        MotionMaster* mm = cr->GetMotionMaster();
        switch (lu.ordre)
        {
            case EvOrdre::Tenir:
                // Relecture du 07/10 : les gabarits 9100001-9100004 sont faction 14 en SmartAI, donc AGRESSIFS par
                // defaut (SmartAI::MoveInLineOfSight attaque sous REACT_AGGRESSIVE) : un invoque tenu a portee d'aggro
                // d'un joueur absent l'engageait quand meme. DEFENSIF : il ne riposte qu'a qui le frappe (les bots
                // inscrits peuvent toujours le tuer) ; `attaquer`, `errer`, `charger_vers` rendent l'agressivite.
                cr->SetReactState(REACT_DEFENSIVE);
                cr->SetHomePosition(cr->GetPosition());
                cr->SetDefaultMovementType(IDLE_MOTION_TYPE);
                mm->Clear();
                mm->MoveIdle();
                break;
            case EvOrdre::Errer:
                cr->SetReactState(REACT_AGGRESSIVE);
                cr->SetHomePosition(cr->GetPosition());
                cr->SetWanderDistance(c.rayon);
                cr->SetDefaultMovementType(RANDOM_MOTION_TYPE);
                mm->Clear();
                mm->MoveRandom(c.rayon);
                break;
            case EvOrdre::ChargerVers:
                cr->SetReactState(REACT_AGGRESSIVE);
                cr->SetHomePosition(c.x, c.y, sol, cr->GetOrientation());
                cr->SetDefaultMovementType(IDLE_MOTION_TYPE);
                mm->Clear();
                mm->MovePoint(0, c.x, c.y, sol, FORCED_MOVEMENT_RUN);   // 10/10 : en COURSE, jamais au pas
                break;
            default:
                break;
        }
        ++appliques;
    }
    if (!vises)
    {
        RefuserApparition(c, lu, "aucun_invoque");
        return;
    }
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    // Relecture du 07/10 : `occupes` EN FIN (invoques laisses a leur combat contre un autre que le joueur).
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=ordre ordre={} joueur={} valeur={} motif=pose"
             " vises={} appliques={} occupes={}", c.id, NomEvOrdre(lu.ordre), c.cible, vise, vises, appliques, occupes);
    Accuser(c, "execute", appliques ? nullptr : "aucun_applique");
}

void ExecuterRetirer(CoaDirCommande const& c, EvAppLu const& lu)
{
    uint32 const vise = static_cast<uint32>(c.valeur);   // 0 : tous ceux du joueur
    uint32 const joueur = c.cible;
    // `vises` : les invoques du joueur designes (morts compris), dans les deux formes.
    size_t n = static_cast<size_t>(std::count_if(evInvoques.begin(), evInvoques.end(), [vise, joueur](EvInvoque const& v)
                                                 { return v.joueur == joueur && (!vise || v.id == vise); }));
    if (lu.immediat)
        LeverInvoquesSi([vise, joueur](uint32 id, uint32 j) { return j == joueur && (!vise || id == vise); }, "retirer");
    else
    {
        uint32 const maintenant = Maintenant();
        for (EvInvoque& v : evInvoques)
            if (v.joueur == joueur && (!vise || v.id == vise) && !v.retraitDepuisS)
                v.retraitDepuisS = std::max<uint32>(maintenant, 1);
        for (auto& [id, l] : evLots)
            if (l.joueur == joueur && (!vise || id == vise))
                l.retraitDemande = true;
    }
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=retirer joueur={} valeur={} motif=pose"
             " immediat={} vises={}", c.id, joueur, vise, lu.immediat ? 1 : 0, n);
    // Rien a retirer n'est pas un refus (meme regle que `lever`) : ils ont pu mourir ou s'effacer.
    Accuser(c, "execute", n ? nullptr : "rien_a_retirer");
}

void ExecuterApparition(CoaDirCommande const& c)
{
    EvAppLu lu;
    if (!TexteBusValide(c.texte) || !LireApparition(c.texte, lu))
    {
        RefuserApparition(c, lu, "texte");
        return;
    }
    if (!CibleJoueurValide(c) || c.valeur < 0)
    {
        RefuserApparition(c, lu, "bornes");
        return;
    }
    if (lu.sous == EvApp::Retirer)
    {
        ExecuterRetirer(c, lu);
        return;
    }
    if (!sPlayerbotAIConfig.directeurEvenementApparitions)
    {
        RefuserApparition(c, lu, "apparitions_eteintes");
        return;
    }
    if (lu.sous == EvApp::Invoquer && c.valeur == 0)
    {
        RefuserApparition(c, lu, "liste_blanche_ou_bornes");
        return;
    }
    Player* p = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!p)
    {
        RefuserApparition(c, lu, "hors_ligne");
        return;
    }
    if (!AncreDEvenement(p))   // 11/10 : un bot non mercenaire si EvenementAncreBot (sinon humain seul)
    {
        RefuserApparition(c, lu, "pas_humain");
        return;
    }
    if (!p->IsInWorld() || p->IsBeingTeleported() || !CarteDeSuspension(p->GetMapId()))
    {
        RefuserApparition(c, lu, "carte");
        return;
    }
    if (lu.sous == EvApp::Invoquer)
        ExecuterInvoquer(c, lu, p);
    else
        ExecuterOrdre(c, lu, p);
}

// 10/10/2026 : la garde des egares, chaque seconde, pour un invoque VIVANT qui a recu `attaquer` (v.attaqueS).
// Egare : aux prises avec un PNJ (victime non controlee par un joueur : un bot inscrit qui le tient n'en est pas un),
// ou hors combat a plus de EV_EGARE_YD du joueur ; en combat contre un joueur ou un bot : jamais. Egare EV_EGARE_S secondes d'affilee : RELANCE (combat rompu s'il se bat
// contre un PNJ, AttackStart sur le joueur), EV_EGARE_RELANCES fois ; ensuite -> vrai (l'appelant le RETIRE et le
// compte). Un FORT (v.fort : chef, elite) est relance a chaque EV_EGARE_S et n'est jamais retire. Joueur absent, mort, sur une autre carte ou en teleportation : la garde attend (les fermetures implicites
// et la regle de mort du service jugent). Journal `sous_action=egare` (le service ne lit que `sous_action=invoquer`).
bool SuivreEgare(EvInvoque& v, Creature* cr, uint32 maintenant)
{
    Player* p = JoueurConnecte(v.joueur);
    if (!p || !p->IsInWorld() || p->IsBeingTeleported() || p->GetMapId() != cr->GetMapId() || !p->IsAlive())
    {
        v.egareDepuisS = 0;
        return false;
    }
    Unit* victime = cr->GetVictim();
    // 13/10 : aux prises avec un PROTEGE du convoi de son joueur, il fait ce qu'on lui a dit (`assaillir`) : pas un PNJ.
    bool const pnj = cr->IsInCombat() && victime && !victime->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED) &&
                     !EstProtegeDe(victime, v.joueur);
    float const d = cr->GetExactDist2d(p);
    if (!pnj && (cr->IsInCombat() || d <= EV_EGARE_YD))
    {
        v.egareDepuisS = 0;
        return false;
    }
    if (!v.egareDepuisS)
    {
        v.egareDepuisS = std::max<uint32>(maintenant, 1);
        return false;
    }
    if (maintenant - v.egareDepuisS < EV_EGARE_S)
        return false;
    if (v.relances < EV_EGARE_RELANCES || v.fort)   // un fort : relance a chaque EV_EGARE_S, jamais retire
    {
        if (v.relances < 255)
            ++v.relances;
        v.egareDepuisS = std::max<uint32>(maintenant, 1);
        bool relance = false;
        if (pnj)
            cr->CombatStop(true);
        if (cr->AI() && cr->IsValidAttackTarget(p))
        {
            cr->SetReactState(REACT_AGGRESSIVE);
            cr->AI()->AttackStart(p);
            relance = true;
        }
        LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=egare joueur={} valeur={} motif=relance"
                 " distance={:.0f} pnj={} relance={} relances={} fort={}", v.id, v.joueur, v.entree, d, pnj ? 1 : 0,
                 relance ? 1 : 0, v.relances, v.fort ? 1 : 0);
        return false;
    }
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=egare joueur={} valeur={} motif=retire"
             " distance={:.0f} pnj={} relances={}", v.id, v.joueur, v.entree, d, pnj ? 1 : 0, v.relances);
    return true;
}

// Chaque seconde (Entretien, bus allume ou eteint) : renforts inscrits ; joueur parti (hors ligne, autre
// carte) -> retrait immediat ; morts relevees ; niveau surveille (reecrit -> retire) ; retraits hors de
// vue ; fins de lot.
void SuivreInvoques()
{
    DrainerRenforts();   // toujours : la file se vide meme registre vide (renforts sans parent ignores)
    if (evInvoques.empty() && evLots.empty())
        return;
    uint32 const maintenant = Maintenant();

    // 1. Fermetures implicites (grimoire § 1.3) : le joueur deconnecte ou change de carte.
    std::vector<std::pair<uint32, char const*>> partis;
    for (EvInvoque const& v : evInvoques)
    {
        if (std::any_of(partis.begin(), partis.end(), [&v](auto const& x) { return x.first == v.joueur; }))
            continue;
        Player* p = JoueurConnecte(v.joueur);
        if (!p)
            partis.emplace_back(v.joueur, "hors_ligne");
        else if (p->IsInWorld() && !p->IsBeingTeleported() && p->GetMapId() != v.carte)
            partis.emplace_back(v.joueur, "carte");
    }
    for (auto const& [joueur, motif] : partis)
    {
        uint32 const j = joueur;
        LeverInvoquesSi([j](uint32, uint32 x) { return x == j; }, motif);
    }

    // 2. Chaque invoque.
    for (auto it = evInvoques.begin(); it != evInvoques.end();)
    {
        EvInvoque& v = *it;
        Creature* cr = CreatureInvoquee(v);
        if (!cr)
        {
            auto l = evLots.find(v.id);
            if (!v.mort && l != evLots.end())
                ++l->second.disparus;
            if (v.protege && !v.mort)
                JournaliserProtege(v, nullptr, "disparu");   // 13/10 : le convoi perdu sans mort vue (grille, echeance)
            it = evInvoques.erase(it);
            continue;
        }
        if (!v.mort && !cr->IsAlive())
        {
            NoterMort(v, cr);
            if (v.protege)
                JournaliserProtege(v, cr, "mort");             // 13/10 : tout de suite (le lot finit a la passe)
        }
        else if (!v.mort && cr->GetLevel() != v.niveau)
        {
            // Niveau reecrit apres la pose : le coeur ne porte pas l'exclusion CanScale de la plage
            // (OnAllCreatureUpdate d'Ascension, chaque seconde hors combat). Le monstre n'est plus a
            // la mesure voulue : RETIRE tout de suite (compte `retires`), jamais laisse en place.
            LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=invoquer joueur={} valeur={} motif=echelle"
                     " niveau_voulu={} niveau_lu={} renfort={} action=retire", v.id, v.joueur, v.entree, v.niveau,
                     cr->GetLevel(), v.renfort ? 1 : 0);
            EffacerInvoque(v);
            it = evInvoques.erase(it);
            continue;
        }
        if (v.retraitDepuisS &&
            (maintenant - v.retraitDepuisS >= EV_RETRAIT_HORS_VUE_MAX_S || !VueDUnHumain(cr)))
        {
            EffacerInvoque(v);
            it = evInvoques.erase(it);
            continue;
        }
        // 10/10 : arrivee `ciel` -- GARDE-FOU seulement : le coeur rend deja la gravite a ces gabarits terrestres
        // (Creature::UpdateMovementFlags) ; ceci la rend au cas ou un gabarit porterait un jour Flight != None.
        // Jamais avant 2 s, et jamais pendant la spline de MoveLand avant EV_CIEL_MAX_S.
        if (v.cielDepuisS && (v.mort || maintenant - v.cielDepuisS >= EV_CIEL_MAX_S ||
                              (maintenant - v.cielDepuisS >= 2 && cr->movespline->Finalized())))
        {
            if (!v.mort)
                cr->SetDisableGravity(false);
            v.cielDepuisS = 0;
        }
        // 13/10 : protege du convoi -- reaction passive, et son etat toutes les EV_CONVOI_ETAT_S secondes.
        SuivreProtege(v, cr, maintenant);
        // Revue 16/10 : assaillant dont le protege n'est plus -- l'immunite aux PNJ lui est rendue.
        SuivreAssaillant(v, cr);
        // 10/10 : garde des egares (relance, puis retrait compte).
        if (!v.mort && v.attaqueS && !v.retraitDepuisS && SuivreEgare(v, cr, maintenant))
        {
            auto l = evLots.find(v.id);
            if (l != evLots.end())
                ++l->second.egares;
            EffacerInvoque(v);
            it = evInvoques.erase(it);
            continue;
        }
        ++it;
    }

    // 2 bis. Lot G : renforts a mi-vie (avant les fins de lot : le lot du Gardien reste ouvert).
    DeclencherRenforts();
    // 2 ter. Chefs a mecaniques (10/10) : renforts a paliers 75/50/25 et bouclier lie (meme place que le mi-vie).
    ReprendrePaliers();     // relecture : evasion -> paliers et bouclier remis au debut
    DeclencherPaliers();
    SuivreBoucliers();

    // 3. Lots finis : plus aucun invoque VIVANT (08/10, commanditaire : « on attend betement entre les vagues ») :
    // la fin part des la derniere mort, sans attendre l'effacement des corps (~60 s). Les entrees mortes du
    // lot quittent le registre (morts deja comptes par NoterMort ; le corps disparait seul, TempSummon).
    std::vector<EvLot> finis;
    for (auto it = evLots.begin(); it != evLots.end();)
    {
        uint32 const id = it->first;
        if (std::none_of(evInvoques.begin(), evInvoques.end(),
                         [id](EvInvoque const& v) { return v.id == id && !v.mort; }))
        {
            finis.push_back(it->second);
            evInvoques.erase(std::remove_if(evInvoques.begin(), evInvoques.end(),
                                            [id](EvInvoque const& v) { return v.id == id; }),
                             evInvoques.end());
            it = evLots.erase(it);
        }
        else
            ++it;
    }
    for (EvLot const& l : finis)
    {
        JournaliserLot(l, l.retraitDemande ? "retire" : "fin");
        if (l.retraitDemande)
            CloreEvenement(l.id, l.joueur, "leve", "retirer");
    }
}

// Lot P des evenements (06/10/2026) : le portail a refermer (fragment a part, meme espace de noms).
#include "CoaDirecteurPortail.inc"
// Lot G des evenements (08/10/2026) : Gardien dose, renfort a mi-vie, coffre (fragment a part, meme espace).
#include "CoaDirecteurGardien.inc"
// Objectifs d'evenement (10/10/2026) : etats du monde au joueur (fragment a part, meme espace de noms).
#include "CoaDirecteurObjectifs.inc"
// Chefs a mecaniques (10/10/2026) : paliers, bouclier lie, degats en % (fragment a part, meme espace).
#include "CoaDirecteurBoss.inc"
// Convoi de bandits (13/10/2026, lot V) : protege, escorter, assaillir, etat du protege (fragment a part, meme espace).
#include "CoaDirecteurConvoi.inc"
// Ressources rares des evenements (11/10/2026) : la tranche suit le niveau du MONSTRE (fragment a part).
#include "CoaDirecteurRessources.inc"

// ------------------------------------------------------------------ lot D4c : prime et morts tracees
//
// SPEC-directeur-D4.md § 3.3, § 6.2. DEUX ETATS NEUFS :
//  - le magasin `primes`, meme verrou et meme patron que les treves : ecrit au
//    fil monde seul sous le verrou exclusif, lu par les fils de carte sous le
//    verrou partage (ProieImposee) apres son compteur atomique ;
//  - la file des morts, ecrite par les crochets de mort (fils de carte) sous
//    un verrou court et par valeur, videe par le fil monde seul (P-137).
// Aucune strategie touchee (P-130), aucun drapeau (P-134) : le FFA du joueur
// reste a la regle High Risk, celui du bot a playerbots.

constexpr uint32 PRIME_DUREE_MIN_S = 600;
constexpr uint32 PRIME_DUREE_MAX_S = 3600;

// Regle High Risk, dans l'ordre de P-138 : 1004119 est AUSSI porte en PvE, et le
// PvE (9931032) l'emporte sur tout.
bool HighRisk(Player* p)
{
    return p->HasAura(AURA_REGLE_HIGH_RISK) && !p->HasAura(AURA_REGLE_PVE);
}

// Le joueur porte-t-il une aura d'un defi bloquant (lues au demarrage) ? Fil
// monde. O(auras du joueur).
bool DefiBloquant(Player* p)
{
    if (aurasDefiBloquant.empty())
        return false;
    for (auto const& [sort, application] : p->GetAppliedAuras())
        if (aurasDefiBloquant.count(sort))
            return true;
    return false;
}

// Fil monde, au demarrage (DemarrerBus) : les auras des defis PVE_ONLY, a vies
// ou sans resurrection, depuis la DONNEE (acore_world.coa_challenge_definition
// et coa_challenge_spell, colonnes pve et pvp : listes d'ids separes par `;`),
// sans lien avec mod-coa-challenges. MESURE le 28/09 : 104 defis bloquants,
// 355 auras, aucune partagee avec un defi non bloquant ; TROIS defis bloquants
// n'ont AUCUNE aura (187 Hardcore, 201 et 202 Vitality) : le jeu ne les voit
// pas, le controle D4-3 du service les refuse a la pose (limite ecrite).
void ChargerDefisBloquants()
{
    aurasDefiBloquant.clear();
    defisLus = false;
    QueryResult tables = WorldDatabase.Query(
        "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()"
        " AND TABLE_NAME IN ('coa_challenge_definition', 'coa_challenge_spell')");
    if (!tables || (*tables)[0].Get<uint64>() != 2)
    {
        LOG_INFO("playerbots", "coa directeur : defis illisibles (tables absentes), toute prime refusee `defi`");
        return;
    }
    defisLus = true;
    QueryResult r = WorldDatabase.Query(
        "SELECT IFNULL(s.pve, ''), IFNULL(s.pvp, '') FROM coa_challenge_definition d"
        " JOIN coa_challenge_spell s ON s.challengeId = d.id"
        " WHERE d.rules LIKE '%CHALLENGE_RULES_TYPE_PVE_ONLY%' OR d.lives > 0 OR d.noResurrect = 1");
    if (r)
    {
        do
        {
            for (uint32 col = 0; col < 2; ++col)
            {
                std::string const liste = (*r)[col].Get<std::string>();
                size_t debut = 0;
                while (debut < liste.size())
                {
                    size_t fin = liste.find(';', debut);
                    if (fin == std::string::npos)
                        fin = liste.size();
                    std::string const morceau = liste.substr(debut, fin - debut);
                    if (!morceau.empty() && morceau.size() <= 10 &&
                        std::all_of(morceau.begin(), morceau.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
                    {
                        unsigned long long const v = std::strtoull(morceau.c_str(), nullptr, 10);
                        if (v > 0 && v <= 0xFFFFFFFFull)
                            aurasDefiBloquant.insert(static_cast<uint32>(v));
                    }
                    debut = fin + 1;
                }
            }
        } while (r->NextRow());
    }
    LOG_INFO("playerbots", "coa directeur : {} aura(s) de defi bloquant lue(s)", aurasDefiBloquant.size());
}

// Fin d'une prime retiree du magasin (hors verrou). `statut` nul : fin
// naturelle, rien en base (le service la connait : vu_le + duree_s).
// `leverChasseurs` : TOUTE fin de prime leve ses chasseurs, raison `fin_prime`
// (le jeu nettoie son propre etat sans attendre le service) -- sauf un
// remplacement, ou les chasseurs suivent la nouvelle prime du meme joueur.
void FinirPrime(Prime const& p, char const* statut, char const* raison, char const* motif, uint32 tueur,
                bool chasseurTueur, bool leverChasseurs)
{
    std::string_view const m(motif);
    if (m == "tombe")
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PRIMES_TOMBEES);
    else if (m == "echue")
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PRIMES_ECHUES);
    else if (m != "remplace")
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PRIMES_LEVEES);
    LOG_INFO("playerbots", "coa directeur prime: id={} joueur={} plafond={} motif={} tueur={} chasseur={}", p.id,
             p.joueur, p.plafond, motif, tueur, chasseurTueur ? 1 : 0);
    if (statut)
    {
        LOG_INFO("playerbots", "coa directeur cmd: id={} verbe=prime cible={} joueur={} statut={} raison={}", p.id,
                 p.joueur, p.joueur, statut, raison ? raison : "-");
        if (source)
            source->Clore(p.id, statut, raison);
    }
    if (leverChasseurs)
    {
        // Retraite (03/10/2026) : AVANT la levee, pour que chacun garde l'id de CETTE
        // prime (la levee y passe aussi, sans effet sur une retraite deja commencee).
        for (uint32 g : p.servis)
            CommencerRetraite(g, p.id, p.joueur, "fin_prime");
        uint32 const j = p.joueur;
        LeverAffectationsSi([j](AffectationInterne const& f) { return f.a.chasseur && f.joueur == j; }, "fin_prime");
    }
}

template <typename Predicat>
size_t LeverPrimesSi(Predicat lever, char const* raison)
{
    if (!nbPrimes.load(std::memory_order_relaxed))
        return 0;
    std::vector<Prime> levees;
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto it = primes.begin(); it != primes.end();)
        {
            if (lever(it->second))
            {
                levees.push_back(it->second);
                it = primes.erase(it);
            }
            else
                ++it;
        }
        nbPrimes.store(static_cast<uint32>(primes.size()), std::memory_order_relaxed);
    }
    for (Prime const& p : levees)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        FinirPrime(p, "leve", raison, raison, 0, false, true);
    }
    return levees.size();
}

// Verbe `prime` (§ 6.2). Cible = le joueur prime ; `joueur` vaut 0 ou le meme ;
// `valeur` = plafond de chasseurs. Fil monde, cartes a l'arret (P-137).
void ExecuterPrime(CoaDirCommande const& c)
{
    // 6. bornes
    if (c.cible == 0 || (c.joueur != 0 && c.joueur != c.cible) || c.dureeS < PRIME_DUREE_MIN_S ||
        c.dureeS > PRIME_DUREE_MAX_S || c.valeur < 1 ||
        static_cast<uint32>(c.valeur) > sPlayerbotAIConfig.directeurMaxChasseursParPrime)
    {
        Refuser(c, "bornes");
        return;
    }
    // 7. plafond : une prime reposee sur un joueur deja prime la remplace.
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        if (!primes.count(c.cible) && primes.size() >= sPlayerbotAIConfig.directeurMaxPrimes)
        {
            garde.unlock();
            Refuser(c, "plafond");
            return;
        }
    }
    // 8. en ligne, humain
    Player* p = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!p)
    {
        Refuser(c, "hors_ligne");
        return;
    }
    // Banc d'essai (03/10/2026) : un bot de Directeur.PrimeBanc tient lieu d'humain.
    bool const banc = EstBanc(p);
    if (!Humain(p) && !banc)
    {
        Refuser(c, "pas_humain");
        return;
    }
    // 9. controles propres. `mort` (ecart a la spec, ecrit) : une prime posee
    // sur un corps tomberait a la seconde suivante.
    if (!p->IsAlive())
    {
        Refuser(c, "mort");
        return;
    }
    if (p->IsGameMaster())
    {
        Refuser(c, "mj");
        return;
    }
    if (p->InBattleground() || p->InArena())
    {
        Refuser(c, "champ_de_bataille");
        return;
    }
    if (!CarteDeSuspension(p->GetMapId()))
    {
        Refuser(c, "carte");
        return;
    }
    if (!RegleDePrime(p, true))
    {
        Refuser(c, "regle");
        return;
    }
    if (!defisLus || DefiBloquant(p))
    {
        Refuser(c, "defi");
        return;
    }

    Prime nouvelle{c.id, c.cible, Maintenant() + c.dureeS, static_cast<uint32>(c.valeur), c.origine};
    // Reserve de chasseurs (02/10) : l'AFK est lu ici (fil monde), puis suivi
    // chaque seconde par SuivrePrimes.
    // Banc (03/10/2026) : l'AFK d'un bot est un artefact de playerbots (activite
    // minimale), jamais celui d'un joueur absent.
    nouvelle.afk = p->isAFK() && !banc;
    bool remplace = false, occupe = false;
    Prime ancienne{};
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        auto it = primes.find(c.cible);
        // Un proprietaire par prime (P-134) : seul le meme `origine` (le joueur
        // est la cible), ou l'operateur (`manuel`), la remplace.
        if (it != primes.end() && it->second.finS > Maintenant() && c.origine != "manuel" &&
            it->second.origine != c.origine)
        {
            occupe = true;
            ancienne = it->second;
        }
        else
        {
            if (it != primes.end())
            {
                remplace = true;
                ancienne = it->second;
                // Retraite (03/10/2026) : les chasseurs suivent la nouvelle prime, et
                // leur liste avec eux (toute origine).
                nouvelle.servis = ancienne.servis;
                // Revue M3 (embuscade) : une REPOSE de meme origine (le service
                // releve le plafond en reposant la prime) garde l'etat de
                // l'embuscade et l'id d'origine des replis. Un autre proprietaire
                // (`manuel` sur une prime `arc`...) repart de zero.
                if (ancienne.origine == c.origine && ancienne.finS > Maintenant())
                {
                    nouvelle.assaut = ancienne.assaut;
                    nouvelle.raisonAssaut = ancienne.raisonAssaut;
                    nouvelle.places = ancienne.places;
                    nouvelle.premierEnPositionS = ancienne.premierEnPositionS;
                    nouvelle.calmeDepuisS = ancienne.calmeDepuisS;
                    nouvelle.procheDepuisS = ancienne.procheDepuisS;
                    nouvelle.idOrigine = ancienne.idOrigine ? ancienne.idOrigine : ancienne.id;
                }
            }
            primes[c.cible] = nouvelle;
            nbPrimes.store(static_cast<uint32>(primes.size()), std::memory_order_relaxed);
        }
    }
    if (occupe)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_OCCUPEES);
        LOG_INFO("playerbots", "coa directeur occupe: id={} guid={} origine={} joueur={} par_id={} par_origine={}"
                 " par_verbe=prime", c.id, c.cible, c.origine, c.joueur, ancienne.id, ancienne.origine);
        Refuser(c, "occupe");
        return;
    }
    if (remplace)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REMPLACEES);
        std::string const par = Acore::StringFormat("par_{}", c.id);
        FinirPrime(ancienne, "remplace", par.c_str(), "remplace", 0, false, false);
        // Les chasseurs suivent la nouvelle prime ; ceux au-dela de son plafond
        // sont leves `plafond` (lesquels : l'ordre du magasin, arbitraire).
        uint32 gardes = 0;
        uint32 const plafond = nouvelle.plafond, j = c.cible;
        LeverAffectationsSi(
            [&gardes, plafond, j](AffectationInterne const& f)
            {
                if (!f.a.chasseur || f.joueur != j)
                    return false;
                return ++gardes > plafond;
            },
            "plafond");
    }
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PRIMES);
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots", "coa directeur prime: id={} joueur={} plafond={} motif=pose fin_s={} origine={}{}", c.id,
             c.cible, nouvelle.plafond, nouvelle.finS, c.origine, banc ? " banc=1" : "");
    Accuser(c, "execute", nullptr);
}

// Fil monde : type d'un bot vu dans une mort (R bot aleatoire, B autre bot).
// Deconnecte depuis : B (on ne sait plus le dire).
char TypeBot(uint32 guidLow)
{
    Player* p = JoueurConnecte(guidLow);
    return p && sRandomPlayerbotMgr.IsRandomBot(p) ? 'R' : 'B';
}

// Chaque seconde, fil monde : la file des morts devient des lignes
// `coa directeur mort:` (§ 3.3). AVANT SuivrePrimes (elle nomme le tueur d'un
// prime tombe) et SuivreAffectations (un chasseur tue a cette seconde y serait
// leve `mort` avant d'etre reconnu comme chasseur).
void DrainerMorts()
{
    uint32 const maintenant = Maintenant();
    while (!mortsRecentes.empty() && mortsRecentes.front().e.t + MORT_RECENTE_S < maintenant)
        mortsRecentes.pop_front();
    std::vector<EvenementMort> lot;
    {
        std::lock_guard<std::mutex> garde(verrouMorts);
        if (fileMorts.empty())
            return;
        lot.swap(fileMorts);
    }
    for (EvenementMort const& e : lot)
    {
        char const victimeType = e.victimeHumaine ? 'H' : TypeBot(e.victime);
        char const tueurType = e.tueurType == 'J' ? TypeBot(e.tueur) : e.tueurType;
        // Banc d'essai de prime (03/10/2026) : un bot de banc tient la place de l'humain
        // de cette mort (son type reste R ou B : la memoire des humains l'ignore).
        bool const victimeCible = e.victimeHumaine || e.victimeBanc;
        // L'humain de cette mort, et le bot en face (s'il y en a un).
        uint32 humain = victimeCible ? e.victime : (tueurType == 'H' ? e.tueur : 0);
        uint32 const bot = victimeCible ? ((tueurType == 'R' || tueurType == 'B') ? e.tueur : 0) : e.victime;
        uint32 prime = 0;
        bool chasseur = false;
        if ((humain || e.victimeChasseur) &&
            (nbPrimes.load(std::memory_order_relaxed) || nbChasseurs.load(std::memory_order_relaxed)))
        {
            std::shared_lock<std::shared_mutex> garde(verrouConsignes);
            auto a = bot ? affectations.find(bot) : affectations.end();
            // Lot R3 : un chasseur tue par un bot ou une creature -- l'humain est SON prime.
            if (!humain && e.victimeChasseur && a != affectations.end() && a->second.a.chasseur)
                humain = a->second.joueur;
            auto p = humain ? primes.find(humain) : primes.end();
            if (p != primes.end() && p->second.finS > maintenant)
                prime = p->second.id;
            chasseur = humain && a != affectations.end() && a->second.a.chasseur && a->second.joueur == humain;
        }
        // Lot R3 (03/10/2026) : le tueur (maitre d'un familier, deja ramene par le
        // crochet) est-il du GROUPE de l'humain de cette mort (lui excepte) ? Fil monde :
        // le groupe se lit sur le joueur connecte. `prime_joueur` : cet humain (0 : aucun).
        bool groupe = false;
        if (humain && e.tueur && e.tueur != humain)
            if (Player* ph = JoueurConnecte(humain))
                if (Group* gr = ph->GetGroup())
                    groupe = gr->IsMember(ObjectGuid::Create<HighGuid::Player>(e.tueur));
        ++nMorts;
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_MORTS_TRACEES);
        LOG_INFO("playerbots",
                 "coa directeur mort: epoque={} n={} t={} victime={} victime_type={} tueur={} tueur_type={} entree={}"
                 " niveau_v={} niveau_t={} carte={} zone={} aire={} x={:.1f} y={:.1f} prime={} chasseur={}"
                 " groupe_du_prime={} prime_joueur={}{}",
                 EpoqueMonde(), nMorts, e.t, e.victime, victimeType, e.tueur, tueurType, e.entree,
                 static_cast<uint32>(e.niveauV), static_cast<uint32>(e.niveauT), e.carte, e.zone, e.aire, e.x, e.y,
                 prime, chasseur ? 1 : 0, groupe ? 1 : 0, humain, e.victimeBanc ? " banc=1" : "");
        if (victimeCible)
            mortsRecentes.push_back(MortRecente{e, chasseur});
    }
}

// Chaque seconde, fil monde, cartes a l'arret (§ 6.2) : echeance, deconnexion,
// MORT DU JOUEUR (par son etat : corps ou fantome, quel que soit le tueur --
// chute, noyade et lave comprises -- et independamment de TracerMorts), regle
// quittee, defi bloquant commence en cours de prime. Meme patron que les
// affectations : copie sous verrou partage, examen hors verrou, retrait sous
// verrou exclusif (meme id), fins hors verrou.
void SuivrePrimes()
{
    if (!nbPrimes.load(std::memory_order_relaxed))
        return;
    uint32 const maintenant = Maintenant();
    std::vector<Prime> copies;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        copies.reserve(primes.size());
        for (auto const& [j, p] : primes)
            copies.push_back(p);
    }
    struct Fin
    {
        Prime p;
        char const* motif;
        uint32 tueur;
        bool chasseur;
    };
    std::vector<Fin> fins;
    // Reserve de chasseurs (02/10) : primes dont l'etat AFK du joueur a change
    // (id de la prime, nouvel etat). Lu ici, sur le joueur, au fil monde
    // (P-137) ; ecrit plus bas sous le verrou exclusif, SEULEMENT s'il change.
    std::vector<std::pair<Prime, bool>> afkChanges;
    for (Prime const& p : copies)
    {
        if (p.finS <= maintenant)
        {
            fins.push_back(Fin{p, "echue", 0, false});
            continue;
        }
        Player* j = JoueurConnecte(p.joueur);
        if (!j)
        {
            fins.push_back(Fin{p, "hors_ligne", 0, false});
            continue;
        }
        if (!j->IsInWorld() || j->IsBeingTeleported())
            continue;   // on repassera dans une seconde
        if (j->isDead())
        {
            // Le tueur, si la file des morts l'a vu (TracerMorts) : la mort la
            // plus recente de ce joueur, de moins de MORT_RECENTE_S.
            uint32 tueur = 0;
            bool chasseur = false;
            for (auto it = mortsRecentes.rbegin(); it != mortsRecentes.rend(); ++it)
                if (it->e.victime == p.joueur)
                {
                    tueur = it->e.tueur;
                    chasseur = it->chasseur;
                    break;
                }
            fins.push_back(Fin{p, "tombe", tueur, chasseur});
            continue;
        }
        // Banc d'essai (03/10/2026) : pour un humain, HighRisk a l'identique.
        if (!RegleDePrime(j, false))
        {
            fins.push_back(Fin{p, "regle", 0, false});
            continue;
        }
        // Revue D4c (m4) : MJ, champ de bataille, arene ou carte hors monde
        // ouvert : les chasseurs ne peuvent plus l'atteindre, la prime tombe.
        if (j->IsGameMaster())
        {
            fins.push_back(Fin{p, "mj", 0, false});
            continue;
        }
        if (j->InBattleground() || j->InArena() || !CarteDeSuspension(j->GetMapId()))
        {
            fins.push_back(Fin{p, "champ_de_bataille", 0, false});
            continue;
        }
        if (!defisLus || DefiBloquant(j))
        {
            fins.push_back(Fin{p, "defi", 0, false});
            continue;
        }
        bool const afk = j->isAFK() && !EstBanc(j);   // banc : voir ExecuterPrime
        if (afk != p.afk)
            afkChanges.emplace_back(p, afk);
    }
    if (!afkChanges.empty())
    {
        std::vector<std::pair<Prime, bool>> faits;
        {
            std::unique_lock<std::shared_mutex> garde(verrouConsignes);
            for (auto const& [p, afk] : afkChanges)
            {
                auto it = primes.find(p.joueur);
                if (it == primes.end() || it->second.id != p.id)
                    continue;
                it->second.afk = afk;
                // Revue m3 (embuscade) : a l'entree en AFK, l'assaut est rendu ;
                // au retour, l'approche reprend a couvert.
                if (afk)
                {
                    it->second.assaut = false;
                    it->second.raisonAssaut = '-';
                    it->second.premierEnPositionS = 0;
                    it->second.calmeDepuisS = 0;
                    it->second.procheDepuisS = 0;
                }
                faits.emplace_back(it->second, afk);
            }
        }
        // Meme forme que les autres lignes `prime:` (le service ne les lit pas,
        // MESURE le 02/10 : aucun lecteur dans exploitation/directeur).
        for (auto const& [p, afk] : faits)
            LOG_INFO("playerbots", "coa directeur prime: id={} joueur={} plafond={} motif={} tueur=0 chasseur=0", p.id,
                     p.joueur, p.plafond, afk ? "afk" : "de_retour");
    }
    if (fins.empty())
        return;
    std::vector<Fin> faites;
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        for (Fin const& f : fins)
        {
            auto it = primes.find(f.p.joueur);
            if (it == primes.end() || it->second.id != f.p.id)
                continue;
            primes.erase(it);
            faites.push_back(f);
        }
        nbPrimes.store(static_cast<uint32>(primes.size()), std::memory_order_relaxed);
    }
    for (Fin const& f : faites)
    {
        bool const naturelle = std::string_view(f.motif) == "echue";
        if (!naturelle)
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        else
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ECHUES);
        FinirPrime(f.p, naturelle ? nullptr : "leve", naturelle ? nullptr : f.motif, f.motif, f.tueur, f.chasseur,
                   true);
    }
}

// ------------------------------------------------------------------ embuscade (02/10)
//
// Option Directeur.Embuscade. Chaque seconde, fil monde, APRES SuivrePrimes (une
// prime finie n'a plus d'assaut a donner) : pour chaque prime, ses chasseurs (les
// affectations `chasser` de ce joueur), lus sur le joueur et sur eux, cartes a
// l'arret (P-137 : World::Update appelle OnWorldUpdate apres sMapMgr->Update, qui
// attend les fils de carte). Seul ecrivain de Prime::assaut, ::places,
// ::premierEnPositionS, ::calmeDepuisS (P-134).
//
// ACTIF : connecte, en vie, meme carte que le joueur, a PrimeDetectionYd au plus.
// EN POSITION : actif, a pied, a EMB_POSITION_MIN_YD-EMB_POSITION_MAX_YD du joueur,
// hors du cone de 120 degres devant lui -- ou furtif (il ne se voit pas).
// L'ASSAUT est donne, une fois, par la premiere raison vraie, dans cet ordre :
//   d) le joueur voit un chasseur furtif a EMB_REPERE_YD (CanSeeOrDetect joueur ->
//      bot, sans test de distance : la detection de furtivite porte la sienne), ou
//      il est en combat avec un chasseur (ou son familier) ;
//   b) il est en combat avec autre chose ;
//   c) il est sous 50 % de ses PV ;
//   a) tous les actifs sont en position, ou EMB_ATTENTE_S apres le premier (le
//      compte repart de zero des que plus personne n'est en position, revue m1) ;
//   e) un chasseur a EMB_PROCHE_YD depuis EMB_PROCHE_S (revue M5).
// b, c et e exigent un chasseur EN POSITION ou a EMB_PROCHE_YD (revue B1 : sans
// cela, un joueur en combat contre une creature donnait l'assaut a des chasseurs
// encore a 200-360 yd). Sans chasseur actif, aucun assaut. L'assaut RETOMBE
// (ligne raison=retombe) apres EMB_RETOMBE_S sans AUCUN chasseur actif (revue B1 :
// un chasseur encore en approche ne fait plus retomber l'assaut) : une vague
// suivante s'approche alors de nouveau a couvert. A l'entree en AFK, l'assaut est
// remis a faux (SuivrePrimes, revue m3).
constexpr float EMB_POSITION_MIN_YD = 18.0f;
constexpr float EMB_POSITION_MAX_YD = 40.0f;
constexpr float EMB_CONE_DEMI_RAD = float(M_PI) / 3.0f;   // 60 degres de part et d'autre de l'orientation
constexpr float EMB_REPERE_YD = 60.0f;
constexpr float EMB_ENGAGE_YD = 45.0f;
constexpr uint32 EMB_ATTENTE_S = 20;
// Banc du 03/10 (arc 15) : l'assaut `a` partait 20 s apres le premier en position alors que
// le second chasseur etait encore a 136 yd (premiers coups non synchronises). Le delai ne
// donne l'assaut que si TOUS les actifs sont a EMB_PROCHE_YD, ou au plus tard EMB_ATTENTE_MAX_S.
constexpr uint32 EMB_ATTENTE_MAX_S = 45;
constexpr uint32 EMB_RETOMBE_S = 15;
// Revue B1 : b, c et e ne valent que si un chasseur est en position OU a
// EMB_PROCHE_YD du joueur ; apres l'assaut, un chasseur plus loin garde l'approche
// discrete (CoaChasse.h, ProcheAssautYd, meme valeur).
constexpr float EMB_PROCHE_YD = 60.0f;
// Revue M5 : un chasseur a EMB_PROCHE_YD depuis EMB_PROCHE_S sans assaut (le joueur
// bouge, personne ne tient son point) : raison e.
constexpr uint32 EMB_PROCHE_S = 45;

// Angle de (x, y) vu du joueur, relatif a son orientation, dans [-pi, pi].
float AngleVuDuJoueur(Player const* j, float x, float y)
{
    float a = std::atan2(y - j->GetPositionY(), x - j->GetPositionX()) - j->GetOrientation();
    while (a > float(M_PI))
        a -= 2.0f * float(M_PI);
    while (a < -float(M_PI))
        a += 2.0f * float(M_PI);
    return a;
}

void SuivreEmbuscades()
{
    // Pas de garde sur nbChasseurs (02/10, arcs 7 et 9) : une vague entierement
    // tombee n'a plus d'affectation, et sortir ici figeait l'assaut a vrai --
    // la vague suivante partait "a l'assaut" des son saut, a 210 yd, au lieu de
    // s'approcher a couvert. C'est justement sans chasseur que l'assaut retombe.
    if (!sPlayerbotAIConfig.directeurEmbuscade || !nbPrimes.load(std::memory_order_relaxed))
        return;
    uint32 const maintenant = Maintenant();
    struct Vue
    {
        Prime p;
        std::vector<uint32> chasseurs;   // guid bas, tries (places attribuees dans cet ordre)
    };
    std::vector<Vue> vues;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        vues.reserve(primes.size());
        for (auto const& [j, p] : primes)
            if (p.finS > maintenant)
                vues.push_back(Vue{p, {}});
        for (auto const& [guid, f] : affectations)
            if (f.a.chasseur && f.a.finS > maintenant)
                for (Vue& v : vues)
                    if (v.p.joueur == f.joueur)
                        v.chasseurs.push_back(guid);
    }
    float const detection = float(sPlayerbotAIConfig.directeurPrimeDetectionYd);
    struct Ecrit
    {
        uint32 joueur, id;
        bool assaut;
        char raison;
        uint32 premier, calme, proche;
        std::array<uint32, CoaDirecteur::EMBUSCADE_PLACES> places;
    };
    std::vector<Ecrit> ecrits;
    for (Vue& v : vues)
    {
        Prime& p = v.p;
        std::sort(v.chasseurs.begin(), v.chasseurs.end());
        // Les places : rendues au depart du chasseur, attribuees a son arrivee (la
        // plus basse libre). Stables tant qu'il reste : son angle ne saute pas.
        for (uint32& g : p.places)
            if (g && !std::binary_search(v.chasseurs.begin(), v.chasseurs.end(), g))
                g = 0;
        for (uint32 g : v.chasseurs)
        {
            if (std::find(p.places.begin(), p.places.end(), g) != p.places.end())
                continue;
            auto libre = std::find(p.places.begin(), p.places.end(), 0u);
            if (libre != p.places.end())
                *libre = g;
        }
        Player* j = JoueurConnecte(p.joueur);
        // Relecture : prime AFK -- ses chasseurs se tiennent a 100 yd (CoaChasse.h) ;
        // aucun assaut n'est donne ni ne retombe pendant ce temps (sinon un combat
        // ou des PV bas pendant l'AFK le donneraient, et il partirait au retour).
        bool const lisible = j && j->IsInWorld() && !j->IsBeingTeleported() && j->IsAlive() && !p.afk;
        uint32 actifs = 0, enPosition = 0, proches = 0;
        char raison = 0;
        if (lisible)
        {
            for (uint32 g : v.chasseurs)
            {
                Player* b = JoueurConnecte(g);
                if (!b || !b->IsInWorld() || b->IsBeingTeleported() || !b->IsAlive() || b->GetMapId() != j->GetMapId())
                    continue;
                float const d = b->GetExactDist2d(j);
                if (d > detection)
                    continue;
                ++actifs;
                bool const furtif = b->HasAuraType(SPELL_AURA_MOD_STEALTH);
                bool const horsCone = std::fabs(AngleVuDuJoueur(j, b->GetPositionX(), b->GetPositionY())) >= EMB_CONE_DEMI_RAD;
                if (d >= EMB_POSITION_MIN_YD && d <= EMB_POSITION_MAX_YD && !b->IsMounted() && (furtif || horsCone))
                    ++enPosition;
                bool const enCombat = b->IsInCombatWith(j);
                if (enCombat || d <= EMB_PROCHE_YD)
                    ++proches;
                if (!raison && (enCombat || (furtif && d <= EMB_REPERE_YD && j->CanSeeOrDetect(b))))
                    raison = 'd';
            }
            // b) et d) par les references de combat du JOUEUR : un chasseur (ou ce
            // qu'il controle) -> d ; toute autre unite -> b.
            if (!raison && j->IsInCombat())
            {
                auto const classer = [&](Unit* autre)
                {
                    if (!autre)
                        return;
                    Unit* maitre = autre->GetCharmerOrOwnerOrSelf();
                    bool const chasseur = maitre && maitre->IsPlayer() &&
                                          std::binary_search(v.chasseurs.begin(), v.chasseurs.end(),
                                                             maitre->GetGUID().GetCounter());
                    if (chasseur)
                        raison = 'd';
                    else if (raison != 'd')
                        raison = 'b';
                };
                for (auto const& [guid, ref] : j->GetCombatManager().GetPvPCombatRefs())
                    classer(ref->GetOther(j));
                for (auto const& [guid, ref] : j->GetCombatManager().GetPvECombatRefs())
                    classer(ref->GetOther(j));
            }
            if (!raison && j->GetHealthPct() < 50.0f)
                raison = 'c';
            // Revue B1 : b et c seulement si un chasseur est en position ou proche.
            bool const aPortee = enPosition || proches;
            if ((raison == 'b' || raison == 'c') && !aPortee)
                raison = 0;
            // Revue m1 : plus personne en position -> le compte de a repart de zero.
            if (!enPosition)
                p.premierEnPositionS = 0;
            else if (!p.premierEnPositionS)
                p.premierEnPositionS = maintenant;
            if (!raison && actifs &&
                (enPosition == actifs ||
                 (p.premierEnPositionS && maintenant >= p.premierEnPositionS + EMB_ATTENTE_S &&
                  (proches >= actifs || maintenant >= p.premierEnPositionS + EMB_ATTENTE_MAX_S))))
                raison = 'a';
            // Revue M5 : e.
            if (p.assaut || !proches)
                p.procheDepuisS = 0;
            else if (!p.procheDepuisS)
                p.procheDepuisS = maintenant;
            if (!raison && proches && p.procheDepuisS && maintenant >= p.procheDepuisS + EMB_PROCHE_S)
                raison = 'e';
        }
        bool const avant = p.assaut;
        if (!p.assaut && lisible && actifs && raison)
        {
            p.assaut = true;
            p.raisonAssaut = raison;
            p.calmeDepuisS = 0;
            p.procheDepuisS = 0;
            LOG_INFO("playerbots", "coa directeur assaut: prime={} joueur={} chasseurs={} en_position={} raison={}",
                     p.id, p.joueur, actifs, enPosition, raison);
        }
        else if (avant && lisible)
        {
            // L'assaut retombe sans aucun chasseur ACTIF pendant EMB_RETOMBE_S
            // (revue B1 : un chasseur en approche le tient).
            if (actifs)
                p.calmeDepuisS = 0;
            else if (!p.calmeDepuisS)
                p.calmeDepuisS = maintenant;
            else if (maintenant >= p.calmeDepuisS + EMB_RETOMBE_S)
            {
                p.assaut = false;
                p.raisonAssaut = '-';
                p.premierEnPositionS = 0;
                p.calmeDepuisS = 0;
                LOG_INFO("playerbots", "coa directeur assaut: prime={} joueur={} chasseurs={} en_position={} raison=retombe",
                         p.id, p.joueur, actifs, enPosition);
            }
        }
        ecrits.push_back(Ecrit{p.joueur, p.id, p.assaut, p.raisonAssaut, p.premierEnPositionS, p.calmeDepuisS,
                               p.procheDepuisS, p.places});
    }
    if (ecrits.empty())
        return;
    std::unique_lock<std::shared_mutex> garde(verrouConsignes);
    for (Ecrit const& e : ecrits)
    {
        auto it = primes.find(e.joueur);
        // La prime a pu etre remplacee entre les deux verrous (fil monde seul :
        // impossible aujourd'hui, garde par l'id quand meme).
        if (it == primes.end() || it->second.id != e.id)
            continue;
        it->second.assaut = e.assaut;
        it->second.raisonAssaut = e.raison;
        it->second.premierEnPositionS = e.premier;
        it->second.calmeDepuisS = e.calme;
        it->second.procheDepuisS = e.proche;
        it->second.places = e.places;
    }
}

// ------------------------------------------------------------------ banc d'essai : la trace (03/10/2026)
//
// Chaque seconde, fil monde, cartes a l'arret (P-137), APRES SuivreEmbuscades : pour
// toute prime dont la cible est un bot de BANC (Directeur.PrimeBanc), une ligne pour
// la cible et une par chasseur affecte. Seule source des mesures fines du banc
// (outils/banc-prime.py) : distance et angle vus de la cible, monture, furtivite,
// combat et victime, ligne de vue. Sans banc : un chargement atomique.
//   vu=1 : la cible VOIT le chasseur -- a BANC_VISIBILITE_YD au plus, detectable
//   (CanSeeOrDetect : furtivite comprise), en ligne de vue, et dans le cone de 120
//   degres devant elle ; `ligne` : les memes tests SANS le cone.
constexpr float BANC_VISIBILITE_YD = 100.0f;   // Visibility.Distance.Continents (worldserver.conf, lu le 27/09)

void TracerBanc()
{
    if (!nbBancs.load(std::memory_order_relaxed) || !nbPrimes.load(std::memory_order_relaxed))
        return;
    uint32 const maintenant = Maintenant();
    struct Vue
    {
        Prime p;
        std::vector<std::pair<uint32, CoaDirecteur::CoaDirPhase>> chasseurs;
    };
    std::vector<Vue> vues;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto const& [j, p] : primes)
            if (p.finS > maintenant)
                vues.push_back(Vue{p, {}});
        for (Vue& v : vues)
            for (auto const& [guid, f] : affectations)
                if (f.a.chasseur && f.joueur == v.p.joueur && f.a.finS > maintenant)
                    v.chasseurs.emplace_back(guid, f.a.phase);
    }
    for (Vue const& v : vues)
    {
        // Hors du verrou des consignes : verrouBancs n'est jamais pris sous un autre verrou.
        if (!CoaDirecteur::CibleDeBanc(v.p.joueur))
            continue;
        Player* j = JoueurConnecte(v.p.joueur);
        if (!j || !j->IsInWorld() || j->IsBeingTeleported())
            continue;
        Unit const* vj = j->GetVictim();
        LOG_INFO("playerbots",
                 "coa directeur banc: prime={} joueur={} t={} role=cible guid={} carte={} x={:.1f} y={:.1f} o={:.2f} pv={}"
                 " vivant={} combat={} victime={} assaut={} raison={} chasseurs={}",
                 v.p.id, v.p.joueur, maintenant, v.p.joueur, j->GetMapId(), j->GetPositionX(), j->GetPositionY(),
                 j->GetOrientation(), static_cast<uint32>(j->GetHealthPct()), j->IsAlive() ? 1 : 0,
                 j->IsInCombat() ? 1 : 0, vj ? vj->GetGUID().GetCounter() : 0, v.p.assaut ? 1 : 0, v.p.raisonAssaut,
                 v.chasseurs.size());
        for (auto const& [g, phase] : v.chasseurs)
        {
            Player* b = JoueurConnecte(g);
            if (!b || !b->IsInWorld() || b->IsBeingTeleported() || b->GetMapId() != j->GetMapId())
            {
                LOG_INFO("playerbots", "coa directeur banc: prime={} joueur={} t={} role=chasseur guid={} etat={}",
                         v.p.id, v.p.joueur, maintenant, g,
                         !b ? "hors_ligne" : (b->GetMapId() != j->GetMapId() ? "autre_carte" : "en_saut"));
                continue;
            }
            float const d = b->GetExactDist2d(j);
            float const angle = AngleVuDuJoueur(j, b->GetPositionX(), b->GetPositionY()) * 180.0f / float(M_PI);
            bool const furtif = b->HasAuraType(SPELL_AURA_MOD_STEALTH);
            bool ligne = false;
            if (d <= BANC_VISIBILITE_YD && b->IsAlive())
                ligne = j->CanSeeOrDetect(b) && j->IsWithinLOSInMap(b);
            bool const vu = ligne && std::fabs(angle) < 60.0f;
            // Victime : la cible (J), un autre joueur (P), une creature (C, avec son entree).
            Unit const* vb = b->GetVictim();
            char type = '-';
            uint32 entree = 0;
            if (vb)
            {
                if (vb == j)
                    type = 'J';
                else if (vb->IsPlayer())
                    type = 'P';
                else
                {
                    type = 'C';
                    if (Creature const* cr = vb->ToCreature())
                        entree = cr->GetEntry();
                }
            }
            LOG_INFO("playerbots",
                     "coa directeur banc: prime={} joueur={} t={} role=chasseur guid={} etat={} phase={} d={:.1f} angle={:.0f}"
                     " x={:.1f} y={:.1f} monte={} furtif={} combat={} frappe={} victime={} entree={} vu={} ligne={} pv={}",
                     v.p.id, v.p.joueur, maintenant, g, b->IsAlive() ? "vivant" : "mort", NomPhase(phase), d, angle,
                     b->GetPositionX(), b->GetPositionY(), b->IsMounted() ? 1 : 0, furtif ? 1 : 0,
                     b->IsInCombat() ? 1 : 0, b->IsInCombatWith(j) ? 1 : 0, type, entree, vu ? 1 : 0, ligne ? 1 : 0,
                     static_cast<uint32>(b->GetHealthPct()));
        }
    }
}

// Crochets de mort (FIL DE CARTE, Unit::Kill) : un evenement par valeur, et
// rien d'autre. Seulement si un HUMAIN est victime ou tueur (session sans IA de
// playerbots) ; les morts de bot a bot -- l'immense majorite -- ne coutent que
// ce test. Les types R/B des bots sont resolus au fil monde (DrainerMorts).
//
// Lot R3 (03/10/2026, coa-project/docs/CONCEPTION-recompenses-primes.md § 2.2) : aussi
// la mort d'un CHASSEUR DE PRIME, quel que soit son tueur (compagnon bot du prime,
// creature, garde) : sans elle, une vague abattue par le groupe du joueur etait
// indiscernable d'une vague perdue (arc 6, bot 2521, 02/10). Hors prime : un test
// atomique de plus (nbChasseurs), aucun verrou.
void PousserMort(Player* victime, Player* tueurJoueur, Creature* tueurCreature)
{
    bool const victimeHumaine = Humain(victime);
    bool const tueurHumain = tueurJoueur && Humain(tueurJoueur);
    bool victimeChasseur = false;
    // Banc d'essai de prime (03/10/2026) : sans banc, un chargement atomique (CibleDeBanc).
    bool const victimeBanc = !victimeHumaine && CoaDirecteur::CibleDeBanc(victime->GetGUID().GetCounter());
    if (!victimeHumaine && !tueurHumain && !victimeBanc)
    {
        if (!nbChasseurs.load(std::memory_order_relaxed))
            return;
        std::lock_guard<std::mutex> garde(verrouMorts);
        if (!chasseursSuivis.count(victime->GetGUID().GetCounter()))
            return;
        victimeChasseur = true;
    }
    EvenementMort e;
    e.victimeChasseur = victimeChasseur;
    e.victimeBanc = victimeBanc;
    e.victime = victime->GetGUID().GetCounter();
    e.victimeHumaine = victimeHumaine;
    e.niveauV = victime->GetLevel();
    e.carte = victime->GetMapId();
    e.zone = victime->GetZoneId();
    e.aire = victime->GetAreaId();
    e.x = victime->GetPositionX();
    e.y = victime->GetPositionY();
    e.t = Maintenant();
    if (tueurJoueur)
    {
        e.tueur = tueurJoueur->GetGUID().GetCounter();
        e.tueurType = tueurHumain ? 'H' : 'J';
        e.niveauT = tueurJoueur->GetLevel();
    }
    else if (tueurCreature)
    {
        e.tueurType = 'C';
        e.entree = tueurCreature->GetEntry();
        e.niveauT = tueurCreature->GetLevel();
    }
    std::lock_guard<std::mutex> garde(verrouMorts);
    if (fileMorts.size() >= MORTS_FILE_MAX)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_MORTS_PERDUES);
        return;
    }
    fileMorts.push_back(e);
}

// Vrai groupe (09/10/2026, coa-project/docs/EVENEMENTS-groupe.md § 2) : un BOT tue par un bot (ou par le familier d'un
// bot) PACTE POUR LE MEME JOUEUR. Apres la Nuit du 09/10, la mort d'Ulmian 40 s apres le dernier monstre n'avait aucun
// tueur lisible : TracerMorts ne pousse que les morts ou un humain est victime ou tueur (PousserMort). Cette ligne est
// la preuve, au prochain essai, qu'un combat entre bots a survecu (ou non) au pacte et au groupe. FIL DE CARTE (crochet
// de mort) : deux tests d'humain, PacteEntreBots (un chargement atomique sans pacte, sinon le verrou partage des
// consignes SEUL, comme AttackersValue : jamais avec verrouMorts), un compteur et une ligne. Independante de
// TracerMorts. Le crochet ne donne pas le sort qui a tue : la ligne ne le dit pas.
void TracerMortEntrePactes(Player* victime, Player* tueur, Creature* familier)
{
    if (!victime || !tueur || victime == tueur || Humain(victime) || Humain(tueur))
        return;
    uint32 const v = victime->GetGUID().GetCounter();
    uint32 const t = tueur->GetGUID().GetCounter();
    if (!CoaDirecteur::PacteEntreBots(v, t))
        return;
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_MORTS_ENTRE_PACTES);
    LOG_INFO("playerbots",
             "coa directeur mort_entre_pactes: victime={} tueur={} familier={} niveau_v={} niveau_t={} groupe={}"
             " carte={} zone={} x={:.1f} y={:.1f}",
             v, t, familier ? familier->GetEntry() : 0, victime->GetLevel(), tueur->GetLevel(),
             victime->GetGroup() && victime->GetGroup() == tueur->GetGroup() ? 1 : 0, victime->GetMapId(),
             victime->GetZoneId(), victime->GetPositionX(), victime->GetPositionY());
}

// Lot S des evenements (08/10/2026) : le soin des bots envers le joueur (fragment a part, meme espace de noms).
// Lot D5 « l'imprevu » (15/10/2026) : verbes `attirer` et `soigner` (fragment a part, meme espace ; AVANT le lot S,
// dont JoueurASoigner consulte le patient impose).
#include "CoaDirecteurImprevu.inc"
#include "CoaDirecteurSoin.inc"
// Vrai groupe (09/10/2026) : les inscrits rejoignent le groupe du joueur servi (fragment a part, meme espace de noms).
#include "CoaDirecteurGroupe.inc"

// ------------------------------------------------------------------ lot F des evenements (06/10/2026) : les bots inscrits
//
// coa-project/docs/EVENEMENTS-lot-F.md ; grimoire § 2.7 (`mission: evenement`), § 5.2 (lot F, reduit au
// besoin de la Faille d'orage). Retour du commanditaire apres la Faille du 06/10 : « les bots, ils n'ont
// strictement rien a faire de l'evenement ». MESURE ce soir-la (faille-orage.log, CoaBots.log) : le bot
// appele est arrive, puis, presence SANS mission, a repris son RPG ordinaire (camp, grind lointain, quete)
// sous la seule laisse de PresenceLaisseYd (110 yd) -- ligne `laisse ... sejour=1 mission=0` a 21:49:11
// (Paris) -- et n'etait plus a 100 yd au recompte de la vague 2 ; `tues_bot=0` sur les 6 lots.
// G1, l'INSCRIPTION, n'a pas de registre propre : c'est l'affectation de presence (finale `Q-`, mission
// Evenement, missionParam = joueur) -- voir LireMission et CoaDirecteur.h. Ce bloc porte G2 : la LISTE DES
// CIBLES. Le registre du lot D (evInvoques) est au fil monde seul (P-137) ; les fils de carte qui choisissent
// une cible (GrindTargetValue) n'y touchent jamais : le fil monde en publie chaque seconde une COPIE (guids
// des invoques vivants, par joueur) sous evfVerrou, le seul verrou du lot F. La creature elle-meme est relue
// par le bot, depuis son fil de carte (meme carte : ObjectAccessor::GetCreature sur le bot).
// Option Directeur.EvenementBots eteinte : rien n'est publie, CibleDEvenement rend nul au premier test.

std::shared_mutex evfVerrou;                                      // le SEUL verrou du lot F
std::unordered_map<uint32, std::vector<ObjectGuid>> evfCibles;    // cle : guid bas du joueur ; sous evfVerrou
std::atomic<uint32> evfNbJoueurs{0};                              // evfCibles.size(), chemin chaud sans verrou

// Vrai groupe (09/10/2026, CoaDirecteurGroupe.inc, sortie `sans_evenement`) : un invoque de l'evenement est-il publie
// pour ce joueur ? Fil monde.
bool EvenementPubliePour(uint32 joueurLow)
{
    if (!evfNbJoueurs.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(evfVerrou);
    return evfCibles.count(joueurLow) != 0;
}
std::atomic<uint32> evfEvaluations{0};                            // cibles rendues a GrindTargetValue (bilan) :
                                                                  // des EVALUATIONS, ni des attaques ni des bots
std::atomic<uint32> evfAssaillants{0};                            // bots actifs (07/10) : calculs de « attackers »
                                                                  // ou un invoque A PORTEE a ete ajoute (bilan)
std::atomic<uint32> evfDefenses{0};                               // ... ou un invoque qui frappe un humain de
                                                                  // l'evenement ou un inscrit a ete ajoute (bilan)
// Bots reactifs (08/10, docs/EVENEMENTS-bots-reactifs.md) : ce que les deux compteurs ci-dessus ne disent pas.
// `defenses`/`assaillants` comptent des invoques AJOUTES avant le tri d'origine, qui retirait toute DEFENSE (anti vol
// de kill, AttackersValue::IsPossibleTarget : victime hors du groupe du bot). Ceux-ci disent ce qui en reste et ce
// que les bots en font.
std::atomic<uint32> evfAdmis{0};                                  // IsPossibleTarget : EVALUATIONS ou un invoque a ete
                                                                  // admis malgre l'anti vol de kill (plusieurs par
                                                                  // seconde et par bot : ni des bots ni des attaques)
std::atomic<uint32> evfRetenus{0};                                // calculs de « attackers » ou un invoque ajoute a
                                                                  // SURVECU au tri d'origine
std::atomic<uint32> evfFiltres{0};                                // ... ou TOUS les invoques ajoutes ont ete retires
// ENGAGEMENT REEL, fil monde seul (aucun verrou) : relu chaque seconde sur les invoques publies -- un bot INSCRIT a
// l'evenement de l'invoque dont la cible de melee/combat (GetVictim) est cet invoque (Unit::getAttackers).
std::unordered_set<uint32> evfEngages;                            // guids bas des inscrits engages sur la periode
uint32 evfEngageS = 0;                                            // somme, par seconde, des inscrits engages
uint32 evfBilanS = 0;                                             // fil monde : derniere ligne de bilan
constexpr uint32 EVF_BILAN_S = 60;

// Fil monde (PublierCiblesEvenement) : les inscrits engages sur l'invoque `cr` de l'evenement du joueur `joueur`.
void CompterEngages(Creature* cr, uint32 joueur)
{
    for (Unit* u : cr->getAttackers())
    {
        Player* p = u ? u->ToPlayer() : nullptr;
        if (!p || !p->IsAlive() || Humain(p))
            continue;
        CoaDirecteur::Affectation aff;
        if (!CoaDirecteur::AffectationDe(p->GetGUID().GetCounter(), &aff) || !aff.presence ||
            aff.mission != CoaDirecteur::CoaDirMission::Evenement || aff.missionParam != joueur)
            continue;
        evfEngages.insert(p->GetGUID().GetCounter());
        ++evfEngageS;
    }
}

// Fil monde, chaque seconde (Entretien, apres SuivreInvoques).
void PublierCiblesEvenement()
{
    std::unordered_map<uint32, std::vector<ObjectGuid>> neuf;
    if (sPlayerbotAIConfig.directeurEvenementBots)
        for (EvInvoque const& v : evInvoques)
            // relecture : un invoque en cours de retrait n'est plus une cible ; 13/10 : un protege du convoi jamais.
            if (!v.mort && !v.retraitDepuisS && !v.protege)
            {
                neuf[v.joueur].push_back(v.guid);
                // Bots reactifs (08/10) : engagement reel, lu ici (fil monde, cartes a l'arret, P-137).
                if (Creature* cr = CreatureInvoquee(v))
                    if (cr->IsInWorld() && cr->IsAlive())
                        CompterEngages(cr, v.joueur);
            }
    uint32 const avant = evfNbJoueurs.load(std::memory_order_relaxed);
    if (neuf.empty() && avant == 0)
        return;   // rien publie, rien a publier : aucun verrou
    uint32 cibles = 0;
    for (auto const& [joueur, guids] : neuf)
        cibles += static_cast<uint32>(guids.size());
    uint32 const joueurs = static_cast<uint32>(neuf.size());
    {
        std::unique_lock<std::shared_mutex> garde(evfVerrou);
        evfCibles.swap(neuf);
        evfNbJoueurs.store(joueurs, std::memory_order_relaxed);
    }
    uint32 const maintenant = Maintenant();
    // Bots reactifs (08/10) : une ligne AUSSI a la fin de la publication (dernier invoque mort ou retire), pour que
    // chaque vague ait son bilan -- avant, ce qui suivait la derniere ligne de 60 s etait reporte a l'evenement
    // suivant (doc bots actifs, § 1 : « aucune ligne ne couvre la vague 2 »).
    bool const finPublication = !joueurs && avant;
    if (!finPublication && (!joueurs || maintenant < evfBilanS + EVF_BILAN_S))
        return;
    evfBilanS = maintenant;
    uint32 inscrits = 0;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto const& [guid, f] : affectations)
            if (f.a.presence && f.a.mission == CoaDirecteur::CoaDirMission::Evenement && f.a.finS > maintenant)
                ++inscrits;
    }
    // Bots actifs (07/10) : deux champs EN FIN de ligne (un lecteur de l'ancienne forme s'arrete a evaluations).
    // Bots reactifs (08/10) : cinq champs de plus, EN FIN de ligne (l'ancienne forme reste un prefixe).
    //   retenus  : calculs de « attackers » ou un invoque ajoute a survecu au tri d'origine ;
    //   filtres  : calculs ou tous les invoques ajoutes ont ete retires par ce tri ;
    //   admis    : invoques admis par IsPossibleTarget malgre l'anti vol de kill ;
    //   engages  : inscrits DISTINCTS vus en combat sur un invoque (GetVictim, relu chaque seconde) ;
    //   engage_s : inscrits-secondes de ce combat. C'est la preuve d'engagement ; les autres sont des evaluations.
    uint32 const engages = static_cast<uint32>(evfEngages.size());
    uint32 const engageS = evfEngageS;
    evfEngages.clear();
    evfEngageS = 0;
    LOG_INFO("playerbots", "coa directeur evenement_bots: joueurs={} cibles={} inscrits={} evaluations={} assaillants={}"
             " defenses={}"
             " retenus={} filtres={} admis={} engages={} engage_s={} fin={}"
             " soins={} soins_pv={} soin_choix={}",
             joueurs, cibles, inscrits, evfEvaluations.exchange(0, std::memory_order_relaxed),
             evfAssaillants.exchange(0, std::memory_order_relaxed), evfDefenses.exchange(0, std::memory_order_relaxed),
             evfRetenus.exchange(0, std::memory_order_relaxed), evfFiltres.exchange(0, std::memory_order_relaxed),
             evfAdmis.exchange(0, std::memory_order_relaxed), engages, engageS, finPublication ? 1 : 0,
             evsSoins.exchange(0, std::memory_order_relaxed), evsSoinsPv.exchange(0, std::memory_order_relaxed),
             evsSoinChoix.exchange(0, std::memory_order_relaxed));
}
}  // namespace (lot F : la fonction publique, puis retour a l'espace anonyme)

Unit* CoaDirecteur::CibleDEvenement(Player* bot)
{
    if (!bot || evfNbJoueurs.load(std::memory_order_relaxed) == 0)
        return nullptr;
    CoaDirecteur::Affectation aff;
    if (!CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) || !aff.presence ||
        aff.mission != CoaDirecteur::CoaDirMission::Evenement || !aff.missionParam || aff.carte != bot->GetMapId() ||
        !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive())
        return nullptr;
    std::vector<ObjectGuid> guids;
    {
        std::shared_lock<std::shared_mutex> garde(evfVerrou);
        auto const it = evfCibles.find(aff.missionParam);
        if (it == evfCibles.end())
            return nullptr;
        guids = it->second;
    }
    float const laisse = aff.rayon + CoaDirecteur::MISSION_LAISSE_MARGE_YD;
    // Chefs a mecaniques (10/10) : un invoque INSENSIBLE (bouclier du chef : immunite physique et magique) n'est choisi
    // que si aucune autre cible d'evenement n'est valide -- les bots frappent les renforts qui portent le bouclier.
    Creature* meilleure = nullptr;
    float meilleureD = 0.0f;
    Creature* protegee = nullptr;
    float protegeeD = 0.0f;
    for (ObjectGuid const& g : guids)
    {
        Creature* cr = ObjectAccessor::GetCreature(*bot, g);   // carte du bot : nul si l'invoque est ailleurs
        if (!cr || !cr->IsInWorld() || cr->IsDuringRemoveFromWorld() || !cr->IsAlive())
            continue;
        float const cx = cr->GetPositionX() - aff.x;
        float const cy = cr->GetPositionY() - aff.y;
        if (cx * cx + cy * cy > laisse * laisse)
            continue;   // hors de la laisse : il ne s'y laisse pas entrainer
        float const d = bot->GetDistance(cr);
        if (d > CoaDirecteur::EVENEMENT_ENGAGEMENT_YD)
            continue;
        bool const insensible = Insensible(cr);
        if (insensible ? (protegee && d >= protegeeD) : (meilleure && d >= meilleureD))
            continue;
        if (!bot->IsValidAttackTarget(cr) || !bot->IsWithinLOSInMap(cr))
            continue;
        (insensible ? protegee : meilleure) = cr;
        (insensible ? protegeeD : meilleureD) = d;
    }
    if (!meilleure)
        meilleure = protegee;
    if (meilleure)
        evfEvaluations.fetch_add(1, std::memory_order_relaxed);
    return meilleure;
}

// Bots actifs (07/10/2026) : voir CoaDirecteur.h. Fil de carte du bot (AttackersValue::Calculate). Meme garde que
// CibleDEvenement ; la copie des guids sous verrou partage ; chaque creature relue PAR LE BOT (P-137).
void CoaDirecteur::AssaillantsDEvenement(Player* bot, std::vector<Unit*>& sortie, std::vector<Unit*>* ecartes)
{
    if (!bot || evfNbJoueurs.load(std::memory_order_relaxed) == 0)
        return;
    CoaDirecteur::Affectation aff;
    if (!CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) || !aff.presence ||
        aff.mission != CoaDirecteur::CoaDirMission::Evenement || !aff.missionParam || aff.carte != bot->GetMapId() ||
        !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive())
        return;
    std::vector<ObjectGuid> guids;
    {
        std::shared_lock<std::shared_mutex> garde(evfVerrou);
        auto const it = evfCibles.find(aff.missionParam);
        if (it == evfCibles.end())
            return;
        guids = it->second;
    }
    float const laisse = aff.rayon + CoaDirecteur::MISSION_LAISSE_MARGE_YD;
    // defense : frappent le joueur servi ; defenseInscrits : frappent un autre inscrit ; autres : le reste des
    // invoques vivants de l'evenement (y compris celui qui frappe le bot lui-meme).
    std::vector<Unit*> defense, defenseInscrits, aPortee, autres;
    for (ObjectGuid const& g : guids)
    {
        Creature* cr = ObjectAccessor::GetCreature(*bot, g);   // carte du bot : nul si l'invoque est ailleurs
        if (!cr || !cr->IsInWorld() || cr->IsDuringRemoveFromWorld() || !cr->IsAlive())
            continue;
        float const cx = cr->GetPositionX() - aff.x;
        float const cy = cr->GetPositionY() - aff.y;
        float const centre2 = cx * cx + cy * cy;
        float const d = bot->GetDistance(cr);
        // (b) DEFENSE : sa victime est le joueur servi, ou un autre inscrit du MEME evenement (meme joueur servi).
        bool protegeJoueur = false, protegeInscrit = false;
        if (Unit* victime = cr->GetVictim())
        {
            if (victime->IsPlayer() && victime != bot)
            {
                uint32 const v = victime->GetGUID().GetCounter();
                CoaDirecteur::Affectation autre;
                protegeJoueur = v == aff.missionParam;
                protegeInscrit = !protegeJoueur && CoaDirecteur::AffectationDe(v, &autre) && autre.presence &&
                                 autre.mission == CoaDirecteur::CoaDirMission::Evenement &&
                                 autre.missionParam == aff.missionParam;
            }
        }
        if ((protegeJoueur || protegeInscrit) && d <= CoaDirecteur::EVENEMENT_DEFENSE_YD &&
            centre2 <= CoaDirecteur::EVENEMENT_DEFENSE_CENTRE_YD * CoaDirecteur::EVENEMENT_DEFENSE_CENTRE_YD)
        {
            (protegeJoueur ? defense : defenseInscrits).push_back(cr);
            if (protegeJoueur)
                continue;
        }
        autres.push_back(cr);   // un invoque qui frappe un autre inscrit est aussi `autres` : le joueur passe avant
        // (a) A PORTEE : dans la laisse, a la portee d'engagement (celle de CibleDEvenement).
        if (centre2 <= laisse * laisse && d <= CoaDirecteur::EVENEMENT_ENGAGEMENT_YD)
            aPortee.push_back(cr);
    }
    // Chefs a mecaniques (10/10) : un invoque INSENSIBLE (bouclier du chef) quitte les trois listes tant qu'un autre
    // invoque de l'evenement, SENSIBLE, figure dans l'une d'elles (a portee, ou en defense) -- les bots frappent les
    // renforts qui portent le bouclier. Relecture : un renfort sensible mais loin (hors laisse, egare) ne compte pas,
    // sinon le chef sortait des listes sans autre cible a portee. Aucun : il reste (la regle d'origine).
    auto const sensible = [](Unit* u) { return !Insensible(u); };
    if (std::any_of(defense.begin(), defense.end(), sensible) ||
        std::any_of(defenseInscrits.begin(), defenseInscrits.end(), sensible) ||
        std::any_of(aPortee.begin(), aPortee.end(), sensible))
    {
        for (std::vector<Unit*>* liste : {&defense, &defenseInscrits, &aPortee})
            liste->erase(std::remove_if(liste->begin(), liste->end(), [](Unit* u) { return Insensible(u); }),
                         liste->end());
    }
    if (!defense.empty())
    {
        sortie.insert(sortie.end(), defense.begin(), defense.end());
        // Relecture du 07/10 : la defense DU JOUEUR PRIME sur les autres invoques de l'evenement, y compris celui
        // qui frappe le bot (DpsTargetValue prefere sinon la cible a portee aux PV les plus bas : un bot de melee au
        // contact ne quittait jamais son invoque pour celui qui tue le joueur). L'appelant les retire de ses
        // assaillants ; un adversaire HORS evenement (combat legitime) n'est jamais dans cette liste.
        if (ecartes)
            ecartes->insert(ecartes->end(), autres.begin(), autres.end());
        evfDefenses.fetch_add(1, std::memory_order_relaxed);
    }
    else if (!defenseInscrits.empty())
    {
        // Un autre inscrit est frappe (pas le joueur) : on l'ajoute SANS rien retirer -- deux bots attaques chacun
        // par un invoque ne doivent pas echanger leurs cibles ; le choix d'origine les departage.
        sortie.insert(sortie.end(), defenseInscrits.begin(), defenseInscrits.end());
        evfDefenses.fetch_add(1, std::memory_order_relaxed);
    }
    else if (!aPortee.empty())
    {
        sortie.insert(sortie.end(), aPortee.begin(), aPortee.end());
        evfAssaillants.fetch_add(1, std::memory_order_relaxed);
    }
}

bool CoaDirecteur::JoueurDEvenement(Player* bot, float& x, float& y, float& z)
{
    // Relecture : PAS de garde sur evfNbJoueurs ici -- entre deux vagues aucun invoque n'est publie (compteur a 0),
    // et c'est justement quand il n'y a rien a frapper que le bot doit rejoindre le joueur. Appelee seulement pour
    // un bot dont l'affectation porte la mission Evenement (NewRpgWanderRandomAction) : l'option suffit.
    if (!bot || !sPlayerbotAIConfig.directeurEvenementBots)
        return false;
    CoaDirecteur::Affectation aff;
    if (!CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) || !aff.presence ||
        aff.mission != CoaDirecteur::CoaDirMission::Evenement || !aff.missionParam ||
        aff.phase != CoaDirecteur::CoaDirPhase::Sejour || aff.carte != bot->GetMapId() || !bot->IsInWorld())
        return false;
    Player* j = ObjectAccessor::GetPlayer(*bot, ObjectGuid::Create<HighGuid::Player>(aff.missionParam));
    if (!j || !j->IsInWorld() || !j->IsAlive() || j->GetMapId() != bot->GetMapId())
        return false;
    float const laisse = aff.rayon + CoaDirecteur::MISSION_LAISSE_MARGE_YD;
    float const dx = j->GetPositionX() - aff.x;
    float const dy = j->GetPositionY() - aff.y;
    if (dx * dx + dy * dy > laisse * laisse)
        return false;   // il kite hors de la laisse : le bot reste au lieu (la laisse le ramenerait)
    x = j->GetPositionX();
    y = j->GetPositionY();
    z = j->GetPositionZ();
    return true;
}

// Bots reactifs (08/10/2026) : voir CoaDirecteur.h. Fil de carte du bot (AttackersValue::IsPossibleTarget, dernier
// recours). Sans evenement publie, ou pour une creature hors de la plage des gabarits : aucun verrou. Pendant un
// evenement, TOUT bot (inscrit ou non) qui evalue un invoque deja marque ou en combat avec un autre prend le verrou
// partage d'AffectationDe, puis (inscrit seulement) evfVerrou en lecture : deux verrous partages successifs, jamais
// imbriques, cout borne a la duree d'une Faille (relecture du 08/10, accepte).
bool CoaDirecteur::InvoqueDEvenementAdmis(Player* bot, Creature* cr)
{
    if (!bot || !cr || evfNbJoueurs.load(std::memory_order_relaxed) == 0)
        return false;
    uint32 const entree = cr->GetEntry();
    if (entree < EV_ENTREE_MIN || entree > EV_ENTREE_MAX)
        return false;
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive() || !cr->IsAlive() ||
        cr->GetMapId() != bot->GetMapId())
        return false;
    CoaDirecteur::Affectation aff;
    if (!CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) || !aff.presence ||
        aff.mission != CoaDirecteur::CoaDirMission::Evenement || !aff.missionParam || aff.carte != bot->GetMapId())
        return false;
    // Laisse (relecture du 08/10) : memes bornes que AssaillantsDEvenement. Un invoque qui frappe le bot lui-meme est
    // toujours admis (il se defend) ; un invoque qui frappe le joueur servi ou un autre inscrit du MEME evenement
    // l'est jusqu'a EVENEMENT_DEFENSE_CENTRE_YD du centre (la defense) ; tout autre jusqu'a la laisse
    // (rayon + MISSION_LAISSE_MARGE_YD), comme CibleDEvenement : un inscrit ne se laisse pas entrainer au-dela.
    float const laisse = aff.rayon + CoaDirecteur::MISSION_LAISSE_MARGE_YD;
    float const cx = cr->GetPositionX() - aff.x;
    float const cy = cr->GetPositionY() - aff.y;
    float const centre2 = cx * cx + cy * cy;
    Unit* const victime = cr->GetVictim();
    if (victime != bot && centre2 > laisse * laisse)
    {
        bool defense = false;
        if (victime && victime->IsPlayer() &&
            centre2 <= CoaDirecteur::EVENEMENT_DEFENSE_CENTRE_YD * CoaDirecteur::EVENEMENT_DEFENSE_CENTRE_YD)
        {
            uint32 const v = victime->GetGUID().GetCounter();
            CoaDirecteur::Affectation autre;
            defense = v == aff.missionParam ||
                      (CoaDirecteur::AffectationDe(v, &autre) && autre.presence &&
                       autre.mission == CoaDirecteur::CoaDirMission::Evenement &&
                       autre.missionParam == aff.missionParam);
        }
        if (!defense)
            return false;   // hors de la laisse, et pas une defense : regle d'origine
    }
    ObjectGuid const g = cr->GetGUID();
    {
        std::shared_lock<std::shared_mutex> garde(evfVerrou);
        auto const it = evfCibles.find(aff.missionParam);
        if (it == evfCibles.end() || std::find(it->second.begin(), it->second.end(), g) == it->second.end())
            return false;   // pas un invoque PUBLIE de CET evenement (autre joueur, retrait, mort) : regle d'origine
    }
    evfAdmis.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void CoaDirecteur::NoterAssaillantsRetenus(size_t ajoutes, size_t retenus)
{
    if (!ajoutes)
        return;
    if (retenus)
        evfRetenus.fetch_add(1, std::memory_order_relaxed);
    else
        evfFiltres.fetch_add(1, std::memory_order_relaxed);
}

namespace
{

// Entretien de chaque seconde, bus allume OU eteint : la purge d'un bus
// eteint met des mercenaires en file, et il faut bien les reconstruire.
void Entretien(uint32 diff)
{
    secondeMs += diff;
    if (secondeMs < IN_MILLISECONDS)
        return;
    secondeMs = 0;
    // Lot D4c : les morts d'abord (elles nomment le tueur d'un prime tombe),
    // puis les primes (dont la fin leve les chasseurs), AVANT le suivi des
    // affectations.
    DrainerMorts();
    SuivrePrimes();
    // Embuscade (02/10) : apres les primes (une prime finie n'a plus d'assaut).
    SuivreEmbuscades();
    // Banc d'essai de prime (03/10/2026) : la liste relue, puis la trace (apres
    // l'embuscade : elle dit l'assaut de CETTE seconde). Sans banc : rien.
    SuivreBancs();
    SuivreImmobiles();
    TracerBanc();
    // Retraite des chasseurs de reserve (03/10/2026). Sans retraite : un chargement.
    SuivreRetraites();
    // Lots T4-T5 (12/10/2026) : reactions des cibles de contrat. Option eteinte : un test. AVANT Reconstruire
    // (fin d'Entretien) : une cible ou un garde sorti de l'immobilite est recompose dans la meme seconde.
    SuivreReactions();
    LeverEchues();
    LeverPaixEchues();   // paix de zone (19/10/2026)
    LeverSuspensionsEchues();
    LeverSuspensionsMaitreHumain();
    // D2b : APRES les fins de suspension (levee `fin_suspension` dans la meme
    // seconde) et AVANT la file de reconstruction (§ 6.3).
    SuivreAffectations();
    // APRES le suivi : une mort constatee a cette seconde y a pose son etat.
    SuivreMortsEnTrajet();
    SuivreTrajetsLeves();
    SuivrePactes();
    // Vrai groupe (09/10) : APRES le suivi (une arrivee de cette seconde est en sejour, une levee est faite).
    SuivreGroupesEvenement();
    // Lot D3 : ciels, meteos, paroles (fil monde seul).
    SuivreCiels();
    SuivreMeteos();
    // Lot C des evenements : ambiances reaffirmees, envois differes, echeances.
    SuivreEvenements();
    // Lot S des evenements : inscrits, lieux (gardes des soins), reliquat. AVANT PublierCiblesEvenement, qui lit et
    // remet a zero ses compteurs ; place loin de l'appel du lot P pour ne pas heurter le lot G a la fusion.
    SuivreSoinsEvenement();
    // Lot D5 (15/10/2026) : creatures attirees (course, engagement, laisse, borne anti-traine, retour), puis soigneurs
    // (echeances, assistance, publication pour les fils de carte). Sans registre : un test chacun.
    SuivreAttires();
    SuivreSoigneurs();
    // Lot D des evenements : invoques (morts, retraits hors de vue, joueur parti, fins de lot).
    SuivreInvoques();
    // Lot P des evenements : portails a refermer (clics, echeances).
    SuivrePortails();
    // Convoi v2 (13/10) : barricades (clics, demolitions humaines ou par un bot, echeances). APRES les invoques (la garde).
    SuivreBarricades();
    // Lot G des evenements : coffres de la faille (pilles, echeances).
    SuivreCoffres();
    // Gisement d'apres-evenement (19/10) : noeuds echus, meme entames.
    SuivreGisements();
    // Objectifs d'evenement (10/10) : APRES les invoques (ennemis restants de cette seconde).
    SuivreObjectifs();
    // Lot F des evenements : les invoques vivants par joueur, publies pour les bots inscrits.
    PublierCiblesEvenement();
    LivrerParoles();
    if (!aReconstruire.empty())
        Reconstruire();
    filetMs += IN_MILLISECONDS;
    if (filetMs >= FILET_MS)
    {
        filetMs = 0;
        Filet();
    }
}

// Verbe `lever` : cible = id de la commande a lever, 0 = tout.
void ExecuterLever(CoaDirCommande const& c)
{
    // Lot D3 : ciels (heure vraie renvoyee), meteos (temps de la zone
    // retabli) et paroles en file se levent comme les autres consignes.
    size_t const n =
        c.cible == 0
            ? LeverSi([](Treve const&) { return true; }, "lever") +
                  LeverPaixSi([](Paix const&) { return true; }, "lever") +
                  LeverSuspensionsSi([](Suspension const&) { return true; }, "lever") +
                  LeverAffectationsSi([](AffectationInterne const&) { return true; }, "lever") +
                  LeverCielsSi([](Ciel const&) { return true; }, "lever") +
                  LeverMeteosSi([](MeteoJoueur const&) { return true; }, "lever") +
                  LeverParolesSi([](Parole const&) { return true; }, "lever") +
                  LeverPrimesSi([](Prime const&) { return true; }, "lever") +
                  LeverEvenementsSi([](uint32, uint32) { return true; }, "lever")
            : LeverSi([&c](Treve const& t) { return t.id == c.cible; }, "lever") +
                  LeverPaixSi([&c](Paix const& z) { return z.id == c.cible; }, "lever") +
                  LeverSuspensionsSi([&c](Suspension const& s) { return s.id == c.cible; }, "lever") +
                  LeverAffectationsSi([&c](AffectationInterne const& f) { return f.a.id == c.cible; }, "lever") +
                  LeverCielsSi([&c](Ciel const& x) { return x.id == c.cible; }, "lever") +
                  LeverMeteosSi([&c](MeteoJoueur const& m) { return m.id == c.cible; }, "lever") +
                  LeverParolesSi([&c](Parole const& p) { return p.id == c.cible; }, "lever") +
                  LeverPrimesSi([&c](Prime const& p) { return p.id == c.cible; }, "lever") +
                  LeverEvenementsSi([&c](uint32 id, uint32) { return id == c.cible; }, "lever");
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    // Rien a lever n'est pas un refus : la consigne a pu echoir ou etre
    // remplacee entre-temps, et l'intention (qu'elle ne soit plus) est tenue.
    Accuser(c, "execute", n ? nullptr : "rien_a_lever");
}

// Verbe `treve` (SPEC § 4.3). Pendant duree_s, aucun bot n'INITIE de combat
// contre la cible. Un combat deja engage n'est pas interrompu (decision 5 du
// commanditaire) : ni GetVictim ni les references de combat ne sont touches,
// la seule lecture est PossibleTargetsValue::AcceptUnit, apres l'autodefense.
// Pas de levee `maitre_humain` ici, a la difference de la suspension : la
// treve porte sur la CIBLE, pas sur un bot, et vaut pour tout bot, y compris
// ceux qui suivent un maitre humain. Elle ne borne que leur convoitise
// autonome : l'attaque commandee par le maitre (AttackMyTargetAction::Execute
// puis AttackAction::Attack, LU) ne passe pas par AcceptUnit.
void ExecuterTreve(CoaDirCommande const& c)
{
    // 6. bornes
    if (c.cible == 0 || c.dureeS < 10 || c.dureeS > 600)
    {
        Refuser(c, "bornes");
        return;
    }
    // 7. plafond : une treve de plus sur une cible deja couverte n'en ajoute pas.
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        if (!treves.count(c.cible) && treves.size() >= sPlayerbotAIConfig.directeurMaxTreves)
        {
            garde.unlock();
            Refuser(c, "plafond");
            return;
        }
    }
    // 8. en ligne. Fil monde, cartes a l'arret : FindPlayer est sur (P-137).
    if (!ObjectAccessor::FindPlayerByLowGUID(c.cible))
    {
        Refuser(c, "hors_ligne");
        return;
    }

    uint32 const fin = Maintenant() + c.dureeS;
    bool remplace = false;
    Treve ancienne{};
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        auto it = treves.find(c.cible);
        uint32 finRetenue = fin;
        if (it != treves.end())
        {
            remplace = true;
            ancienne = it->second;
            // Deux treves sur la meme cible. La consigne unique porte l'id et
            // le joueur de la PLUS RECENTE ; l'ancienne est close `remplace`
            // (le service leve donc par le dernier id). La fin la plus tardive
            // ne gagne que si les deux sont posees autour du MEME joueur :
            // sinon la duree de A survivrait sous le joueur B, et un stop de A
            // (SPEC § 4.5, qui leve par t.joueur) ne leverait plus ce que la
            // commande de A a pose. Joueurs differents : la derniere commande
            // gagne entierement, y compris sa fin.
            if (it->second.joueur == c.joueur)
                finRetenue = std::max(fin, it->second.finS);
        }
        treves[c.cible] = Treve{c.id, c.cible, c.joueur, finRetenue};
        nbTreves.store(static_cast<uint32>(treves.size()), std::memory_order_relaxed);
    }
    if (remplace)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REMPLACEES);
        std::string const par = Acore::StringFormat("par_{}", c.id);
        CloreConsigne(ancienne, "remplace", par.c_str());
    }
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    Accuser(c, "execute", nullptr);
}

// Verbe `paix` (19/10/2026, CoaDirecteur.h). Pendant duree_s, dans la ZONE `cible` : aucun bot n'y porte le FFA de
// WildPvp ni n'y recoit le drapeau JcJ de BotsPvpMinLevel, aucun bot n'y convoite un joueur (riposte, champ de bataille,
// arene, duel et prime exceptes). Un combat deja engage n'est pas interrompu par le ciblage ; le retrait du FFA, lui,
// rompt les attaques que seul le FFA rendait legales (UpdateFFAPvPState(true), PlayerUpdates.cpp:1539-1554) -- c'est
// l'effet voulu contre un massacre entre bots d'une meme faction.
void ExecuterPaix(CoaDirCommande const& c)
{
    // 6. bornes : une zone, 1 a 60 minutes, personne vise.
    if (c.cible == 0 || c.dureeS < 60 || c.dureeS > 3600 || c.joueur != 0)
    {
        Refuser(c, "bornes");
        return;
    }
    // 7. la zone : une entree de AreaTable.dbc, sur la carte annoncee, et cette carte est du monde ouvert (jamais un
    // champ de bataille, une arene, une instance). On n'exige PAS `aire->zone == 0` : la cible est ce que
    // Player::GetZoneId a rendu (ligne `coa jcj zones:`), c'est-a-dire le parent DIRECT de l'aire, et l'AreaTable
    // d'Ascension a deplace des aires sous de nouveaux parents (P-275) -- un parent qui aurait lui-meme un parent serait
    // refuse a tort, alors que PaixDeZone le compare exactement a GetZoneId.
    AreaTableEntry const* aire = sAreaTableStore.LookupEntry(c.cible);
    if (!aire)
    {
        Refuser(c, "zone_inconnue");
        return;
    }
    if (aire->mapid != c.carte)
    {
        Refuser(c, "carte");
        return;
    }
    MapEntry const* carte = sMapStore.LookupEntry(c.carte);
    if (!carte || !carte->IsWorldMap())
    {
        Refuser(c, "carte_hors_monde");
        return;
    }
    // 8. plafond : une paix reposee sur une zone deja en paix n'en ajoute pas.
    auto it = paix.find(c.cible);
    if (it == paix.end() && paix.size() >= CoaDirecteur::PAIX_MAX)
    {
        Refuser(c, "plafond");
        return;
    }

    uint32 fin = Maintenant() + c.dureeS;
    bool remplace = false;
    Paix ancienne{};
    uint32 kase = CoaDirecteur::PAIX_MAX;
    if (it != paix.end())
    {
        // Meme zone : la consigne unique porte l'id de la PLUS RECENTE (le service leve par le dernier id), la fin la
        // plus tardive gagne (aucune paix ne vise un joueur : pas de proprietaire a departager, a la difference de la
        // treve). Meme case.
        remplace = true;
        ancienne = it->second;
        fin = std::max(fin, it->second.finS);
        kase = it->second.kase;
    }
    else
    {
        // Une case libre existe : paix.size() < PAIX_MAX, et chaque paix du magasin tient exactement une case.
        for (uint32 i = 0; i < CoaDirecteur::PAIX_MAX; ++i)
        {
            if (!casesPaix[i].load(std::memory_order_relaxed))
            {
                kase = i;
                break;
            }
        }
        if (kase >= CoaDirecteur::PAIX_MAX)
        {
            Refuser(c, "plafond");   // inatteignable (invariant ci-dessus) ; refuser plutot qu'ecrire hors du tableau
            return;
        }
    }
    paix[c.cible] = Paix{c.id, c.cible, c.carte, fin, kase};
    casesPaix[kase].store((static_cast<uint64>(c.cible) << 32) | fin, std::memory_order_release);
    PublierNbPaix();
    if (remplace)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REMPLACEES);
        std::string const par = Acore::StringFormat("par_{}", c.id);
        CloreConsigne(ancienne, "remplace", par.c_str());
    }
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PAIX_POSEES);
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots", "coa directeur paix: pose zone={} carte={} id={} fin_dans_s={} remplace={}", c.cible, c.carte,
             c.id, fin - Maintenant(), remplace ? ancienne.id : 0);
    Accuser(c, "execute", nullptr);
}

// Verbe `suspendre_mercenaire` (SPEC § 5). Pendant duree_s, le mercenaire
// `cible` ne convoite aucun joueur (PossibleTargetsValue::AcceptUnit lit
// MercenaireSuspendu) ; en mode RPG, la fabrique lui compose en plus celle
// d'un bot ordinaire (AiFactory lit MercenaireEnRpg), et le fil monde la fait
// reconstruire. Ni IsMercenary, ni le drapeau FFA, ni MercenaryRewards ne sont
// touches : il reste une proie, il riposte, il est paye de ses ripostes.
void ExecuterSuspension(CoaDirCommande const& c)
{
    // 6. bornes
    if (c.cible == 0 || c.dureeS < 60 || c.dureeS > 1800)
    {
        Refuser(c, "bornes");
        return;
    }
    // 7. plafond : une suspension reposee sur un bot deja suspendu la remplace.
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        if (!suspensions.count(c.cible) && suspensions.size() >= sPlayerbotAIConfig.directeurMaxSuspensions)
        {
            garde.unlock();
            Refuser(c, "plafond");
            return;
        }
    }
    // 8. en ligne. Fil monde, cartes a l'arret : FindPlayer est sur (P-137).
    Player* bot = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!bot)
    {
        Refuser(c, "hors_ligne");
        return;
    }
    // 9. controles propres (§ 5.4). EstMercenaire exige aussi IsRandomBot, et
    // rend faux WildPvp eteint.
    if (!MercenaryRewards::EstMercenaire(bot))
    {
        Refuser(c, "pas_mercenaire");
        return;
    }
    PlayerbotAI* ia = GET_PLAYERBOT_AI(bot);
    if (!ia)
    {
        Refuser(c, "sans_moteur");
        return;
    }
    if (ia->HasGameClientMaster())
    {
        Refuser(c, "maitre_humain");
        return;
    }
    if (bot->InBattleground() || bot->InArena())
    {
        Refuser(c, "champ_de_bataille");
        return;
    }
    if (!CarteDeSuspension(bot->GetMapId()))
    {
        Refuser(c, "carte");
        return;
    }
    // Lot D4c (SPEC-directeur-D4.md § 6.3, revue n° 16) : un chasseur designe
    // n'est jamais suspendu. Sans ce refus, la regle de fosse, une fois reelle,
    // pourrait le neutraliser sans passer par `occupe`, qui ne couvre que les
    // affectations. Son proprietaire le leve par `lever`.
    if (nbChasseurs.load(std::memory_order_relaxed))
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        auto it = affectations.find(c.cible);
        if (it != affectations.end() && it->second.a.chasseur && it->second.a.finS > Maintenant())
        {
            uint32 const parId = it->second.a.id;
            garde.unlock();
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_OCCUPEES);
            LOG_INFO("playerbots", "coa directeur occupe: id={} guid={} origine={} joueur={} par_id={} par_verbe=chasser",
                     c.id, c.cible, c.origine, c.joueur, parId);
            Refuser(c, "occupe");
            return;
        }
    }

    // Le mode est FIGE ici : changer l'option ne bascule pas les suspensions
    // en cours.
    Suspension nouvelle{c.id, c.cible, c.joueur, Maintenant() + c.dureeS, sPlayerbotAIConfig.directeurSuspensionModeRpg,
                        false, ia, ia->rpgStatistic.questAccepted, ia->rpgStatistic.questRewarded};
    bool remplace = false;
    Suspension ancienne{};
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        auto it = suspensions.find(c.cible);
        if (it != suspensions.end())
        {
            remplace = true;
            ancienne = it->second;
            // Meme mode, deja reconstruit : la composition est deja la bonne.
            if (ancienne.rpg && nouvelle.rpg)
                nouvelle.reconstruit = ancienne.reconstruit;
        }
        // La derniere remplace entierement, fin comprise (§ 5.4).
        suspensions[c.cible] = nouvelle;
        nbSuspensions.store(static_cast<uint32>(suspensions.size()), std::memory_order_relaxed);
    }
    if (remplace)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REMPLACEES);
        std::string const par = Acore::StringFormat("par_{}", c.id);
        // Pas de reconstruction pour l'ancienne : c'est la nouvelle qui decide.
        FinirSuspension(ancienne, "remplace", par.c_str(), "remplace", false);
    }
    // Reconstruire si la composition doit changer : passage en RPG (sauf si
    // l'ancienne l'avait deja fait), ou retour d'un RPG vers le mode 0.
    bool const reconstruire = nouvelle.rpg ? !nouvelle.reconstruit : (remplace && ancienne.rpg);
    filetFait.erase(c.cible);
    if (reconstruire)
        aReconstruire.insert(c.cible);
    if (!nouvelle.rpg && remplace && ancienne.rpg)
        surveillesJusqua[c.cible] = Maintenant() + FILET_LEVEE_S;

    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots", "coa directeur : suspension posee id={} guid={} mode={} fin_s={} duree_s={}", c.id,
             c.cible, nouvelle.rpg ? 1 : 0, nouvelle.finS, c.dureeS);
    Accuser(c, "execute", nullptr);
}

constexpr uint32 METIER_HERBORISTERIE = 182;   // SKILL_HERBALISM
constexpr uint32 METIER_MINAGE = 186;          // SKILL_MINING

struct MissionLue
{
    CoaDirecteur::CoaDirMission type = CoaDirecteur::CoaDirMission::Aucune;
    uint32 param = 0;
    char const* etat = nullptr;   // nul : aucune mission demandee, aucune ligne
    char const* raison = "-";
};

// La mission d'une commande. Fil monde, cartes a l'arret (P-137). Jamais de
// refus : une mission inconnue est IGNOREE (presence ordinaire), une mission
// caduque des la pose (quete absente, metier absent, chef parti) est DEGRADEE
// en Chasse -- le bot est deja en route, le service ne doit rien reemettre.
// `finale` : presence `peupler` d'action `Q-` (ni etape, ni saut, ni `C-`).
MissionLue LireMission(CoaDirCommande const& c, Player* bot, bool finale)
{
    MissionLue m;
    if (c.texte.empty())
        return m;
    m.etat = "ignoree";
    if (!finale)
    {
        m.raison = "pas_une_finale";
        return m;
    }
    if (!sPlayerbotAIConfig.directeurMissions)
    {
        m.raison = "option_eteinte";
        return m;
    }
    using M = CoaDirecteur::CoaDirMission;
    M type = M::Aucune;
    if (c.texte == "quete")
        type = M::Quete;
    else if (c.texte == "chasse")
        type = M::Chasse;
    else if (c.texte == "recolte")
        type = M::Recolte;
    else if (c.texte == "passage")
        type = M::Passage;
    else if (c.texte == "bande")
        type = M::Bande;
    else if (c.texte == "evenement")
        type = M::Evenement;   // lot F des evenements (06/10) : valeur 0, voir plus bas
    if (type == M::Aucune || c.valeur < 0)
    {
        m.raison = "type_inconnu";
        return m;
    }
    uint32 const param = static_cast<uint32>(c.valeur);
    m.etat = "posee";
    m.type = type;
    m.param = param;
    auto degrader = [&m](char const* raison)
    {
        m.type = M::Chasse;
        m.param = 0;
        m.etat = "degradee";
        m.raison = raison;
    };
    if (type == M::Quete)
    {
        if (!param || !sObjectMgr->GetQuestTemplate(param) || bot->GetQuestStatus(param) != QUEST_STATUS_INCOMPLETE)
            degrader("quete_absente");
    }
    else if (type == M::Recolte)
    {
        if ((param != METIER_HERBORISTERIE && param != METIER_MINAGE) || !bot->HasSkill(param))
            degrader("sans_metier");
    }
    else if (type == M::Bande)
    {
        // Le chef : un bot qui porte une presence ACTIVE du MEME joueur, encore
        // en route (etape, saut : il part juste avant son suiveur, d'ailleurs,
        // et peut arriver apres lui) ou deja pose (finale). Pas de bande de
        // bande, pas de bot chef de lui-meme. Le suivi recale le suiveur sur lui
        // seulement une fois sa finale arrivee.
        bool chef = false;
        if (param && param != c.cible)
        {
            std::shared_lock<std::shared_mutex> garde(verrouConsignes);
            auto it = affectations.find(param);
            chef = it != affectations.end() && it->second.a.finS > Maintenant() && it->second.a.presence &&
                   !it->second.a.compagnon && it->second.joueur == c.joueur && it->second.a.mission != M::Bande;
        }
        if (!chef)
            degrader("sans_chef");
    }
    else if (type == M::Evenement)
    {
        // Lot F des evenements (06/10) : le parametre est le JOUEUR dont le bot sert l'evenement (le
        // demandeur de la presence), jamais lu dans `valeur` (0 attendu, emetteur.py). Option eteinte :
        // degradee en Chasse (le bot reste au lieu ; aucune cible imposee). Valeur non nulle : ignoree.
        if (c.valeur != 0)
        {
            m = MissionLue();
            m.etat = "ignoree";
            m.raison = "valeur";
        }
        else if (!sPlayerbotAIConfig.directeurEvenementBots)
            degrader("evenement_eteint");
        else if (!c.joueur)
            degrader("sans_joueur");
        else
            m.param = c.joueur;
    }
    else
        m.param = 0;   // chasse, passage : sans parametre
    return m;
}

// Teleportation cachee et saut lointain : controles du DEPART, communs a une
// presence `peupler` et a un chasseur `chasser` (01/10, D4h). Fil monde,
// cartes a l'arret (P-137). -> le code de refus, ou nul.
bool EstDeLaReserve(Player* bot);   // plus bas (section reserve)

char const* RefusDepartSaut(uint32 cible, Player* bot, Player* joueur, PlayerbotAI* ia, bool dejaSaute)
{
    auto const fait = sautsFaits.find(cible);
    // Banc du 03/10 (arc 17) : Karnzald, chasseur de la prime precedente, refuse `deja_saute`
    // et renvoye a pied sur 1 067 yd. Le repos de 15 min entre deux sauts vise les bots
    // d'animation qui surgiraient en boucle ; un membre de la RESERVE ne saute que pour une
    // prime (et sa retraite, hors de vue) : il n'y est pas soumis. Le double saut d'une
    // meme affectation (`dejaSaute`) reste refuse.
    bool const reposApplicable = !EstDeLaReserve(bot);
    if (dejaSaute || (reposApplicable && fait != sautsFaits.end() && Maintenant() < fait->second + SAUT_REPOS_S))
        return "deja_saute";
    if (bot->IsInCombat())
        return "combat";
    if (!bot->IsInWorld() || bot->IsBeingTeleported())
        return "en_teleportation";
    if (!joueur->IsInWorld() || joueur->IsBeingTeleported() || !joueur->IsAlive())
        return "joueur_indisponible";
    if (PresDeWintergrasp(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY()) ||
        PresDeWintergrasp(joueur->GetMapId(), joueur->GetPositionX(), joueur->GetPositionY()))
        return "wintergrasp";
    float const ox = bot->GetPositionX() - joueur->GetPositionX();
    float const oy = bot->GetPositionY() - joueur->GetPositionY();
    if (ox * ox + oy * oy < SAUT_DEPART_MIN_YD * SAUT_DEPART_MIN_YD)
        return "trop_pres";
    // Personne ne le voit disparaitre (tout humain, le demandeur compris).
    if (ia->HasPlayerNearby(SAUT_HUMAIN_YD))
        return "depart_a_vue";
    return nullptr;
}

// ------------------------------------------------------------------ reserve de chasseurs (02/10)
//
// coa-project/docs/CONCEPTION-reserve-chasseurs.md § 2.1-2.2. La reserve, c'est
// une ou deux guildes de mercenaires (Directeur.ReserveGuildes) ; la guilde EST
// le registre. Deux effets, tous deux au FIL MONDE :
//  - la reconnaissance (EstDeLaReserve), lue a la pose d'un `chasser` et figee
//    dans l'affectation (Affectation::reserve) : les fils de carte ne lisent
//    jamais la chaine de conf, qu'un `reload config` reecrit ;
//  - la PREPARATION : `chasser` d'action `P-<arc>`, `valeur` = niveau voulu. Le
//    bot est mis a ce niveau par la fabrique de playerbots
//    (PlayerbotFactory::Randomize(false), le chemin de RandomBotLevelMgr::
//    AdjustBotToRange, ResetBot et SkipBotLevel) : talents, sorts, equipement du
//    niveau. Il reste en ligne, garde sa guilde (InitGuild rend la main des que
//    GetGuildId() est non nul) et sa composition de mercenaire (Randomize appelle
//    ResetStrategies, donc la fabrique, qui relit IsMercenary et MercenaireEnRpg).
//    Aucune affectation n'est posee : le service enchaine avec son saut `T-`/`L-`.
//
// UNE PREPARATION PAR TICK AU PLUS. Randomize coute sur le fil monde (sorts,
// equipement, SaveToDB) : les `P-` vont dans une file, videe a raison d'un
// Randomize par appel de TraiterBus (un tick monde). Les refus et les « deja au
// niveau » ne coutent rien et ne consomment pas le tick.
//
// EFFETS DE BORD DE Randomize(false), LUS (Bot/Factory/PlayerbotFactory.cpp) et
// assumes pour un mercenaire : combat arrete, toutes les auras retirees
// (RemoveAllAuras : furtivite, monture, buffs), quetes remises a zero, instances
// deliees, competences et sorts reinitialises (ClearSpells ne fait RIEN pour une
// classe CoA : mod-ascension-compat les reconcilie au changement de niveau),
// sacs, consommables, monture, familier refaits, argent retire au sort, et
// sauvegarde (SaveToDB, transaction asynchrone). Equipement vide si le niveau
// BAISSE (EquipAndSpecPersistence), refait dans tous les cas.

constexpr uint32 PREPARATION_REPOS_S = 60;           // refus `deja_prepare` en deca
// 02/10 (coa-project-c0, mesure JcJ) : Dereir, niveau 31, porte un niveau d'objet
// moyen de 19 ; les chasseurs de son niveau 29 a 34 (1 344 a 2 273 PV contre 1 056).
// A la preparation, l'equipement du chasseur est rapproche de celui de la cible par
// la QUALITE, pas par un plafond de niveau d'objet : revue du 02/10, un plafond strict
// (cible + 5) ne laisse passer presque aucun vert a ce niveau (0 vert d'armure ou
// d'arme de niveau requis 22-31 et de niveau d'objet <= 24 dans acore_world), et la
// fabrique se rabat alors sur des blancs et gris, non plafonnes et sans stats.
// Echelle : le score mixte de la fabrique (niveau d'objet x 1,1 par qualite,
// CalcMixedGearScore), emplacement vide = 0. Au-dessus de cible + ECART : verts au
// plus, puis blancs au plus, jusqu'a passer sous la borne ; en dessous de cible -
// ECART : un equipement ordinaire de la fabrique.
constexpr uint32 PREPARATION_SCORE_ECART = 6;   // ~ 5 niveaux d'objet en vert (x 1,21)

// Score mixte moyen, un emplacement vide comptant 0, sur les emplacements que compte
// Player::GetAverageItemLevel (sans tabard, chemise, distance ni main gauche) MOINS
// ceux que la fabrique ne remplit jamais au niveau `niveau` (InitEquipment : bijoux
// sous 50, tete et cou sous 30, anneaux sous 20 ; contre-revue du 02/10 : sinon un
// chasseur de niveau 31 reste « sous » un joueur qui porte un collier, a jamais).
// Joueur et chasseur se mesurent au niveau du CHASSEUR. Heritage : son niveau d'objet
// suit le porteur (ItemTemplate::GetItemLevelIncludingQuality), pas la table.
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

// Contre-revue du 02/10 : un ecart qui ne se resorbe pas (chasseur encore trop haut
// en blancs) referait le reequipement a chaque `P-` (une par 60 s). Un chasseur deja
// au niveau, sur place, n'est reequipe qu'une fois par REEQUIPEMENT_REPOS_S.
constexpr uint32 REEQUIPEMENT_REPOS_S = 1800;
std::unordered_map<uint32, uint32> reequipesLe;   // sous verrouPrepares

// Reequipe `bot` a son niveau, qualite au plus `qualite` (0 : celle de la conf), sans
// plafond de niveau d'objet. Tout ote d'abord : InitEquipment(false) garde un objet
// en place quand il ne trouve rien pour son emplacement. Munitions refaites (revue :
// un arc remplace par un fusil garderait ses fleches).
void Reequiper(Player* bot, uint32 qualite)
{
    PlayerbotFactory::DestroyEquippedGear(bot);
    PlayerbotFactory fabrique(bot, bot->GetLevel(), qualite, 0);
    fabrique.InitEquipment(false);
    fabrique.InitAmmo();
}
constexpr size_t PREPARATIONS_EN_FILE_MAX = 16;      // au-dela : refus `plafond`
std::deque<CoaDirCommande> preparations;             // fil monde seul
// guid -> Maintenant() de la derniere preparation FAITE. Ecrit par le fil monde ;
// lu aussi par RandomPlayerbotMgr::RandomTeleport (revue m2), qui peut tourner sur
// un fil de carte (Revive...) : verrou court, et un compteur atomique devant
// (aucune preparation faite depuis le demarrage : un chargement).
std::mutex verrouPrepares;
std::unordered_map<uint32, uint32> preparesLe;
std::atomic<uint32> nbPrepares{0};

// Revue M4 (decision du coordinateur, 02/10) : le REPOS d'un membre de la reserve
// prepare par `P-0` (hors prime, sans affectation). REPOS_RESERVE_S a partir de la
// preparation, il ne convoite AUCUN joueur (PossibleTargetsValue::AcceptUnit, meme
// place et memes exceptions que MercenaireSuspendu : la riposte -- l'autodefense,
// testee avant -- reste permise ; champ de bataille, arene, duel permis). Un seul
// proprietaire (P-134) : le fil monde, sous le verrou exclusif des consignes ;
// lecteurs (CoaDirecteur::EnRepos) sous le verrou partage apres le compteur
// atomique. Le repos tombe des qu'une affectation `chasser` est posee sur le bot
// (ExecuterAffectation), au bus eteint, et a son terme (lu, puis purge a la pose
// suivante). Il protege aussi le bot de la teleportation aleatoire
// (PrepareRecemment, lu par RandomPlayerbotMgr::RandomTeleport).
constexpr uint32 REPOS_RESERVE_S = 30 * MINUTE;
std::unordered_map<uint32, uint32> reposReserve;   // guid bas -> Maintenant() de fin
std::atomic<uint32> nbRepos{0};

// 04/10 (contrat C1 en jeu : « la traque est fatigante », la cible errait de 900 a 190 yd entre
// deux indices) : un bot prepare par `P-0` HORS prime (cible de contrat, reserve tenue prete) reste
// IMMOBILE le temps de son repos : composition « stay » + riposte, celle de la cible de banc
// (AiFactory). Pas la retraite (PoserRepos seul) : un chasseur en retraite doit pouvoir s'eloigner
// a pied. Leve avec le repos (affectation `chasser`, bus eteint) et a son terme (SuivreImmobiles).
// Fil monde seul pour l'ecriture ; lecture par AiFactory sous verrou partage.
std::shared_mutex verrouImmobiles;
std::unordered_map<uint32, uint32> immobiles;   // guid bas -> Maintenant() de fin
std::atomic<uint32> nbImmobiles{0};

void PoserImmobile(uint32 guid, uint32 finS)
{
    bool nouveau = false;
    {
        std::unique_lock<std::shared_mutex> garde(verrouImmobiles);
        nouveau = !immobiles.count(guid);
        immobiles[guid] = finS;
        nbImmobiles.store(static_cast<uint32>(immobiles.size()), std::memory_order_relaxed);
    }
    if (nouveau)
        BasculerComposition(guid);
}

void LeverImmobile(uint32 guid)
{
    if (!nbImmobiles.load(std::memory_order_relaxed))
        return;
    bool leve = false;
    {
        std::unique_lock<std::shared_mutex> garde(verrouImmobiles);
        leve = immobiles.erase(guid) != 0;
        nbImmobiles.store(static_cast<uint32>(immobiles.size()), std::memory_order_relaxed);
    }
    if (leve)
        BasculerComposition(guid);
}

void SuivreImmobiles()
{
    if (!nbImmobiles.load(std::memory_order_relaxed))
        return;
    std::vector<uint32> finis;
    {
        std::unique_lock<std::shared_mutex> garde(verrouImmobiles);
        uint32 const maintenant = Maintenant();
        for (auto it = immobiles.begin(); it != immobiles.end();)
            if (it->second <= maintenant)
            {
                finis.push_back(it->first);
                it = immobiles.erase(it);
            }
            else
                ++it;
        nbImmobiles.store(static_cast<uint32>(immobiles.size()), std::memory_order_relaxed);
    }
    for (uint32 g : finis)
        BasculerComposition(g);
}

void PoserRepos(uint32 guid, uint32 finS)
{
    std::unique_lock<std::shared_mutex> garde(verrouConsignes);
    for (auto it = reposReserve.begin(); it != reposReserve.end();)
        it = it->second <= Maintenant() ? reposReserve.erase(it) : std::next(it);
    reposReserve[guid] = finS;
    nbRepos.store(static_cast<uint32>(reposReserve.size()), std::memory_order_relaxed);
}

// -> vrai si un repos a ete leve (journal).
bool LeverRepos(uint32 guid)
{
    if (!nbRepos.load(std::memory_order_relaxed))
        return false;
    std::unique_lock<std::shared_mutex> garde(verrouConsignes);
    bool const leve = reposReserve.erase(guid) != 0;
    nbRepos.store(static_cast<uint32>(reposReserve.size()), std::memory_order_relaxed);
    return leve;
}

void ViderRepos()
{
    {
        std::vector<uint32> tous;
        {
            std::unique_lock<std::shared_mutex> garde(verrouImmobiles);
            for (auto const& [g, f] : immobiles)
                tous.push_back(g);
            immobiles.clear();
            nbImmobiles.store(0, std::memory_order_relaxed);
        }
        for (uint32 g : tous)
            BasculerComposition(g);
    }
    if (!nbRepos.load(std::memory_order_relaxed))
        return;
    std::unique_lock<std::shared_mutex> garde(verrouConsignes);
    reposReserve.clear();
    nbRepos.store(0, std::memory_order_relaxed);
}

// ================================================================== reactions des cibles de contrat (T4, T5, T6)
//
// 12/10/2026, coa-project/docs/CONCEPTION-primes-traqueur-bots.md (decision du commanditaire : la cible TIRE une
// reaction au hasard a chaque contrat, table extensible). Le service (contrats.py) tire la reaction et l'ecrit dans
// params de l'arc `contrat` : `reaction` (nom) et `gardes` (guids PRETS, prepares par `chasser P-0` au niveau du
// chasseur). Le jeu ne fait que LIRE l'arc (P-134), toutes les REACTIONS_RELIRE_S, et agit chaque seconde au fil
// monde (Entretien, cartes a l'arret, P-137). Une reaction = une branche de SuivreReactions ; un nom que ce binaire
// ne connait pas est journalise une fois et la cible reste passive (C1). Option AiPlayerbot.Directeur.Reactions.
//   defense      : le chasseur a 40 yd et en ligne de vue (ou deja aux prises avec lui) : elle sort du repos et de
//                  l'immobilite et l'engage la premiere (AttackAction recopie : cible courante, moteur de combat).
//   fuite_gardes : tant que le chasseur est loin (> 250 yd, ou autre carte) et qu'aucun humain n'est a 150 yd du
//                  depart ni de l'arrivee, chaque garde PRET (au niveau du chasseur a 3 pres) qui s'est eloigne de
//                  plus de 35 yd rejoint la cible par un saut cache (6-12 yd d'elle, sol sec), 15 s au plus souvent ;
//                  puis, le chasseur a 60 yd en vue (ou aux prises avec elle a 100 yd), elle fuit UNE fois a 120 yd
//                  (mouvement FORCE : le moteur ne le reprend pas avant son terme) et chaque garde a 100 yd du
//                  chasseur l'engage.
// Lot T6 (13/10/2026, meme document § 10 ; option AiPlayerbot.Directeur.ReactionsT6, eteinte) :
//   fuite_monture : le chasseur a 60 yd en vue (ou aux prises), elle MONTE (sorts de monture terrestre qu'elle
//                  connait, lus comme l'action « check mount state » : CollectMountData) puis fuit a 200 yd ; sans
//                  monture possible (interieur, combat, aucun sort, incantation ratee), a pied a 120 yd. A l'arrivee
//                  elle DESCEND et redevient immobile (elle riposte) : un chasseur a pied la rattrape. Apres
//                  REACTION_SOUFFLE_S, le chasseur de nouveau a 60 yd en vue : une SECONDE fuite, a pied, a 120 yd,
//                  dans une autre direction que la premiere si le sol le permet (`etape=fuite2`). Jamais trois.
//   fuite_gardes  : la meme seconde fuite (apres la fin de la premiere et le souffle), ses gardes a 100 yd l'engagent.
//   soudoiement   : le chasseur a 40 yd en vue, hors combat avec lui : elle s'arrete, se tourne vers lui, ne
//                  l'attaque pas (elle garde son repos) et lui CHUCHOTE un pot-de-vin (params.pot_de_vin_c, ecrit
//                  par le service). Le traqueur (CorpsTraqueur) montre l'offre a l'addon, qui offre deux boutons. Le
//                  SERVICE tranche (ligne `etape=offre`, lue dans CoaBots.log) : accepte -> l'arc est clos, et la
//                  cible REPART (immobilite levee, quelques pas loin du chasseur) ; refus ou 30 s -> il reecrit la
//                  reaction en `defense`. Le chasseur qui la frappe : elle se defend tout de suite (`etape=frappe`),
//                  et le service, qui lit la ligne, fait de meme. Sans decision du service en
//                  REACTION_SOUDOIEMENT_ABANDON_S, elle se defend (le service ne peut plus accepter, borne a 35 s).
constexpr uint32 REACTIONS_RELIRE_S = 5;
constexpr uint32 REACTIONS_MAX = 16;
constexpr float REACTION_DEFENSE_YD = 40.0f;
constexpr float REACTION_FUITE_YD = 60.0f;
constexpr float REACTION_FUITE_PAS_YD = 120.0f;
constexpr float REACTION_FUITE_ECART_Z = 15.0f;
constexpr uint32 REACTION_FUITE_MIN_MS = 4000;
constexpr uint32 REACTION_FUITE_MAX_MS = 20000;
constexpr float REACTION_GARDE_PROCHE_YD = 35.0f;
constexpr float REACTION_CHASSEUR_LOIN_YD = 250.0f;
constexpr float REACTION_SAUT_HUMAIN_YD = 150.0f;
constexpr float REACTION_ENGAGE_YD = 100.0f;
constexpr uint32 REACTION_SAUT_INTERVALLE_S = 15;
constexpr int32 REACTION_GARDE_NIVEAU_ECART = 3;
constexpr size_t REACTION_GARDES_MAX = 4;
// Lot T6.
constexpr float REACTION_FUITE_MONTEE_PAS_YD = 200.0f;
constexpr uint32 REACTION_FUITE_MONTEE_MAX_MS = 25000;   // 200 yd a la vitesse d'une monture lente (~10 yd/s) : 20 s
constexpr uint32 REACTION_MONTE_MAX_S = 3;               // incantation d'une monture : 1,5 s ; au-dela, a pied
constexpr uint32 REACTION_MONTE_MIN_S = 2;               // une incantation finie sans monture : a pied
constexpr uint32 REACTION_SOUFFLE_S = 6;                 // apres une fuite, pas de seconde avant ce delai
constexpr float REACTION_ARRIVEE_YD = 6.0f;
constexpr float REACTION_AUTRE_DIRECTION_RAD = 0.52f;    // 30 degres
constexpr float REACTION_REPART_PAS_YD = 60.0f;
constexpr float REACTION_SOUDOIEMENT_YD = 40.0f;
constexpr uint32 REACTION_SOUDOIEMENT_S = 30;            // = contrats.SOUDOIEMENT_S (offre montree au joueur)
constexpr uint32 REACTION_SOUDOIEMENT_ABANDON_S = 45;    // > contrats.SOUDOIEMENT_S + SOUDOIEMENT_GRACE_S (35)
constexpr uint32 REACTION_IMMOBILE_APRES_S = 30 * MINUTE;   // = REPOS_RESERVE_S : immobile apres son arrivee

enum PhaseFuite : uint8
{
    FUITE_ATTENTE = 0,   // pas encore partie
    FUITE_MONTE   = 1,   // incantation de la monture (fuite_monture)
    FUITE_COURT   = 2,   // en route vers le point
    FUITE_ARRIVEE = 3,   // arrivee, a pied, immobile (fuite_monture)
};

struct ReactionContrat
{
    uint32 arc = 0;
    uint32 joueur = 0;
    uint32 cible = 0;
    std::string reaction;
    std::vector<uint32> gardes;
    bool declenchee = false;
    bool inconnueDite = false;
    std::unordered_map<uint32, uint32> sautGarde;   // garde -> Maintenant() du dernier saut
    // Lot T6 : fuites (fuite_monture, fuite_gardes) et soudoiement. Remis a zero quand la reaction change (le service
    // reecrit `soudoiement` en `defense` apres un refus).
    uint32 potDeVinC = 0;            // params.pot_de_vin_c (soudoiement)
    uint8 fuites = 0;                // fuites parties (0, 1, 2)
    uint8 phase = FUITE_ATTENTE;
    uint32 phaseS = 0;               // Maintenant() au debut de la phase
    uint32 finFuiteS = 0;            // terme du mouvement en cours
    float pointX = 0.0f, pointY = 0.0f, pointZ = 0.0f;
    float cap = 0.0f;                // direction de la premiere fuite (radians)
    bool montee = false;             // fuite en cours a monture
    bool offre = false;              // soudoiement : offre chuchotee
    uint32 offreS = 0;
    bool frappe = false;             // le chasseur l'a frappee (avant ou pendant l'offre)
    bool defenseLocale = false;      // sans decision du service : elle se defend
    bool defenseFaite = false;
    bool sansMontantDit = false;
};
std::unordered_map<uint32, ReactionContrat> reactionsContrat;   // arc -> etat ; fil monde seul
uint32 reactionsLuesS = 0;
// Lot T6 : arcs dont l'offre est deja faite (arc -> Maintenant() de l'offre). Survit a l'oubli d'un arc par
// LireReactions (requete en echec un instant) : une offre n'est jamais chuchotee deux fois. Fil monde seul.
std::unordered_map<uint32, uint32> offresFaites;

// Les guids d'un texte JSON (« 123 » ou « [12, 34] ») : suites de chiffres (10 au plus, <= UINT32_MAX), `max` au plus.
std::vector<uint32> GuidsTexte(std::string const& v, size_t max)
{
    std::vector<uint32> out;
    uint64 n = 0;
    size_t chiffres = 0;
    auto fin = [&]()
    {
        if (chiffres && chiffres <= 10 && n && n <= std::numeric_limits<uint32>::max() && out.size() < max)
            out.push_back(static_cast<uint32>(n));
        n = 0;
        chiffres = 0;
    };
    for (char ch : v)
    {
        if (ch >= '0' && ch <= '9')
        {
            if (++chiffres <= 10)
                n = n * 10 + static_cast<uint64>(ch - '0');
        }
        else
            fin();
    }
    fin();
    return out;
}

void JournaliserReaction(ReactionContrat const& e, char const* etape, std::string const& detail)
{
    LOG_INFO("playerbots", "coa directeur reaction: arc={} joueur={} cible={} reaction={} etape={}{}", e.arc,
             e.joueur, e.cible, e.reaction, etape, detail);
}

struct Fuite
{
    float pas = 0.0f;                // 0 : aucun point de fuite
    float cap = 0.0f;
    uint32 delaiMs = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool autreDirection = true;      // seconde fuite : la direction s'ecarte de la premiere d'au moins 30 degres
};

// Fuite : un point a `pas` yd, a l'oppose du chasseur, sol sec, puis un mouvement de priorite FORCEE (LastMovement) :
// MovementAction::IsWaitingForLastMove refuse tout mouvement de priorite inferieure (errance, combat) jusqu'a son
// terme. Premiere fuite : ecarts de 0, +-35, +-70 degres. `capEvite` (seconde fuite) : d'abord les directions a plus
// de 30 degres de la premiere (+-70, +-35, +-100, puis 0), puis, s'il n'y a de sol sec que la, n'importe laquelle.
// `garderMonture` (fuite_monture) : la monture reste ; sinon elle est otee. -> la fuite (pas = 0 : aucun point).
Fuite FuirChasseur(Player* bot, Player* chasseur, float pas = REACTION_FUITE_PAS_YD, bool garderMonture = false,
                   float const* capEvite = nullptr)
{
    Fuite f;
    PlayerbotAI* ia = GET_PLAYERBOT_AI(bot);
    Map* map = bot->GetMap();
    if (!ia || !map)
        return f;
    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();
    float const bz = bot->GetPositionZ();
    float const dx = bx - chasseur->GetPositionX();
    float const dy = by - chasseur->GetPositionY();
    float const cap = (dx * dx + dy * dy) > 0.01f ? std::atan2(dy, dx) : bot->GetOrientation();
    static constexpr float ECARTS[] = {0.0f, 0.61f, -0.61f, 1.22f, -1.22f};
    static constexpr float ECARTS_AUTRE[] = {1.22f, -1.22f, 0.61f, -0.61f, 1.75f, -1.75f, 0.0f};
    float const* const ecarts = capEvite ? ECARTS_AUTRE : ECARTS;
    size_t const n = capEvite ? sizeof(ECARTS_AUTRE) / sizeof(ECARTS_AUTRE[0]) : sizeof(ECARTS) / sizeof(ECARTS[0]);
    for (int passe = 0; passe < (capEvite ? 2 : 1); ++passe)
    {
        for (size_t i = 0; i < n; ++i)
        {
            float const c = cap + ecarts[i];
            bool const autre = !capEvite || std::fabs(std::remainder(c - *capEvite, 2.0f * float(M_PI))) >=
                                                REACTION_AUTRE_DIRECTION_RAD;
            if (passe == 0 && !autre)
                continue;
            float const px = bx + pas * std::cos(c);
            float const py = by + pas * std::sin(c);
            float sol = 0.0f;
            if (!SolSec(map, px, py, bz + REACTION_FUITE_ECART_Z, sol) || std::fabs(sol - bz) > REACTION_FUITE_ECART_Z)
                continue;
            if (bot->IsMounted() && !garderMonture)
                bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            // Relecture (M3) : aux prises, le moteur de COMBAT poursuivrait le chasseur (ChaseTo ne consulte pas la
            // priorite du dernier mouvement) et Reconstruire saute un bot en combat (« stay » resterait). On le sort
            // du combat : il fuit, et ne riposte que si le chasseur le rattrape.
            if (bot->IsInCombat())
            {
                bot->AttackStop();
                bot->CombatStop(true);
                ia->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
                bot->SetSelection(ObjectGuid::Empty);
                ia->ChangeEngine(BOT_STATE_NON_COMBAT);
            }
            bot->GetMotionMaster()->MovePoint(0, px, py, sol, FORCED_MOVEMENT_RUN);
            float const vitesse = std::max(1.0f, bot->GetSpeed(MOVE_RUN));
            uint32 const plafond = garderMonture ? REACTION_FUITE_MONTEE_MAX_MS : REACTION_FUITE_MAX_MS;
            uint32 const delai = std::min(plafond, std::max(REACTION_FUITE_MIN_MS,
                                          static_cast<uint32>(pas / vitesse * 1000.0f)));
            ia->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().Set(
                map->GetId(), px, py, sol, bot->GetOrientation(), static_cast<float>(delai),
                MovementPriority::MOVEMENT_FORCED);
            f.pas = pas;
            f.cap = c;
            f.delaiMs = delai;
            f.x = px;
            f.y = py;
            f.z = sol;
            f.autreDirection = autre;
            return f;
        }
    }
    return f;
}

// Lot T6 : immobile JUSQU'A `finS` au moins -- PoserImmobile ecrase l'echeance, et raccourcirait celle de la
// preparation (REPOS_RESERVE_S). Fil monde seul (ecriture), comme PoserImmobile.
void PoserImmobileAuMoins(uint32 guid, uint32 finS)
{
    if (nbImmobiles.load(std::memory_order_relaxed))
    {
        std::shared_lock<std::shared_mutex> garde(verrouImmobiles);
        auto const it = immobiles.find(guid);
        if (it != immobiles.end() && it->second >= finS)
            return;
    }
    PoserImmobile(guid, finS);
}

// Lot T6 : une fuite partie, gardee dans l'etat (point, terme, cap de la premiere).
void NoterFuite(ReactionContrat& e, Fuite const& f, uint32 maintenant, bool montee)
{
    if (!e.fuites)
        e.cap = f.cap;
    ++e.fuites;
    e.phase = FUITE_COURT;
    e.phaseS = maintenant;
    e.finFuiteS = maintenant + (f.delaiMs + 999) / 1000;
    e.pointX = f.x;
    e.pointY = f.y;
    e.pointZ = f.z;
    e.montee = montee;
}

// Lot T6 : en route, la fuyarde a pu etre arretee (une monture lancee par sa propre IA, qui arrete le mouvement ; un
// sort) : le mouvement vers le point est relance, avec le temps qui reste. Jamais en combat (le moteur de combat la
// tient : elle riposte) ni pendant une incantation.
void RelancerFuite(Player* bot, ReactionContrat const& e, uint32 maintenant)
{
    PlayerbotAI* ia = GET_PLAYERBOT_AI(bot);
    if (!ia || bot->isMoving() || bot->IsInCombat() || bot->IsNonMeleeSpellCast(false) || maintenant >= e.finFuiteS ||
        bot->GetExactDist2d(e.pointX, e.pointY) <= REACTION_ARRIVEE_YD)
        return;
    bot->GetMotionMaster()->MovePoint(0, e.pointX, e.pointY, e.pointZ, FORCED_MOVEMENT_RUN);
    ia->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().Set(
        bot->GetMapId(), e.pointX, e.pointY, e.pointZ, bot->GetOrientation(),
        static_cast<float>((e.finFuiteS - maintenant) * IN_MILLISECONDS), MovementPriority::MOVEMENT_FORCED);
}

// Lot T6 : monter pour fuir. Les sorts de monture TERRESTRE (indice 0) que le bot connait, lus par CollectMountData
// (CheckMountStateAction.cpp, la lecture de l'action « check mount state »), du plus rapide au plus lent ; le premier
// que l'IA peut lancer (PlayerbotAI::CanCastSpell, comme TryRandomMountFiltered) est INCANTE (1,5 s : le chasseur
// peut l'interrompre). Jamais en interieur, en combat, sous le niveau des montures de la conf. -> vrai si
// l'incantation est partie (ou si elle est deja montee).
bool MonterPourFuir(Player* bot, PlayerbotAI* ia)
{
    if (bot->IsMounted())
        return true;
    if (!ia || !bot->IsOutdoors() || bot->IsInCombat() || bot->IsInWater() ||
        bot->GetLevel() < sPlayerbotAIConfig.useGroundMountAtMinLevel)
        return false;
    MountData const donnees = CollectMountData(bot);
    auto const terrestres = donnees.allSpells.find(0);
    if (terrestres == donnees.allSpells.end())
        return false;
    for (auto vitesse = terrestres->second.rbegin(); vitesse != terrestres->second.rend(); ++vitesse)
        for (uint32 const sort : vitesse->second)
        {
            if (!ia->CanCastSpell(sort, bot, true))
                continue;
            if (bot->isMoving())
                bot->StopMoving();
            if (bot->CastSpell(bot, sort, false) == SPELL_CAST_OK)
                return true;
        }
    return false;
}

void LireReactions();

// Lot T6 : l'arc a quitte les arcs actifs en `piste` (contrat fini). Soudoiement accepte -- l'arc est CLOS avec la raison
// `soudoye` (relu ici, une requete, rare) -- : la cible REPART : immobilite levee (son IA reprend), quelques pas loin du
// chasseur ; elle GARDE son repos : elle ne l'attaque pas. Fuite montee finie (relecture m1) : l'immobilite posee a son
// arrivee est levee (sinon « stay » jusqu'a 30 min hors contrat).
void RepartirApresSoudoiement(ReactionContrat const& e)
{
    if (e.reaction == "fuite_monture" && e.fuites)
    {
        LeverImmobile(e.cible);
        return;
    }
    if (e.reaction != "soudoiement" || !e.offre || e.frappe || e.defenseLocale)
        return;
    QueryResult clos = CharacterDatabase.Query(
        "SELECT 1 FROM coa_dir_arc WHERE id = {} AND genre = 'contrat' AND statut = 'clos' AND raison = 'soudoye'", e.arc);
    if (!clos)
        return;   // fini autrement (expire, abandon, joueur parti...) : elle reste ou elle est
    Player* cible = JoueurConnecte(e.cible);
    if (!cible || !cible->IsInWorld() || cible->IsBeingTeleported() || !cible->IsAlive() ||
        !sRandomPlayerbotMgr.IsRandomBot(cible))
        return;
    PlayerbotAI* ia = GET_PLAYERBOT_AI(cible);
    if (!ia || ia->HasGameClientMaster())
        return;
    LeverImmobile(e.cible);
    Player* chasseur = JoueurConnecte(e.joueur);
    float pas = 0.0f;
    if (chasseur && chasseur->IsInWorld() && chasseur->GetMapId() == cible->GetMapId() &&
        chasseur->GetInstanceId() == cible->GetInstanceId() && !cible->IsInCombat())
        pas = FuirChasseur(cible, chasseur, REACTION_REPART_PAS_YD).pas;
    JournaliserReaction(e, "repart", Acore::StringFormat(" pas_yd={:.0f}", pas));
}

void LireReactions()
{
    QueryResult r = CharacterDatabase.Query(
        "SELECT id, joueur, IFNULL(CAST(JSON_EXTRACT(params, '$.cible') AS CHAR), ''),"
        " IFNULL(JSON_UNQUOTE(JSON_EXTRACT(params, '$.reaction')), ''),"
        " IFNULL(CAST(JSON_EXTRACT(params, '$.gardes') AS CHAR), ''),"
        " IFNULL(CAST(JSON_EXTRACT(params, '$.pot_de_vin_c') AS CHAR), '')"
        " FROM coa_dir_arc WHERE genre = 'contrat' AND statut = 'actif' AND etape = 'piste'"
        // Revue 16/10 : un arc echu que le service n'a pas clos (service arrete) ne fait plus reagir sa cible.
        " AND (fin_prevue IS NULL OR fin_prevue > UTC_TIMESTAMP())"
        " AND JSON_EXTRACT(params, '$.reaction') IS NOT NULL ORDER BY id LIMIT {}",
        REACTIONS_MAX);
    // Relecture T6 (M2) : `nullptr` dit aussi bien « aucun contrat » que « requete en echec ». Une requete temoin
    // tranche : en echec, rien n'est oublie (sinon une cible perdrait ses fuites, ou repartirait sans marche conclu).
    if (!r && !reactionsContrat.empty() && !CharacterDatabase.Query("SELECT 1"))
        return;
    std::unordered_set<uint32> vus;
    if (r)
    {
        do
        {
            Field* f = r->Fetch();
            uint32 const arc = f[0].Get<uint32>();
            std::vector<uint32> const cible = GuidsTexte(f[2].Get<std::string>(), 1);
            std::string const reaction = f[3].Get<std::string>();
            if (cible.empty() || reaction.empty() || reaction.size() > 24)
                continue;
            vus.insert(arc);
            ReactionContrat& e = reactionsContrat[arc];
            // Lot T6 : une reaction qui CHANGE (soudoiement refuse -> defense) repart de zero.
            if ((!e.reaction.empty() && e.reaction != reaction) || (e.cible && e.cible != cible[0]))
                e = ReactionContrat();
            e.arc = arc;
            e.joueur = f[1].Get<uint32>();
            e.cible = cible[0];
            e.reaction = reaction;
            e.gardes = GuidsTexte(f[4].Get<std::string>(), REACTION_GARDES_MAX);
            std::vector<uint32> const pot = GuidsTexte(f[5].Get<std::string>(), 1);
            e.potDeVinC = pot.empty() ? 0 : pot[0];
        } while (r->NextRow());
    }
    for (auto it = reactionsContrat.begin(); it != reactionsContrat.end();)
    {
        if (vus.count(it->first))
        {
            ++it;
            continue;
        }
        RepartirApresSoudoiement(it->second);
        it = reactionsContrat.erase(it);
    }
    if (offresFaites.size() > 256)
        for (auto it = offresFaites.begin(); it != offresFaites.end();)
            it = (Maintenant() - it->second > HOUR) ? offresFaites.erase(it) : std::next(it);
}

// La cible (ou un garde) engage le chasseur : ce que fait AttackAction::Attack (AttackAction.cpp:185-215),
// au fil monde. -> faux si l'attaque n'est pas permise (lieu sans PvP, faction, maitre humain).
bool EngagerChasseur(Player* bot, Player* chasseur)
{
    PlayerbotAI* ia = GET_PLAYERBOT_AI(bot);
    if (!ia || ia->HasGameClientMaster() || !bot->IsAlive() || !chasseur->IsAlive() ||
        !bot->IsValidAttackTarget(chasseur))
        return false;
    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    bot->SetSelection(chasseur->GetGUID());
    ia->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(chasseur);
    ia->ChangeEngine(BOT_STATE_COMBAT);
    bot->Attack(chasseur, !PlayerbotAI::IsRanged(bot));
    return true;
}

// Un garde utilisable : bot aleatoire sans maitre humain, vivant, dans le monde, sur la carte de la cible, au niveau
// du chasseur a REACTION_GARDE_NIVEAU_ECART pres (sinon sa preparation a echoue : on ne l'amene pas).
Player* GardeValide(uint32 g, Player* cible, Player* chasseur)
{
    Player* p = JoueurConnecte(g);
    if (!p || p == cible || !p->IsInWorld() || p->IsBeingTeleported() || !p->IsAlive() || p->IsInFlight() ||
        p->InBattleground() || !sRandomPlayerbotMgr.IsRandomBot(p))
        return nullptr;
    PlayerbotAI* ia = GET_PLAYERBOT_AI(p);
    if (!ia || ia->HasGameClientMaster())
        return nullptr;
    if (p->GetMapId() != cible->GetMapId() || p->GetInstanceId() != cible->GetInstanceId())
        return nullptr;
    if (std::abs(static_cast<int32>(p->GetLevel()) - static_cast<int32>(chasseur->GetLevel())) >
        REACTION_GARDE_NIVEAU_ECART)
        return nullptr;
    return p;
}

// Saut cache d'un garde pres de la cible : 6-12 yd, sol sec a TIRAGE_ECART_Z pres, aucun humain a 150 yd du depart ni
// de l'arrivee (le chasseur compris). -> vrai si le saut est parti.
bool SauterGardePres(Player* garde, Player* cible)
{
    Map* map = cible->GetMap();
    if (!map || garde->IsInCombat())
        return false;
    uint32 const carte = cible->GetMapId();
    if (HumainProcheSauf(WorldPosition(garde), carte, REACTION_SAUT_HUMAIN_YD, 0))
        return false;
    for (int essai = 0; essai < 4; ++essai)
    {
        float const a = frand(0.0f, 2.0f * float(M_PI));
        float const r = frand(6.0f, 12.0f);
        float const px = cible->GetPositionX() + r * std::cos(a);
        float const py = cible->GetPositionY() + r * std::sin(a);
        float sol = 0.0f;
        if (!SolSec(map, px, py, cible->GetPositionZ() + Z_OFFSET_FIND_HEIGHT, sol) ||
            std::fabs(sol - cible->GetPositionZ()) > TIRAGE_ECART_Z)
            continue;
        if (HumainProcheSauf(WorldPosition(carte, px, py, sol), carte, REACTION_SAUT_HUMAIN_YD, 0))
            return false;
        // Relecture (M4) : une position « stay » posee (commande de chat) ramenerait le garde a son ancien point
        // (ReturnToStayPositionTrigger) : oubliee. +0,5 yd au-dessus du sol, comme les autres sauts du fichier.
        if (PlayerbotAI* ia = GET_PLAYERBOT_AI(garde))
            ia->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get()["stay"].Reset();
        return CoaDirecteur::Sauter(garde, WorldLocation(carte, px, py, sol + 0.5f, a + float(M_PI)));
    }
    return false;
}

// Lot T6 : ce que la cible voit du chasseur, cette seconde.
struct VueChasseur
{
    bool memeCarte = false;
    float d = -1.0f;
    bool vu = false;          // a `vuYd` au plus et en ligne de vue (calculee de pres seulement : couteuse)
    bool auxPrises = false;   // en combat AVEC le chasseur, a REACTION_ENGAGE_YD au plus
};

VueChasseur Regarder(Player* cible, Player* chasseur, float vuYd)
{
    VueChasseur v;
    v.memeCarte = chasseur->GetMapId() == cible->GetMapId() && chasseur->GetInstanceId() == cible->GetInstanceId();
    if (!v.memeCarte || !chasseur->IsAlive())
        return v;
    v.d = chasseur->GetExactDist2d(cible);
    v.vu = v.d <= vuYd && cible->IsWithinLOSInMap(chasseur);
    v.auxPrises = v.d <= REACTION_ENGAGE_YD && cible->IsInCombatWith(chasseur);
    return v;
}

// Lot T6 : chaque garde PRET a 100 yd du chasseur, pas deja en combat, l'engage. -> gardes engages.
uint32 EngagerGardes(ReactionContrat const& e, Player* cible, Player* chasseur, bool seulementLibres)
{
    uint32 engages = 0;
    for (uint32 g : e.gardes)
    {
        Player* garde = GardeValide(g, cible, chasseur);
        if (!garde || garde->GetExactDist2d(chasseur) > REACTION_ENGAGE_YD || (seulementLibres && garde->IsInCombat()))
            continue;
        LeverImmobile(g);
        LeverRepos(g);
        if (EngagerChasseur(garde, chasseur))
            ++engages;
    }
    return engages;
}

// Lot T6 : la SECONDE fuite (fuite_monture, fuite_gardes), a pied, ailleurs que la premiere si le sol le permet. Le
// chasseur doit etre de nouveau a 60 yd en vue (ou aux prises), REACTION_SOUFFLE_S apres le terme de la premiere.
// -> vrai si elle est partie (ou si un garde a engage le chasseur, fuite_gardes : la reaction est consommee).
bool SecondeFuite(ReactionContrat& e, Player* cible, Player* chasseur, uint32 maintenant)
{
    // Le souffle court depuis l'arrivee (fuite_monture : elle s'arrete au point) ou depuis le terme du mouvement.
    uint32 const depuis = e.phase == FUITE_ARRIVEE && e.reaction == "fuite_monture" ? e.phaseS : e.finFuiteS;
    if (e.fuites != 1 || maintenant < depuis + REACTION_SOUFFLE_S)
        return false;
    VueChasseur const v = Regarder(cible, chasseur, REACTION_FUITE_YD);
    if (!(v.vu || v.auxPrises))
        return false;
    Fuite const f = FuirChasseur(cible, chasseur, REACTION_FUITE_PAS_YD, false, &e.cap);
    uint32 const engages = e.reaction == "fuite_gardes" ? EngagerGardes(e, cible, chasseur, true) : 0;
    if (f.pas <= 0.0f && !engages)
        return false;   // aucun sol sec ou fuir, aucun garde : reessai la seconde suivante, immobilite gardee
    // Apres le mouvement (meme seconde, Reconstruire passe plus tard dans Entretien) : « stay » ne l'arrete pas.
    LeverImmobile(e.cible);
    float const cap1 = e.cap;
    if (f.pas > 0.0f)
        NoterFuite(e, f, maintenant, false);
    else
        e.fuites = 2;
    JournaliserReaction(e, "fuite2", Acore::StringFormat(" fuite_yd={:.0f} autre_direction={} ecart_deg={:.0f}"
                                                         " gardes_engages={} distance={:.0f}",
                                                         f.pas, f.autreDirection ? 1 : 0,
                                                         f.pas > 0.0f ? std::fabs(std::remainder(f.cap - cap1,
                                                             2.0f * float(M_PI))) * 180.0f / float(M_PI) : 0.0f,
                                                         engages, v.d));
    return true;
}

// Lot T6 : fuite_monture, une seconde de la machine a etats (voir l'en-tete de la section).
void SuivreFuiteMontee(ReactionContrat& e, Player* cible, Player* chasseur, PlayerbotAI* ia, uint32 maintenant)
{
    switch (e.phase)
    {
        case FUITE_ATTENTE:
        {
            VueChasseur const v = Regarder(cible, chasseur, REACTION_FUITE_YD);
            if (!(v.vu || v.auxPrises))
                return;
            // Aux prises : pas d'incantation (la monture est interdite en combat) : a pied, tout de suite.
            if (!v.auxPrises && MonterPourFuir(cible, ia))
            {
                e.phase = FUITE_MONTE;
                e.phaseS = maintenant;
                JournaliserReaction(e, "monte", Acore::StringFormat(" deja_montee={} distance={:.0f}",
                                                                    cible->IsMounted() ? 1 : 0, v.d));
                return;
            }
            Fuite const f = FuirChasseur(cible, chasseur);
            if (f.pas <= 0.0f)
                return;   // aucun sol sec : reessai la seconde suivante, immobile
            LeverImmobile(e.cible);
            NoterFuite(e, f, maintenant, false);
            JournaliserReaction(e, "fuite", Acore::StringFormat(" fuite_yd={:.0f} monture=0 raison={} distance={:.0f}",
                                                                f.pas, v.auxPrises ? "aux_prises" : "sans_monture", v.d));
            return;
        }
        case FUITE_MONTE:
        {
            bool const memeCarte = chasseur->GetMapId() == cible->GetMapId() &&
                                   chasseur->GetInstanceId() == cible->GetInstanceId();
            uint32 const ecoule = maintenant - e.phaseS;
            if (!memeCarte)
            {
                if (ecoule >= REACTION_MONTE_MAX_S)
                    e.phase = FUITE_ATTENTE;   // le chasseur est parti : elle attend, montee ou non
                return;
            }
            if (cible->IsMounted())
            {
                // Relecture T6 (M4) : sans sol sec a 200 yd, 120 yd montee, puis (3 s plus tard) a pied.
                Fuite f = FuirChasseur(cible, chasseur, REACTION_FUITE_MONTEE_PAS_YD, true);
                if (f.pas <= 0.0f)
                    f = FuirChasseur(cible, chasseur, REACTION_FUITE_PAS_YD, ecoule < REACTION_MONTE_MAX_S + 3);
                if (f.pas <= 0.0f)
                {
                    if (ecoule >= REACTION_MONTE_MAX_S + 6)
                        e.phase = FUITE_ATTENTE;   // aucun sol sec : tout reprend (elle descend a sa prochaine fuite)
                    return;
                }
                bool const montee = cible->IsMounted();
                LeverImmobile(e.cible);
                NoterFuite(e, f, maintenant, montee);
                JournaliserReaction(e, "fuite", Acore::StringFormat(" fuite_yd={:.0f} monture={} incantation_s={}"
                                                                    " distance={:.0f}",
                                                                    f.pas, montee ? 1 : 0, ecoule,
                                                                    chasseur->GetExactDist2d(cible)));
                return;
            }
            bool const rate = cible->IsInCombat() || ecoule >= REACTION_MONTE_MAX_S ||
                              (ecoule >= REACTION_MONTE_MIN_S && !cible->IsNonMeleeSpellCast(false));
            if (!rate)
                return;
            cible->InterruptNonMeleeSpells(false);
            Fuite const f = FuirChasseur(cible, chasseur);
            if (f.pas <= 0.0f)
            {
                e.phase = FUITE_ATTENTE;   // ni monture ni sol sec : tout reprend, la seconde suivante
                return;
            }
            LeverImmobile(e.cible);
            NoterFuite(e, f, maintenant, false);
            JournaliserReaction(e, "fuite", Acore::StringFormat(" fuite_yd={:.0f} monture=0 raison=incantation"
                                                                " incantation_s={} distance={:.0f}",
                                                                f.pas, ecoule, chasseur->GetExactDist2d(cible)));
            return;
        }
        case FUITE_COURT:
        {
            // Un `P-0` de renouvellement (contrats.py) la remettrait immobile en route.
            LeverImmobile(e.cible);
            bool const arrivee = maintenant >= e.finFuiteS ||
                                 cible->GetExactDist2d(e.pointX, e.pointY) <= REACTION_ARRIVEE_YD;
            if (!arrivee)
            {
                RelancerFuite(cible, e, maintenant);
                return;
            }
            // Arrivee : elle descend et reste sur place (immobile, elle riposte) : un chasseur a pied la rattrape.
            bool const etaitMontee = cible->IsMounted();
            if (etaitMontee)
                cible->RemoveAurasByType(SPELL_AURA_MOUNTED);
            if (!cible->IsInCombat())
                cible->StopMoving();
            PoserImmobileAuMoins(e.cible, maintenant + REACTION_IMMOBILE_APRES_S);
            e.phase = FUITE_ARRIVEE;
            e.phaseS = maintenant;
            JournaliserReaction(e, "arrivee", Acore::StringFormat(" fuite={} descendue={} ecart_point_yd={:.0f}",
                                                                  e.fuites, etaitMontee ? 1 : 0,
                                                                  cible->GetExactDist2d(e.pointX, e.pointY)));
            return;
        }
        case FUITE_ARRIVEE:
        default:
            if (e.fuites < 2)
                SecondeFuite(e, cible, chasseur, maintenant);
            return;
    }
}

// Lot T6 : soudoiement, une seconde (voir l'en-tete de la section).
void SuivreSoudoiement(ReactionContrat& e, Player* cible, Player* chasseur, uint32 maintenant)
{
    if (e.frappe || e.defenseLocale)
    {
        // Elle se defend : hors du repos et de l'immobilite (un `P-0` de renouvellement les reposerait).
        LeverImmobile(e.cible);
        LeverRepos(e.cible);
        if (e.defenseFaite)
            return;
        VueChasseur const v = Regarder(cible, chasseur, REACTION_DEFENSE_YD);
        if (!(v.auxPrises || (v.vu && cible->IsValidAttackTarget(chasseur))))
            return;
        bool const engage = !v.auxPrises && EngagerChasseur(cible, chasseur);
        e.defenseFaite = true;
        JournaliserReaction(e, "defense", Acore::StringFormat(" engage={} deja_en_combat={} cause={} distance={:.0f}",
                                                              engage ? 1 : 0, v.auxPrises ? 1 : 0,
                                                              e.frappe ? "frappe" : "offre_echue", v.d));
        return;
    }
    VueChasseur const v = Regarder(cible, chasseur, REACTION_SOUDOIEMENT_YD);
    if (!v.memeCarte || !chasseur->IsAlive())
        return;
    if (v.auxPrises)
    {
        // Le chasseur l'a frappee (avant ou pendant l'offre) : elle se defend ; le service lit la ligne.
        e.frappe = true;
        e.defenseFaite = true;   // deja en combat avec lui : le moteur de combat riposte
        LeverImmobile(e.cible);
        LeverRepos(e.cible);
        JournaliserReaction(e, "frappe", Acore::StringFormat(" offre={} t={} distance={:.0f}", e.offre ? 1 : 0,
                                                             maintenant, v.d));
        return;
    }
    if (!e.offre)
    {
        auto const deja = offresFaites.find(e.arc);
        if (deja != offresFaites.end())
        {
            e.offre = true;   // arc oublie puis relu (LireReactions) : l'offre est deja faite, jamais deux fois
            e.offreS = deja->second;
            return;
        }
        if (!e.potDeVinC)
        {
            if (!e.sansMontantDit)
            {
                e.sansMontantDit = true;
                JournaliserReaction(e, "sans_montant", " (params.pot_de_vin_c absent : aucune offre, cible passive)");
            }
            return;
        }
        if (!v.vu || cible->IsInCombat())
            return;
        if (cible->isMoving())
            cible->StopMoving();
        // Elle attend la reponse sur place (« stay » : elle riposte), meme si son immobilite de preparation a cesse.
        PoserImmobileAuMoins(e.cible, maintenant + REACTION_SOUDOIEMENT_ABANDON_S + REACTIONS_RELIRE_S);
        cible->SetFacingToObject(chasseur);
        std::string const texte = Acore::StringFormat(
            "Pas si vite, chasseur. {} pour oublier que tu m'as trouvé ? Personne n'en saura rien.",
            TexteCuivre(e.potDeVinC));
        cible->Whisper(texte, LANG_UNIVERSAL, chasseur);
        e.offre = true;
        e.offreS = maintenant;
        offresFaites[e.arc] = maintenant;
        JournaliserReaction(e, "offre", Acore::StringFormat(" offre_c={} t={} distance={:.0f}", e.potDeVinC, maintenant,
                                                            v.d));
        return;
    }
    // Offre faite : le service tranche. Sans decision dans le delai (service arrete, ligne non lue) : elle se defend.
    if (maintenant - e.offreS >= REACTION_SOUDOIEMENT_ABANDON_S)
    {
        e.defenseLocale = true;
        JournaliserReaction(e, "offre_echue", Acore::StringFormat(" depuis_s={}", maintenant - e.offreS));
        return;
    }
}

// Lot T6 : l'offre de soudoiement en cours pour cet arc, a montrer au joueur (traqueur) : montant en cuivres et
// secondes restantes. -> faux sans offre ouverte. Fil monde (CorpsTraqueur est appele au fil monde, comme Entretien).
bool OffreOuverte(uint32 arc, uint32& cuivre, uint32& resteS)
{
    auto const it = reactionsContrat.find(arc);
    if (it == reactionsContrat.end())
        return false;
    ReactionContrat const& e = it->second;
    uint32 const maintenant = Maintenant();
    if (e.reaction != "soudoiement" || !e.offre || e.frappe || e.defenseLocale || !e.potDeVinC ||
        maintenant < e.offreS || maintenant - e.offreS >= REACTION_SOUDOIEMENT_S)
        return false;
    cuivre = e.potDeVinC;
    resteS = REACTION_SOUDOIEMENT_S - (maintenant - e.offreS);
    return true;
}

// Revue 16/10 : option Reactions coupee ou bus eteint. Les reactions sont oubliees ET l'immobilite qu'elles ont posee
// (fuite montee arrivee : 30 min ; offre de soudoiement : 50 s) est levee, sinon la cible reste en « stay » hors contrat.
// Bus eteint : ViderRepos a deja tout leve, LeverImmobile n'y fait rien (garde nbImmobiles).
void OublierReactions()
{
    if (reactionsContrat.empty())
        return;
    for (auto const& [arc, e] : reactionsContrat)
        if ((e.reaction == "fuite_monture" && e.fuites) || (e.reaction == "soudoiement" && e.offre))
            LeverImmobile(e.cible);
    reactionsContrat.clear();
    reactionsLuesS = 0;   // relecture immediate au rallumage
}

void SuivreReactions()
{
    // Revue 16/10 : bus eteint = inertie. Avant, les cibles de contrat continuaient de reagir (gardes sautes, chasseur
    // engage) bus coupe, et ViderRepos defaisait a chaque tick l'immobilite que PoserImmobileAuMoins reposait.
    if (!sPlayerbotAIConfig.directeurReactions || !sPlayerbotAIConfig.directeurBus)
    {
        OublierReactions();
        return;
    }
    uint32 const maintenant = Maintenant();
    if (maintenant < reactionsLuesS || maintenant - reactionsLuesS >= REACTIONS_RELIRE_S)
    {
        reactionsLuesS = maintenant;
        LireReactions();
    }
    bool const t6 = sPlayerbotAIConfig.directeurReactionsT6;
    for (auto& [arc, e] : reactionsContrat)
    {
        bool const defense = e.reaction == "defense";
        bool const fuite = e.reaction == "fuite_gardes";
        bool const monture = t6 && e.reaction == "fuite_monture";
        bool const soudoiement = t6 && e.reaction == "soudoiement";
        if (!defense && !fuite && !monture && !soudoiement)
        {
            if (!e.inconnueDite)
            {
                e.inconnueDite = true;
                JournaliserReaction(e, "inconnue", " (reaction que ce binaire ne connait pas : cible passive)");
            }
            continue;
        }
        // Revue 16/10 : `.directeur stop` du chasseur : sa cible ne reagit plus (le service clot l'arc ensuite).
        if (CoaDirecteur::EstArrete(e.joueur))
            continue;
        Player* chasseur = JoueurConnecte(e.joueur);
        Player* cible = JoueurConnecte(e.cible);
        if (!chasseur || !cible || !Humain(chasseur) || !chasseur->IsInWorld() || !cible->IsInWorld() ||
            cible->IsBeingTeleported() || !cible->IsAlive() || !sRandomPlayerbotMgr.IsRandomBot(cible))
            continue;
        PlayerbotAI* ia = GET_PLAYERBOT_AI(cible);
        if (!ia || ia->HasGameClientMaster())
            continue;
        if (monture)
        {
            SuivreFuiteMontee(e, cible, chasseur, ia, maintenant);
            continue;
        }
        if (soudoiement)
        {
            SuivreSoudoiement(e, cible, chasseur, maintenant);
            continue;
        }
        if (e.declenchee)
        {
            // Une preparation `P-0` de renouvellement (contrats.py, repos echu) la remettrait immobile (et, pour la
            // defense, au repos). La fuyarde GARDE son repos (relecture M2) : elle ne vise aucun joueur, elle riposte.
            LeverImmobile(e.cible);
            if (defense)
                LeverRepos(e.cible);
            else if (t6)
            {
                if (e.phase == FUITE_COURT && maintenant < e.finFuiteS)
                    RelancerFuite(cible, e, maintenant);
                else if (e.phase == FUITE_COURT)
                    e.phase = FUITE_ARRIVEE;   // fuite_gardes : libre apres sa fuite (T5), sans immobilite
                SecondeFuite(e, cible, chasseur, maintenant);
            }
            continue;
        }
        bool const memeCarte = chasseur->GetMapId() == cible->GetMapId() &&
                               chasseur->GetInstanceId() == cible->GetInstanceId();
        float const d = memeCarte ? chasseur->GetExactDist2d(cible) : -1.0f;
        if (fuite && (!memeCarte || d > REACTION_CHASSEUR_LOIN_YD))
        {
            for (uint32 g : e.gardes)
            {
                Player* garde = GardeValide(g, cible, chasseur);
                if (!garde || garde->GetExactDist2d(cible) <= REACTION_GARDE_PROCHE_YD)
                    continue;
                auto const s = e.sautGarde.find(g);
                if (s != e.sautGarde.end() && maintenant - s->second < REACTION_SAUT_INTERVALLE_S)
                    continue;
                e.sautGarde[g] = maintenant;
                if (SauterGardePres(garde, cible))
                    JournaliserReaction(e, "garde_saut", Acore::StringFormat(" garde={}", g));
            }
        }
        if (!memeCarte || !chasseur->IsAlive())
            continue;
        bool const vu = d <= REACTION_FUITE_YD && cible->IsWithinLOSInMap(chasseur);   // de pres seulement : couteux
        // « Deja aux prises » : en combat AVEC le chasseur (il a tire le premier, de loin) -- jamais un combat contre
        // une creature, qui declencherait la reaction le chasseur a l'autre bout de la carte.
        bool const auxPrises = d <= REACTION_ENGAGE_YD && cible->IsInCombatWith(chasseur);
        // Defense : rien tant que l'attaque n'est pas permise (lieu sans PvP, sanctuaire) : la reaction attend, elle
        // n'est pas consommee pour rien.
        if (defense && (auxPrises || (d <= REACTION_DEFENSE_YD && vu && cible->IsValidAttackTarget(chasseur))))
        {
            bool const dejaEnCombat = auxPrises;
            LeverImmobile(e.cible);
            LeverRepos(e.cible);
            bool const engage = !dejaEnCombat && EngagerChasseur(cible, chasseur);
            e.declenchee = true;
            JournaliserReaction(e, "defense", Acore::StringFormat(" engage={} deja_en_combat={} distance={:.0f}",
                                                                  engage ? 1 : 0, dejaEnCombat ? 1 : 0, d));
        }
        else if (fuite && ((d <= REACTION_FUITE_YD && vu) || auxPrises))
        {
            // Relecture (M2) : la fuyarde GARDE son repos (PossibleTargetsValue : elle ne vise aucun joueur).
            Fuite const f = FuirChasseur(cible, chasseur);
            uint32 const engages = EngagerGardes(e, cible, chasseur, false);
            if (f.pas <= 0.0f && !engages)
                continue;   // aucun sol sec ou fuir, aucun garde a portee : reessai la seconde suivante, immobile
            // Lot T6 : l'immobilite levee APRES le mouvement (avant, une fuite sans point la laissait libre, sans fuir).
            LeverImmobile(e.cible);
            e.declenchee = true;
            if (f.pas > 0.0f)
                NoterFuite(e, f, maintenant, false);   // lot T6 : la seconde fuite part de la premiere
            else
                e.fuites = 2;                          // aucune fuite : pas de « seconde »
            JournaliserReaction(e, "fuite", Acore::StringFormat(" fuite_yd={:.0f} gardes={} gardes_engages={} distance={:.0f}",
                                                                f.pas, e.gardes.size(), engages, d));
        }
    }
}

bool PrepareDepuisMoinsDe(uint32 guid, uint32 secondes)
{
    if (!nbPrepares.load(std::memory_order_relaxed))
        return false;
    std::lock_guard<std::mutex> garde(verrouPrepares);
    auto const it = preparesLe.find(guid);
    return it != preparesLe.end() && Maintenant() < it->second + secondes;
}

// Le bot est-il de la reserve ? Fil monde seul (lit la chaine de conf). Liste
// d'entiers strictement positifs separes par des virgules, espaces toleres. UN
// morceau mal forme (vide compris) et AUCUN bot n'est de la reserve : meme regle
// que le service (emetteur.lire_reserve_guildes, « echoue ferme »), pour que jeu
// et service ne lisent jamais deux reserves differentes. Sans guilde : jamais.
bool EstDeLaReserve(Player* bot)
{
    return bot && sPlayerbotAIConfig.EstGuildeDeReserve(bot->GetGuildId());
}

bool EstPreparation(CoaDirCommande const& c)
{
    return c.verbe == CoaDirVerbe::Chasser && c.action.size() > 2 && c.action[0] == 'P' && c.action[1] == '-';
}

// Recevoir : `chasser` `P-` -> la file (fil monde). File pleine : refus `plafond`.
void MettreEnFilePreparation(CoaDirCommande const& c)
{
    if (preparations.size() >= PREPARATIONS_EN_FILE_MAX)
    {
        Refuser(c, "plafond");
        return;
    }
    preparations.push_back(c);
}

// Preparation, changement de carte (02/10, demande du coordinateur) : le bot de la
// reserve n'est pas sur le continent du joueur prime. Point d'arrivee SUR, a
// PREP_CARTE_MIN_YD-PREP_CARTE_MAX_YD du joueur : sol sec (SolSec : pas d'eau),
// pas sous une structure (ToitAuDessus), hors lieu sans PvP (la chasse y serait
// en REPLI), loin de Wintergrasp, et a plus de SAUT_HUMAIN_YD de tout humain et
// de tout point de vue (HumainProcheSauf, le joueur prime compris).
//
// LES CANDIDATS, LUS AVANT D'ECRIRE. Les noeuds de route de playerbots
// (TravelNodeMap) ne sont PAS charges en service : loadNodeStore n'est appele
// que par TravelMgr::LoadQuestTravelTable, qui n'a aucun appelant (lu ; la
// bible le dit aussi). ChercherPointCache tire ses points autour d'un centre
// fourni par le service, a 150-350 yd : rien a reprendre a 600-1 200 yd. Trois
// sources, dans cet ordre, PREP_CARTE_ESSAIS sondes de terrain au plus chacune :
//  1. les HUBS de la faction du bot (TravelMgr::GetTravelHubs : aubergistes et
//     maitres de vol des zones sans auberge, au niveau VISE) -- un lieu de sa
//     faction ; la plupart sont sous un toit (refuses par ToitAuDessus) ;
//  2. une COURONNE geometrique autour du joueur ;
//  3. les cellules de creatures de RandomTeleport (GetLocsPerLevelCache,
//     niveau-3..niveau+3, z de spawn d'une creature depuis P-331), EN DERNIER : on y atterrit au
//     milieu de creatures, qui l'engageraient -- et un bot en combat fait
//     refuser le saut `T-`/`L-` suivant (`combat`).
// Le point n'est qu'une ETAPE : le saut suivant le teleporte encore. Qu'il soit
// joignable a pied n'importe donc pas ; qu'il soit sur et cache, si.
// Fil monde, cartes a l'arret (P-137) : les grilles de la carte du joueur sont
// lues (et creees au besoin, GetGridHeight) depuis ce fil, comme
// ChercherPointCache le fait deja.
constexpr float PREP_CARTE_MIN_YD = 600.0f;
constexpr float PREP_CARTE_MAX_YD = 1200.0f;
constexpr uint32 PREP_CARTE_ESSAIS = 12;
constexpr int32 PREP_CARTE_ECART_NIVEAU = 3;

struct PointCarte
{
    bool trouve = false;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32 essais = 0;
    char const* source = "-";   // cache (cellules de creatures) ou couronne
};

// Embuscade (02/10, mesure du 02/10 a 14:57:13 Paris : Allinan, prepare du niveau
// 53 au niveau 26 SUR PLACE, tue par les creatures de son ancienne zone avant son
// saut, `preparation:mort`). Une creature HOSTILE et FORTE (niveau max >= niveau
// vise + PREP_CREATURE_ECART_NIVEAU, ou elite d'un niveau au moins egal) a-t-elle
// un point d'apparition a PREP_CREATURES_YD de (x, y) ? Lu dans les DONNEES
// d'apparition (ObjectMgr, par grille de 533 yd), et non dans les grilles
// chargees : le point est d'ordinaire dans une grille que personne n'a chargee, et
// ses creatures n'y apparaitront qu'a l'arrivee du bot. Les creatures de
// creature_multispawn (id2, id3) ne sont pas lues : limite assumee. Hostile : le
// gabarit de faction de la creature est hostile a celui du bot, ou aux joueurs.
// Fil monde (donnees statiques de ObjectMgr).
constexpr float PREP_CREATURES_YD = 60.0f;
constexpr int32 PREP_CREATURE_ECART_NIVEAU = 3;

bool CreatureForteAutour(uint32 carte, float x, float y, Player* bot, uint32 niveau)
{
    FactionTemplateEntry const* moi = bot->GetFactionTemplateEntry();
    std::array<uint32, 4> grilles{};
    size_t nb = 0;
    for (float const dx : {-PREP_CREATURES_YD, PREP_CREATURES_YD})
        for (float const dy : {-PREP_CREATURES_YD, PREP_CREATURES_YD})
        {
            float gx = x + dx, gy = y + dy;
            if (!Acore::IsValidMapCoord(gx, gy))
                continue;
            uint32 const id = Acore::ComputeGridCoord(gx, gy).GetId();
            if (std::find(grilles.begin(), grilles.begin() + nb, id) == grilles.begin() + nb)
                grilles[nb++] = id;
        }
    float const r2 = PREP_CREATURES_YD * PREP_CREATURES_YD;
    for (size_t i = 0; i < nb; ++i)
    {
        CellObjectGuids const& cellule = sObjectMgr->GetGridObjectGuids(uint16(carte), 0, grilles[i]);
        for (ObjectGuid::LowType const spawn : cellule.creatures)
        {
            CreatureData const* d = sObjectMgr->GetCreatureData(spawn);
            if (!d || d->mapid != carte)
                continue;
            float const ex = d->posX - x, ey = d->posY - y;
            if (ex * ex + ey * ey > r2)
                continue;
            CreatureTemplate const* modele = sObjectMgr->GetCreatureTemplate(d->id);
            if (!modele)
                continue;
            bool const forte = int32(modele->maxlevel) >= int32(niveau) + PREP_CREATURE_ECART_NIVEAU ||
                               (modele->rank > CREATURE_ELITE_NORMAL && int32(modele->maxlevel) >= int32(niveau));
            if (!forte)
                continue;
            FactionTemplateEntry const* sienne = sFactionTemplateStore.LookupEntry(modele->faction);
            // Faction inconnue : prudence, le point est refuse.
            if (!moi || !sienne || sienne->IsHostileTo(*moi) || sienne->IsHostileToPlayers())
                return true;
        }
    }
    return false;
}

bool PointCarteSur(Map* map, Player* joueur, Player* bot, uint32 niveau, float x, float y, float zDepart, float& sol)
{
    float const dx = x - joueur->GetPositionX(), dy = y - joueur->GetPositionY();
    float const d2 = dx * dx + dy * dy;
    if (d2 < PREP_CARTE_MIN_YD * PREP_CARTE_MIN_YD || d2 > PREP_CARTE_MAX_YD * PREP_CARTE_MAX_YD)
        return false;
    if (PresDeWintergrasp(map->GetId(), x, y))
        return false;
    if (!SolSec(map, x, y, zDepart, sol))
        return false;
    if (ToitAuDessus(map, x, y, sol, sol))
        return false;
    uint32 zone = 0, aire = 0;
    map->GetZoneAndAreaId(PHASEMASK_NORMAL, zone, aire, x, y, sol);
    if (sPlayerbotAIConfig.CoaLieuSansPvp(zone, aire))
        return false;
    if (HumainProcheSauf(WorldPosition(map->GetId(), x, y, sol), map->GetId(), SAUT_HUMAIN_YD, 0))
        return false;
    // Embuscade (02/10) : loin des creatures hostiles fortes (option eteinte : la regle d'avant).
    return !sPlayerbotAIConfig.directeurEmbuscade || !CreatureForteAutour(map->GetId(), x, y, bot, niveau);
}

PointCarte ChercherPointCarte(Map* map, Player* joueur, Player* bot, uint32 niveau)
{
    PointCarte r;
    uint32 const carte = map->GetId();
    float const jx = joueur->GetPositionX(), jy = joueur->GetPositionY();
    auto const dansAnneau = [&](float x, float y)
    {
        float const dx = x - jx, dy = y - jy;
        float const d2 = dx * dx + dy * dy;
        return d2 >= PREP_CARTE_MIN_YD * PREP_CARTE_MIN_YD && d2 <= PREP_CARTE_MAX_YD * PREP_CARTE_MAX_YD;
    };
    // Essaie au plus PREP_CARTE_ESSAIS candidats (x, y, z de depart), dans un
    // ordre tire au hasard. -> vrai si un point est retenu (r rempli).
    auto const essayer = [&](std::vector<std::array<float, 3>>& candidats, char const* source)
    {
        for (size_t i = candidats.size(); i > 1; --i)
            std::swap(candidats[i - 1], candidats[urand(0, uint32(i - 1))]);
        uint32 n = 0;
        for (auto const& p : candidats)
        {
            if (n++ >= PREP_CARTE_ESSAIS)
                break;
            ++r.essais;
            float sol = 0.0f;
            if (PointCarteSur(map, joueur, bot, niveau, p[0], p[1], p[2], sol))
            {
                r.trouve = true;
                r.x = p[0];
                r.y = p[1];
                r.z = sol;
                r.source = source;
                return true;
            }
        }
        return false;
    };
    std::vector<std::array<float, 3>> candidats;
    // 1. Hubs de la faction du bot, a son niveau (z du PNJ + 0,5 : depart a +2).
    // Revue m4 : au niveau VISE (le bot n'y est pas encore : Randomize vient apres).
    for (WorldLocation const& loc : sTravelMgr.GetTravelHubs(bot->GetTeamId(), uint8(niveau)))
        if (loc.GetMapId() == carte && dansAnneau(loc.GetPositionX(), loc.GetPositionY()))
            candidats.push_back({loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ() + Z_OFFSET_FIND_HEIGHT});
    if (essayer(candidats, "hub"))
        return r;
    // 2. Couronne : PREP_CARTE_ESSAIS directions regulieres depuis un angle tire
    // au hasard, rayon 900, 700 puis 1 100 yd en alternance (z de depart : celui
    // du joueur ; SolSec part du terrain brut s'il est plus haut).
    candidats.clear();
    float const a0 = frand(0.0f, 2.0f * float(M_PI));
    float const rayons[3] = {900.0f, 700.0f, 1100.0f};
    for (uint32 k = 0; k < PREP_CARTE_ESSAIS; ++k)
    {
        float const a = a0 + 2.0f * float(M_PI) * float(k) / float(PREP_CARTE_ESSAIS);
        float const rayon = rayons[k % 3];
        candidats.push_back({jx + std::cos(a) * rayon, jy + std::sin(a) * rayon, joueur->GetPositionZ()});
    }
    if (essayer(candidats, "couronne"))
        return r;
    // 3. Cellules de creatures (z arrondi a 50 yd : depart 25 yd au-dessus).
    candidats.clear();
    for (int32 l = int32(niveau) - PREP_CARTE_ECART_NIVEAU; l <= int32(niveau) + PREP_CARTE_ECART_NIVEAU; ++l)
    {
        if (l < 1 || l > 80)
            continue;
        for (WorldLocation const& loc : sTravelMgr.GetLocsPerLevelCache(uint8(l)))
            if (loc.GetMapId() == carte && dansAnneau(loc.GetPositionX(), loc.GetPositionY()))
                candidats.push_back({loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ() + 25.0f});
    }
    essayer(candidats, "cellule");
    return r;
}

// ------------------------------------------------------------------ retraite des chasseurs de reserve (03/10/2026)
//
// Mesure du 02/10 : Knuhwio (prime 426) puis Krillock (2521, primes 426 et 450),
// chasseurs de reserve, sont restes sur place apres `fin_prime` (`coa directeur
// sejour: ... motif=fin_prime`) ; le joueur les a recroises en boucle, et Knuhwio
// l'a retue a sa resurrection. Desormais, a TOUTE fin de prime (et a la mort d'un
// chasseur de reserve pendant la prime, puisqu'il se relevera pres du prime), le
// chasseur de RESERVE se retire :
//  1. tout de suite : REPOS de la reserve (PoserRepos : il ne convoite plus personne,
//     riposte permise ; aucune teleportation aleatoire) ; mort, l'etat mort_en_trajet
//     (option ReleveCimetiere) l'envoie se relever a l'esprit-guerisseur ;
//  2. vivant, hors combat : tant qu'un humain est a SAUT_HUMAIN_YD de lui, il s'eloigne
//     A PIED de l'humain le plus proche (CoaChasseAction lit EnRetraite) ;
//  3. aucun humain (point de vue compris) a SAUT_HUMAIN_YD : SAUT CACHE sur un point
//     sur a 600-1 200 yd du PRIME (de lui-meme si le prime n'est pas sur sa carte ;
//     deja a 600 yd du prime sans humain a 300 yd : `deja_loin`, pas de saut)
//     (ChercherPointCarte, la recherche de la preparation :
//     sol sec, ciel ouvert, hors lieu sans PvP et Wintergrasp, aucun humain a 150 yd du
//     point, loin des creatures hostiles fortes), puis un repos neuf de 30 min.
// Jamais teleporte sous les yeux d'un humain : le saut n'a lieu qu'au point 3. Borne :
// RETRAITE_MAX_S, puis la retraite tombe (`echue`) et le repos court jusqu'a son terme.
// Ni le saut ni le repos ne comptent comme saut de prime (sautsFaits) : une prime
// suivante peut le reprendre aussitot (ExecuterAffectation et la preparation levent la
// retraite). Etat au fil monde seul ; lecteurs de carte : EnRetraite.
constexpr uint32 RETRAITE_MAX_S = 600;
constexpr uint32 RETRAITE_ESSAI_S = 5;           // une recherche de point par bot et par 5 s au plus
constexpr uint8 RETRAITE_ESSAIS_MAX = 6;         // puis `sans_point`
constexpr size_t RETRAITE_RECHERCHES_PAR_SECONDE = 2;
constexpr float RETRAITE_HUMAIN_LU_YD = 300.0f;  // humain le plus proche cherche jusque-la (fuirDe)

// L'humain (point de vue compris, MJ invisibles exceptes) le plus proche de `bot`, sur
// sa carte, a `rayon` au plus. -> son guid bas (0 : aucun), `distance` remplie. Meme
// boucle que HumainProcheSauf. Fil monde.
uint32 HumainLePlusProche(Player* bot, float rayon, float& distance)
{
    uint32 guid = 0;
    float meilleure = rayon * rayon;
    WorldPosition ici(bot);
    for (Player* humain : sRandomPlayerbotMgr.GetPlayers())
    {
        if (!humain || (humain->IsGameMaster() && !humain->isGMVisible()) || humain->GetMapId() != bot->GetMapId())
            continue;
        float d2 = ici.sqDistance(WorldPosition(humain));
        WorldObject* vue = humain->GetViewpoint();
        if (vue && vue != humain)
            d2 = std::min(d2, ici.sqDistance(WorldPosition(vue)));
        if (d2 <= meilleure)
        {
            meilleure = d2;
            guid = humain->GetGUID().GetCounter();
        }
    }
    distance = std::sqrt(meilleure);
    return guid;
}

// Fil monde, HORS de tout verrou. Idempotent : une retraite deja en cours n'est pas
// refaite. Ignore un bot hors ligne, ou deja chasseur d'une AUTRE prime qui court.
void CommencerRetraite(uint32 bot, uint32 prime, uint32 joueur, char const* cause)
{
    Player* p = JoueurConnecte(bot);
    if (!p || CoaDirecteur::ChasseurDe(bot))
        return;
    uint32 const maintenant = Maintenant();
    bool const mort = !p->IsAlive();
    bool releve = false;
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        if (retraites.count(bot))
            return;
        Retraite nouvelle{bot, prime, joueur, maintenant, joueur, 0, 0};
        nouvelle.carte = p->GetMapId();
        nouvelle.sx = p->GetPositionX();
        nouvelle.sy = p->GetPositionY();
        nouvelle.scene = true;
        retraites[bot] = nouvelle;
        nbRetraites.store(static_cast<uint32>(retraites.size()), std::memory_order_relaxed);
        // Mort : relever a l'esprit-guerisseur plutot qu'au cadavre, pres du prime
        // (meme etat que la releve d'un trajet ; SuivreMortsEnTrajet le retire).
        if (mort && sPlayerbotAIConfig.directeurReleveCimetiere && !p->InBattleground() && !p->InArena() &&
            CarteDeSuspension(p->GetMapId()))
        {
            // `id` : ici l'id de la PRIME servie (pas d'affectation : elle est deja levee) ; la
            // ligne `releve: id=` d'une retraite porte donc un id de prime (revue m6).
            MortTrajet m{prime, bot, p->GetMapId(), p->GetPositionX(), p->GetPositionY(), maintenant,
                         maintenant + sPlayerbotAIConfig.directeurReleveSecondes};
            if (Corpse* corps = p->GetCorpse())
            {
                m.carte = corps->GetMapId();
                m.x = corps->GetPositionX();
                m.y = corps->GetPositionY();
            }
            mortsEnTrajet[bot] = m;
            nbMortsEnTrajet.store(static_cast<uint32>(mortsEnTrajet.size()), std::memory_order_relaxed);
            releve = true;
        }
    }
    PoserRepos(bot, maintenant + REPOS_RESERVE_S);
    float distance = -1.0f;
    if (Player* j = JoueurConnecte(joueur))
        if (j->GetMapId() == p->GetMapId())
            distance = p->GetExactDist2d(j);
    LOG_INFO("playerbots",
             "coa directeur retraite: prime={} bot={} joueur={} etape=debut cause={} vivant={} releve_cimetiere={}"
             " distance_joueur={}",
             prime, bot, joueur, cause, mort ? 0 : 1, releve ? 1 : 0,
             distance < 0.0f ? std::string("NA") : Acore::StringFormat("{:.0f}", distance));
}

// Fil monde. -> vrai si une retraite a ete levee (journalise).
bool LeverRetraite(uint32 bot, char const* motif)
{
    if (!nbRetraites.load(std::memory_order_relaxed))
        return false;
    Retraite r;
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        auto it = retraites.find(bot);
        if (it == retraites.end())
            return false;
        r = it->second;
        retraites.erase(it);
        nbRetraites.store(static_cast<uint32>(retraites.size()), std::memory_order_relaxed);
    }
    LOG_INFO("playerbots", "coa directeur retraite: prime={} bot={} joueur={} etape=fin motif={} duree_s={}", r.prime,
             r.bot, r.joueur, motif, Maintenant() - r.debutS);
    return true;
}

// Bus eteint (TraiterBus, a chaque tick) : comme le repos (ViderRepos), toute retraite
// tombe -- le Directeur eteint ne deplace plus personne. Fil monde.
void ViderRetraites(char const* motif)
{
    if (!nbRetraites.load(std::memory_order_relaxed))
        return;
    std::vector<uint32> bots;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto const& [g, r] : retraites)
            bots.push_back(g);
    }
    for (uint32 g : bots)
        LeverRetraite(g, motif);
}

// Chaque seconde, fil monde, cartes a l'arret (P-137). Meme
// patron que les autres suivis : copie sous verrou partage, examen hors verrou,
// ecriture sous verrou exclusif (meme debut), journal hors verrou.
void SuivreRetraites()
{
    if (!nbRetraites.load(std::memory_order_relaxed))
        return;
    uint32 const maintenant = Maintenant();
    std::vector<Retraite> copies;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        copies.reserve(retraites.size());
        for (auto const& [g, r] : retraites)
            copies.push_back(r);
    }
    struct Fin
    {
        Retraite r;
        char const* motif;
        float x = 0.0f, y = 0.0f, z = 0.0f, depuis = 0.0f, humain = -1.0f;
        uint32 essais = 0;
        char const* source = "-";
    };
    std::vector<Fin> fins;
    std::vector<Retraite> majs;
    size_t recherches = 0;
    for (Retraite r : copies)
    {
        if (maintenant >= r.debutS + RETRAITE_MAX_S)
        {
            fins.push_back(Fin{r, "echue"});
            continue;
        }
        Player* bot = JoueurConnecte(r.bot);
        if (!bot)
        {
            fins.push_back(Fin{r, "hors_ligne"});
            continue;
        }
        if (!bot->IsInWorld() || bot->IsBeingTeleported())
            continue;
        if (CoaDirecteur::ChasseurDe(r.bot))
        {
            fins.push_back(Fin{r, "rappele"});
            continue;
        }
        // Revue m3 : un humain l'a pris (groupe) : c'est son maitre qui le mene, aucun saut.
        if (PlayerbotAI* iaB = GET_PLAYERBOT_AI(bot); !iaB || iaB->HasGameClientMaster())
        {
            fins.push_back(Fin{r, "maitre_humain"});
            continue;
        }
        if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight())
            continue;
        if (bot->InBattleground() || bot->InArena() || !CarteDeSuspension(bot->GetMapId()))
        {
            fins.push_back(Fin{r, "carte"});
            continue;
        }
        float dHumain = 0.0f;
        uint32 const proche = HumainLePlusProche(bot, RETRAITE_HUMAIN_LU_YD, dHumain);
        uint32 const fuirDe = proche && dHumain < SAUT_HUMAIN_YD ? proche : 0;
        if (fuirDe != r.fuirDe)
        {
            r.fuirDe = fuirDe;
            majs.push_back(r);
        }
        // Un humain a portee de vue (marge comprise) : il s'eloigne a pied (CoaChasse.h).
        if (fuirDe || PresDeWintergrasp(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY()))
            continue;
        // L'ancre : le PRIME s'il est sur la carte du bot (le point sera a 600-1 200 yd de
        // lui), sinon le bot lui-meme. Deja a PREP_CARTE_MIN_YD du prime et aucun humain a
        // RETRAITE_HUMAIN_LU_YD : il a quitte la scene sans saut (`deja_loin`).
        // Banc du 03/10 (arc 12) : le prime MORT passe en fantome et part au cimetiere ; mesure
        // depuis lui, le chasseur reste sur le corps (`deja_loin`, Jimabliz a 2 yd de la
        // cible) -- c'est le cas Krillock du 02/10 (le joueur revient a son corps). Prime
        // mort ou fantome : l'ancre est le chasseur lui-meme, donc le lieu du combat.
        Player* ancre = JoueurConnecte(r.joueur);
        if (!ancre || !ancre->IsInWorld() || ancre->IsBeingTeleported() || ancre->GetMapId() != bot->GetMapId() ||
            !ancre->IsAlive() || ancre->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            ancre = bot;
        // `deja_loin` exige AUSSI d'etre a PREP_CARTE_MIN_YD du lieu du combat (fige au debut).
        bool const loinDeLaScene =
            !r.scene || r.carte != bot->GetMapId() ||
            (bot->GetPositionX() - r.sx) * (bot->GetPositionX() - r.sx) +
                    (bot->GetPositionY() - r.sy) * (bot->GetPositionY() - r.sy) >=
                PREP_CARTE_MIN_YD * PREP_CARTE_MIN_YD;
        if (!proche && loinDeLaScene && (ancre == bot || bot->GetExactDist2d(ancre) >= PREP_CARTE_MIN_YD) &&
            (ancre != bot || r.scene))
        {
            fins.push_back(Fin{r, "deja_loin"});
            continue;
        }
        if (recherches >= RETRAITE_RECHERCHES_PAR_SECONDE || (r.essaiS && maintenant < r.essaiS + RETRAITE_ESSAI_S))
            continue;
        ++recherches;
        // La recherche de la preparation : sol sec, ciel ouvert, hors lieu sans PvP, aucun
        // humain a SAUT_HUMAIN_YD du point, loin des creatures hostiles fortes ; a
        // 600-1 200 yd de l'ancre.
        PointCarte const point = ChercherPointCarte(bot->GetMap(), ancre, bot, bot->GetLevel());
        if (!point.trouve)
        {
            r.essaiS = maintenant;
            if (++r.essais >= RETRAITE_ESSAIS_MAX)
            {
                Fin f{r, "sans_point"};
                f.essais = point.essais;
                fins.push_back(f);
            }
            else
                majs.push_back(r);
            continue;
        }
        float const bx = bot->GetPositionX(), by = bot->GetPositionY();
        if (!CoaDirecteur::Sauter(bot, WorldLocation(bot->GetMapId(), point.x, point.y, point.z + 0.5f,
                                                     bot->GetOrientation())))
        {
            r.essaiS = maintenant;
            if (++r.essais >= RETRAITE_ESSAIS_MAX)
                fins.push_back(Fin{r, "saut_echoue"});
            else
                majs.push_back(r);
            continue;
        }
        Fin f{r, "partie"};
        f.x = point.x;
        f.y = point.y;
        f.z = point.z;
        f.depuis = std::sqrt((point.x - bx) * (point.x - bx) + (point.y - by) * (point.y - by));
        f.humain = proche ? dHumain : -1.0f;
        f.essais = point.essais;
        f.source = point.source;
        fins.push_back(f);
    }
    if (fins.empty() && majs.empty())
        return;
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        for (Retraite const& r : majs)
        {
            auto it = retraites.find(r.bot);
            if (it != retraites.end() && it->second.debutS == r.debutS && it->second.prime == r.prime)
                it->second = r;
        }
        for (Fin const& f : fins)
        {
            auto it = retraites.find(f.r.bot);
            if (it != retraites.end() && it->second.debutS == f.r.debutS && it->second.prime == f.r.prime)
                retraites.erase(it);
        }
        nbRetraites.store(static_cast<uint32>(retraites.size()), std::memory_order_relaxed);
    }
    for (Fin const& f : fins)
    {
        bool const partie = std::strcmp(f.motif, "partie") == 0;
        // Parti : un repos NEUF (30 min) a son arrivee ; sinon le repos du debut court.
        if (partie)
            PoserRepos(f.r.bot, Maintenant() + REPOS_RESERVE_S);
        if (partie)
            LOG_INFO("playerbots",
                     "coa directeur retraite: prime={} bot={} joueur={} etape=fin motif=retraite saut=1 duree_s={}"
                     " a_x={:.1f} a_y={:.1f} a_z={:.1f} distance_saut={:.0f} humain_proche={} essais={} source={}",
                     f.r.prime, f.r.bot, f.r.joueur, maintenant - f.r.debutS, f.x, f.y, f.z, f.depuis,
                     f.humain < 0.0f ? std::string("NA") : Acore::StringFormat("{:.0f}", f.humain), f.essais, f.source);
        else
            LOG_INFO("playerbots", "coa directeur retraite: prime={} bot={} joueur={} etape=fin motif={} saut=0 duree_s={}"
                     " essais={}", f.r.prime, f.r.bot, f.r.joueur, f.motif, maintenant - f.r.debutS, f.essais);
    }
}

// Une `P-` : controles, puis (niveau different) la fabrique, et (autre carte que
// le joueur prime) le saut sur sa carte, a 600-1 200 yd de lui. -> vrai si
// Randomize, la recherche de point ou le saut a tourne (le tick est pris). Fil monde, cartes a l'arret
// (P-137).
bool ExecuterPreparation(CoaDirCommande const& c)
{
    uint32 const niveauMax = std::min<uint32>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL), 80u);
    // 6. bornes : un bot, un joueur prime, un niveau 1-80 (et le plafond du monde).
    if (c.cible == 0 || c.joueur == 0 || c.valeur < 1 || static_cast<uint32>(c.valeur) > niveauMax)
    {
        Refuser(c, "bornes");
        return false;
    }
    uint32 const niveau = static_cast<uint32>(c.valeur);
    // Preparation HORS PRIME (02/10, demande du service) : `P-0` (arc 0) prepare
    // la reserve A L'AVANCE au niveau d'un humain en ligne qui n'a pas encore de
    // prime. La prime active n'est alors pas exigee ; a sa place, une fois le
    // joueur trouve, les controles d'ExecuterPrime sur lui (humain, MJ, champ de
    // bataille, carte, regle, defi). Le reste est le meme : reserve, repos, occupe,
    // file, un Randomize par tick, deplacement sur, aucune affectation.
    bool const horsPrime = c.action == "P-0";
    // Controles de `chasser` qui s'appliquent : prime active...
    if (!horsPrime)
    {
        bool primeActive = false;
        {
            std::shared_lock<std::shared_mutex> garde(verrouConsignes);
            auto it = primes.find(c.joueur);
            primeActive = it != primes.end() && it->second.finS > Maintenant();
        }
        if (!primeActive)
        {
            Refuser(c, "sans_prime");
            return false;
        }
    }
    Player* bot = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!bot)
    {
        Refuser(c, "hors_ligne");
        return false;
    }
    PlayerbotAI* ia = GET_PLAYERBOT_AI(bot);
    if (!sRandomPlayerbotMgr.IsRandomBot(bot) || !ia)
    {
        Refuser(c, "pas_un_bot");
        return false;
    }
    if (ia->HasGameClientMaster())
    {
        Refuser(c, "maitre_humain");
        return false;
    }
    // ... `pas_mercenaire`, `suspendu`...
    if (!MercenaryRewards::EstMercenaire(bot))
    {
        Refuser(c, "pas_mercenaire");
        return false;
    }
    if (CoaDirecteur::MercenaireSuspendu(c.cible))
    {
        Refuser(c, "suspendu");
        return false;
    }
    // ... puis les controles propres : de la reserve...
    if (!EstDeLaReserve(bot))
    {
        Refuser(c, "hors_reserve");
        return false;
    }
    // (Randomize ressusciterait un mort, PlayerbotFactory::Prepare : jamais ici.)
    if (!bot->IsAlive())
    {
        Refuser(c, "mort");
        return false;
    }
    // Taxi, ou en l'air sur une monture volante (Randomize la retire : il tomberait).
    if (bot->IsInFlight() || bot->IsFlying())
    {
        Refuser(c, "en_vol");
        return false;
    }
    if (bot->InBattleground() || bot->InArena())
    {
        Refuser(c, "champ_de_bataille");
        return false;
    }
    if (!bot->IsInWorld() || bot->IsBeingTeleported())
    {
        Refuser(c, "en_teleportation");
        return false;
    }
    // ... pas en combat.
    if (bot->IsInCombat())
    {
        Refuser(c, "combat");
        return false;
    }
    // Le joueur prime : sa carte decide d'un changement de continent (02/10).
    Player* joueur = ObjectAccessor::FindPlayerByLowGUID(c.joueur);
    if (!joueur || !joueur->IsInWorld() || joueur->IsBeingTeleported())
    {
        Refuser(c, joueur ? "joueur_indisponible" : "hors_ligne");
        return false;
    }
    // `P-0` : les controles d'ExecuterPrime (§ 6.2, dans son ordre), sur le joueur.
    if (horsPrime)
    {
        char const* refus = !Humain(joueur)                                   ? "pas_humain"
                            : joueur->IsGameMaster()                          ? "mj"
                            : (joueur->InBattleground() || joueur->InArena()) ? "champ_de_bataille"
                            : !CarteDeSuspension(joueur->GetMapId())          ? "carte"
                            : !HighRisk(joueur)                               ? "regle"
                            : (!defisLus || DefiBloquant(joueur))             ? "defi"
                                                                              : nullptr;
        if (refus)
        {
            Refuser(c, refus);
            return false;
        }
    }
    uint32 const avant = bot->GetLevel();
    uint32 const carteAvant = bot->GetMapId();
    uint32 const carteJoueur = joueur->GetMapId();
    bool const changeNiveau = avant != niveau;
    bool const changeCarte = carteAvant != carteJoueur;
    // Embuscade (02/10, mesure : Allinan, 53 -> 26 sur place a 14:57:13 Paris, tue
    // avant son saut, `preparation:mort`) : une preparation qui change le NIVEAU
    // deplace aussi le bot, meme deja sur la carte du joueur, sur le meme point sur
    // (ChercherPointCarte : 600-1 200 yd du joueur, hors de vue, loin des creatures
    // hostiles fortes). Memes controles de depart qu'un changement de carte.
    bool const deplacer = changeCarte || (changeNiveau && sPlayerbotAIConfig.directeurEmbuscade);
    // Monde ouvert seulement (continents, Outreterre, Norfendre) : jamais une
    // instance, un champ de bataille ou une carte de banc.
    // Le bot aussi : on ne le tire pas d'une instance ou d'une carte de banc.
    if (changeCarte && (!CarteDeSuspension(carteJoueur) || !CarteDeSuspension(carteAvant)))
    {
        Refuser(c, "carte");
        return false;
    }
    // 02/10 (revue) : un chasseur deja au niveau, sur la carte du joueur, est quand meme
    // reequipe si son equipement s'ecarte de celui de la cible (ScoreEquipement).
    uint32 const scoreCibleTot = ScoreEquipement(joueur, bot->GetLevel()),
                 scoreBotTot = ScoreEquipement(bot, bot->GetLevel());
    bool ecartEquipement = scoreBotTot > scoreCibleTot + PREPARATION_SCORE_ECART ||
                           scoreBotTot + PREPARATION_SCORE_ECART < scoreCibleTot;
    if (ecartEquipement && !changeNiveau && !changeCarte)
    {
        std::lock_guard<std::mutex> garde(verrouPrepares);
        auto it = reequipesLe.find(c.cible);
        if (it != reequipesLe.end() && Maintenant() < it->second + REEQUIPEMENT_REPOS_S)
            ecartEquipement = false;
    }
    if (!changeNiveau && !changeCarte && !ecartEquipement)
    {
        LOG_INFO("playerbots", "coa directeur reserve: id={} guid={} niveau_avant={} niveau_apres={} carte_avant={}"
                 " carte_apres={} duree_ms=0 motif=deja_au_niveau", c.id, c.cible, avant, avant, carteAvant, carteAvant);
        if (horsPrime)
        {
            PoserRepos(c.cible, Maintenant() + REPOS_RESERVE_S);
            PoserImmobile(c.cible, Maintenant() + REPOS_RESERVE_S);   // 04/10 : cible de contrat immobile
            LOG_INFO("playerbots", "coa directeur reserve: id={} guid={} joueur={} motif=repos repos={}", c.id, c.cible,
                     c.joueur, REPOS_RESERVE_S);
        }
        // Retraite (03/10/2026, revue M2) : rien n'a bouge ; la retraite continue jusqu'a la
        // pose de `chasser`, qui la leve (le service peut ne jamais le designer).
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
        Accuser(c, "execute", nullptr);
        return false;
    }
    // Un chevalier de la mort ne descend pas sous son niveau de depart (la
    // fabrique le ferait sans broncher).
    if (changeNiveau && bot->getClass() == CLASS_DEATH_KNIGHT &&
        niveau < sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL))
    {
        Refuser(c, "bornes");
        return false;
    }
    // Pas deux preparations FAITES du meme bot en PREPARATION_REPOS_S (niveau ou
    // carte : un `deja_au_niveau` sur place ne coute rien et ne compte pas ; un `reequipe`
    // sur place, lui, compte, et il est en plus borne par REEQUIPEMENT_REPOS_S).
    {
        if (PrepareDepuisMoinsDe(c.cible, PREPARATION_REPOS_S))
        {
            Refuser(c, "deja_prepare");
            return false;
        }
    }
    // Une affectation en cours (tout proprietaire) serait levee `niveau` a la
    // seconde suivante (SuivreAffectations) : le bot est pris, on n'y touche pas.
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        auto it = affectations.find(c.cible);
        if (it != affectations.end() && it->second.a.finS > Maintenant())
        {
            uint32 const parId = it->second.a.id;
            std::string const parOrigine = it->second.origine;
            garde.unlock();
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_OCCUPEES);
            LOG_INFO("playerbots", "coa directeur occupe: id={} guid={} origine={} joueur={} par_id={} par_origine={}"
                     " par_verbe=preparation", c.id, c.cible, c.origine, c.joueur, parId, parOrigine);
            Refuser(c, "occupe");
            return false;
        }
    }
    // Changement de carte : controles du DEPART (memes que RefusDepartSaut :
    // personne ne le voit disparaitre, pas pres de Wintergrasp), puis le POINT,
    // AVANT tout effet -- un refus ne laisse jamais un bot a moitie prepare.
    PointCarte point;
    // Revue M5 : la duree compte DEPUIS ICI, recherche de point comprise ; la
    // recherche est aussi mesuree a part (recherche_ms).
    uint32 const debutMs = getMSTime();
    uint32 rechercheMs = 0;
    if (deplacer)
    {
        if (ia->HasPlayerNearby(SAUT_HUMAIN_YD))
        {
            Refuser(c, "depart_a_vue");
            return false;
        }
        if (PresDeWintergrasp(carteAvant, bot->GetPositionX(), bot->GetPositionY()))
        {
            Refuser(c, "wintergrasp");
            return false;
        }
        point = ChercherPointCarte(joueur->GetMap(), joueur, bot, niveau);
        rechercheMs = GetMSTimeDiffToNow(debutMs);
        if (!point.trouve)
        {
            LOG_INFO("playerbots", "coa directeur reserve: id={} guid={} carte_avant={} carte_joueur={} refus=sans_point_carte"
                     " essais={} recherche_ms={}", c.id, c.cible, carteAvant, carteJoueur, point.essais, rechercheMs);
            Refuser(c, "sans_point_carte");
            // Revue M5 : la recherche a coute (sondes de terrain) : le tick est pris.
            return true;
        }
    }

    // L'effet. PIEGE DE VERROU : Randomize appelle ResetStrategies, donc la
    // fabrique, qui prend le verrou PARTAGE (MercenaireEnRpg) : aucun verrou
    // n'est tenu ici.
    uint32 const guilde = bot->GetGuildId();
    if (changeNiveau)
    {
        if (bot->IsMounted())
            bot->Dismount();   // comme AdjustBotToRange : pas une monture d'un autre niveau
        PlayerbotFactory fabrique(bot, niveau);
        fabrique.Randomize(false);
    }
    // Equipement (02/10) : lu APRES la fabrique (elle a pu reequiper au nouveau niveau).
    uint32 const scoreJoueur = ScoreEquipement(joueur, bot->GetLevel());
    uint32 const scoreAvant = ScoreEquipement(bot, bot->GetLevel());
    uint32 scoreApres = scoreAvant;
    uint32 reequipe = 0;   // qualite du dernier reequipement, 0 : aucun
    if (bot->IsMounted() && (scoreAvant > scoreJoueur + PREPARATION_SCORE_ECART ||
                             scoreAvant + PREPARATION_SCORE_ECART < scoreJoueur))
        bot->Dismount();
    if (scoreAvant + PREPARATION_SCORE_ECART < scoreJoueur)
    {
        Reequiper(bot, 0);
        reequipe = sPlayerbotAIConfig.randomGearQualityLimit;
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
    if (reequipe)
    {
        std::lock_guard<std::mutex> garde(verrouPrepares);
        if (reequipesLe.size() > 256)
            for (auto it = reequipesLe.begin(); it != reequipesLe.end();)
                it = Maintenant() >= it->second + REEQUIPEMENT_REPOS_S ? reequipesLe.erase(it) : std::next(it);
        reequipesLe[c.cible] = Maintenant();
    }
    // Le saut entre cartes : TeleportTo LOINTAIN (Player::TeleportTo pose
    // IsBeingTeleportedFar ; le bot quitte sa carte et n'entre dans la nouvelle
    // qu'a l'accuse de teleportation, rendu par PlayerbotHolder::UpdateSessions
    // -> PlayerbotAI::HandleTeleportAck au passage suivant, puis son IA attend 2 a
    // 5 s, SetNextCheckDelay). Meme fil et meme appel que RandomTeleport, qui
    // change deja de carte depuis le fil monde. Ce saut N'EST PAS inscrit dans
    // `sautsFaits` : le saut `T-`/`L-` qui suit ne doit pas etre refuse
    // `deja_saute` (il est borne, lui, par deja_prepare et par sa propre garde).
    uint32 carteApres = carteAvant;
    bool sautRefuse = false;
    // Meme carte (embuscade) : un TeleportTo PROCHE, meme fil, meme garde de depart.
    if (deplacer)
    {
        if (CoaDirecteur::Sauter(bot, WorldLocation(carteJoueur, point.x, point.y, point.z + 0.5f, bot->GetOrientation())))
            carteApres = carteJoueur;   // engage : effectif a l'accuse (tick suivant)
        else
            sautRefuse = true;
    }
    uint32 const dureeMs = GetMSTimeDiffToNow(debutMs);
    uint32 const apres = bot->GetLevel();
    {
        std::lock_guard<std::mutex> garde(verrouPrepares);
        if (preparesLe.size() > 256)
            for (auto it = preparesLe.begin(); it != preparesLe.end();)
                it = Maintenant() >= it->second + PREPARATION_REPOS_S ? preparesLe.erase(it) : std::next(it);
        preparesLe[c.cible] = Maintenant();
        nbPrepares.store(static_cast<uint32>(preparesLe.size()), std::memory_order_relaxed);
    }
    float const pjx = point.x - joueur->GetPositionX(), pjy = point.y - joueur->GetPositionY();
    LOG_INFO("playerbots", "coa directeur reserve: id={} guid={} niveau_avant={} niveau_apres={} carte_avant={}"
             " carte_apres={} duree_ms={} motif={} point={} distance_joueur={:.0f} essais={} recherche_ms={}"
             " score_joueur={} score_avant={} score_apres={} reequipe={}", c.id,
             c.cible, avant, apres, carteAvant, carteApres, dureeMs,
             changeNiveau ? "prepare" : (reequipe ? "reequipe" : "deja_au_niveau"),
             point.source, deplacer ? std::sqrt(pjx * pjx + pjy * pjy) : 0.0f, point.essais, rechercheMs,
             scoreJoueur, scoreAvant, scoreApres, reequipe);
    // Ceinture : la fabrique ne doit toucher ni la guilde ni le niveau demande.
    if (bot->GetGuildId() != guilde || (changeNiveau && apres != niveau))
        LOG_ERROR("playerbots", "coa directeur reserve_ecart: id={} guid={} guilde_avant={} guilde_apres={}"
                  " niveau_demande={} niveau_apres={}", c.id, c.cible, guilde, bot->GetGuildId(), niveau, apres);
    // Revue M4 : `P-0` reussie (niveau et saut faits) : le repos commence ici.
    if (horsPrime && !sautRefuse)
    {
        PoserRepos(c.cible, Maintenant() + REPOS_RESERVE_S);
        PoserImmobile(c.cible, Maintenant() + REPOS_RESERVE_S);   // 04/10 : cible de contrat immobile
        LOG_INFO("playerbots", "coa directeur reserve: id={} guid={} joueur={} motif=repos repos={}", c.id, c.cible,
                 c.joueur, REPOS_RESERVE_S);
    }
    if (sautRefuse)
    {
        // TeleportTo a refuse (Player::TeleportTo : carte interdite, en combat de
        // duel...). Le niveau a pu etre fait : il reste, le bot n'a pas bouge.
        Refuser(c, "saut_echoue");
        return true;
    }
    // Retraite (03/10/2026, revue M2) : DEPLACE par la preparation (point sur, hors de vue,
    // ChercherPointCarte), il a quitte la scene ; sinon la retraite continue jusqu'a `chasser`.
    if (deplacer && !sautRefuse)
        LeverRetraite(c.cible, "preparation");
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    Accuser(c, "execute", nullptr);
    return true;
}

// TraiterBus, bus allume, chaque tick : la file, jusqu'au PREMIER Randomize.
void TraiterPreparations()
{
    while (!preparations.empty())
    {
        CoaDirCommande const c = preparations.front();
        preparations.pop_front();
        if (ExecuterPreparation(c))
            return;
    }
}

// Bus eteint : ce qui attend est refuse, rien n'est prepare.
void ViderPreparations(char const* raison)
{
    while (!preparations.empty())
    {
        CoaDirCommande const c = preparations.front();
        preparations.pop_front();
        Refuser(c, raison);
    }
}

// Verbes `reaffecter` et `alleger` (SPEC § 6) : meme effet cote jeu. Le bot
// `cible` recoit le disque (carte, x, y, z, rayon) pour duree_s. Les controles
// 6 a 9 sont ceux du § 4.4 puis du tableau « Controles propres » du § 6.3, dans
// son ordre. Fil monde, cartes a l'arret (P-137).
void ExecuterAffectation(CoaDirCommande const& recue)
{
    // Compagnons (27/09, revue m1) : le centre d'une `C-` est la position
    // REELLE du joueur, lue ici (fil monde, P-137), et non celle de la photo
    // du service (jusqu'a 5 s + la latence du bus : un joueur monte l'aurait
    // fait refuser `bornes`). Le point du service ne sert qu'a la garde de
    // coherence ci-dessous (COMPAGNON_CENTRE_MAX_YD). Tout autre verbe : la
    // commande telle quelle.
    CoaDirCommande c = recue;
    if (c.verbe == CoaDirVerbe::Peupler && c.action.size() > 2 && c.action[0] == 'C' && c.action[1] == '-' &&
        sPlayerbotAIConfig.directeurCompagnons && c.joueur)
    {
        if (Player* j = ObjectAccessor::FindPlayerByLowGUID(c.joueur))
        {
            if (j->GetMapId() == c.carte)
            {
                float const ex = j->GetPositionX() - c.x;
                float const ey = j->GetPositionY() - c.y;
                if (ex * ex + ey * ey > COMPAGNON_CENTRE_MAX_YD * COMPAGNON_CENTRE_MAX_YD)
                {
                    Refuser(c, "bornes");
                    return;
                }
                c.x = j->GetPositionX();
                c.y = j->GetPositionY();
                c.z = j->GetPositionZ();
            }
        }
    }
    // 6. bornes
    if (c.cible == 0 || c.dureeS < AFFECTATION_DUREE_MIN_S || c.dureeS > AFFECTATION_DUREE_MAX_S ||
        !(c.rayon >= AFFECTATION_RAYON_MIN && c.rayon <= AFFECTATION_RAYON_MAX) || !std::isfinite(c.x) ||
        !std::isfinite(c.y) || !std::isfinite(c.z))
    {
        Refuser(c, "bornes");
        return;
    }
    // 7. plafond : une affectation reposee sur un bot deja affecte la remplace.
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        if (!affectations.count(c.cible) && affectations.size() >= sPlayerbotAIConfig.directeurMaxAffectations)
        {
            garde.unlock();
            Refuser(c, "plafond");
            return;
        }
    }
    // 8. en ligne
    Player* bot = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!bot)
    {
        Refuser(c, "hors_ligne");
        return;
    }
    // 9. controles propres (§ 6.3)
    PlayerbotAI* ia = GET_PLAYERBOT_AI(bot);
    if (!sRandomPlayerbotMgr.IsRandomBot(bot) || !ia)
    {
        Refuser(c, "pas_un_bot");
        return;
    }
    if (ia->HasGameClientMaster())
    {
        Refuser(c, "maitre_humain");
        return;
    }
    if (!bot->IsAlive())
    {
        Refuser(c, "mort");
        return;
    }
    if (bot->IsInFlight())
    {
        Refuser(c, "en_vol");
        return;
    }
    if (bot->InBattleground() || bot->InArena())
    {
        Refuser(c, "champ_de_bataille");
        return;
    }
    // Moteur : sans new rpg ni chasse, rien ne le menerait au disque (un
    // mercenaire de classe vanille n'a que « move random »).
    bool const chasse = ia->HasStrategy("coa chasse", BOT_STATE_NON_COMBAT);
    if (!chasse && !ia->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
    {
        Refuser(c, "sans_moteur");
        return;
    }
    // Piege 11 : MoveFarTo et MoveTo ignorent le mapId. Et monde ouvert
    // seulement : jamais une carte de banc.
    if (c.carte != bot->GetMapId() || !CarteDeSuspension(bot->GetMapId()))
    {
        Refuser(c, "carte");
        return;
    }
    // Lot D3, verbe `peupler` (SPEC-directeur-D3.md § 4.2) : controles propres,
    // en plus de ceux de `reaffecter`. Le joueur qui demande est lu ici, fil
    // monde, cartes a l'arret (P-137).
    bool const presence = c.verbe == CoaDirVerbe::Peupler;
    // Lot D4c, verbe `chasser` (plus bas). D4h (01/10) : un chasseur SAUTE aussi
    // (`T-`, `L-`), hors de la vue du joueur prime, puis finit a pied : assez
    // loin pour que sa chasse (guet, traque, embuscade) ait de la place.
    bool const chasseur = c.verbe == CoaDirVerbe::Chasser;
    // Compagnons (27/09) : un peupler d'action `C-<n>` (l'emetteur admet
    // `[A-Z]-\d+`). Option eteinte : refus explicite, rien d'autre ne change.
    bool const compagnon = presence && c.action.size() > 2 && c.action[0] == 'C' && c.action[1] == '-';
    if (compagnon && !sPlayerbotAIConfig.directeurCompagnons)
    {
        Refuser(c, "compagnon_eteint");
        return;
    }
    // Teleportation cachee (27/09) : un peupler d'action `T-<n>`, qui est une
    // ETAPE (le bot saute puis attend l'etape suivante) : les deux options.
    bool const sautCache = (presence || chasseur) && c.action.size() > 2 && c.action[0] == 'T' && c.action[1] == '-';
    // Saut lointain (30/09) : action `L-<n>`, memes controles que la `T-` sauf la
    // ligne de vue, et des bornes a 250-350 yd du joueur. Exige en plus
    // Directeur.TeleportLointain ; eteinte : refus `lointain_eteint` (le service
    // fait alors marcher le bot).
    bool const sautLointain = (presence || chasseur) && c.action.size() > 2 && c.action[0] == 'L' && c.action[1] == '-';
    bool const saut = sautCache || sautLointain;
    if (saut && (!sPlayerbotAIConfig.directeurTeleportCache || !sPlayerbotAIConfig.directeurEtapes))
    {
        Refuser(c, "teleport_eteint");
        return;
    }
    if (sautLointain && !sPlayerbotAIConfig.directeurTeleportLointain)
    {
        Refuser(c, "lointain_eteint");
        return;
    }
    Player* demandeur = nullptr;   // presence : le joueur qui a demande, lu ici (fil monde)
    if (presence)
    {
        Player* joueur = c.joueur ? ObjectAccessor::FindPlayerByLowGUID(c.joueur) : nullptr;
        demandeur = joueur;
        if (!joueur)
        {
            Refuser(c, "hors_ligne");
            return;
        }
        // Ancre bot (11/10, EvenementAncreBot) : les prefixes des presences d'un evenement (faille_orage.py) --
        // F- (vers la faille), T-/L- (sauts), Q- (finale, mission ou inscription) ; jamais un compagnon (C-) ni une
        // etape E-, et jamais le bot lui-meme pour ancre. LIMITE : le prefixe ne distingue pas une animation
        // (ecoute.py emet aussi T-/L-/Q-) ; c'est le service qui ne met un bot en `joueur` que pour un evenement.
        // Option eteinte : Humain(joueur), a l'identique (un bot appele pour lui-meme tombait deja `pas_humain`).
        bool const presenceDEvenement = c.action.size() > 2 && c.action[1] == '-' &&
                                        (c.action[0] == 'F' || c.action[0] == 'T' || c.action[0] == 'L' ||
                                         c.action[0] == 'Q');
        if (!Humain(joueur) && (!presenceDEvenement || joueur == bot || !AncreDEvenement(joueur)))
        {
            Refuser(c, "pas_humain");
            return;
        }
        if (bot->GetTeamId() != joueur->GetTeamId())
        {
            Refuser(c, "faction");
            return;
        }
        // Un chasseur amene pres d'un joueur High Risk (FFA) le chasserait, et
        // chasserait les autres arrivants (WildPvp.AllBotsAreTargets).
        if (MercenaryRewards::EstMercenaire(bot) && !SuspenduEnRpg(c.cible))
        {
            Refuser(c, "mercenaire");
            return;
        }
        if (joueur->GetMapId() != c.carte)
        {
            Refuser(c, "carte");
            return;
        }
        // Continuation : le bot porte deja une affectation ACTIVE du meme
        // proprietaire pour ce joueur (etape precedente du meme parcours).
        // Sans elle, les dernieres etapes, posees quand le bot approche,
        // seraient toutes refusees `a_vue`.
        bool continuation = false;
        bool presenceArrivee = false;
        bool dejaSaute = false;
        {
            std::shared_lock<std::shared_mutex> garde(verrouConsignes);
            auto it = affectations.find(c.cible);
            continuation = it != affectations.end() && it->second.a.finS > Maintenant() &&
                           it->second.joueur == c.joueur && it->second.origine == c.origine;
            presenceArrivee = continuation && it->second.a.presence && !it->second.a.etape &&
                              it->second.a.phase == CoaDirecteur::CoaDirPhase::Sejour;
            // Double saut : une affectation en cours (tout proprietaire) issue d'un saut.
            dejaSaute = it != affectations.end() && it->second.a.finS > Maintenant() && it->second.a.saut;
        }
        // Teleportation cachee : controles du DEPART, avant tout calcul de point.
        if (saut)
            if (char const* refus = RefusDepartSaut(c.cible, bot, joueur, ia, dejaSaute))
            {
                Refuser(c, refus);
                return;
            }
        // Compagnons : seulement un bot deja ARRIVE a sa presence (finale en
        // sejour, meme proprietaire, meme joueur). Il vient donc de l'anneau,
        // a pied : jamais un bot tire d'ailleurs qui surgirait pres du joueur.
        if (compagnon && !presenceArrivee)
        {
            Refuser(c, "sans_presence");
            return;
        }
        float const bx = bot->GetPositionX() - joueur->GetPositionX();
        float const by = bot->GetPositionY() - joueur->GetPositionY();
        // Lot F des evenements (06/10) : une INSCRIPTION (finale `Q-`, mission `evenement`, options
        // EvenementBots et Missions) vise aussi les bots DEJA presents : il n'apparait rien, il marche
        // de quelques yards vers l'evenement. Exemptee du refus `a_vue` ; les autres controles restent.
        bool const inscriptionEvenement = sPlayerbotAIConfig.directeurEvenementBots &&
                                          sPlayerbotAIConfig.directeurMissions && !compagnon && !saut &&
                                          c.texte == "evenement" && c.valeur == 0 && c.action.size() > 2 &&
                                          c.action[0] == 'Q' && c.action[1] == '-';
        if (!continuation && !inscriptionEvenement && bx * bx + by * by <= PRESENCE_A_VUE_YD * PRESENCE_A_VUE_YD)
        {
            Refuser(c, "a_vue");
            return;
        }
        float const cx = c.x - joueur->GetPositionX();
        float const cy = c.y - joueur->GetPositionY();
        float const centreJoueur = cx * cx + cy * cy;
        // Compagnons : le centre EST le joueur (sa position a l'emission ; il a
        // pu bouger depuis). Le suivi le recale ensuite chaque seconde.
        // Teleportation cachee : le centre a SAUT_JOUEUR_MIN_YD-SAUT_JOUEUR_MAX_YD
        // du joueur (chaque point essaye l'est aussi, ChercherPointCache).
        // Saut lointain : SAUT_LOINTAIN_MIN_YD-SAUT_LOINTAIN_MAX_YD, de meme.
        bool const horsBornes = compagnon
            ? centreJoueur > 1.0f   // le centre vient d'etre pris sur le joueur (plus haut)
            : sautLointain ? (centreJoueur < SAUT_LOINTAIN_MIN_YD * SAUT_LOINTAIN_MIN_YD ||
                              centreJoueur > SAUT_LOINTAIN_MAX_YD * SAUT_LOINTAIN_MAX_YD)
            : saut ? (centreJoueur < SAUT_JOUEUR_MIN_YD * SAUT_JOUEUR_MIN_YD ||
                      centreJoueur > SAUT_JOUEUR_MAX_YD * SAUT_JOUEUR_MAX_YD)
                   // Lot F, relecture (06/10) : une INSCRIPTION n'a pas de borne basse. Son centre EST
                   // l'evenement (la faille), ou se tient le joueur : a 40 yd au moins, le disque de 40 yd
                   // et la laisse de 70 yd tombaient a cote des invoques qui collent au joueur. Rien
                   // n'apparait (finale a pied, `saut` exclu) : la borne basse n'a rien a proteger ici.
                   : ((!inscriptionEvenement && centreJoueur < PRESENCE_CENTRE_MIN_YD * PRESENCE_CENTRE_MIN_YD) ||
                      centreJoueur > PRESENCE_CENTRE_MAX_YD * PRESENCE_CENTRE_MAX_YD);
        if (horsBornes)
        {
            Refuser(c, "bornes");
            return;
        }
    }
    // Lot D4c, verbe `chasser` (SPEC-directeur-D4.md § 6.3) : controles propres,
    // APRES ceux de toute affectation (le refus `occupe` reste le dernier, sous
    // le verrou). VOIE RETENUE POUR D4-0 (§ 10) : une ETAPE `E-` de chasseur est
    // admise -- elle reste en TRAJET, donc le chasseur y est compose en mode RPG
    // et new rpg le mene (laisse, trajet pacifique : il ne chasse pas en
    // route, c'est voulu) ; la FINALE (sans `E-`) passe en SEJOUR a l'arrivee,
    // et la fabrique lui rend sa chasse.
    bool const etapeChasse = chasseur && c.action.size() > 2 && c.action[0] == 'E' && c.action[1] == '-';
    if (chasseur)
    {
        uint32 const maintenantS = Maintenant();
        uint32 plafond = 0;
        bool primeActive = false;
        {
            std::shared_lock<std::shared_mutex> garde(verrouConsignes);
            auto it = c.joueur ? primes.find(c.joueur) : primes.end();
            if (it != primes.end() && it->second.finS > maintenantS)
            {
                primeActive = true;
                plafond = it->second.plafond;
            }
        }
        if (!primeActive)
        {
            Refuser(c, "sans_prime");
            return;
        }
        // EstMercenaire exige aussi IsRandomBot, et rend faux WildPvp eteint.
        if (!MercenaryRewards::EstMercenaire(bot))
        {
            Refuser(c, "pas_mercenaire");
            return;
        }
        // Sans strategie `pvp` (mode RPG de la suspension, AiFactory.cpp), il ne
        // pourrait pas engager ; et la suspension a son propre proprietaire.
        if (CoaDirecteur::MercenaireSuspendu(c.cible))
        {
            Refuser(c, "suspendu");
            return;
        }
        // Une etape `E-` sans Directeur.Etapes serait une affectation ordinaire :
        // le chasseur passerait en SEJOUR, donc en chasse, loin du joueur.
        if (etapeChasse && !sPlayerbotAIConfig.directeurEtapes)
        {
            Refuser(c, "etapes_eteintes");
            return;
        }
        Player* joueur = ObjectAccessor::FindPlayerByLowGUID(c.joueur);
        if (!joueur)
        {
            Refuser(c, "hors_ligne");
            return;
        }
        if (joueur->GetMapId() != c.carte)
        {
            Refuser(c, "carte");
            return;
        }
        demandeur = joueur;   // D4h : le joueur prime, pour ChercherPointCache
        // D4h (01/10) : un chasseur qui SAUTE passe les memes controles de depart
        // qu'une presence (ne pas disparaitre sous des yeux, pas en combat, pas
        // deux fois en 15 min...). Le saut est une etape : il reste EN TRAJET, donc
        // compose en mode RPG, jusqu'a l'etape suivante du service.
        if (saut)
        {
            bool dejaSaute = false;
            {
                std::shared_lock<std::shared_mutex> garde(verrouConsignes);
                auto it = affectations.find(c.cible);
                dejaSaute = it != affectations.end() && it->second.a.finS > maintenantS && it->second.a.saut;
            }
            if (char const* refus = RefusDepartSaut(c.cible, bot, joueur, ia, dejaSaute))
            {
                Refuser(c, refus);
                return;
            }
        }
        // Continuation : le bot porte deja une affectation `chasser` ACTIVE du
        // meme proprietaire pour ce joueur (etape precedente, redisposition).
        bool continuation = false;
        uint32 autres = 0;
        {
            std::shared_lock<std::shared_mutex> garde(verrouConsignes);
            auto it = affectations.find(c.cible);
            continuation = it != affectations.end() && it->second.a.finS > maintenantS && it->second.a.chasseur &&
                           it->second.joueur == c.joueur && MemeProprietaire(it->second, c);
            for (auto const& [guid, f] : affectations)
                if (guid != c.cible && f.a.chasseur && f.joueur == c.joueur && f.a.finS > maintenantS)
                    ++autres;
        }
        // Premiere pose : un mercenaire DE LA CHASSE (classe CoA, composition
        // de mercenaire). Un mercenaire de classe vanille n'a pas de chasse
        // (AiFactory.cpp) : arrive, rien ne lui ferait lire sa proie.
        if (!continuation && !chasse)
        {
            Refuser(c, "sans_moteur");
            return;
        }
        if (!continuation && autres >= plafond)
        {
            Refuser(c, "plafond");
            return;
        }
        // Revue D4c (M1) : une REDISPOSITION d'un chasseur deja arrive (SEJOUR)
        // le recomposerait en mode RPG, donc couperait sa traque. Refusee tant
        // qu'il est en combat ou assez pres pour lire son prime ; le service
        // reessaie plus tard.
        if (continuation)
        {
            bool enSejour = false;
            {
                std::shared_lock<std::shared_mutex> garde(verrouConsignes);
                auto it = affectations.find(c.cible);
                enSejour = it != affectations.end() && it->second.a.phase == CoaDirecteur::CoaDirPhase::Sejour;
            }
            float const px = bot->GetPositionX() - joueur->GetPositionX();
            float const py = bot->GetPositionY() - joueur->GetPositionY();
            float const detection = float(sPlayerbotAIConfig.directeurPrimeDetectionYd);
            if (enSejour && (bot->IsInCombat() || px * px + py * py <= detection * detection))
            {
                Refuser(c, "en_chasse");
                return;
            }
        }
        float const bx = bot->GetPositionX() - joueur->GetPositionX();
        float const by = bot->GetPositionY() - joueur->GetPositionY();
        if (!continuation && bx * bx + by * by <= PRESENCE_A_VUE_YD * PRESENCE_A_VUE_YD)
        {
            Refuser(c, "a_vue");
            return;
        }
        // Centre : la finale a CHASSEUR_CENTRE_MIN_YD-CHASSEUR_CENTRE_MAX_YD du
        // joueur (au-dela de la visibilite de 100 yd : il le voit arriver a
        // pied) ; une etape jamais plus pres que le minimum.
        float const ex = c.x - joueur->GetPositionX();
        float const ey = c.y - joueur->GetPositionY();
        float const carre = ex * ex + ey * ey;
        // D4h : un saut vise les bornes du saut (cache 150-260, lointain 250-350 yd).
        bool const horsBornesChasse = sautLointain
            ? (carre < SAUT_LOINTAIN_MIN_YD * SAUT_LOINTAIN_MIN_YD || carre > SAUT_LOINTAIN_MAX_YD * SAUT_LOINTAIN_MAX_YD)
            : saut ? (carre < SAUT_JOUEUR_MIN_YD * SAUT_JOUEUR_MIN_YD || carre > SAUT_JOUEUR_MAX_YD * SAUT_JOUEUR_MAX_YD)
                   : (carre < CHASSEUR_CENTRE_MIN_YD * CHASSEUR_CENTRE_MIN_YD ||
                      (!etapeChasse && carre > CHASSEUR_CENTRE_MAX_YD * CHASSEUR_CENTRE_MAX_YD));
        if (horsBornesChasse)
        {
            Refuser(c, "bornes");
            return;
        }
    }
    float const dx = bot->GetPositionX() - c.x;
    float const dy = bot->GetPositionY() - c.y;
    // Teleportation cachee : le bot vient de loin, c'est tout l'objet du saut.
    if (!saut && dx * dx + dy * dy > AFFECTATION_DISTANCE_MAX * AFFECTATION_DISTANCE_MAX)
    {
        Refuser(c, "trop_loin");
        return;
    }
    Map* map = bot->GetMap();
    // Sol du CENTRE, laisse de hauteur a la pose (P-145, revue du 26/09). Le
    // centre est le repli de TirerDansLeDisque, la reference de sa fenetre
    // (a.z +- 6) et une cible admise du saut d'enlisement dirige : un centre
    // pose sur le terrain brut SOUS une ville les fausse tous les trois.
    //  - z connu : depart a z + 2 (Z_OFFSET_FIND_HEIGHT, la marge du coeur : un
    //    rayon parti EXACTEMENT du plancher peut le manquer), puis z + 6 si le
    //    sol sort de la fenetre ; dans la fenetre, garde sans autre sonde (un
    //    centre sous un arbre, un auvent : c'est le point demande).
    //  - z inconnu (Z_INCONNU, destinations de quete) : SolSec part du terrain
    //    brut ; tout ce qui est au-dessus est une structure.
    //  - hors fenetre ou z inconnu : REFUS `sol_sous_structure` si une surface
    //    existe au-dessus (ToitAuDessus). On ne releve PAS le centre sur cette
    //    surface : rien ne dit qu'elle est un plancher joignable (toit, rempart,
    //    branche) et le verifier demanderait un chemin mmap ici, au fil monde.
    //    Le service choisit une autre destination ; le refus se voit au compte
    //    `refus_jeu` de l'emetteur.
    // LIMITE CONNUE (non corrigee) : une ville SOUS le terrain (Undercity,
    // Ironforge) n'est jamais visable -- SolSec part toujours de max(z, terrain
    // brut), donc au-dessus d'elle, et rend la surface (ou la refuse).
    float sol = 0.0f;
    bool const zConnu = std::isfinite(c.z) && c.z > POSE_Z_INCONNU_MAX;
    if (!map || map->GetId() != c.carte ||
        !SolSec(map, c.x, c.y, zConnu ? c.z + Z_OFFSET_FIND_HEIGHT : c.z, sol))
    {
        Refuser(c, "sol");
        return;
    }
    if (zConnu && std::fabs(sol - c.z) > TIRAGE_ECART_Z)
    {
        float solHaut = 0.0f;
        if (SolSec(map, c.x, c.y, c.z + TIRAGE_ECART_Z, solHaut) && std::fabs(solHaut - c.z) <= TIRAGE_ECART_Z)
            sol = solHaut;
    }
    if ((!zConnu || std::fabs(sol - c.z) > TIRAGE_ECART_Z) && ToitAuDessus(map, c.x, c.y, sol, zConnu ? c.z : sol))
    {
        Refuser(c, "sol_sous_structure");
        return;
    }
    // Mercenaire EN CHASSE (pas en mode RPG) : un centre sans PvP ferait
    // repasser la chasse en REPLI, qui en ressortirait (CoaChasse.h, REPLI).
    bool const mercenaireRpg = SuspenduEnRpg(c.cible);
    // Lot D4c : la FINALE d'un chasseur n'est jamais posee en lieu sans PvP,
    // meme quand il marche en mode RPG (`chasse` faux) : arrive, il y serait
    // inerte, et son prime y est un refuge (SPEC § 12, « Refuge »).
    // Revue D4c (m2, m7) : pour `chasser`, la FINALE seule (une etape traverse) ;
    // pour tout autre verbe, aussi un mercenaire compose en mode RPG par une
    // affectation `chasser` en trajet (il retrouvera sa chasse a la fin de celle-ci).
    bool const enChasseApres = chasse || (!chasseur && ChasseurEnTrajet(c.cible));
    // Reserve de chasseurs (02/10) : lu ici, fil monde, et fige dans l'affectation.
    // Un chasseur de reserve garde sa chasse EN TRAJET (ChasseurEnTrajet) : une
    // ETAPE `E-` ou un SAUT `T-`/`L-` dont le centre est en lieu sans PvP le
    // ferait passer en REPLI, qui l'en ressortirait aussitot : refuses comme une
    // finale (`lieu_sans_pvp`, test sur le centre).
    bool const reserveChasseur = chasseur && EstDeLaReserve(bot);
    if (chasseur ? (!(etapeChasse || saut) || reserveChasseur) : (enChasseApres && !mercenaireRpg))
    {
        uint32 zone = 0, aire = 0;
        map->GetZoneAndAreaId(PHASEMASK_NORMAL, zone, aire, c.x, c.y, sol);
        if (sPlayerbotAIConfig.CoaLieuSansPvp(zone, aire))
        {
            Refuser(c, "lieu_sans_pvp");
            return;
        }
    }
    WorldPosition centre(c.carte, c.x, c.y, sol);
    // `peupler` exempte le joueur qui demande, et lui seul (§ 4.2) ; `chasser`
    // exempte le joueur prime, et lui seul (SPEC-directeur-D4.md § 6.3).
    // 18/10 (convoi entre bots, revue B1) : l'ATTACHE de l'ancre bot d'un convoi (inscription `Q-` dont le `joueur` est
    // un autre bot d'evenement) est exemptee du controle des humains si l'ancre est DEJA a moins de
    // AFFECTATION_HUMAIN_YD du centre et sans saut : elle ne fait que marcher a cote du convoi, rien n'apparait a l'ecran.
    // Sans cela, un humain venu escorter le convoi d'un bot ferait lacher l'ancre, puis abandonner le convoi.
    // Portee (revue du 18/10, m1, assumee) : TOUTE presence `Q-` sans saut d'un porteur ancre bot -- inscriptions des
    // failles, nuits et rares entre bots comprises : un bot deja a moins de 200 yd, qui ne fait que marcher, rejoint
    // l'evenement meme si un humain est la (conception de l'autonomie : « un humain qui arrive peut participer »).
    bool attacheSurPlace = false;
    if (presence && !saut && c.action.size() > 2 && c.action[0] == 'Q' && c.action[1] == '-' && bot &&
        bot->GetMapId() == c.carte &&
        centre.sqDistance(WorldPosition(bot)) < AFFECTATION_HUMAIN_YD * AFFECTATION_HUMAIN_YD)
    {
        Player* porteur = c.joueur ? ObjectAccessor::FindPlayerByLowGUID(c.joueur) : nullptr;
        attacheSurPlace = porteur && !Humain(porteur) && AncreDEvenement(porteur);
    }
    if (!attacheSurPlace &&
        ((presence || chasseur) ? HumainProcheSauf(centre, c.carte, AFFECTATION_HUMAIN_YD, c.joueur)
                                : ia->HasPlayerNearby(&centre, AFFECTATION_HUMAIN_YD)))
    {
        Refuser(c, "humain_proche");
        return;
    }

    // Teleportation cachee : le point, puis le SAUT, AVANT l'ecriture. Le fil
    // monde execute les commandes une a une et il est le seul ecrivain des
    // affectations : le refus `occupe` teste ici sous verrou partage est donc
    // celui que la section exclusive plus bas rendrait. Rien n'est ecrit si le
    // saut ne part pas.
    PointSaut point;
    uint32 sautMs = 0;
    if (saut)
    {
        {
            std::shared_lock<std::shared_mutex> garde(verrouConsignes);
            auto it = affectations.find(c.cible);
            if (it != affectations.end() && it->second.a.finS > Maintenant() && c.origine != "manuel" &&
                !MemeProprietaire(it->second, c))
            {
                AffectationInterne const ancienne = it->second;
                garde.unlock();
                CoaDirecteur::Compter(CoaDirecteur::COA_DIR_OCCUPEES);
                LOG_INFO("playerbots", "coa directeur occupe: id={} guid={} origine={} joueur={} par_id={}"
                         " par_origine={} par_joueur={}", c.id, c.cible, c.origine, c.joueur, ancienne.a.id,
                         ancienne.origine, ancienne.joueur);
                Refuser(c, "occupe");
                return;
            }
        }
        point = ChercherPointCache(map, demandeur, c.x, c.y, sol, c.rayon, sautLointain);
        if (!point.trouve)
        {
            // `sans_point` pour un saut lointain : aucun sol sur a 250-350 yd (le
            // service compte les `sans_cache` a part, pour couper le saut cache).
            char const* refus = sautLointain ? "sans_point" : "sans_cache";
            LOG_INFO("playerbots", "coa directeur teleport_cache: id={} guid={} joueur={} refus={} essais={} mode={}",
                     c.id, c.cible, c.joueur, refus, point.essais, sautLointain ? "lointain" : "cache");
            Refuser(c, refus);
            return;
        }
        float const departX = bot->GetPositionX(), departY = bot->GetPositionY();
        float const vx = departX - demandeur->GetPositionX(), vy = departY - demandeur->GetPositionY();
        float const px = point.x - demandeur->GetPositionX(), py = point.y - demandeur->GetPositionY();
        // (Pas de MotionMaster::Clear ici : PlayerbotAI::HandleTeleportAck le
        // fait a l'arrivee. Un saut refuse par TeleportTo a deja retire les
        // auras rompues par une teleportation -- nourriture, boisson : assume.)
        if (!CoaDirecteur::Sauter(bot, WorldLocation(c.carte, point.x, point.y, point.z + 0.5f, bot->GetOrientation())))
        {
            LOG_INFO("playerbots", "coa directeur teleport_cache: id={} guid={} joueur={} refus=saut_echoue", c.id,
                     c.cible, c.joueur);
            Refuser(c, "saut_echoue");
            return;
        }
        sautMs = getMSTime();
        if (sautsFaits.size() > 256)
            for (auto it = sautsFaits.begin(); it != sautsFaits.end();)
                it = Maintenant() >= it->second + SAUT_REPOS_S ? sautsFaits.erase(it) : std::next(it);
        sautsFaits[c.cible] = Maintenant();
        CoaDirecteur::Compter(sautLointain ? CoaDirecteur::COA_DIR_TELEPORTS_LOINTAINS
                                           : CoaDirecteur::COA_DIR_TELEPORTS_CACHES);
        LOG_INFO("playerbots",
                 "coa directeur teleport_cache: id={} guid={} joueur={} de_x={:.1f} de_y={:.1f} a_x={:.1f} a_y={:.1f}"
                 " a_z={:.1f} distance_joueur={:.0f} depart_joueur={:.0f} dans_le_champ={} essais={} mode={}",
                 c.id, c.cible, c.joueur, departX, departY, point.x, point.y, point.z, std::sqrt(px * px + py * py),
                 std::sqrt(vx * vx + vy * vy), point.dansLeChamp ? 1 : 0, point.essais,
                 sautLointain ? "lointain" : "cache");
    }

    uint32 const maintenant = Maintenant();
    uint32 fin = maintenant + c.dureeS;
    // Mercenaire suspendu en mode RPG : l'affectation ne survit pas a la
    // suspension, sa fin est bornee par celle-ci (§ 6.3).
    if (mercenaireRpg)
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        auto it = suspensions.find(c.cible);
        if (it != suspensions.end())
            fin = std::min(fin, it->second.finS);
    }

    AffectationInterne nouvelle{};
    nouvelle.a.id = c.id;
    nouvelle.a.carte = c.carte;
    nouvelle.a.x = c.x;
    nouvelle.a.y = c.y;
    nouvelle.a.z = sol;
    nouvelle.a.rayon = c.rayon;
    nouvelle.a.finS = fin;
    nouvelle.a.phase = CoaDirecteur::CoaDirPhase::Trajet;   // l'arrivee est constatee par le suivi
    // Etape d'itineraire : colonne `action` en `E-...` (l'emetteur n'admet que
    // `[A-Z]-\d+`), et seulement si l'option est allumee ; sinon, affectation
    // ordinaire. Figee a la pose, comme le reste.
    nouvelle.a.etape = sPlayerbotAIConfig.directeurEtapes && c.action.size() > 2 && c.action[0] == 'E' &&
                       c.action[1] == '-';
    // Teleportation cachee : une etape (Directeur.Etapes exigee plus haut).
    nouvelle.a.etape = nouvelle.a.etape || saut;
    nouvelle.a.saut = saut;
    nouvelle.a.sautMs = sautMs;
    nouvelle.a.presence = presence;
    nouvelle.a.compagnon = compagnon;
    nouvelle.a.chasseur = chasseur;
    // Reserve de chasseurs (02/10) : lu ici, fil monde, et fige.
    nouvelle.a.reserve = reserveChasseur;
    if (compagnon)
        nouvelle.a.rayon = CoaDirecteur::COMPAGNON_MAX_YD;   // arrivee = dans la bande
    // Missions (30/09) : la finale d'une presence seulement (`Q-`).
    bool const finalePresence = presence && !compagnon && !saut && !nouvelle.a.etape && c.action.size() > 2 &&
                                c.action[0] == 'Q' && c.action[1] == '-';
    MissionLue const mission = LireMission(c, bot, finalePresence);
    nouvelle.a.mission = mission.type;
    nouvelle.a.missionParam = mission.param;
    if (mission.type == CoaDirecteur::CoaDirMission::Bande)
        nouvelle.a.rayon = CoaDirecteur::BANDE_MAX_YD;       // arrivee = aupres du chef
    nouvelle.verbe = c.verbe;
    nouvelle.origine = c.origine;
    nouvelle.guid = c.cible;
    nouvelle.joueur = c.joueur;
    nouvelle.poseS = maintenant;
    nouvelle.niveau = bot->GetLevel();
    nouvelle.mercenaireRpg = mercenaireRpg;
    nouvelle.ia = nullptr;
    nouvelle.quetesAcceptees0 = 0;
    nouvelle.quetesRendues0 = 0;

    bool remplace = false;
    bool occupe = false;
    AffectationInterne ancienne{};
    {
        std::unique_lock<std::shared_mutex> garde(verrouConsignes);
        auto it = affectations.find(c.cible);
        // REFUS `occupe` (lot D3, SPEC-directeur-D3.md § 3.6, decision n° 15),
        // APRES tous les autres controles (il ne masque ni un `hors_ligne` ni un
        // `bornes`) et DANS la meme section que l'ecriture : le fil monde
        // execute les commandes une a une, le test voit donc toute pose
        // anterieure, meme lue dans la meme passe. Le premier proprietaire
        // garde le bot ; le meme proprietaire continue (remplacement, comme en
        // D2) ; `manuel` (l'operateur) remplace toujours. Une affectation echue
        // mais pas encore levee ne compte plus.
        if (it != affectations.end() && it->second.a.finS > maintenant && c.origine != "manuel" &&
            !MemeProprietaire(it->second, c))
        {
            occupe = true;
            ancienne = it->second;
        }
        else
        {
            if (it != affectations.end())
            {
                remplace = true;
                ancienne = it->second;
            }
            // Une affectation par bot, la derniere remplace entierement (§ 6.3).
            affectations[c.cible] = nouvelle;
            MajCompteursAffectations();
            // Compagnons : le pacte, dans la MEME section (un lecteur ne voit
            // jamais le compagnon sans son pacte). Pose par TOUTE presence
            // (etape, finale, `C-`) option allumee : un bot amene vers un joueur
            // ne le convoite pas en route, ni dans son disque de l'anneau
            // (80-250 yd, rayon 50 : il peut errer a 30 yd de lui). Un pacte
            // anterieur de ce bot (autre joueur) est remplace.
            if (presence && sPlayerbotAIConfig.directeurCompagnons)
            {
                pactes[c.cible] = PacteCompagnon{c.id, c.joueur, fin + PACTE_MARGE_S};
                nbPactes.store(static_cast<uint32>(pactes.size()), std::memory_order_relaxed);
            }
        }
    }
    if (occupe)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_OCCUPEES);
        LOG_INFO("playerbots", "coa directeur occupe: id={} guid={} origine={} joueur={} par_id={} par_origine={}"
                 " par_joueur={}", c.id, c.cible, c.origine, c.joueur, ancienne.a.id, ancienne.origine, ancienne.joueur);
        Refuser(c, "occupe");
        return;
    }
    // Lot D4c : la pose d'un chasseur (toujours EN TRAJET) le fait composer en
    // mode RPG, sauf s'il l'etait deja (continuation d'un chasseur en trajet).
    // AVANT FinirAffectation de l'ancienne : celle-ci voit alors le nouveau
    // chasseur en trajet et ne demande pas le retour a la chasse.
    if (chasseur && !(remplace && ancienne.a.chasseur && ancienne.a.phase == CoaDirecteur::CoaDirPhase::Trajet))
        BasculerComposition(c.cible);
    if (remplace)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REMPLACEES);
        std::string const par = Acore::StringFormat("par_{}", c.id);
        FinirAffectation(ancienne, "remplace", par.c_str(), "remplace");
    }

    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots",
             "coa directeur : affectation posee id={} guid={} verbe={} carte={} x={:.1f} y={:.1f} rayon={:.0f}"
             " fin_s={} distance={:.0f} mercenaire_rpg={} etape={} origine={}",
             c.id, c.cible, NomVerbe(c.verbe), c.carte, c.x, c.y, nouvelle.a.rayon, fin, std::sqrt(dx * dx + dy * dy),
             mercenaireRpg ? 1 : 0, nouvelle.a.etape ? 1 : 0, c.origine);
    if (presence)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PEUPLEMENTS);
        LOG_INFO("playerbots", "coa directeur presence: id={} guid={} joueur={} etape={} remplace={}", c.id, c.cible,
                 c.joueur, nouvelle.a.etape ? 1 : 0, remplace ? 1 : 0);
    }
    if (chasseur)
    {
        // Revue M4 : une affectation `chasser` met fin au repos d'un `P-0`.
        if (LeverRepos(c.cible))
            LOG_INFO("playerbots", "coa directeur reserve: id={} guid={} joueur={} motif=repos_fini raison=chasser", c.id,
                     c.cible, c.joueur);
        LeverImmobile(c.cible);
        // Retraite (03/10/2026) : un chasseur de nouveau designe ne se retire plus ; un
        // chasseur de RESERVE est inscrit sur sa prime (il se retirera a sa fin).
        LeverRetraite(c.cible, "rappele");
        if (nouvelle.a.reserve)
        {
            std::unique_lock<std::shared_mutex> garde(verrouConsignes);
            auto it = primes.find(c.joueur);
            if (it != primes.end())
            {
                std::vector<uint32>& servis = it->second.servis;
                if (std::find(servis.begin(), servis.end(), c.cible) == servis.end() && servis.size() < PRIME_SERVIS_MAX)
                    servis.push_back(c.cible);
            }
        }
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_CHASSEURS);
        LOG_INFO("playerbots", "coa directeur chasse: bot={} joueur={} id={} motif=pose etape={} remplace={} reserve={}",
                 c.cible, c.joueur, c.id, nouvelle.a.etape ? 1 : 0, remplace ? 1 : 0, nouvelle.a.reserve ? 1 : 0);
    }
    if (mission.etat)
    {
        CoaDirecteur::Compter(mission.type == CoaDirecteur::CoaDirMission::Aucune ? CoaDirecteur::COA_DIR_MISSIONS_IGNOREES
                                                                                  : CoaDirecteur::COA_DIR_MISSIONS);
        if (std::strcmp(mission.etat, "degradee") == 0)
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_MISSIONS_DEGRADEES);
        LOG_INFO("playerbots",
                 "coa directeur mission: id={} guid={} joueur={} type={} param={} demande={} etat={} raison={}"
                 " rayon={:.0f}",
                 c.id, c.cible, c.joueur, NomMission(mission.type), mission.param, c.texte, mission.etat,
                 mission.raison, nouvelle.a.rayon);
    }
    if (compagnon)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_COMPAGNONS);
        LOG_INFO("playerbots", "coa directeur compagnon: id={} guid={} joueur={} fin_s={} pacte_fin_s={}", c.id,
                 c.cible, c.joueur, fin, fin + PACTE_MARGE_S);
    }
    Accuser(c, "execute", nullptr);
}

// Controles avant tout verbe, dans l'ordre de la SPEC § 4.4.
void Recevoir(CoaDirCommande const& c)
{
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_RECUES);

    // 1. echeance (calculee par MySQL a la lecture)
    if (c.expiree)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXPIREES);
        Accuser(c, "expire", nullptr);
        return;
    }
    // 2. epoque : une commande emise pour un monde precedent n'est jamais
    // executee ; c'est au service de la reemettre.
    if (c.epoqueMonde != EpoqueMonde())
    {
        Refuser(c, "epoque");
        return;
    }
    // `lever` ne passe que 1 et 2 : sa cible est un id de commande, et une
    // levee ne doit jamais etre refusee faute de jeton.
    if (c.verbe == CoaDirVerbe::Lever)
    {
        ExecuterLever(c);
        return;
    }
    // Lot D des evenements (relecture du 05/10) : `retirer` est du MENAGE, comme `lever` -- il ne pose
    // rien et ne fait que retirer des invoques deja la. Il passe donc outre l'interrupteur du verbe (un
    // `evenement` retire de Directeur.Verbes ne doit pas laisser des monstres jusqu'a 3600 s), l'arret
    // et le budget ; il garde l'implementation (3) et les colonnes (3 quinquies).
    bool const menage = c.verbe == CoaDirVerbe::Evenement && (c.texte == "retirer" || c.texte == "retirer:immediat");
    // 3. verbe allume ET porte par ce binaire
    if (c.verbe == CoaDirVerbe::Inconnu || !VerbeImplemente(c.verbe) || (!menage && !VerbeAllume(c.verbe)))
    {
        Refuser(c, "verbe_eteint");
        return;
    }
    // 3 bis (lot D3). Verbe D3 sans les colonnes D3 (migration non appliquee au
    // demarrage, P-131) : son texte, son canal, sa valeur n'ont pas ete lus.
    if (VerbeD3(c.verbe) && (!source || !source->ColonnesD3()))
    {
        Refuser(c, "colonnes_d3");
        return;
    }
    // 3 ter (lot D4c, SPEC-directeur-D4.md § 3.2). Verbe D4 sans l'ENUM D4 au
    // demarrage (migration non appliquee, P-131) : il ne devrait pas exister en
    // base, et `valeur` (plafond de prime) pourrait ne pas avoir ete lue.
    if (VerbeD4(c.verbe) && (!source || !source->ColonnesD4() || !source->ColonnesD3()))
    {
        Refuser(c, "colonnes_d4");
        return;
    }
    // 3 quater (lot R3, 03/10/2026). `recompenser` sans l'ENUM R3 au demarrage : il
    // ne devrait pas exister en base, et `valeur`/`texte` pourraient ne pas avoir ete lus.
    if (VerbeR(c.verbe) && (!source || !source->ColonnesRecompense() || !source->ColonnesD3()))
    {
        Refuser(c, "colonnes_recompense");
        return;
    }
    // 3 quinquies (lot C des evenements, 04/10/2026). `evenement` sans la valeur d'ENUM au
    // demarrage : il ne devrait pas exister en base, `texte`/`valeur` pourraient manquer.
    if (VerbeEv(c.verbe) && (!source || !source->ColonnesEvenement() || !source->ColonnesD3()))
    {
        Refuser(c, "colonnes_evenement");
        return;
    }
    // 3 sexies (lot D5, 15/10/2026). `attirer` / `soigner` sans les valeurs d'ENUM au demarrage.
    if (VerbeImprevu(c.verbe) && (!source || !source->ColonnesImprevu() || !source->ColonnesD3()))
    {
        Refuser(c, "colonnes_imprevu");
        return;
    }
    // 3 septies (paix de zone, 19/10/2026). `paix` sans la valeur d'ENUM au demarrage : elle ne devrait pas exister
    // en base (migration 2026_10_19_00 non appliquee, P-131).
    if (VerbePaix(c.verbe) && (!source || !source->ColonnesPaix()))
    {
        Refuser(c, "colonnes_paix");
        return;
    }
    // 4. arret, avant le budget : un arret ne consomme pas de jeton. `repondre`
    // en est exempte (decision n° 4 du 26/09) : une ligne qui dit « vous etes a
    // l'arret » n'est pas une mise en scene. Le ciel et la meteo visent le
    // joueur lui-meme (cible), comme la treve.
    // Lot D4c : `prime` vise le joueur lui-meme (sa cible) ; `chasser` est
    // couvert par le test de `joueur` (le prime).
    bool const visePersonne = c.verbe == CoaDirVerbe::Treve || c.verbe == CoaDirVerbe::HeureDuJour ||
                              c.verbe == CoaDirVerbe::Meteo || c.verbe == CoaDirVerbe::Prime ||
                              c.verbe == CoaDirVerbe::Evenement;   // lot C : vise le joueur lui-meme
    // Lot R3 : `recompenser` en est exempte aussi : une prime GAGNEE se paie, arret ou non.
    if (c.verbe != CoaDirVerbe::Repondre && c.verbe != CoaDirVerbe::Recompenser && !menage &&
        ((c.joueur && CoaDirecteur::EstArrete(c.joueur)) || (visePersonne && CoaDirecteur::EstArrete(c.cible))))
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REFUS_ARRET);
        Refuser(c, "arret");
        return;
    }
    // Arene1v1 (10/10/2026, Ai/Coa/CoaArene1v1.cpp) : un gladiateur engage (en file, invite,
    // en arene) n'est ni affecte, ni prepare, ni deplace. `c.cible` d'un verbe qui vise un
    // joueur est un humain, jamais un gladiateur : le test ne l'ecarte pas.
    // Paix de zone : sa cible est un id de ZONE, pas un guid (une zone 215 n'est pas le personnage 215).
    if (!menage && c.verbe != CoaDirVerbe::Paix && c.cible && CoaArene1v1::GladiateurEngage(c.cible))
    {
        Refuser(c, "arene");
        return;
    }
    // 5. budget (`retirer` du lot D en est exempt : `menage`, plus haut). Revue 16/10 : `recompenser` puise dans son
    // seau propre (PrendreJetonDe), pas dans le seau commun : une part gagnee n'attend plus la fin d'un gros evenement.
    bool const recompense = c.verbe == CoaDirVerbe::Recompenser;
    if (!menage && !(recompense ? PrendreJetonDe(jetonsRecompense, jetonsRecompenseMs) : PrendreJeton()))
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REFUS_BUDGET);
        Refuser(c, "budget");
        return;
    }
    // 6 a 9 : propres au verbe.
    switch (c.verbe)
    {
        case CoaDirVerbe::Treve:
            ExecuterTreve(c);
            break;
        case CoaDirVerbe::SuspendreMercenaire:
            ExecuterSuspension(c);
            break;
        case CoaDirVerbe::Reaffecter:
        case CoaDirVerbe::Alleger:
        case CoaDirVerbe::Peupler:
            ExecuterAffectation(c);
            break;
        case CoaDirVerbe::Chasser:
            // Reserve de chasseurs (02/10) : `P-<arc>` est une PREPARATION (file,
            // une par tick), pas une affectation.
            if (EstPreparation(c))
                MettreEnFilePreparation(c);
            else
                ExecuterAffectation(c);
            break;
        case CoaDirVerbe::Prime:
            ExecuterPrime(c);
            break;
        case CoaDirVerbe::Repondre:
            ExecuterRepondre(c);
            break;
        case CoaDirVerbe::FaireParler:
            ExecuterParole(c);
            break;
        case CoaDirVerbe::HeureDuJour:
            ExecuterCiel(c);
            break;
        case CoaDirVerbe::Meteo:
            ExecuterMeteo(c);
            break;
        case CoaDirVerbe::Recompenser:
            ExecuterRecompense(c);
            break;
        case CoaDirVerbe::Evenement:
            ExecuterEvenement(c);
            break;
        case CoaDirVerbe::Attirer:
            ExecuterAttirer(c);
            break;
        case CoaDirVerbe::Soigner:
            ExecuterSoigner(c);
            break;
        case CoaDirVerbe::Paix:
            ExecuterPaix(c);
            break;
        default:
            Refuser(c, "verbe_eteint");   // inatteignable : VerbeImplemente l'a filtre
            break;
    }
}

void Bilan()
{
    using namespace CoaDirecteur;
    uint32 enRpg = 0;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto const& [guid, s] : suspensions)
            if (s.rpg)
                ++enRpg;
    }
    // `tirages_refuses_hauteur` compte les essais refuses par la laisse de
    // hauteur de TirerDansLeDisque pour ses DEUX appelants : la laisse new rpg
    // (NewRpgAction.cpp) ET la chasse (CoaChasse.h, poste affecte). La part rpg
    // se lit dans les lignes `coa directeur laisse:` (champ refus_hauteur) ; la
    // chasse n'a pas de ligne propre : chasse = bilan - somme des laisse:.
    LOG_INFO("playerbots",
             "coa directeur bus: epoque={} recues={} executees={} refusees={} expirees={} levees={} remplacees={}"
             " treves={} affectations={} suspensions={} en_rpg={} ecartes_treve={} ecartes_suspension={}"
             " teleports_suspendus={} laisses_rpg={} trajets_arrives={} trajets_abandonnes={}"
             " quetes_trajet_evitees={} enlisements_diriges={} postes_affectes={} reconstructions={}"
             " reconstructions_differees={} refus_arret={} refus_budget={} divergences={}"
             " affectations_levees_mort={} affectations_levees_hors_ligne={}"
             " affectations_levees_fin_suspension={} enlisements_retenus={} echues={}"
             " ecartes_trajet={} morts_en_trajet={} releves_ressuscites={} releves_loin_du_corps={}"
             " etapes_arrivees={} en_releve={} tirages_refuses_hauteur={} rpg_remis_idle={} trajets_leves={}"
             " reponses={} paroles_livrees={} paroles_perdues={} peuplements={} arrivees_presence={}"
             " enlisements_presence={} ciels={} meteos={} reaffirmations_meteo={} occupees={} laisses_presence={}"
             " paroles_en_file={} ciels_actifs={} meteos_actives={} compagnons={} approches={} ecartes_pacte={}"
             " pactes={} teleports_caches={} sauts_remis_idle={} primes={} chasseurs={} primes_tombees={}"
             " primes_echues={} primes_levees={} proies_imposees={} traques_prime={} meute_permise={}"
             " morts_tracees={} morts_perdues={} primes_actives={} chasseurs_actifs={} teleports_lointains={}"
             " paroles_hors_vue={} missions={} missions_ignorees={} missions_degradees={} patrouilles={}"
             " bandes_recalees={} bandes_rejointes={} ecartes_pacte_bots={} morts_entre_pactes={}{}",
             EpoqueMonde(), Lire(COA_DIR_RECUES), Lire(COA_DIR_EXECUTEES), Lire(COA_DIR_REFUSEES),
             Lire(COA_DIR_EXPIREES), Lire(COA_DIR_LEVEES), Lire(COA_DIR_REMPLACEES),
             nbTreves.load(std::memory_order_relaxed),
             nbAffectations.load(std::memory_order_relaxed),
             nbSuspensions.load(std::memory_order_relaxed), enRpg,
             Lire(COA_DIR_ECARTE_TREVE), Lire(COA_DIR_ECARTE_SUSPENSION), Lire(COA_DIR_TELEPORTS_SUSPENDUS),
             Lire(COA_DIR_LAISSES_RPG), Lire(COA_DIR_TRAJETS_ARRIVES), Lire(COA_DIR_TRAJETS_ABANDONNES),
             Lire(COA_DIR_QUETES_TRAJET_EVITEES), Lire(COA_DIR_ENLISEMENTS_DIRIGES), Lire(COA_DIR_POSTES_AFFECTES),
             Lire(COA_DIR_RECONSTRUCTIONS), Lire(COA_DIR_RECONSTRUCTIONS_DIFFEREES), Lire(COA_DIR_REFUS_ARRET),
             Lire(COA_DIR_REFUS_BUDGET), Lire(COA_DIR_DIVERGENCES), Lire(COA_DIR_AFFECTATIONS_LEVEES_MORT),
             Lire(COA_DIR_AFFECTATIONS_LEVEES_HORS_LIGNE), Lire(COA_DIR_AFFECTATIONS_LEVEES_FIN_SUSPENSION),
             Lire(COA_DIR_ENLISEMENTS_RETENUS), Lire(COA_DIR_ECHUES), Lire(COA_DIR_ECARTES_TRAJET),
             Lire(COA_DIR_MORTS_EN_TRAJET), Lire(COA_DIR_RELEVES_RESSUSCITES), Lire(COA_DIR_RELEVES_LOIN_DU_CORPS),
             Lire(COA_DIR_ETAPES_ARRIVEES), nbMortsEnTrajet.load(std::memory_order_relaxed),
             Lire(COA_DIR_TIRAGES_REFUSES_HAUTEUR), Lire(COA_DIR_RPG_REMIS_IDLE),
             nbTrajetsLeves.load(std::memory_order_relaxed), Lire(COA_DIR_REPONSES), Lire(COA_DIR_PAROLES_LIVREES),
             Lire(COA_DIR_PAROLES_PERDUES), Lire(COA_DIR_PEUPLEMENTS), Lire(COA_DIR_ARRIVEES_PRESENCE),
             Lire(COA_DIR_ENLISEMENTS_PRESENCE), Lire(COA_DIR_CIELS), Lire(COA_DIR_METEOS),
             Lire(COA_DIR_REAFFIRMATIONS_METEO), Lire(COA_DIR_OCCUPEES), Lire(COA_DIR_LAISSES_PRESENCE),
             paroles.size(), ciels.size(), meteos.size(), Lire(COA_DIR_COMPAGNONS), Lire(COA_DIR_APPROCHES),
             Lire(COA_DIR_ECARTE_PACTE), nbPactes.load(std::memory_order_relaxed), Lire(COA_DIR_TELEPORTS_CACHES),
             Lire(COA_DIR_SAUTS_REMIS_IDLE), Lire(COA_DIR_PRIMES), Lire(COA_DIR_CHASSEURS),
             Lire(COA_DIR_PRIMES_TOMBEES), Lire(COA_DIR_PRIMES_ECHUES), Lire(COA_DIR_PRIMES_LEVEES),
             Lire(COA_DIR_PROIES_IMPOSEES), Lire(COA_DIR_TRAQUES_PRIME), Lire(COA_DIR_MEUTE_PERMISE),
             Lire(COA_DIR_MORTS_TRACEES), Lire(COA_DIR_MORTS_PERDUES), nbPrimes.load(std::memory_order_relaxed),
             nbChasseurs.load(std::memory_order_relaxed), Lire(COA_DIR_TELEPORTS_LOINTAINS),
             Lire(COA_DIR_PAROLES_HORS_VUE), Lire(COA_DIR_MISSIONS), Lire(COA_DIR_MISSIONS_IGNOREES),
             Lire(COA_DIR_MISSIONS_DEGRADEES), Lire(COA_DIR_PATROUILLES), Lire(COA_DIR_BANDES_RECALEES),
             Lire(COA_DIR_BANDES_REJOINTES), Lire(COA_DIR_ECARTE_PACTE_BOTS), Lire(COA_DIR_MORTS_ENTRE_PACTES),
             BilanImprevu() + BilanPaix());   // lot D5 : ` attires= ...` ; paix de zone : ` paix= ...`, EN FIN de ligne
}

// ------------------------------------------------------------------ lectures 8888 (fil monde)

// `consignes` : une entree par consigne, separees par une tabulation :
//   id,verbe,cible,joueur,fin_s,carte,x,y,rayon,phase,mode,etape
// Une treve n'a ni lieu, ni phase, ni mode : 0 et '-'. Aucune consigne : ligne vide.
// Une suspension (D2c) porte son mode en onzieme champ.
// `etape` (12e champ, lot D2b suite) : '-' si la consigne n'est pas une etape
// d'itineraire, 0 si l'etape est en route, 1 si son disque est atteint : le
// service peut alors emettre l'etape suivante.
std::string Consignes()
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);   // les flottants des affectations ; entiers inchanges
    bool premier = true;
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    for (auto const& [cible, t] : treves)
    {
        if (!premier)
            out << '\t';
        premier = false;
        out << t.id << ",treve," << t.cible << ',' << t.joueur << ',' << t.finS << ",0,0,0,0,-,-,-";
    }
    // Paix de zone (19/10/2026) : cible = la ZONE, joueur 0, carte en sixieme champ ; ni point, ni phase, ni mode.
    for (auto const& [zone, z] : paix)
    {
        if (!premier)
            out << '\t';
        premier = false;
        out << z.id << ",paix," << z.zone << ",0," << z.finS << ',' << z.carte << ",0,0,0,-,-,-";
    }
    // Une suspension n'a ni lieu ni phase ; mode 0 (garde sa chasse) ou 1 (RPG).
    for (auto const& [guid, s] : suspensions)
    {
        if (!premier)
            out << '\t';
        premier = false;
        out << s.id << ",suspendre_mercenaire," << s.guid << ',' << s.joueur << ',' << s.finS << ",0,0,0,0,-,"
            << (s.rpg ? 1 : 0) << ",-";
    }
    // Une affectation (D2b) porte son disque et sa phase (trajet, sejour) ;
    // pas de mode.
    for (auto const& [guid, f] : affectations)
    {
        if (!premier)
            out << '\t';
        premier = false;
        // Lot D4c : un chasseur porte `c` en onzieme champ (mode).
        out << f.a.id << ',' << NomVerbe(f.verbe) << ',' << f.guid << ',' << f.joueur << ',' << f.a.finS << ','
            << f.a.carte << ',' << f.a.x << ',' << f.a.y << ',' << f.a.rayon << ',' << NomPhase(f.a.phase) << ','
            << (f.a.chasseur ? "c" : "-") << ',' << (f.a.etape ? (f.etapeArrivee ? "1" : "0") : "-");
    }
    // Lot D4c : une prime porte son plafond en onzieme champ ; cible et joueur
    // sont le joueur prime ; ni lieu ni phase.
    for (auto const& [j, p] : primes)
    {
        if (!premier)
            out << '\t';
        premier = false;
        out << p.id << ",prime," << p.joueur << ',' << p.joueur << ',' << p.finS << ",0,0,0,0,-," << p.plafond << ",-";
    }
    // Lot D3 : un ciel porte sa minute, une meteo son etat, en onzieme champ ;
    // ni lieu ni phase. Les paroles en file ne sont pas des consignes.
    // (ciels, meteos : fil monde seul, comme cette fonction.)
    for (auto const& [guid, ciel] : ciels)
    {
        if (!premier)
            out << '\t';
        premier = false;
        out << ciel.id << ",heure_du_jour," << ciel.joueur << ',' << ciel.joueur << ',' << ciel.finS << ",0,0,0,0,-,"
            << ciel.minute << ",-";
    }
    for (auto const& [guid, m] : meteos)
    {
        if (!premier)
            out << '\t';
        premier = false;
        out << m.id << ",meteo," << m.joueur << ',' << m.joueur << ',' << m.finS << ",0,0,0,0,-," << m.etat << ",-";
    }
    return out.str();
}

bool LireFlottant(std::string const& s, float& v)
{
    if (s.empty())
        return false;
    char* fin = nullptr;
    v = std::strtof(s.c_str(), &fin);
    return fin && *fin == '\0' && std::isfinite(v);
}

std::vector<std::string> Decouper(std::string const& s, char sep)
{
    std::vector<std::string> morceaux;
    size_t debut = 0;
    while (true)
    {
        size_t fin = s.find(sep, debut);
        morceaux.push_back(s.substr(debut, fin == std::string::npos ? std::string::npos : fin - debut));
        if (fin == std::string::npos)
            break;
        debut = fin + 1;
    }
    return morceaux;
}

constexpr size_t LIEUX_MAX = 16;
// Budget de points `lieux` par tick monde, toutes requetes confondues (revue
// D2) : sans lui, 64 requetes de 16 points (PAR_TICK x LIEUX_MAX) feraient
// 1 024 lectures de terrain par tick, et autant de grilles creees au besoin,
// qui restent chargees (UnloadGrid n'a qu'un appelant, UnloadAll). Le budget du
// service (Validateur) devient une politesse, plus la seule garde. Remis a
// zero par ViderRequetes ; fil monde seul, aucun verrou.
constexpr size_t LIEUX_POINTS_PAR_TICK = 64;
size_t lieuxPointsCeTick = 0;

// `lieux,<carte>,x,y,z;x,y,z;...` (16 points au plus). Par point, separes par
// ';' : zone,aire,sans_pvp,sol_z|NA,eau. Carte inconnue ou non chargee : NA
// pour chaque point. Requete malformee : "invalid".
//
// PIEGE : GetGridHeight cree la grille si elle n'existe pas (Map.cpp:1108).
// D'ou le plafond de 16 points par requete, le budget de points par tick
// (au-dela : "busy") ; au service de ne valider un point qu'une fois.
//
// INERTIE : bus eteint (AiPlayerbot.Directeur.Bus = 0), `lieux` rend "invalid"
// sans rien lire, pour qu'un binaire installe sans rien allumer ne cree aucune
// grille a la demande du service (le Validateur se tait alors, destinations.py).
std::string Lieux(std::string const& texte)
{
    if (!sPlayerbotAIConfig.directeurBus)
        return "invalid";
    std::string const corps = texte.substr(6);   // apres "lieux,"
    size_t const virgule = corps.find(',');
    if (virgule == std::string::npos || virgule == 0)
        return "invalid";
    char* finCarte = nullptr;
    std::string const texteCarte = corps.substr(0, virgule);
    unsigned long const carte = std::strtoul(texteCarte.c_str(), &finCarte, 10);
    if (!finCarte || *finCarte != '\0' || carte > 0xFFFF)
        return "invalid";

    std::vector<std::string> const points = Decouper(corps.substr(virgule + 1), ';');
    if (points.empty() || points.size() > LIEUX_MAX)
        return "invalid";

    struct Point { float x, y, z; };
    std::vector<Point> lus;
    for (std::string const& p : points)
    {
        std::vector<std::string> const c = Decouper(p, ',');
        Point pt{};
        if (c.size() != 3 || !LireFlottant(c[0], pt.x) || !LireFlottant(c[1], pt.y) || !LireFlottant(c[2], pt.z))
            return "invalid";
        lus.push_back(pt);
    }
    if (lieuxPointsCeTick + lus.size() > LIEUX_POINTS_PAR_TICK)
        return "busy";
    lieuxPointsCeTick += lus.size();

    // Cartes a instances exclues : leur carte de base n'est pas celle ou les
    // bots jouent, et y creer des grilles n'aurait aucun sens.
    Map* map = sMapMgr->FindBaseMap(static_cast<uint32>(carte));
    if (map && map->Instanceable())
        map = nullptr;
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    bool premier = true;
    for (Point const& pt : lus)
    {
        if (!premier)
            out << ';';
        premier = false;
        if (!map || !Acore::IsValidMapCoord(pt.x, pt.y))
        {
            out << "NA";
            continue;
        }
        uint32 zone = 0, aire = 0;
        map->GetZoneAndAreaId(PHASEMASK_NORMAL, zone, aire, pt.x, pt.y, pt.z);
        bool const sansPvp = sPlayerbotAIConfig.CoaLieuSansPvp(zone, aire);
        // Regle de P-132 : partir d'au-dessus du terrain brut, sinon un point
        // tire sous la surface ne trouve aucun sol.
        float const depart = std::max(pt.z, map->GetGridHeight(pt.x, pt.y));
        float const sol = map->GetHeight(pt.x, pt.y, depart);
        bool const solValide = sol > INVALID_HEIGHT;
        bool const eau = map->IsInWater(PHASEMASK_NORMAL, pt.x, pt.y, solValide ? sol : pt.z, DEFAULT_COLLISION_HEIGHT);
        out << zone << ',' << aire << ',' << (sansPvp ? 1 : 0) << ',';
        if (solValide)
            out << sol;
        else
            out << "NA";
        out << ',' << (eau ? 1 : 0);
    }
    return out.str();
}

// `joueur,<guid>` (lot D3, SPEC-directeur-D3.md § 3.4), lecture seule, fil
// monde (P-137). Une ligne :
//   guid,regle,equipe,race,classe,niveau,groupe,mj,bg,ffa,pvp,arret,ciel,meteo,afk
// (`afk`, 02/10 : PLAYER_FLAGS_AFK, 0 ou 1, ajoute EN FIN de ligne.)
// regle : pve, high_risk, war_mode ou '-', par HasAura dans l'ordre 9931032,
// 1004019, 1004119 (P-138 : 1004119 est AUSSI porte en PvE) ; equipe A ou H ;
// groupe : taille du groupe, 0 sans groupe ; ciel, meteo : minute et etat poses
// par le Directeur, '-' sinon. Personnage absent : `absent`. Requete mal formee,
// ou bus eteint (meme interrupteur que `lieux`) : `invalid`.
// (AURA_REGLE_* : definies plus haut, avec la prime du lot D4c.)

std::string Joueur(std::string const& texte)
{
    if (!sPlayerbotAIConfig.directeurBus)
        return "invalid";
    std::string const nombre = texte.substr(7);   // apres "joueur,"
    if (nombre.empty() || nombre.size() > 10 ||
        !std::all_of(nombre.begin(), nombre.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        return "invalid";
    unsigned long long const valeur = std::strtoull(nombre.c_str(), nullptr, 10);
    if (valeur == 0 || valeur > 0xFFFFFFFFull)
        return "invalid";
    uint32 const guid = static_cast<uint32>(valeur);
    Player* p = ObjectAccessor::FindPlayerByLowGUID(guid);
    if (!p)
        return "absent";
    char const* regle = p->HasAura(AURA_REGLE_PVE)         ? "pve"
                        : p->HasAura(AURA_REGLE_HIGH_RISK) ? "high_risk"
                        : p->HasAura(AURA_REGLE_WAR_MODE)  ? "war_mode"
                                                           : "-";
    Group* groupe = p->GetGroup();
    std::ostringstream out;
    out << guid << ',' << regle << ',' << (p->GetTeamId() == TEAM_ALLIANCE ? 'A' : 'H') << ','
        << static_cast<uint32>(p->getRace()) << ',' << static_cast<uint32>(p->getClass()) << ','
        << static_cast<uint32>(p->GetLevel()) << ',' << (groupe ? groupe->GetMembersCount() : 0) << ','
        << (p->IsGameMaster() ? 1 : 0) << ',' << ((p->InBattleground() || p->InArena()) ? 1 : 0) << ','
        << (p->IsFFAPvP() ? 1 : 0) << ',' << (p->IsPvP() ? 1 : 0) << ','
        << (CoaDirecteur::EstArrete(guid) ? 1 : 0) << ',';
    auto const ciel = ciels.find(guid);
    if (ciel != ciels.end())
        out << ciel->second.minute;
    else
        out << '-';
    out << ',';
    auto const meteo = meteos.find(guid);
    if (meteo != meteos.end())
        out << meteo->second.etat;
    else
        out << '-';
    // Reserve de chasseurs (02/10) : 15e champ, EN FIN de ligne (un lecteur qui
    // s'arrete au 14e ne voit rien changer) : 1 si le joueur est AFK.
    out << ',' << (p->isAFK() ? 1 : 0);
    // Lot D5 (15/10/2026, etude § 3.2) : 16e champ `soin`, EN FIN de ligne. PlayerbotAI::IsHeal(p) (bySpec faux),
    // EXACTEMENT le test du refus `pas_soigneur` de `soigner` : pour un BOT, une strategie de type soin dans un de ses
    // moteurs (« coa heal », « heal » : ce sont elles qui lisent « party member to heal », donc JoueurASoigner) ; pour
    // un humain (sans moteur), sa specialisation. Les 15 premiers champs sont inchanges.
    out << ',' << (PlayerbotAI::IsHeal(p) ? 1 : 0);
    return out.str();
}
}  // namespace

// ================================================================== interface

std::string CoaDirecteur::SoumettreRequete(std::string const& requete)
{
    // C5 : serveur en arret, on ne touche plus a la file.
    if (arretServeur.load(std::memory_order_acquire))
        return "timeout";
    auto r = std::make_shared<Requete>();
    r->texte = requete;
    std::future<std::string> reponse = r->reponse.get_future();
    {
        std::lock_guard<std::mutex> garde(verrouFile);
        if (file.size() >= FILE_MAX)
            return "busy";
        file.push_back(r);
    }
    // Apres expiration, la requete reste en file et s'executera quand meme : le
    // shared_ptr garde la promesse en vie, et une reponse que personne n'attend
    // est simplement jetee. Aucune requete du port 8888 n'ecrit dans le jeu.
    //
    // Attente par tranches de 50 ms, pour rendre la main des l'arret du
    // serveur au lieu d'attendre les deux secondes. `r` (shared_ptr local)
    // garde la promesse en vie pendant toute l'attente.
    auto const limite = std::chrono::steady_clock::now() + DELAI_REPONSE;
    while (true)
    {
        if (reponse.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready)
            return reponse.get();
        if (arretServeur.load(std::memory_order_acquire) || std::chrono::steady_clock::now() >= limite)
            return "timeout";
    }
}

void CoaDirecteur::ViderRequetes()
{
    lieuxPointsCeTick = 0;   // budget de `lieux`, par tick monde
    std::vector<std::shared_ptr<Requete>> lot;
    {
        std::lock_guard<std::mutex> garde(verrouFile);
        while (!file.empty() && lot.size() < PAR_TICK)
        {
            lot.push_back(std::move(file.front()));
            file.pop_front();
        }
    }
    for (auto& r : lot)
    {
        try
        {
            r->reponse.set_value(Executer(r->texte));
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("playerbots", "coa directeur : requete '{}' en echec : {}", r->texte, e.what());
            try { r->reponse.set_value("error"); } catch (...) { }
        }
    }
}

void CoaDirecteur::ChargerArrets()
{
    // Table absente (migration non passee, P-131) : Query rend nul et le coeur
    // l'ecrit dans le journal. Le Directeur demarre alors sans arret connu -- ce
    // qui est sur, puisque rien en D1 n'agit sur les joueurs.
    std::lock_guard<std::mutex> garde(verrouArrets);
    arrets.clear();
    if (QueryResult r = CharacterDatabase.Query("SELECT guid FROM coa_dir_arret"))
    {
        do
            arrets.insert((*r)[0].Get<uint32>());
        while (r->NextRow());
    }
    LOG_INFO("playerbots", "coa directeur : {} arret(s) charge(s)", arrets.size());
}

bool CoaDirecteur::EstArrete(uint32 guidLow)
{
    std::lock_guard<std::mutex> garde(verrouArrets);
    return arrets.count(guidLow) != 0;
}

// ================================================================== bus (D2a)

void CoaDirecteur::DemarrerBus()
{
    // Lot D5 : presence des tables de l'onglet (audace, opt-in), independamment du bus.
    {
        tableAudaceLue = false;
        tableOptinLue = false;
        QueryResult t = CharacterDatabase.Query(
            "SELECT (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()"
            " AND TABLE_NAME = 'coa_dir_audace'),"
            " (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()"
            " AND TABLE_NAME = 'coa_dir_imprevu_optin')");
        if (t)
        {
            Field* f = t->Fetch();
            tableAudaceLue = f[0].Get<uint64>() != 0;
            tableOptinLue = f[1].Get<uint64>() != 0;
        }
        LOG_INFO("playerbots", "coa directeur : onglet, table audace {}, table opt-in {}",
                 tableAudaceLue ? "presente" : "absente", tableOptinLue ? "presente" : "absente");
    }
    bool const bus = sPlayerbotAIConfig.directeurBus;
    dernierEtatBus = bus;
    source = sourceMysql.Demarrer(EpoqueMonde(), bus) ? &sourceMysql : nullptr;
    // Lot D4c : les auras des defis bloquants, une fois (requete synchrone,
    // admise au demarrage comme la garde de colonnes).
    if (source)
        ChargerDefisBloquants();
    // 21/10 : zones a variante de tranche du gisement d'apres-evenement (CoaDirecteurGardien.inc).
    if (source)
        ChargerTranchesGisement();
    if (source)
        LOG_INFO("playerbots", "coa directeur : bus pret, epoque {}, interrupteur {}", EpoqueMonde(), bus ? 1 : 0);
}

void CoaDirecteur::TraiterBus(uint32 diff)
{
    if (!source)
        return;

    bool const bus = sPlayerbotAIConfig.directeurBus;
    if (bus != dernierEtatBus)
    {
        dernierEtatBus = bus;
        source->SignalerBus(bus);
        LOG_INFO("playerbots", "coa directeur : interrupteur du bus passe a {}", bus ? 1 : 0);
        bilanMs = 0;
    }

    // 1. Bus eteint : rien de neuf n'est lu, tout est leve, et ce qui avait
    // deja ete lu est refuse plutot que garde pour le rallumage.
    if (!bus)
    {
        LeverSi([](Treve const&) { return true; }, "bus_eteint");
        LeverPaixSi([](Paix const&) { return true; }, "bus_eteint");   // paix de zone (19/10/2026)
        LeverSuspensionsSi([](Suspension const&) { return true; }, "bus_eteint");
        LeverAffectationsSi([](AffectationInterne const&) { return true; }, "bus_eteint");
        // Lot D3 : heure vraie renvoyee, temps de la zone retabli, paroles oubliees.
        LeverCielsSi([](Ciel const&) { return true; }, "bus_eteint");
        LeverMeteosSi([](MeteoJoueur const&) { return true; }, "bus_eteint");
        LeverParolesSi([](Parole const&) { return true; }, "bus_eteint");
        // Lot D4c : les chasseurs sont deja leves (affectations) ; la prime aussi.
        LeverPrimesSi([](Prime const&) { return true; }, "bus_eteint");
        // Lot C des evenements : aures retirees, musiques coupees, declencheurs effaces.
        LeverEvenementsSi([](uint32, uint32) { return true; }, "bus_eteint");
        PurgerMortsEnTrajet();
        PurgerPactes();
        ViderPreparations("bus_eteint");
        ViderRepos();
        ViderRetraites("bus_eteint");   // apres les primes : celles qu'elles viennent d'ouvrir aussi
        source->Vidanger([](CoaDirCommande const& c)
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_RECUES);
            Refuser(c, "bus_eteint");
        });
        // La purge a pu mettre des mercenaires en mode RPG en file : leur
        // retour a la composition de mercenaire ne doit pas attendre le bus.
        Entretien(diff);
        return;
    }

    // 2-4. Drainage : controles, effets, accuses.
    source->Sonder(diff, Recevoir);
    // Reserve de chasseurs (02/10) : au plus UNE preparation (Randomize) par tick.
    TraiterPreparations();

    // 5. Chaque seconde : echeances, file de reconstruction ; filet a 30 s.
    Entretien(diff);

    // Bilan, seulement bus allume : aucune ligne `coa directeur bus:` bus
    // eteint, c'est la preuve d'inertie choisie d'avance (SPEC § 8).
    bilanMs += diff;
    if (bilanMs >= BILAN_MS)
    {
        bilanMs = 0;
        Bilan();
    }
}

void CoaDirecteur::Persister()
{
    PersisterArrets(false);
}

bool CoaDirecteur::SousTreve(uint32 guidLow)
{
    if (!nbTreves.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = treves.find(guidLow);
    // Echue mais pas encore levee (la levee passe chaque seconde) : plus sous treve.
    return it != treves.end() && it->second.finS > Maintenant();
}

bool CoaDirecteur::PaixDeZone(uint32 zoneId)
{
    // LA QUESTION D'ABORD : sans paix, un chargement atomique.
    if (!nbPaix.load(std::memory_order_acquire) || !zoneId)
        return false;
    uint32 const maintenant = Maintenant();
    for (auto const& kase : casesPaix)
    {
        uint64 const v = kase.load(std::memory_order_acquire);
        // Echue mais pas encore levee (la levee passe chaque seconde) : plus en paix.
        if (v && static_cast<uint32>(v >> 32) == zoneId && static_cast<uint32>(v) > maintenant)
            return true;
    }
    return false;
}

bool CoaDirecteur::BotEnPaix(Player* bot)
{
    if (!bot || !nbPaix.load(std::memory_order_relaxed))
        return false;
    if (!PaixDeZone(bot->GetZoneId()) || bot->InBattleground() || bot->InArena())
        return false;
    // Une aire arene du monde ouvert (Gurubashi) garde son FFA de coeur et sa convoitise : la paix ne la vise pas.
    AreaTableEntry const* aire = sAreaTableStore.LookupEntry(bot->GetAreaId());
    return !(aire && (aire->flags & AREA_FLAG_ARENA));
}

bool CoaDirecteur::CiblePaix(Unit const* unit)
{
    if (!unit || !nbPaix.load(std::memory_order_relaxed) || !PaixDeZone(unit->GetZoneId()))
        return false;
    AreaTableEntry const* aire = sAreaTableStore.LookupEntry(unit->GetAreaId());
    return !(aire && (aire->flags & AREA_FLAG_ARENA));
}

bool CoaDirecteur::MercenaireSuspendu(uint32 guidLow)
{
    if (!nbSuspensions.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = suspensions.find(guidLow);
    // Echue mais pas encore levee : plus suspendu (la levee passe chaque seconde).
    return it != suspensions.end() && it->second.finS > Maintenant();
}

bool CoaDirecteur::MercenaireEnRpg(uint32 guidLow)
{
    if (SuspenduEnRpg(guidLow))
        return true;
    // Lot D4c : un chasseur EN TRAJET est compose en mode RPG (voie retenue
    // apres D4-0, SPEC-directeur-D4.md § 10). Sans chasseur : un chargement.
    return ChasseurEnTrajet(guidLow);
}

bool CoaDirecteur::ChasseurDe(uint32 botLow, uint32* joueur, uint32* id, bool* afk, bool* reserve)
{
    // LA QUESTION D'ABORD : sans chasseur, un chargement atomique.
    if (!nbChasseurs.load(std::memory_order_relaxed))
        return false;
    uint32 const maintenant = Maintenant();
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = affectations.find(botLow);
    if (it == affectations.end() || !it->second.a.chasseur || it->second.a.finS <= maintenant)
        return false;
    // La prime doit courir encore : sa fin leve les chasseurs a la seconde, mais
    // un lecteur de carte peut passer entre les deux.
    auto p = primes.find(it->second.joueur);
    if (p == primes.end() || p->second.finS <= maintenant)
        return false;
    if (joueur)
        *joueur = it->second.joueur;
    if (id)
        *id = it->second.a.id;
    if (afk)
        *afk = p->second.afk;
    if (reserve)
        *reserve = it->second.a.reserve;
    return true;
}

bool CoaDirecteur::CibleImmobile(uint32 guidLow)
{
    if (!guidLow || !nbImmobiles.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouImmobiles);
    auto it = immobiles.find(guidLow);
    return it != immobiles.end() && it->second > Maintenant();
}

bool CoaDirecteur::CibleDeBanc(uint32 guidLow)
{
    if (!guidLow || !nbBancs.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouBancs);
    return bancs.count(guidLow) != 0;
}

bool CoaDirecteur::EnRetraite(uint32 botLow, uint32* fuirDe)
{
    if (!nbRetraites.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = retraites.find(botLow);
    if (it == retraites.end())
        return false;
    if (fuirDe)
        *fuirDe = it->second.fuirDe;
    return true;
}

bool CoaDirecteur::PrepareRecemment(uint32 guidLow)
{
    // Revue M4 : un bot au repos `P-0` n'est pas teleporte au hasard non plus.
    return PrepareDepuisMoinsDe(guidLow, PREPARATION_REPOS_S) || EnRepos(guidLow);
}

bool CoaDirecteur::EnRepos(uint32 guidLow)
{
    if (!nbRepos.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = reposReserve.find(guidLow);
    return it != reposReserve.end() && it->second > Maintenant();
}

bool CoaDirecteur::ProieImposee(uint32 botLow, uint32* joueur, uint32* id)
{
    // Reserve de chasseurs (02/10) : un prime AFK n'est pas une proie.
    // Les sorties ne sont ecrites que si la reponse est vraie (contrat d'avant).
    bool afk = false;
    uint32 j = 0, i = 0;
    if (!ChasseurDe(botLow, &j, &i, &afk, nullptr) || afk)
        return false;
    if (joueur)
        *joueur = j;
    if (id)
        *id = i;
    return true;
}

bool CoaDirecteur::Embuscade(uint32 botLow, EtatEmbuscade* sortie)
{
    // LA QUESTION D'ABORD : sans chasseur, un chargement atomique.
    if (!nbChasseurs.load(std::memory_order_relaxed))
        return false;
    if (!sPlayerbotAIConfig.directeurEmbuscade || !sPlayerbotAIConfig.wildPvpChasse || !sPlayerbotAIConfig.wildPvpTraque)
        return false;
    uint32 const maintenant = Maintenant();
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = affectations.find(botLow);
    if (it == affectations.end() || !it->second.a.chasseur || it->second.a.finS <= maintenant)
        return false;
    auto p = primes.find(it->second.joueur);
    // Meme regle que ProieImposee : un prime AFK n'est la proie de personne.
    if (p == primes.end() || p->second.finS <= maintenant || p->second.afk)
        return false;
    if (sortie)
    {
        sortie->joueur = it->second.joueur;
        sortie->id = it->second.a.id;
        sortie->prime = p->second.idOrigine ? p->second.idOrigine : p->second.id;
        sortie->plafond = p->second.plafond;
        sortie->reserve = it->second.a.reserve;
        sortie->assaut = p->second.assaut;
        sortie->raison = p->second.raisonAssaut;
        sortie->place = 0xFF;
        for (uint8 i = 0; i < EMBUSCADE_PLACES; ++i)
            if (p->second.places[i] == botLow)
                sortie->place = i;
    }
    return true;
}

bool CoaDirecteur::PointDEmbuscade(Map* carte, float jx, float jy, float jz, float angle, float distance, float& x,
                                   float& y, float& z)
{
    x = jx + distance * std::cos(angle);
    y = jy + distance * std::sin(angle);
    z = jz;
    if (!carte)
        return false;
    static constexpr float ECARTS[] = {0.0f, 0.26f, -0.26f, 0.52f, -0.52f};   // 0, +-15, +-30 degres
    static constexpr float DECALAGES[] = {0.0f, -5.0f, 5.0f};
    for (float const dd : DECALAGES)
        for (float const e : ECARTS)
        {
            float const r = std::max(5.0f, distance + dd);
            float const px = jx + r * std::cos(angle + e);
            float const py = jy + r * std::sin(angle + e);
            float sol = 0.0f;
            // Laisse de hauteur (P-145) : un sol a plus de 6 yd du joueur est un
            // autre etage (pont, rempart, falaise) : essai perdu.
            if (!SolSec(carte, px, py, jz + Z_OFFSET_FIND_HEIGHT, sol) || std::fabs(sol - jz) > TIRAGE_ECART_Z)
                continue;
            if (ToitAuDessus(carte, px, py, sol, jz))
                continue;
            // Ligne de vue des tetes (+ 1,5 yd) : modeles seulement (Map::isInLineOfSight).
            if (!carte->isInLineOfSight(px, py, sol + 1.5f, jx, jy, jz + 1.5f, PHASEMASK_NORMAL, LINEOFSIGHT_ALL_CHECKS,
                                        VMAP::ModelIgnoreFlags::Nothing))
                continue;
            x = px;
            y = py;
            z = sol;
            return true;
        }
    return false;
}

bool CoaDirecteur::AffectationDe(uint32 guidLow, Affectation* sortie)
{
    if (!nbAffectations.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = affectations.find(guidLow);
    // Echue mais pas encore levee (le suivi passe chaque seconde) : plus affecte.
    if (it == affectations.end() || it->second.a.finS <= Maintenant())
        return false;
    if (sortie)
        *sortie = it->second.a;
    return true;
}

bool CoaDirecteur::EnTrajetPacifique(Player const* bot)
{
    // LA QUESTION D'ABORD : sans affectation, un chargement atomique.
    if (!nbAffectations.load(std::memory_order_relaxed))
        return false;
    if (!bot || !sPlayerbotAIConfig.directeurTrajetPacifique)
        return false;
    uint32 carte = 0;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        auto it = affectations.find(bot->GetGUID().GetCounter());
        if (it == affectations.end() || it->second.a.finS <= Maintenant() ||
            it->second.a.phase != CoaDirecteur::CoaDirPhase::Trajet)
            return false;
        // Reserve de chasseurs (02/10) : un chasseur de reserve n'est jamais
        // pacifique. Il ne choisit que son prime (NearestEnemyPlayersValue) et
        // aucune creature (GrindTargetValue, ChasseurDe) : rien a lui retirer.
        if (it->second.a.chasseur && it->second.a.reserve)
            return false;
        carte = it->second.a.carte;
    }
    // Champ de bataille, arene, instance : autre carte, jamais pacifique.
    return bot->GetMapId() == carte;
}

bool CoaDirecteur::Pacte(uint32 botLow, uint32 uniteLow)
{
    if (!nbPactes.load(std::memory_order_relaxed))
        return false;
    if (!sPlayerbotAIConfig.directeurCompagnons)
        return false;
    uint32 const maintenant = Maintenant();
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = pactes.find(botLow);
    // Echu mais pas encore retire (le suivi passe chaque seconde) : plus de pacte.
    if (it == pactes.end() || it->second.finS <= maintenant)
        return false;
    if (it->second.joueur == uniteLow)
        return true;
    auto autre = pactes.find(uniteLow);
    return autre != pactes.end() && autre->second.finS > maintenant && autre->second.joueur == it->second.joueur;
}

bool CoaDirecteur::PacteEntreBots(uint32 botLow, uint32 autreLow)
{
    if (!nbPactes.load(std::memory_order_relaxed) || botLow == autreLow)
        return false;
    if (!sPlayerbotAIConfig.directeurCompagnons)
        return false;
    uint32 const maintenant = Maintenant();
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto const it = pactes.find(botLow);
    if (it == pactes.end() || it->second.finS <= maintenant)
        return false;
    auto const autre = pactes.find(autreLow);
    return autre != pactes.end() && autre->second.finS > maintenant && autre->second.joueur == it->second.joueur;
}

bool CoaDirecteur::PointDApproche(Map* carte, Affectation const& a, float bx, float by, float& x, float& y, float& z)
{
    return PointPres(carte, a, bx, by, COMPAGNON_CIBLE_YD, x, y, z);
}

bool CoaDirecteur::PointPres(Map* carte, Affectation const& a, float bx, float by, float distance, float& x, float& y,
                             float& z)
{
    // Repli : le centre (le joueur, sol connu).
    x = a.x;
    y = a.y;
    z = a.z;
    if (!carte || carte->GetId() != a.carte)
        return false;
    float const dx = bx - a.x;
    float const dy = by - a.y;
    float const d = std::sqrt(dx * dx + dy * dy);
    // Bot sur le centre (d nul) : n'importe quel cap.
    float const cap = d > 0.1f ? std::atan2(dy, dx) : 0.0f;
    static constexpr float ECARTS[] = {0.0f, 0.61f, -0.61f, 1.22f, -1.22f};   // 0, +-35, +-70 degres
    for (float const e : ECARTS)
    {
        float const px = a.x + distance * std::cos(cap + e);
        float const py = a.y + distance * std::sin(cap + e);
        float sol = 0.0f;
        // Laisse de hauteur (P-145) : un sol a plus de 6 yd du joueur est un
        // autre etage (pont, rempart, terrain sous une ville) : essai perdu.
        if (!SolSec(carte, px, py, a.z + Z_OFFSET_FIND_HEIGHT, sol) || std::fabs(sol - a.z) > TIRAGE_ECART_Z)
            continue;
        x = px;
        y = py;
        z = sol;
        return true;
    }
    return false;
}

bool CoaDirecteur::Sauter(Player* bot, WorldLocation const& dest)
{
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    return bot->TeleportTo(dest);
}

bool CoaDirecteur::PointHorsDeVue(Player* joueur, Player* bot, uint32 niveau, float& x, float& y, float& z,
                                  uint32& essais)
{
    essais = 0;
    if (!joueur || !bot || !joueur->IsInWorld())
        return false;
    Map* map = joueur->GetMap();
    if (!map)
        return false;
    PointCarte const r = ChercherPointCarte(map, joueur, bot, niveau);
    essais = r.essais;
    if (!r.trouve)
        return false;
    x = r.x;
    y = r.y;
    z = r.z;
    return true;
}

bool CoaDirecteur::MortEnTrajet(uint32 guidLow)
{
    if (!nbMortsEnTrajet.load(std::memory_order_relaxed))
        return false;
    if (!sPlayerbotAIConfig.directeurReleveCimetiere)
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = mortsEnTrajet.find(guidLow);
    return it != mortsEnTrajet.end() && it->second.finS > Maintenant();
}

bool CoaDirecteur::DansLeDisque(Affectation const& a, uint32 carte, float x, float y)
{
    if (carte != a.carte)
        return false;
    float const dx = x - a.x;
    float const dy = y - a.y;
    return dx * dx + dy * dy <= a.rayon * a.rayon;
}

bool CoaDirecteur::TrajetLeveVers(uint32 guidLow, uint32 carte, float x, float y, uint32 debutMs)
{
    // LA QUESTION D'ABORD : ni trajet leve ni affectation, deux chargements atomiques.
    if (!nbTrajetsLeves.load(std::memory_order_relaxed) && !nbAffectations.load(std::memory_order_relaxed))
        return false;
    uint32 const maintenant = Maintenant();
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    bool leve = false;
    auto en = affectations.find(guidLow);
    if (en != affectations.end())
    {
        Affectation const& a = en->second.a;
        if (a.finS > maintenant)
        {
            // Affectation en cours : son disque la gouverne (laisse, sejour).
            if (DansLeDisque(a, carte, x, y))
                return false;
        }
        // Echue, pas encore levee (le suivi passe chaque seconde) : deja
        // levee pour ses lecteurs (AffectationDe rend faux), donc ici aussi.
        else if (a.phase == CoaDirPhase::Trajet && DansLeDisque(a, carte, x, y))
            leve = true;
    }
    if (!leve)
    {
        auto it = trajetsLeves.find(guidLow);
        if (it == trajetsLeves.end() || it->second.finS <= maintenant || it->second.carte != carte)
            return false;
        // GO_GRIND ecrit apres la levee : un choix du bot, pas le point tire
        // pour l'affectation. Difference signee : getMSTime reboucle a 49 j.
        if (static_cast<int32>(it->second.leveMs - debutMs) < 0)
            return false;
        float const dx = x - it->second.x;
        float const dy = y - it->second.y;
        leve = dx * dx + dy * dy <= it->second.rayon * it->second.rayon;
    }
    return leve;
}

// Laisse de hauteur (correctif du 26/09, bot 195 a Orgrimmar). SolSec part de
// max(z, terrain brut) et le rayon vmap descend EXACTEMENT de ce z : si le vrai
// sol du point tire est un peu plus haut que a.z (ville en pente sous WMO), le
// rayon demarre sous lui, ne le trouve pas, et GetHeight rend le terrain brut
// de la tuile (8.8, 33 yd sous la ville) -- un point injoignable. D'ou :
//   1. depart a a.z + 2, puis a a.z + TIRAGE_ECART_Z si le premier sol sort de
//      la fenetre : tout sol de [a.z - 6, a.z + 6] est vu (au second depart, un
//      plafond plus bas que a.z + 6 peut etre pris pour le sol : un etage
//      voisin, joignable par ses escaliers ; c'est le prix de la marge) ;
//   2. sol dans la fenetre : garde ;
//   3. hors fenetre : garde seulement si RIEN n'est au-dessus (sonde depuis
//      max(sol, a.z) + 50 jusqu'a sol, ToitAuDessus : la premiere surface
//      rencontree est ce sol, a 2 yd pres) ET si
//      la pente moyenne depuis le centre est d'au plus TIRAGE_PENTE_MAX. Le
//      terrain brut sous une ville, un pont, une grotte a un toit : refuse.
//      Une colline a ciel ouvert (pente naturelle > 6 yd sur 40) : gardee.
// (Constantes TIRAGE_* et sonde ToitAuDessus : pres de SolSec, la pose s'en sert.)
// Le centre lui-meme (repli) a passe la meme laisse a la pose : `sol_sous_structure`.

CoaDirecteur::TirageDisque CoaDirecteur::TirerDansLeDisque(Map* carte, Affectation const& a, float& x, float& y,
                                                          float& z, Position const* depuis, float distanceMax,
                                                          bool hauteurStricte)
{
    TirageDisque t;
    // Repli : le centre, dont le sol a ete trouve a la pose (a.z).
    x = a.x;
    y = a.y;
    z = a.z;
    if (!carte || carte->GetId() != a.carte)
        return t;
    for (int essai = 0; essai < TIRAGES_DISQUE; ++essai)
    {
        ++t.essais;
        // Uniforme sur le disque : rayon en racine carree.
        float const angle = frand(0.0f, 2.0f * static_cast<float>(M_PI));
        float const r = a.rayon * std::sqrt(static_cast<float>(rand_norm()));
        float const px = a.x + r * std::cos(angle);
        float const py = a.y + r * std::sin(angle);
        // Hors de portee de l'appelant : essai perdu, avant toute lecture de
        // terrain (GetGridHeight peut creer une grille, voir SolSec).
        if (depuis)
        {
            float const ex = px - depuis->GetPositionX();
            float const ey = py - depuis->GetPositionY();
            if (ex * ex + ey * ey > distanceMax * distanceMax)
                continue;
        }
        // Deux departs : d'abord a.z + 2 (Z_OFFSET_FIND_HEIGHT, la marge du
        // coeur), qui garde le rez-de-chaussee sous un plafond bas ; puis, si le
        // sol trouve sort de la fenetre, a.z + 6, qui voit un sol un peu plus
        // haut que le centre (le cas d'Orgrimmar). Le second n'est lu que si le
        // premier echoue a la fenetre.
        float sol = 0.0f;
        if (!SolSec(carte, px, py, a.z + Z_OFFSET_FIND_HEIGHT, sol))
            continue;
        float ecart = std::fabs(sol - a.z);
        if (ecart > TIRAGE_ECART_Z)
        {
            float solHaut = 0.0f;
            if (SolSec(carte, px, py, a.z + TIRAGE_ECART_Z, solHaut) && std::fabs(solHaut - a.z) <= TIRAGE_ECART_Z)
            {
                sol = solHaut;
                ecart = std::fabs(sol - a.z);
            }
        }
        if (ecart > TIRAGE_ECART_Z)
        {
            // Revue P-145 : la sonde part de max(sol, a.z) + 50, pas de sol + 50.
            // Missions (30/09) : hauteur stricte, ni ciel ouvert ni pente douce.
            if (hauteurStricte || ToitAuDessus(carte, px, py, sol, a.z) || ecart > TIRAGE_PENTE_MAX * r)
            {
                ++t.refusesHauteur;
                Compter(COA_DIR_TIRAGES_REFUSES_HAUTEUR);
                continue;
            }
        }
        x = px;
        y = py;
        z = sol;
        t.repli = false;
        return t;
    }
    return t;
}

void CoaDirecteur::Compter(CoaDirCompteur c)
{
    if (c < COA_DIR_NB_COMPTEURS)
        compteurs[c].fetch_add(1, std::memory_order_relaxed);
}

// ================================================================== commandes

using namespace Acore::ChatCommands;

class coa_directeur_commandscript : public CommandScript
{
public:
    coa_directeur_commandscript() : CommandScript("coa_directeur_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        // Console::No : chaque demande porte la position et le contexte d'un
        // personnage ; la console n'en a pas.
        static ChatCommandTable directeurTable =
        {
            { "note",    HandleNote,    SEC_PLAYER, Console::No },
            { "ask",     HandleAsk,     SEC_PLAYER, Console::No },
            { "stop",    HandleStop,    SEC_PLAYER, Console::No },
            { "reprise", HandleReprise, SEC_PLAYER, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "directeur", directeurTable },
        };
        return commandTable;
    }

    static bool HandleNote(ChatHandler* handler, char const* args)
    {
        Player* joueur = JoueurDe(handler);
        if (!joueur)
            return false;
        std::string texte = Nettoyer(args);
        if (texte.empty())
        {
            handler->PSendSysMessage("Usage : .directeur note <ce que vous ressentez ici>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (TropTot(joueur->GetGUID().GetCounter()))
        {
            handler->PSendSysMessage("Doucement : une commande du Directeur toutes les {} secondes.", INTERVALLE_MIN_MS / 1000);
            handler->SetSentErrorMessage(true);
            return false;
        }
        Enregistrer(joueur, "note", texte);
        handler->PSendSysMessage("Le Directeur a note votre remarque, avec le lieu et le moment.");
        return true;
    }

    static bool HandleAsk(ChatHandler* handler, char const* args)
    {
        Player* joueur = JoueurDe(handler);
        if (!joueur)
            return false;
        std::string texte = Nettoyer(args);
        if (texte.empty())
        {
            handler->PSendSysMessage("Usage : .directeur ask <votre demande>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (TropTot(joueur->GetGUID().GetCounter()))
        {
            handler->PSendSysMessage("Doucement : une commande du Directeur toutes les {} secondes.", INTERVALLE_MIN_MS / 1000);
            handler->SetSentErrorMessage(true);
            return false;
        }
        // Lot D3 : le service d'ecoute lit la demande et repond (verbe `repondre`).
        // Accuse immediat, dans le meme appel (SPEC-directeur-D3.md § 2.8). Le jeu ne
        // sait pas si le service tourne : l'accuse ne dit que ce qui est sur (28/09 :
        // « Le Directeur vous a entendu » mentait quand l'ecoute etait coupee).
        Enregistrer(joueur, "ask", texte);
        handler->PSendSysMessage("Demande transmise au Directeur.");
        return true;
    }

    static bool HandleStop(ChatHandler* handler, char const* /*args*/)
    {
        Player* joueur = JoueurDe(handler);
        if (!joueur)
            return false;
        uint32 const guidLow = joueur->GetGUID().GetCounter();
        // L'arret n'est jamais freine par la cadence : il doit toujours passer.
        {
            std::lock_guard<std::mutex> garde(verrouArrets);
            arrets.insert(guidLow);
        }
        // C2 : la base suit, au plus une fois par seconde, le dernier etat.
        NoterAPersister(guidLow, true);
        // SPEC § 4.5 : lever TOUT DE SUITE ce qui entoure le joueur. On est sur
        // le fil monde (§ 0) : le magasin garde un seul fil ecrivain.
        size_t const levees =
            LeverSi([guidLow](Treve const& t) { return t.cible == guidLow || t.joueur == guidLow; }, "arret") +
            LeverSuspensionsSi([guidLow](Suspension const& s) { return s.joueur == guidLow; }, "arret") +
            LeverAffectationsSi([guidLow](AffectationInterne const& f) { return f.joueur == guidLow; }, "arret") +
            // Lot D3 : son ciel et sa meteo sont retablis, ses paroles en file oubliees.
            LeverCielsSi([guidLow](Ciel const& x) { return x.joueur == guidLow; }, "arret") +
            LeverMeteosSi([guidLow](MeteoJoueur const& m) { return m.joueur == guidLow; }, "arret") +
            LeverParolesSi([guidLow](Parole const& p) { return p.joueur == guidLow; }, "arret") +
            // Lot D4c : sa prime (ses chasseurs, affectations de joueur = lui,
            // sont deja leves `arret` juste au-dessus).
            LeverPrimesSi([guidLow](Prime const& p) { return p.joueur == guidLow; }, "arret") +
            // Lot C des evenements : son ambiance, sa musique, ses declencheurs.
            LeverEvenementsSi([guidLow](uint32, uint32 joueur) { return joueur == guidLow; }, "arret");
        if (levees)
            LOG_INFO("playerbots", "coa directeur : stop de {} leve {} consigne(s)", guidLow, levees);
        if (!DemandeArretTropTot(guidLow, true))
            Enregistrer(joueur, "stop", "");
        handler->PSendSysMessage("Le Directeur s'arrete autour de vous. Tapez .directeur reprise pour le relancer.");
        return true;
    }

    static bool HandleReprise(ChatHandler* handler, char const* /*args*/)
    {
        Player* joueur = JoueurDe(handler);
        if (!joueur)
            return false;
        uint32 const guidLow = joueur->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> garde(verrouArrets);
            arrets.erase(guidLow);
        }
        NoterAPersister(guidLow, false);
        if (!DemandeArretTropTot(guidLow, false))
            Enregistrer(joueur, "reprise", "");
        handler->PSendSysMessage("Le Directeur reprend son travail autour de vous.");
        return true;
    }
};

// C5 : a l'arret du serveur, fermer la file du 8888 AVANT la destruction des
// statiques, et ecrire les arrets encore en attente (C2).
class coa_directeur_worldscript : public WorldScript
{
public:
    coa_directeur_worldscript() : WorldScript("coa_directeur_worldscript", { WORLDHOOK_ON_SHUTDOWN }) {}

    void OnShutdown() override
    {
        arretServeur.store(true, std::memory_order_release);
        std::deque<std::shared_ptr<Requete>> restantes;
        {
            std::lock_guard<std::mutex> garde(verrouFile);
            restantes.swap(file);
        }
        for (auto& r : restantes)
        {
            try { r->reponse.set_value("timeout"); } catch (...) { }
        }
        PersisterArrets(true);
        // Vrai groupe (09/10) : les bots quittent les groupes d'evenement (ils sont gardes en base sinon).
        PurgerGroupesEvenement("arret_serveur");
        LOG_INFO("playerbots", "coa directeur : arret du serveur, file 8888 fermee ({} requete(s) abandonnee(s))",
                 restantes.size());
    }
};

// Lot D4c (SPEC-directeur-D4.md § 3.3) : les crochets de mort. FIL DE CARTE :
// ils ne lisent que la victime et le tueur qu'ils recoivent, et ne font que
// pousser un evenement par valeur (PousserMort). Option TracerMorts a 0 : ils
// rendent la main au premier test.
//  - OnPlayerPVPKill(tueur, victime) : tueur == victime est une mort de
//    l'ENVIRONNEMENT (chute, noyade, lave : Player::EnvironmentalDamage tue par
//    soi-meme, AscensionRulesets.cpp le traite de meme) -> tueur 0, type E ;
//  - OnPlayerKilledByCreature(creature, victime) : Unit::Kill passe un FAMILIER
//    ou un gardien comme Creature (Unit.cpp, « Hook for OnPVPKill Event ») ; le
//    tueur est ramene a son maitre joueur, type C seulement sans maitre.
class coa_directeur_playerscript : public PlayerScript
{
public:
    coa_directeur_playerscript()
        : PlayerScript("coa_directeur_playerscript",
                       { PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE })
    {
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        // Vrai groupe (09/10) : bot tue par un bot pacte pour le meme joueur, independamment de TracerMorts.
        if (killer && killed && killer != killed)
            TracerMortEntrePactes(killed, killer, nullptr);
        if (!sPlayerbotAIConfig.directeurTracerMorts || !killer || !killed)
            return;
        PousserMort(killed, killer == killed ? nullptr : killer, nullptr);
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        // Vrai groupe (09/10) : le familier d'un bot pacte pour le meme joueur.
        if (killer && killed)
            if (Player* proprietaire = killer->GetCharmerOrOwnerPlayerOrPlayerItself())
                if (proprietaire != killed)
                    TracerMortEntrePactes(killed, proprietaire, killer);
        if (!sPlayerbotAIConfig.directeurTracerMorts || !killer || !killed)
            return;
        Player* maitre = killer->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (maitre == killed)
            maitre = nullptr;   // son propre familier : pas un meurtre, la creature reste le tueur
        PousserMort(killed, maitre, maitre ? nullptr : killer);
    }
};

// ================================================================== onglet « Bounty Hunting »
//
// D4 (01/10/2026, remontee R-20260930-2217) : l'addon client CoABounty
// (coa-project/client/CoABounty) ajoute un 3e sous-onglet a la fenetre PvP
// d'Ascension et DEMANDE ses donnees au serveur par un chuchotement d'addon a
// soi-meme : prefixe `COABH`, texte `TOUT` (meme voie que CoABugReport.cpp).
// Le serveur LIT seulement (coa_dir_arc, coa_dir_relation : ecrites par les
// services du Directeur, P-134) et repond par des chuchotements d'addon :
// `COABH\t<i>/<n>|<morceau>`, morceaux de 180 octets au plus. Le corps est fait
// de lignes separees par `^`, champs separes par `|` :
//   V|1                                         version du protocole
//   P|arc|statut|etape|depuis_s|reste_s|plafond|simulee|chasseurs_actifs|vague|pot   prime en cours (ou `P|-`)
//     (chasseurs_actifs, vague : 02/10, EN FIN ; l'addon d'avant lit par index et les ignore.
//     chasseurs_actifs : affectations `chasser` actives de ce joueur, magasin du jeu ;
//     vague : <n> de `etape` = `vague_<n>` (coa_dir_arc, ecrit par primes.py), vide sinon.
//     pot : 03/10 (lot R4), EN FIN : le pot en cours, en cuivres (`params.pot`, ecrit par
//     primes.py a chaque gain), vide sans recompense.)
//   H|arc|issue|etape|tenu_s|vague|tueur|fin_unix|simulee|gain  primes closes, 4 au plus
//     (gain : 03/10 (lot R4), EN FIN : cuivres encaisses, `params.resultat.recompense.encaisse_cuivre`
//     -- 0 pour une prime perdue --, vide sans recompense.)
//   R|bot|nom|score|morts_du_joueur|morts_du_bot           rivaux, 4 au plus
//   W|rang|nom|niveau|zone|prime_c|reste_s|rival         avis de recherche, 3 au plus (lot T3, option Directeur.Avis)
// (4 : la fenetre PvP n'en tient pas plus ; `simulee` vaut 1 sauf `false` explicite, comme primes.py)
//   F                                           fin
// Une demande par joueur toutes les ONGLET_INTERVALLE_MS au plus (le reste est
// ignore, sans reponse). Requetes synchrones bornees (LIMIT), au fil du joueur,
// sur des tables de quelques lignes : seulement a l'ouverture de l'onglet et a
// son rafraichissement (10 s, addon). `TOUT` ne lit aucun objet de jeu, hors le
// joueur lui-meme ; `TRAQUE` (lot T1, plus bas) lit aussi la position du bot cible,
// au fil monde (cartes a l'arret).
namespace
{
constexpr char ONGLET_PREFIXE[] = "COABH\t";
constexpr size_t ONGLET_MORCEAU = 180;
constexpr uint32 ONGLET_INTERVALLE_MS = 2000;

std::mutex verrouOnglet;   // CMSG_MESSAGECHAT est traite par le fil monde (PROCESS_THREADUNSAFE) : ceinture
std::unordered_map<uint32, uint32> derniereDemandeOnglet;   // guid -> getMSTime()

// Un champ du protocole : ni separateur de champ, ni de ligne, ni retour.
std::string ChampOnglet(std::string v)
{
    for (char& ch : v)
        if (ch == '|' || ch == '^' || ch == '\n' || ch == '\r' || ch == '\t')
            ch = ' ';
    if (v.size() > 48)
        v.resize(48);
    return v;
}

std::string ChampOnglet(Field const& f)
{
    return f.IsNull() ? std::string() : ChampOnglet(f.Get<std::string>());
}

// Lot D5 (15/10/2026) : derniere audace / opt-in lus pour l'onglet, par guid, SOUS verrouOnglet. Remplis par une
// requete ASYNCHRONE (processeur de la session du joueur) posee a chaque demande `TOUT` : le fil monde n'attend
// jamais MySQL pour cette ligne. La valeur disponible est celle lue pour la demande precedente ; elle n'est servie
// que si elle a moins de IMPREVU_ONGLET_AGE_MS. Sinon (premiere demande, onglet rouvert apres une pause) la ligne `^A`
// est OMISE : l'addon retombe sur son dernier envoi au lieu de montrer un etat perime comme celui du serveur.
constexpr uint32 IMPREVU_ONGLET_AGE_MS = 3000;
struct ImprevuOnglet
{
    std::string audace;   // coa_dir_audace.niveau du COMPTE, vide sans ligne
    std::string optin;    // coa_dir_imprevu_optin.optin du personnage, "1"/"0", vide sans ligne
    uint32 luMs = 0;      // getMSTime() a la reception du resultat
};
std::unordered_map<uint32, ImprevuOnglet> imprevuOnglet;

void RelireImprevuOnglet(uint32 guid, WorldSession* session)
{
    if (!session || (!tableAudaceLue && !tableOptinLue))
        return;
    std::string const sql = Acore::StringFormat(
        "SELECT {}, {}",
        tableAudaceLue ? Acore::StringFormat("(SELECT CAST(a.niveau AS CHAR) FROM coa_dir_audace a JOIN characters c"
                                             " ON c.account = a.compte WHERE c.guid = {})", guid)
                       : std::string("NULL"),
        tableOptinLue ? Acore::StringFormat("(SELECT CAST(o.optin AS CHAR) FROM coa_dir_imprevu_optin o WHERE o.guid = {})",
                                            guid)
                      : std::string("NULL"));
    // Rappel : traite par le processeur de la session (WorldSession::Update), qui meurt avec elle -- une session
    // fermee entre-temps ne rappelle jamais. Ne capture que le guid ; n'ecrit que le cache, sous son verrou.
    session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(sql).WithCallback(
        [guid](QueryResult r)
        {
            ImprevuOnglet e;
            e.luMs = getMSTime();
            if (r)
            {
                Field* f = r->Fetch();
                e.audace = ChampOnglet(f[0]);
                std::string const o = ChampOnglet(f[1]);
                e.optin = o.empty() ? std::string() : (o == "0" ? "0" : "1");
            }
            std::lock_guard<std::mutex> garde(verrouOnglet);
            if (imprevuOnglet.size() > 512)
                imprevuOnglet.clear();
            imprevuOnglet[guid] = e;
        }));
}

std::string CorpsOnglet(uint32 guid, WorldSession* session)
{
    std::string corps = "V|1";
    // Prime en cours (demande ou active).
    QueryResult p = CharacterDatabase.Query(
        "SELECT id, CAST(statut AS CHAR), IFNULL(etape, ''),"
        " CAST(TIMESTAMPDIFF(SECOND, IFNULL(debut, cree_le), UTC_TIMESTAMP()) AS CHAR),"
        // 03/10 (arc 25, en jeu : « reste 12 min » pour une prime de 10 min) : `fin_prevue` porte
        // la marge de balayage du service (primes.MARGE_FIN_S = 600 s, en plus de la duree) ;
        // l'echeance vue du joueur est fin_prevue - 600 s. Une prime prolongee (donjon) l'est
        // aussi par fin_prevue, donc la soustraction reste juste.
        " CAST(IFNULL(GREATEST(TIMESTAMPDIFF(SECOND, UTC_TIMESTAMP(), fin_prevue) - 600, 0), -1) AS CHAR),"
        " IFNULL(JSON_UNQUOTE(JSON_EXTRACT(params, '$.plafond')), ''),"
        " IFNULL(JSON_UNQUOTE(JSON_EXTRACT(params, '$.simulee')), ''),"
        " IFNULL(NULLIF(JSON_UNQUOTE(JSON_EXTRACT(params, '$.pot')), 'null'), '')"
        " FROM coa_dir_arc WHERE joueur = {} AND genre = 'prime' AND statut IN ('demande', 'actif')"
        " ORDER BY id DESC LIMIT 1", guid);
    if (p)
    {
        Field* f = p->Fetch();
        // 02/10 : chasseurs actifs (le magasin des affectations, LU sous le verrou
        // partage : il est ecrit par le fil monde, qui traite aussi ce paquet) et
        // la vague, lue dans `etape` (`vague_<n>`, primes.py).
        uint32 chasseursActifs = 0;
        if (nbChasseurs.load(std::memory_order_relaxed))
        {
            uint32 const maintenant = Maintenant();
            std::shared_lock<std::shared_mutex> garde(verrouConsignes);
            for (auto const& [bot, a] : affectations)
                if (a.a.chasseur && a.joueur == guid && a.a.finS > maintenant)
                    ++chasseursActifs;
        }
        std::string const etape = ChampOnglet(f[2]);
        std::string vague;
        if (etape.rfind("vague_", 0) == 0 && etape.size() > 6 && etape.size() <= 9 &&
            std::all_of(etape.begin() + 6, etape.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
            vague = etape.substr(6);
        corps += Acore::StringFormat("^P|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", f[0].Get<uint32>(), ChampOnglet(f[1]), etape,
                                     ChampOnglet(f[3]), ChampOnglet(f[4]), ChampOnglet(f[5]),
                                     ChampOnglet(f[6]) == "false" ? 0 : 1, chasseursActifs, vague, ChampOnglet(f[7]));
    }
    else
        corps += "^P|-";
    // Primes closes : le resultat de D4h (params.resultat) quand il existe, sinon la duree mesuree.
    if (QueryResult h = CharacterDatabase.Query(
            "SELECT id, IFNULL(raison, ''), IFNULL(etape, ''),"
            " IFNULL(NULLIF(JSON_UNQUOTE(JSON_EXTRACT(params, '$.resultat.tenu_s')), 'null'),"
            " CAST(TIMESTAMPDIFF(SECOND, IFNULL(debut, cree_le), IFNULL(fin, cree_le)) AS CHAR)),"
            " IFNULL(NULLIF(JSON_UNQUOTE(JSON_EXTRACT(params, '$.resultat.vague')), 'null'), ''),"
            " IFNULL(NULLIF(JSON_UNQUOTE(JSON_EXTRACT(params, '$.resultat.tueur.nom')), 'null'), ''),"
            " CAST(UNIX_TIMESTAMP(IFNULL(fin, cree_le)) AS CHAR), IFNULL(JSON_UNQUOTE(JSON_EXTRACT(params, '$.simulee')), ''),"
            " IFNULL(NULLIF(JSON_UNQUOTE(JSON_EXTRACT(params, '$.resultat.recompense.encaisse_cuivre')), 'null'), '')"
            " FROM coa_dir_arc WHERE joueur = {} AND genre = 'prime' AND statut IN ('clos', 'refuse')"
            " ORDER BY id DESC LIMIT 4", guid))
    {
        do
        {
            Field* f = h->Fetch();
            corps += Acore::StringFormat("^H|{}|{}|{}|{}|{}|{}|{}|{}|{}", f[0].Get<uint32>(), ChampOnglet(f[1]),
                                         ChampOnglet(f[2]), ChampOnglet(f[3]), ChampOnglet(f[4]), ChampOnglet(f[5]),
                                         ChampOnglet(f[6]), ChampOnglet(f[7]) == "false" ? 0 : 1, ChampOnglet(f[8]));
        } while (h->NextRow());
    }
    // Rivaux.
    if (QueryResult r = CharacterDatabase.Query(
            "SELECT r.bot, IFNULL(c.name, ''), r.score, r.morts_du_joueur, r.morts_du_bot FROM coa_dir_relation r"
            " LEFT JOIN characters c ON c.guid = r.bot WHERE r.joueur = {} AND r.genre = 'rival'"
            " ORDER BY r.score DESC, r.derniere DESC LIMIT 4", guid))
    {
        do
        {
            Field* f = r->Fetch();
            corps += Acore::StringFormat("^R|{}|{}|{}|{}|{}", f[0].Get<uint32>(), ChampOnglet(f[1]), f[2].Get<int32>(),
                                         f[3].Get<uint32>(), f[4].Get<uint32>());
        } while (r->NextRow());
    }
    // Lot T3 (12/10/2026) : les avis de recherche du joueur (coa_dir_avis, ecrite par contrats.py), si l'option
    // le permet (la table n'existe qu'apres sa migration). EN FIN, avant `F` : un addon d'avant ignore la ligne.
    if (sPlayerbotAIConfig.directeurAvis)
    {
        if (QueryResult w = CharacterDatabase.Query(
                "SELECT rang, nom, niveau, zone, montant_c,"
                " CAST(GREATEST(TIMESTAMPDIFF(SECOND, UTC_TIMESTAMP(), fin), 0) AS CHAR), rival"
                " FROM coa_dir_avis WHERE joueur = {} AND fin > UTC_TIMESTAMP() ORDER BY rang LIMIT 3", guid))
        {
            do
            {
                Field* f = w->Fetch();
                corps += Acore::StringFormat("^W|{}|{}|{}|{}|{}|{}|{}", uint32(f[0].Get<uint8>()), ChampOnglet(f[1]),
                                             uint32(f[2].Get<uint8>()), ChampOnglet(f[3]), f[4].Get<uint32>(),
                                             ChampOnglet(f[5]), uint32(f[6].Get<uint8>()));
            } while (w->NextRow());
        }
    }
    // Lot D5 (15/10/2026) : `^A|audace|optin` (CoABounty 1.7, Lire : c[1] == "A"), EN FIN avant `F` : un addon
    // d'avant ignore la ligne. Valeurs du cache (requete asynchrone de la demande precedente), vides si la ligne
    // manque en base ; ligne OMISE si le cache est absent ou a IMPREVU_ONGLET_AGE_MS ou plus (revue de 154446f).
    {
        bool frais = false;
        ImprevuOnglet e;
        {
            std::lock_guard<std::mutex> garde(verrouOnglet);
            auto it = imprevuOnglet.find(guid);
            if (it != imprevuOnglet.end() && getMSTimeDiff(it->second.luMs, getMSTime()) < IMPREVU_ONGLET_AGE_MS)
            {
                e = it->second;
                frais = true;
            }
        }
        if (frais)
            corps += Acore::StringFormat("^A|{}|{}", e.audace, e.optin);
        RelireImprevuOnglet(guid, session);
    }
    corps += "^F";
    return corps;
}

// ---- traqueur de contrat, lot T1 (12/10/2026, coa-project/docs/CONCEPTION-primes-traqueur-bots.md)
//
// L'addon chuchote `COABH\tTRAQUE` a soi-meme ; le serveur repond UN chuchotement d'addon
// `COABT\t<ligne>` (prefixe distinct de COABH : jamais melange au reassemblage des morceaux de TOUT) :
//   T|-                                                    aucun contrat actif
//   T|arc|etat|nom|distance_yd|angle_deg|zone|reste_s|intervalle_ms
//     etat : `prep` (contrat en preparation, cible pas encore confirmee), `piste` (meme carte, vivante),
//            `refuge` (meme carte, sans drapeau FFA : lieu sans PvP), `carte` (autre carte : zone seule),
//            `mort`, `absent` (cible hors ligne) ;
//     distance_yd : 2D, arrondie (5 yd sous 100, 10 sous 500, 50 au-dela) ; angle_deg : 0-359, direction
//     ABSOLUE du joueur vers la cible (0 = nord, sens trigonometrique, comme GetPlayerFacing du client) ;
//     vides hors `piste` et `refuge` ; zone : nom de la zone de la cible (vide si `prep`, `absent`) ;
//     reste_s : fin_prevue - 600 s (ARC_MARGE_S de contrats.py), -1 si illisible.
//   Lot T6 (13/10/2026), EN FIN (un addon d'avant ne les lit pas) : `|offre_c|offre_reste_s`, l'offre de soudoiement
//     ouverte de la cible (reactions des cibles, OffreOuverte : cuivres, secondes restantes), vides sans offre.
// Lecture seule : une requete bornee (LIMIT 1) sur coa_dir_arc (ecrite par contrats.py, P-134), la
// cible lue par ObjectAccessor. CMSG_MESSAGECHAT est traite par le fil monde (PROCESS_THREADUNSAFE,
// Opcodes.cpp) : les cartes sont a l'arret, la position du bot se lit sans course (P-137).
constexpr char TRAQUE_PREFIXE[] = "COABT\t";
constexpr char TRAQUE_DEMANDE[] = "TRAQUE";
constexpr uint32 TRAQUE_MARGE_ARC_S = 600;   // contrats.ARC_MARGE_S
constexpr uint32 TRAQUE_RESTE_MAX_S = 2100;  // contrats.DUREE_S + PREP_MAX_S

std::unordered_map<uint32, uint32> derniereTraque;       // guid -> getMSTime(), sous verrouOnglet

// Revue 16/10 : l'addon sonde toutes les 5 s meme sans contrat ; chaque sondage payait une requete synchrone au fil
// monde, pour repondre `T|-` presque toujours. Une SEULE requete, au plus toutes les TRAQUE_CONTRATS_RELIRE_MS, dit
// quels joueurs ont un contrat actif ; les autres recoivent `T|-` sans base. Un contrat accepte se voit au plus 2 s
// plus tard ; un contrat clos depuis la lecture retombe sur la requete complete, qui repond `T|-` comme avant.
// Fil monde seul (CorpsTraqueur), sous verrouOnglet par prudence.
constexpr uint32 TRAQUE_CONTRATS_RELIRE_MS = 2000;
constexpr uint32 TRAQUE_CONTRATS_MAX = 4096;
std::unordered_set<uint32> traqueAvecContrat;
uint32 traqueContratsLusMs = 0;
bool traqueContratsValides = false;   // faux : lecture en echec ou tronquee -> requete par joueur, comme avant

// -> faux si le joueur n'a surement aucun contrat actif (la requete par joueur est alors inutile).
bool PeutAvoirContrat(uint32 guid)
{
    uint32 const maintenantMs = getMSTime();
    std::lock_guard<std::mutex> garde(verrouOnglet);
    if (!traqueContratsLusMs || getMSTimeDiff(traqueContratsLusMs, maintenantMs) >= TRAQUE_CONTRATS_RELIRE_MS)
    {
        traqueContratsLusMs = maintenantMs ? maintenantMs : 1;
        traqueAvecContrat.clear();
        QueryResult r = CharacterDatabase.Query(
            "SELECT DISTINCT joueur FROM coa_dir_arc WHERE genre = 'contrat' AND statut = 'actif' LIMIT {}",
            TRAQUE_CONTRATS_MAX);
        if (r)
        {
            do
                traqueAvecContrat.insert(r->Fetch()[0].Get<uint32>());
            while (r->NextRow());
            traqueContratsValides = traqueAvecContrat.size() < TRAQUE_CONTRATS_MAX;
        }
        else
            // `nullptr` = aucun contrat OU requete en echec : une requete temoin tranche.
            traqueContratsValides = static_cast<bool>(CharacterDatabase.Query("SELECT 1"));
    }
    return !traqueContratsValides || traqueAvecContrat.count(guid) != 0;
}
// guid -> (arc, etat) de la derniere ligne journalisee, sous verrouOnglet : une ligne par changement d'etat
// d'un contrat (prep -> piste, piste <-> refuge, carte, mort...), jamais une par lecture.
std::unordered_map<uint32, std::pair<uint32, std::string>> traqueJournalisee;

uint32 DistanceTraqueur(float d)
{
    if (!std::isfinite(d) || d < 0.0f)
        return 0;
    if (d < 100.0f)
        return static_cast<uint32>(std::lround(d / 5.0f)) * 5;
    if (d < 500.0f)
        return static_cast<uint32>(std::lround(d / 10.0f)) * 10;
    return static_cast<uint32>(std::lround(d / 50.0f)) * 50;
}

std::string NomZoneTraqueur(uint32 zoneId)
{
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId);
    return zone ? ChampOnglet(PlayerbotAI::GetLocalizedAreaName(zone)) : std::string();
}

// Un guid lu en texte (JSON) : chiffres seuls, 1 a 10, <= UINT32_MAX ; sinon 0.
uint32 GuidTraqueur(std::string const& v)
{
    if (v.empty() || v.size() > 10 || !std::all_of(v.begin(), v.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        return 0;
    uint64 const n = std::stoull(v);
    return n <= std::numeric_limits<uint32>::max() ? static_cast<uint32>(n) : 0;
}

// La ligne du traqueur pour `joueur`. Sorties, pour le journal : arc et cible lus, etat, distance.
std::string CorpsTraqueur(Player* joueur, uint32 intervalleMs, uint32& arcLu, uint32& cibleLue, std::string& etatLu,
                          uint32& distanceLue)
{
    arcLu = cibleLue = distanceLue = 0;
    etatLu = "-";
    uint32 const guid = joueur->GetGUID().GetCounter();
    if (!PeutAvoirContrat(guid))
        return "T|-";
    QueryResult a = CharacterDatabase.Query(
        "SELECT id, IFNULL(etape, ''),"
        " IFNULL(NULLIF(JSON_UNQUOTE(JSON_EXTRACT(params, '$.cible')), 'null'), ''),"
        " IFNULL(NULLIF(JSON_UNQUOTE(JSON_EXTRACT(params, '$.cible_nom')), 'null'), ''),"
        " CAST(IFNULL(GREATEST(TIMESTAMPDIFF(SECOND, UTC_TIMESTAMP(), fin_prevue) - {}, 0), -1) AS CHAR)"
        " FROM coa_dir_arc WHERE joueur = {} AND genre = 'contrat' AND statut = 'actif'"
        " ORDER BY id DESC LIMIT 1",
        TRAQUE_MARGE_ARC_S, guid);
    if (!a)
        return "T|-";
    Field* f = a->Fetch();
    arcLu = f[0].Get<uint32>();
    std::string const etape = ChampOnglet(f[1]);
    uint32 const cible = GuidTraqueur(f[2].IsNull() ? std::string() : f[2].Get<std::string>());
    std::string const nom = ChampOnglet(f[3]);
    // Pendant une pause en donjon, contrats.py repousse fin_prevue d'une heure de plus (PAUSE_MAX_S) : au-dela de
    // la duree d'un contrat (30 min + 5 min de preparation), l'echeance n'est pas lisible ici, `-1`.
    std::string reste = ChampOnglet(f[4]);
    if (uint32 const r = GuidTraqueur(reste); r > TRAQUE_RESTE_MAX_S)
        reste = "-1";
    std::string etat;
    std::string distance;
    std::string angle;
    std::string zone;
    Player* bot = cible ? JoueurConnecte(cible) : nullptr;
    cibleLue = cible;
    if (etape != "piste")
        etat = "prep";   // preparation (`chasser P-0`) : la cible n'est pas encore confirmee hors de vue
    else if (!bot || !bot->IsInWorld() || bot == joueur || !sRandomPlayerbotMgr.IsRandomBot(bot))
        etat = "absent";   // jamais la position d'un humain ni d'un altbot : la cible est un bot aleatoire de la reserve
    else
    {
        zone = NomZoneTraqueur(bot->GetZoneId());
        if (bot->GetMapId() != joueur->GetMapId() || bot->GetInstanceId() != joueur->GetInstanceId())
            etat = "carte";
        else if (!bot->IsAlive())
            etat = "mort";
        else
        {
            etat = bot->IsFFAPvP() ? "piste" : "refuge";
            uint32 const d = DistanceTraqueur(joueur->GetExactDist2d(bot));
            float const rad = joueur->GetAbsoluteAngle(bot);   // [0, 2 pi[, 0 = nord (+x), sens trigonometrique
            uint32 const deg = static_cast<uint32>(std::lround(rad * 180.0f / static_cast<float>(M_PI))) % 360;
            distanceLue = d;
            distance = std::to_string(d);
            angle = std::to_string(deg);
        }
    }
    etatLu = etat;
    // Lot T6 : l'offre de soudoiement (reactionsContrat, ecrit et lu au fil monde seul).
    uint32 offreC = 0;
    uint32 offreResteS = 0;
    bool const offre = OffreOuverte(arcLu, offreC, offreResteS);
    return Acore::StringFormat("T|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", arcLu, etat, nom, distance, angle, zone, reste,
                               intervalleMs, offre ? std::to_string(offreC) : std::string(),
                               offre ? std::to_string(offreResteS) : std::string());
}

// Repond a `COABH\tTRAQUE`. Option eteinte, ou demande trop rapprochee : rien n'est envoye.
void RepondreTraqueur(Player* player)
{
    if (!sPlayerbotAIConfig.directeurTraqueur)
        return;
    uint32 const guid = player->GetGUID().GetCounter();
    uint32 const intervalleMs = sPlayerbotAIConfig.directeurTraqueurIntervalleMs;
    uint32 const maintenantMs = getMSTime();
    {
        std::lock_guard<std::mutex> garde(verrouOnglet);
        auto it = derniereTraque.find(guid);
        // Tolerance d'un quart : le minuteur de l'addon (OnUpdate) arrive parfois quelques ms avant.
        if (it != derniereTraque.end() && getMSTimeDiff(it->second, maintenantMs) < intervalleMs - intervalleMs / 4)
            return;
        if (derniereTraque.size() > 512)
            derniereTraque.clear();
        derniereTraque[guid] = maintenantMs;
    }
    uint32 arc = 0;
    uint32 cible = 0;
    uint32 distance = 0;
    std::string etat;
    std::string const ligne = CorpsTraqueur(player, intervalleMs, arc, cible, etat, distance);
    if (arc)
    {
        bool change = false;
        {
            std::lock_guard<std::mutex> garde(verrouOnglet);
            if (traqueJournalisee.size() > 512)
                traqueJournalisee.clear();
            auto [it, neuf] = traqueJournalisee.try_emplace(guid, arc, etat);
            change = neuf || it->second.first != arc || it->second.second != etat;
            it->second = {arc, etat};
        }
        if (change)
            LOG_INFO("playerbots", "coa directeur traqueur: joueur={} arc={} cible={} etat={} distance={} intervalle_ms={}",
                     guid, arc, cible, etat, distance, intervalleMs);
    }
    std::string const texte = std::string(TRAQUE_PREFIXE) + ligne;
    WorldPacket paquet;
    ChatHandler::BuildChatPacket(paquet, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(), texte, 0,
                                 player->GetName(), player->GetName(), 0, false);
    player->SendDirectMessage(&paquet);
}

// Carte des evenements (17/10/2026) : prefixe COAEV, addon CoAMonde. Option AiPlayerbot.Directeur.CarteEvenements.
#include "CoaDirecteurCarte.inc"
}  // namespace

class coa_directeur_onglet_playerscript : public PlayerScript
{
public:
    coa_directeur_onglet_playerscript()
        : PlayerScript("coa_directeur_onglet_playerscript", {PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT})
    {
    }

    using PlayerScript::OnPlayerCanUseChat;

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message,
                            Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER || language != LANG_ADDON || !player)
            return true;
        // Carte des evenements (COAEV, CoaDirecteurCarte.inc) : consommee dans tous les cas, jamais transmise.
        if (message.compare(0, sizeof(CARTE_PREFIXE) - 1, CARTE_PREFIXE) == 0)
        {
            if (receiver == player && message.size() <= 255)
                RepondreCarte(player, message.substr(sizeof(CARTE_PREFIXE) - 1));
            return false;
        }
        if (message.compare(0, sizeof(ONGLET_PREFIXE) - 1, ONGLET_PREFIXE) != 0)
            return true;
        // Le protocole est consomme dans tous les cas : jamais transmis a un autre joueur.
        if (receiver != player || message.size() > 255)
            return false;
        std::string const demande = message.substr(sizeof(ONGLET_PREFIXE) - 1);
        if (demande == TRAQUE_DEMANDE)
        {
            RepondreTraqueur(player);   // lot T1 : traqueur de contrat (option Directeur.Traqueur)
            return false;
        }
        if (demande != "TOUT")
            return false;
        uint32 const guid = player->GetGUID().GetCounter();
        uint32 const maintenantMs = getMSTime();
        {
            std::lock_guard<std::mutex> garde(verrouOnglet);
            auto it = derniereDemandeOnglet.find(guid);
            if (it != derniereDemandeOnglet.end() && getMSTimeDiff(it->second, maintenantMs) < ONGLET_INTERVALLE_MS)
                return false;
            if (derniereDemandeOnglet.size() > 512)
                derniereDemandeOnglet.clear();
            derniereDemandeOnglet[guid] = maintenantMs;
        }
        std::string const corps = CorpsOnglet(guid, player->GetSession());
        // Morceaux de ONGLET_MORCEAU octets au plus, jamais au milieu d'un caractere
        // UTF-8 (revue m3) : la coupe recule sur un octet de continuation 10xxxxxx.
        std::vector<std::string> morceaux;
        for (size_t debut = 0; debut < corps.size();)
        {
            size_t fin = std::min(corps.size(), debut + ONGLET_MORCEAU);
            while (fin < corps.size() && fin > debut + 1 && (static_cast<unsigned char>(corps[fin]) & 0xC0) == 0x80)
                --fin;
            morceaux.push_back(corps.substr(debut, fin - debut));
            debut = fin;
        }
        size_t const n = morceaux.size();
        for (size_t i = 0; i < n; ++i)
        {
            std::string const texte = Acore::StringFormat("{}{}/{}|{}", ONGLET_PREFIXE, i + 1, n, morceaux[i]);
            WorldPacket paquet;
            ChatHandler::BuildChatPacket(paquet, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(),
                                         texte, 0, player->GetName(), player->GetName(), 0, false);
            player->SendDirectMessage(&paquet);
        }
        return false;
    }
};

// Lot D des evenements (05/10/2026) : le NIVEAU d'un monstre d'evenement. Creature::SelectLevel
// (Creature.cpp) appelle OnBeforeCreatureSelectLevel pendant la creation (UpdateEntry). Seul
// ExecuterInvoquer pose evNiveauForce, sur le fil monde et autour de son appel a
// Map::SummonCreature ; partout ailleurs il vaut 0 et ce crochet ne fait rien. La plage
// 9100000-9109999 est exclue de l'echelle de mod-ascension-compat (CanScale). Sans cette exclusion,
// notre niveau gagne a la creation (scripts d'Ascension enregistres avant les notres, ScriptRegistry
// les parcourt dans l'ordre) mais OnAllCreatureUpdate d'Ascension le reecrit ensuite : SuivreInvoques
// le lit chaque seconde et RETIRE l'invoque (`motif=echelle`).
// OnCreatureAddWorld (relecture du 05/10) : signale les RENFORTS, creatures de la plage invoquees par
// une autre creature (TempSummon a invocateur), depuis le fil de leur carte ; ne lit que la creature
// elle-meme et ne touche que la file sous verrou (DrainerRenforts l'inscrit au fil monde).
class coa_directeur_niveau_creaturescript : public AllCreatureScript
{
public:
    coa_directeur_niveau_creaturescript() : AllCreatureScript("coa_directeur_niveau_creaturescript") {}

    void OnBeforeCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* creature, uint8& level) override
    {
        if (!evNiveauForce || !creature)
            return;
        uint32 const entree = creature->GetEntry();
        if (entree < EV_ENTREE_MIN || entree > EV_ENTREE_MAX)
            return;
        level = evNiveauForce;
    }

    void OnCreatureAddWorld(Creature* creature) override
    {
        if (!creature)
            return;
        uint32 const entree = creature->GetEntry();
        if (entree < EV_ENTREE_MIN || entree > EV_ENTREE_MAX)
            return;
        TempSummon const* ts = creature->ToTempSummon();
        if (!ts)
            return;
        ObjectGuid const invocateur = ts->GetSummonerGUID();
        if (invocateur.IsEmpty())
            return;   // pose par ExecuterInvoquer (sans invocateur) : deja au registre
        std::lock_guard<std::mutex> garde(evRenfortsVerrou);
        if (evRenfortsVus.size() >= EV_RENFORTS_FILE_MAX)
        {
            ++evRenfortsPerdus;
            return;
        }
        evRenfortsVus.push_back(EvRenfortVu{creature->GetGUID(), invocateur, creature->GetMapId()});
    }
};

// Butin des bots aux evenements (12/10/2026) : fonctions publiques, hors de l'espace de noms anonyme.
#include "CoaDirecteurButin.inc"

void AddSC_coa_directeur()
{
    new coa_directeur_niveau_creaturescript();
    new coa_directeur_soin_unitscript();    // lot S des evenements (crochet OnHeal : compteur `soins`)
    new coa_directeur_portail_goscript();   // lot P des evenements
    new coa_directeur_barricade_goscript(); // convoi v2 : la barricade a demolir
    new coa_directeur_dose_unitscript();       // lot G : degats doses
    new coa_directeur_coffre_globalscript();   // lot G : qui ouvre le coffre
    new coa_directeur_coffre_playerscript();   // lot G : chacun sa tranche au coffre
    new coa_directeur_ressources_globalscript();   // ressources rares : tranche du niveau du monstre
    new coa_directeur_commandscript();
    new coa_directeur_worldscript();
    new coa_directeur_playerscript();
    new coa_directeur_onglet_playerscript();
}
