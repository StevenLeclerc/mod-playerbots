/*
 * CoA -- le Directeur, lot D1. Voir CoaDirecteur.h pour le pourquoi.
 */

#include "CoaDirecteur.h"
#include "CoaDirecteurSource.h"

#include "Channel.h"
#include "ChannelMgr.h"
#include "Chat.h"
#include "Corpse.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GridDefines.h"
#include "GridTerrainData.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MercenaryRewards.h"
#include "MiscPackets.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "Weather.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
std::unordered_map<uint32, PacteCompagnon> pactes;   // cle : guid bas du bot
std::atomic<uint32> nbPactes{0};

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
    return CoaDirVerbe::Inconnu;
}

// Verbes du lot D3 : ils exigent les colonnes de la migration D3 (§ 3.2).
bool VerbeD3(CoaDirVerbe v)
{
    return v == CoaDirVerbe::Peupler || v == CoaDirVerbe::FaireParler || v == CoaDirVerbe::Repondre ||
           v == CoaDirVerbe::HeureDuJour || v == CoaDirVerbe::Meteo;
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
           v == CoaDirVerbe::Alleger || VerbeD3(v);
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
        _colonnesD3 = nbColonnes == 5 && typeVerbe == ENUM_VERBE_D3;
        if (_colonnesD3)
            LOG_INFO("playerbots", "coa directeur : colonnes D3 presentes, verbes D3 lisibles");
        else
            LOG_INFO("playerbots", "coa directeur : colonnes D3 absentes, verbes D3 inertes (colonnes={}/5, verbe={})",
                     nbColonnes, typeVerbe.empty() ? std::string("?") : typeVerbe);
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

    QueryCallbackProcessor _queries;
    std::vector<CoaDirCommande> _pretes;
    uint32 _dernierId = 0;
    uint32 _attenteMs = 0;
    uint32 _balayageMs = 0;
    bool _enVol = false;
    bool _colonnesD3 = false;
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

bool PrendreJeton()
{
    double const capacite = sPlayerbotAIConfig.directeurMaxCommandesParMinute;
    uint32 const maintenant = getMSTime();
    if (jetons < 0.0)
        jetons = capacite;
    else
        jetons = std::min(capacite, jetons + getMSTimeDiff(jetonsMs, maintenant) * capacite / 60000.0);
    jetonsMs = maintenant;
    if (jetons < 1.0)
        return false;
    jetons -= 1.0;
    return true;
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

constexpr uint32 FILET_MS = 30 * IN_MILLISECONDS;
constexpr uint32 FILET_LEVEE_S = 5 * MINUTE;       // surveillance apres une levee
constexpr size_t RECONSTRUCTIONS_PAR_SECONDE = 32;

std::unordered_set<uint32> aReconstruire;            // guid bas
std::unordered_map<uint32, uint32> surveillesJusqua; // guid -> fin de surveillance (s)
std::unordered_set<uint32> filetFait;                // le filet a deja relance ce guid
uint32 filetMs = 0;

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
        nbAffectations.store(static_cast<uint32>(affectations.size()), std::memory_order_relaxed);
    }
    for (AffectationInterne const& f : levees)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        FinirAffectation(f, "leve", raison, raison);
    }
    return levees.size();
}

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

    enum class Issue : uint8 { Arrivee, Levee, Echue, Niveau, Releve, EtapeArrivee, Centre };
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
        if (ia->HasGameClientMaster())
        {
            lever(f, "maitre_humain", CoaDirecteur::COA_DIR_NB_COMPTEURS);
            continue;
        }
        // Lot D3 : une presence n'a de sens qu'autour du joueur qui l'a
        // demandee. Deconnecte (FindConnectedPlayer : une teleportation
        // lointaine n'est pas une deconnexion), la presence tombe.
        if (f.a.presence && (!f.joueur || !JoueurConnecte(f.joueur)))
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
        if (f.mercenaireRpg && !CoaDirecteur::MercenaireEnRpg(f.guid))
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
        if (!f.etapeArrivee && maintenant - f.poseS >= trajetMax)
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
        nbAffectations.store(static_cast<uint32>(affectations.size()), std::memory_order_relaxed);
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
    for (AffectationInterne const& r : releves)
        JournaliserFinAffectation(r, "arrivee_5min");
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

constexpr float PRESENCE_A_VUE_YD = 150.0f;       // visibilite 100 yd (worldserver.conf) + marge
constexpr float PRESENCE_CENTRE_MIN_YD = 40.0f;   // centre du disque, depuis le joueur a la pose
constexpr float PRESENCE_CENTRE_MAX_YD = 300.0f;

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
    paroles.push_back(Parole{c.id, c.cible, c.joueur, c.canal, c.texte, maintenantMs, c.delaiS * IN_MILLISECONDS});
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    Accuser(c, "execute", nullptr);
}

void JournaliserParole(Parole const& p, char const* statut, char const* raison)
{
    LOG_INFO("playerbots", "coa directeur parole: id={} guid={} joueur={} canal={} statut={} raison={}", p.id, p.bot,
             p.joueur, NomCanal(p.canal), statut, raison ? raison : "-");
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
        if (!perdue)
        {
            bool const memeCarte = bot->GetMapId() == joueur->GetMapId();
            float const distance = memeCarte ? bot->GetExactDist(joueur) : 1e9f;
            switch (p.canal)
            {
                case CoaDirCanal::Dire:
                    if (distance > PAROLE_DIRE_YD)
                        perdue = "loin";
                    else
                        ia->Say(p.texte);
                    break;
                case CoaDirCanal::Crier:
                    if (distance > PAROLE_CRIER_YD)
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
            JournaliserParole(p, "perdue", perdue);
        }
        else
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PAROLES_LIVREES);
            JournaliserParole(p, "livree", reserve);
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

// Entretien de chaque seconde, bus allume OU eteint : la purge d'un bus
// eteint met des mercenaires en file, et il faut bien les reconstruire.
void Entretien(uint32 diff)
{
    secondeMs += diff;
    if (secondeMs < IN_MILLISECONDS)
        return;
    secondeMs = 0;
    LeverEchues();
    LeverSuspensionsEchues();
    LeverSuspensionsMaitreHumain();
    // D2b : APRES les fins de suspension (levee `fin_suspension` dans la meme
    // seconde) et AVANT la file de reconstruction (§ 6.3).
    SuivreAffectations();
    // APRES le suivi : une mort constatee a cette seconde y a pose son etat.
    SuivreMortsEnTrajet();
    SuivreTrajetsLeves();
    SuivrePactes();
    // Lot D3 : ciels, meteos, paroles (fil monde seul).
    SuivreCiels();
    SuivreMeteos();
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
                  LeverSuspensionsSi([](Suspension const&) { return true; }, "lever") +
                  LeverAffectationsSi([](AffectationInterne const&) { return true; }, "lever") +
                  LeverCielsSi([](Ciel const&) { return true; }, "lever") +
                  LeverMeteosSi([](MeteoJoueur const&) { return true; }, "lever") +
                  LeverParolesSi([](Parole const&) { return true; }, "lever")
            : LeverSi([&c](Treve const& t) { return t.id == c.cible; }, "lever") +
                  LeverSuspensionsSi([&c](Suspension const& s) { return s.id == c.cible; }, "lever") +
                  LeverAffectationsSi([&c](AffectationInterne const& f) { return f.a.id == c.cible; }, "lever") +
                  LeverCielsSi([&c](Ciel const& x) { return x.id == c.cible; }, "lever") +
                  LeverMeteosSi([&c](MeteoJoueur const& m) { return m.id == c.cible; }, "lever") +
                  LeverParolesSi([&c](Parole const& p) { return p.id == c.cible; }, "lever");
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
    // Compagnons (27/09) : un peupler d'action `C-<n>` (l'emetteur admet
    // `[A-Z]-\d+`). Option eteinte : refus explicite, rien d'autre ne change.
    bool const compagnon = presence && c.action.size() > 2 && c.action[0] == 'C' && c.action[1] == '-';
    if (compagnon && !sPlayerbotAIConfig.directeurCompagnons)
    {
        Refuser(c, "compagnon_eteint");
        return;
    }
    if (presence)
    {
        Player* joueur = c.joueur ? ObjectAccessor::FindPlayerByLowGUID(c.joueur) : nullptr;
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
        // Un chasseur amene pres d'un joueur High Risk (FFA) le chasserait, et
        // chasserait les autres arrivants (WildPvp.AllBotsAreTargets).
        if (MercenaryRewards::EstMercenaire(bot) && !CoaDirecteur::MercenaireEnRpg(c.cible))
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
        {
            std::shared_lock<std::shared_mutex> garde(verrouConsignes);
            auto it = affectations.find(c.cible);
            continuation = it != affectations.end() && it->second.a.finS > Maintenant() &&
                           it->second.joueur == c.joueur && it->second.origine == c.origine;
            presenceArrivee = continuation && it->second.a.presence && !it->second.a.etape &&
                              it->second.a.phase == CoaDirecteur::CoaDirPhase::Sejour;
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
        if (!continuation && bx * bx + by * by <= PRESENCE_A_VUE_YD * PRESENCE_A_VUE_YD)
        {
            Refuser(c, "a_vue");
            return;
        }
        float const cx = c.x - joueur->GetPositionX();
        float const cy = c.y - joueur->GetPositionY();
        float const centreJoueur = cx * cx + cy * cy;
        // Compagnons : le centre EST le joueur (sa position a l'emission ; il a
        // pu bouger depuis). Le suivi le recale ensuite chaque seconde.
        bool const horsBornes = compagnon
            ? centreJoueur > 1.0f   // le centre vient d'etre pris sur le joueur (plus haut)
            : (centreJoueur < PRESENCE_CENTRE_MIN_YD * PRESENCE_CENTRE_MIN_YD ||
               centreJoueur > PRESENCE_CENTRE_MAX_YD * PRESENCE_CENTRE_MAX_YD);
        if (horsBornes)
        {
            Refuser(c, "bornes");
            return;
        }
    }
    float const dx = bot->GetPositionX() - c.x;
    float const dy = bot->GetPositionY() - c.y;
    if (dx * dx + dy * dy > AFFECTATION_DISTANCE_MAX * AFFECTATION_DISTANCE_MAX)
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
    bool const mercenaireRpg = CoaDirecteur::MercenaireEnRpg(c.cible);
    if (chasse && !mercenaireRpg)
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
    // `peupler` exempte le joueur qui demande, et lui seul (§ 4.2).
    if (presence ? HumainProcheSauf(centre, c.carte, AFFECTATION_HUMAIN_YD, c.joueur)
                 : ia->HasPlayerNearby(&centre, AFFECTATION_HUMAIN_YD))
    {
        Refuser(c, "humain_proche");
        return;
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
    nouvelle.a.presence = presence;
    nouvelle.a.compagnon = compagnon;
    if (compagnon)
        nouvelle.a.rayon = CoaDirecteur::COMPAGNON_MAX_YD;   // arrivee = dans la bande
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
            nbAffectations.store(static_cast<uint32>(affectations.size()), std::memory_order_relaxed);
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
    // 3. verbe allume ET porte par ce binaire
    if (c.verbe == CoaDirVerbe::Inconnu || !VerbeImplemente(c.verbe) || !VerbeAllume(c.verbe))
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
    // 4. arret, avant le budget : un arret ne consomme pas de jeton. `repondre`
    // en est exempte (decision n° 4 du 26/09) : une ligne qui dit « vous etes a
    // l'arret » n'est pas une mise en scene. Le ciel et la meteo visent le
    // joueur lui-meme (cible), comme la treve.
    bool const visePersonne = c.verbe == CoaDirVerbe::Treve || c.verbe == CoaDirVerbe::HeureDuJour ||
                              c.verbe == CoaDirVerbe::Meteo;
    if (c.verbe != CoaDirVerbe::Repondre &&
        ((c.joueur && CoaDirecteur::EstArrete(c.joueur)) || (visePersonne && CoaDirecteur::EstArrete(c.cible))))
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REFUS_ARRET);
        Refuser(c, "arret");
        return;
    }
    // 5. budget
    if (!PrendreJeton())
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
             " pactes={}",
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
             Lire(COA_DIR_ECARTE_PACTE), nbPactes.load(std::memory_order_relaxed));
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
        out << f.a.id << ',' << NomVerbe(f.verbe) << ',' << f.guid << ',' << f.joueur << ',' << f.a.finS << ','
            << f.a.carte << ',' << f.a.x << ',' << f.a.y << ',' << f.a.rayon << ',' << NomPhase(f.a.phase) << ",-,"
            << (f.a.etape ? (f.etapeArrivee ? "1" : "0") : "-");
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
//   guid,regle,equipe,race,classe,niveau,groupe,mj,bg,ffa,pvp,arret,ciel,meteo
// regle : pve, high_risk, war_mode ou '-', par HasAura dans l'ordre 9931032,
// 1004019, 1004119 (P-138 : 1004119 est AUSSI porte en PvE) ; equipe A ou H ;
// groupe : taille du groupe, 0 sans groupe ; ciel, meteo : minute et etat poses
// par le Directeur, '-' sinon. Personnage absent : `absent`. Requete mal formee,
// ou bus eteint (meme interrupteur que `lieux`) : `invalid`.
constexpr uint32 AURA_REGLE_PVE = 9931032;        // mod-ascension-compat, AscensionRulesets.cpp
constexpr uint32 AURA_REGLE_HIGH_RISK = 1004019;
constexpr uint32 AURA_REGLE_WAR_MODE = 1004119;

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
    bool const bus = sPlayerbotAIConfig.directeurBus;
    dernierEtatBus = bus;
    source = sourceMysql.Demarrer(EpoqueMonde(), bus) ? &sourceMysql : nullptr;
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
        LeverSuspensionsSi([](Suspension const&) { return true; }, "bus_eteint");
        LeverAffectationsSi([](AffectationInterne const&) { return true; }, "bus_eteint");
        // Lot D3 : heure vraie renvoyee, temps de la zone retabli, paroles oubliees.
        LeverCielsSi([](Ciel const&) { return true; }, "bus_eteint");
        LeverMeteosSi([](MeteoJoueur const&) { return true; }, "bus_eteint");
        LeverParolesSi([](Parole const&) { return true; }, "bus_eteint");
        PurgerMortsEnTrajet();
        PurgerPactes();
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
    if (!nbSuspensions.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = suspensions.find(guidLow);
    return it != suspensions.end() && it->second.rpg && it->second.finS > Maintenant();
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

bool CoaDirecteur::PointDApproche(Map* carte, Affectation const& a, float bx, float by, float& x, float& y, float& z)
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
        float const px = a.x + COMPAGNON_CIBLE_YD * std::cos(cap + e);
        float const py = a.y + COMPAGNON_CIBLE_YD * std::sin(cap + e);
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
                                                          float& z, Position const* depuis, float distanceMax)
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
            if (ToitAuDessus(carte, px, py, sol, a.z) || ecart > TIRAGE_PENTE_MAX * r)
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
        // Accuse immediat, dans le meme appel (SPEC-directeur-D3.md § 2.8).
        Enregistrer(joueur, "ask", texte);
        handler->PSendSysMessage("Le Directeur vous a entendu.");
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
            LeverParolesSi([guidLow](Parole const& p) { return p.joueur == guidLow; }, "arret");
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
        LOG_INFO("playerbots", "coa directeur : arret du serveur, file 8888 fermee ({} requete(s) abandonnee(s))",
                 restantes.size());
    }
};

void AddSC_coa_directeur()
{
    new coa_directeur_commandscript();
    new coa_directeur_worldscript();
}
