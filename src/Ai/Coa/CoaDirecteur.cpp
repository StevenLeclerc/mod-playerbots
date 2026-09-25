/*
 * CoA -- le Directeur, lot D1. Voir CoaDirecteur.h pour le pourquoi.
 */

#include "CoaDirecteur.h"

#include "Chat.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <chrono>
#include <deque>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
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

/*
 * `monde` : une ligne par personnage en jeu, champs separes par ',', lignes
 * par une tabulation, pour tenir dans le protocole a une ligne du port 8888.
 *
 *   guid,nom,type,carte,zone,aire,x,y,z,niveau,pv%,combat,etat,ffa,pvp,victime
 *
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
        char const etat = p->isDead() ? (p->GetCorpse() ? 'G' : 'D') : 'A';
        Unit const* victime = p->GetVictim();

        out << p->GetGUID().GetCounter() << ',' << p->GetName() << ',' << type << ','
            << p->GetMapId() << ',' << p->GetZoneId() << ',' << p->GetAreaId() << ','
            << p->GetPositionX() << ',' << p->GetPositionY() << ',' << p->GetPositionZ() << ','
            << static_cast<uint32>(p->GetLevel()) << ','
            << static_cast<uint32>(p->GetHealthPct()) << ','
            << (p->IsInCombat() ? 1 : 0) << ',' << etat << ','
            << (p->IsFFAPvP() ? 1 : 0) << ',' << (p->IsPvP() ? 1 : 0) << ','
            << (victime ? victime->GetGUID().GetCounter() : 0);
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

std::string Executer(std::string const& texte)
{
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
        t.resize(TEXTE_MAX);
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
}  // namespace

// ================================================================== interface

std::string CoaDirecteur::SoumettreRequete(std::string const& requete)
{
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
    if (reponse.wait_for(DELAI_REPONSE) != std::future_status::ready)
        return "timeout";
    return reponse.get();
}

void CoaDirecteur::ViderRequetes()
{
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
            return false;
        }
        if (TropTot(joueur->GetGUID().GetCounter()))
        {
            handler->PSendSysMessage("Doucement : une commande du Directeur toutes les {} secondes.", INTERVALLE_MIN_MS / 1000);
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
            return false;
        }
        if (TropTot(joueur->GetGUID().GetCounter()))
        {
            handler->PSendSysMessage("Doucement : une commande du Directeur toutes les {} secondes.", INTERVALLE_MIN_MS / 1000);
            return false;
        }
        // D1 : la demande est gardee, elle n'est pas encore executee (D3).
        Enregistrer(joueur, "ask", texte);
        handler->PSendSysMessage("Demande gardee. Le Directeur ne sait pas encore y repondre : ce sera la prochaine etape.");
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
        CharacterDatabase.Execute("REPLACE INTO coa_dir_arret (guid, depuis) VALUES ({}, NOW())", guidLow);
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
        CharacterDatabase.Execute("DELETE FROM coa_dir_arret WHERE guid = {}", guidLow);
        Enregistrer(joueur, "reprise", "");
        handler->PSendSysMessage("Le Directeur reprend son travail autour de vous.");
        return true;
    }
};

void AddSC_coa_directeur()
{
    new coa_directeur_commandscript();
}
