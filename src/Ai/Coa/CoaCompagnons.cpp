/*
 * Conquest of Azeroth -- bots compagnons du Directeur (« visages connus »), lot 1. Voir CoaCompagnons.h.
 *
 * Conception : coa-project/docs/CONCEPTION-bots-compagnons-directeur.md. Ce que le lot 1 fait : le registre (table
 * `coa_compagnon`), sa constitution automatique, le reveil a la connexion de l'humain, le sommeil apres son depart,
 * les exemptions (tirage, paliers, remise a zero du niveau, refonte, teleportation aleatoire), les commandes
 * `.coa compagnon liste|eveiller|endormir` et le journal « coa compagnon: ». Ce qu'il ne fait PAS : ni XP, ni
 * rattrapage de niveau, ni placement pres du joueur, ni Directeur, ni panneau (lots 2 a 5).
 *
 * LES TROIS GARANTIES, ET CE QUI LES TIENT
 *
 *  1. Un compagnon endormi n'est jamais tire comme un bot ordinaire. Il porte « logout » pour PermanentlyInWorldTime
 *     (AddRandomBots ecarte tout bot qui a « add » ou « logout »). Au demarrage, ChargerEtEndormir repose ce
 *     « logout » sur TOUT le registre avant le premier tirage : un compagnon eveille au moment de l'arret l'avait
 *     perdu (CoaEnroler le leve). ProcessBot et OnPlayerLoginError le reposent quand un compagnon sort sans nous.
 *  2. Un compagnon eveille ne reste pas en jeu sans son humain. Sa presence (« add ») est posee pour
 *     2 x Renouvellement et renouvelee par MettreAJour : si le renouvellement cesse, ProcessBot le deconnecte seul.
 *  3. Un compagnon ne change pas de niveau hors de l'XP : EstCompagnon est teste par la refonte periodique et la
 *     teleportation (ProcessBot(Player*)), par les paliers (boucle, GetOrFlagPlayerBracket, AdjustBotToRange) et par
 *     la remise a zero (ResetBot, SkipBotLevel).
 *
 * BASE. Lectures et ecritures rares sur la base PERSONNAGES (CharacterDatabase) : au demarrage, a la constitution
 * (une requete, synchrone, au fil monde, comme GetBots), et une ecriture asynchrone par reveil, sommeil ou
 * renouvellement. L'etat qui decide est EN MEMOIRE ; la table en est le reflet, que le Directeur et les outils lisent.
 * Les heures de la table sont en UTC (UTC_TIMESTAMP), comme le reste de la base.
 */

#include "CoaCompagnons.h"

#include "CharacterCache.h"
#include "Chat.h"
#include "CoaBancSoin.h"
#include "CoaDirecteur.h"
#include "CoaGelBanc.h"
#include "CoaSpecialization.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
enum class Camp : uint8
{
    Allie = 0,
    VisAVis = 1,
};

enum class Etat : uint8
{
    Dort = 0,
    Eveille = 1,
    Retire = 2,
};

struct Membre
{
    uint32 bot = 0;
    Camp camp = Camp::Allie;
    uint8 rang = 0;
    Etat etat = Etat::Dort;
};

struct Seance
{
    uint32 compte = 0;
    time_t debut = 0;                // arrivee : ordre de priorite sous MaxEveilles
    time_t depart = 0;               // 0 : en jeu
    bool present = false;
    bool commande = false;           // ouverte par `.coa compagnon eveiller` pour un humain ABSENT
    time_t prochaineConstitution = 0;  // registre incomplet faute de candidats : pas de nouvel essai avant
};

// Pas de nouvel essai de constitution avant ce delai quand le vivier n'a pas suffi (une requete par essai).
constexpr time_t CONSTITUTION_REESSAI_S = 900;
// Releve « coa compagnon releve: » et avis de plafond : au plus une ligne par periode.
constexpr time_t RELEVE_S = 300;
// Echelonnement 0 : au plus ce nombre de reveils par seconde.
constexpr uint32 REVEILS_PAR_SECONDE_MAX = 10;

// --- etat du fil monde
std::unordered_map<uint32, std::vector<Membre>> gRegistres;   // humain -> membres (retires compris)
std::unordered_map<uint32, Seance> gSeances;                  // humain -> seance en jeu, en grace ou par commande
std::unordered_map<uint32, uint32> gEveilles;                 // bot eveille -> son humain
std::deque<std::pair<uint32, uint32>> gFile;                  // (humain, bot) a reveiller, dans l'ordre
std::unordered_set<uint32> gEnFile;                           // les bots de gFile
// Endormis restes EN JEU : un humain en ligne de leur groupe les retient (CoaEndormir rend faux). Redemandes a chaque
// renouvellement jusqu'a leur sortie (revue du 26/10, M2) : sans cette liste, plus rien ne s'en occupait.
std::unordered_map<uint32, uint32> gRetenus;                  // bot -> son humain
bool gCharge = false;                                         // table lue au demarrage
bool gInitialise = false;                                     // ChargerEtEndormir deja appele une fois
uint32 gCumulMs = 0;
time_t gProchainReveil = 0;
time_t gProchainRenouvellement = 0;
time_t gProchainReleve = 0;
time_t gDernierAvisPlafond = 0;

// --- lu par tout fil (EstCompagnon), ecrit au fil monde seul
// Enable FIGE a la premiere lecture (revue du 26/10, M1) : une recharge de la conf ne l'allume ni ne l'eteint. Sans ca,
// passer Enable de 1 a 0 a chaud faisait tomber les exemptions de bots en jeu et figeait les endormis 360 jours.
std::atomic<bool> gEnableFige{false};
std::shared_mutex gVerrou;
std::unordered_set<uint32> gEnsemble;   // bots au registre, NON retires
// Revision du 26/10 (reserves par tranche, plus bas) : meme regle, meme verrou.
std::atomic<bool> gReserveFige{false};
std::unordered_set<uint32> gEnsembleR;  // bots de reserve, NON retires
thread_local uint32 tPlacement = 0;     // bot que ce fil place lui-meme (PlacementAutorise)

time_t Maintenant() { return GameTime::GetGameTime().count(); }

bool Actif() { return gEnableFige.load(std::memory_order_relaxed) && gCharge; }

uint32 DureePresence() { return 2 * sPlayerbotAIConfig.coaCompagnonsRenouvellement; }

char const* NomCamp(Camp c) { return c == Camp::Allie ? "allie" : "vis_a_vis"; }

char const* NomEtat(Etat e)
{
    switch (e)
    {
        case Etat::Eveille:
            return "eveille";
        case Etat::Retire:
            return "retire";
        default:
            return "dort";
    }
}

ObjectGuid Guid(uint32 bas) { return ObjectGuid::Create<HighGuid::Player>(bas); }

std::string NomDe(uint32 bas)
{
    std::string nom;
    if (!sCharacterCache->GetCharacterNameByGuid(Guid(bas), nom) || nom.empty())
        return "#" + std::to_string(bas);
    return nom;
}

void EnsembleAjouter(uint32 bot)
{
    std::unique_lock<std::shared_mutex> verrou(gVerrou);
    gEnsemble.insert(bot);
}

void EnsembleRetirer(uint32 bot)
{
    std::unique_lock<std::shared_mutex> verrou(gVerrou);
    gEnsemble.erase(bot);
}

Membre* Trouver(uint32 humain, uint32 bot)
{
    auto it = gRegistres.find(humain);
    if (it == gRegistres.end())
        return nullptr;
    for (Membre& m : it->second)
        if (m.bot == bot)
            return &m;
    return nullptr;
}

// Endort un bot via RandomPlayerbotMgr ; s'il reste en jeu (un humain en ligne de son groupe le retient), le note dans
// gRetenus pour redemander au renouvellement suivant. Rend vrai s'il est sorti (ou n'etait pas en jeu).
bool Coucher(uint32 humain, uint32 bot)
{
    if (sRandomPlayerbotMgr.CoaEndormir(Guid(bot)))
    {
        gRetenus.erase(bot);
        return true;
    }
    if (gRetenus.emplace(bot, humain).second)
        LOG_INFO("playerbots", "coa compagnon: retenu humain={} bot={} raison=groupe_humain", NomDe(humain), NomDe(bot));
    return false;
}

// Un humain : personnage d'un compte qui n'est pas un compte de bots.
bool CompteHumain(uint32 compte) { return compte && !sPlayerbotAIConfig.IsInRandomAccountList(compte); }

void Retirer(uint32 humain, Membre& m, char const* motif)
{
    if (m.etat == Etat::Retire)
        return;
    m.etat = Etat::Retire;
    gEveilles.erase(m.bot);
    EnsembleRetirer(m.bot);
    CharacterDatabase.Execute(
        "UPDATE coa_compagnon SET etat = 'retire', motif_retrait = '{}' WHERE humain_guid = {} AND bot_guid = {}", motif,
        humain, m.bot);
    LOG_INFO("playerbots", "coa compagnon: retrait humain={} bot={} motif={}", NomDe(humain), m.bot, motif);
}

void EndormirBot(uint32 humain, uint32 bot, char const* motif, time_t maintenant)
{
    Coucher(humain, bot);
    gEveilles.erase(bot);
    if (Membre* m = Trouver(humain, bot))
        if (m->etat == Etat::Eveille)
            m->etat = Etat::Dort;
    CharacterDatabase.Execute(
        "UPDATE coa_compagnon SET etat = 'dort' WHERE humain_guid = {} AND bot_guid = {} AND etat = 'eveille'", humain,
        bot);

    time_t apres = 0;
    auto s = gSeances.find(humain);
    if (s != gSeances.end() && s->second.depart && maintenant > s->second.depart)
        apres = maintenant - s->second.depart;
    LOG_INFO("playerbots", "coa compagnon: sommeil humain={} bot={} apres={} motif={}", NomDe(humain), NomDe(bot),
             uint64(apres), motif);
}

// Endort tous les compagnons eveilles de cet humain et oublie ses reveils en attente. Rend leur nombre.
// La seance n'est PAS effacee : a l'appelant de le faire.
uint32 Endormir(uint32 humain, char const* motif, time_t maintenant)
{
    std::vector<uint32> retiresDeLaFile;
    for (auto it = gFile.begin(); it != gFile.end();)
    {
        if (it->first == humain)
        {
            gEnFile.erase(it->second);
            retiresDeLaFile.push_back(it->second);
            it = gFile.erase(it);
        }
        else
            ++it;
    }
    // Un membre en file n'est pas encore eveille ; s'il a ete pris EN JEU a la constitution, il y est encore.
    for (uint32 bot : retiresDeLaFile)
        if (!gEveilles.count(bot))
            Coucher(humain, bot);

    std::vector<uint32> aEndormir;
    for (auto const& [bot, h] : gEveilles)
        if (h == humain)
            aEndormir.push_back(bot);
    for (uint32 bot : aEndormir)
        EndormirBot(humain, bot, motif, maintenant);
    return aEndormir.size();
}

struct Candidat
{
    uint32 guid;
    uint8 classe;
    uint8 niveau;
    bool enLigne;
    uint32 alea;
};

std::vector<uint32> GladiateursDeLaConf()
{
    std::vector<uint32> out;
    std::string const& s = sPlayerbotAIConfig.arene1v1Gladiateurs;
    size_t debut = 0;
    while (debut <= s.size())
    {
        size_t fin = s.find(',', debut);
        if (fin == std::string::npos)
            fin = s.size();
        std::string morceau = s.substr(debut, fin - debut);
        morceau.erase(std::remove(morceau.begin(), morceau.end(), ' '), morceau.end());
        if (!morceau.empty() && morceau.find_first_not_of("0123456789") == std::string::npos && morceau.size() < 10)
            out.push_back(static_cast<uint32>(std::stoul(morceau)));
        debut = fin + 1;
    }
    return out;
}

bool NomExclu(std::string const& nom)
{
    auto const& a = sPlayerbotAIConfig.levelBracketsExcludeNames;
    auto const& b = sPlayerbotAIConfig.resetBotLevelExcludeNames;
    return std::find(a.begin(), a.end(), nom) != a.end() || std::find(b.begin(), b.end(), nom) != b.end();
}

// Complete le registre de cet humain jusqu'a Allies + VisAVis (§ 5 de la conception). Rend les bots ajoutes.
std::vector<uint32> Constituer(uint32 humain, uint8 niveau, TeamId camp, Seance& seance, time_t maintenant)
{
    std::vector<uint32> ajoutes;
    std::vector<Membre>& reg = gRegistres[humain];

    uint32 nAllies = 0, nVisAVis = 0;
    uint8 rangAllie = 0, rangVisAVis = 0;
    std::unordered_set<uint8> classesAlliees;
    for (Membre const& m : reg)
    {
        uint8& rang = m.camp == Camp::Allie ? rangAllie : rangVisAVis;
        rang = std::max(rang, m.rang);
        if (m.etat == Etat::Retire)
            continue;
        if (m.camp == Camp::Allie)
        {
            ++nAllies;
            if (CharacterCacheEntry const* e = sCharacterCache->GetCharacterCacheByGuid(Guid(m.bot)))
                classesAlliees.insert(e->Class);
        }
        else
            ++nVisAVis;
    }
    uint32 besoinAllies = sPlayerbotAIConfig.coaCompagnonsAllies > nAllies ? sPlayerbotAIConfig.coaCompagnonsAllies - nAllies : 0;
    uint32 besoinVisAVis =
        sPlayerbotAIConfig.coaCompagnonsVisAVis > nVisAVis ? sPlayerbotAIConfig.coaCompagnonsVisAVis - nVisAVis : 0;
    if (!besoinAllies && !besoinVisAVis)
        return ajoutes;
    if (maintenant < seance.prochaineConstitution)
        return ajoutes;

    std::vector<uint32> const& comptes = sRandomPlayerbotMgr.CoaComptesAleatoires();
    if (comptes.empty())
    {
        LOG_INFO("playerbots", "coa compagnon: constitution humain={} refus=aucun_compte_de_bots", NomDe(humain));
        seance.prochaineConstitution = maintenant + CONSTITUTION_REESSAI_S;
        return ajoutes;
    }
    std::string listeComptes;
    for (uint32 c : comptes)
        listeComptes += (listeComptes.empty() ? "" : ",") + std::to_string(c);

    std::string clauseReserve;
    std::vector<uint32> const reserve = sPlayerbotAIConfig.GuildesDeReserve();
    if (!reserve.empty())
    {
        std::string ids;
        for (uint32 id : reserve)
            ids += (ids.empty() ? "" : ",") + std::to_string(id);
        clauseReserve = " AND NOT EXISTS (SELECT 1 FROM guild_member g WHERE g.guid = c.guid AND g.guildid IN (" + ids + "))";
    }

    // Jamais au-dessus de humain + 2 (§ 5), ni plus de EcartBas sous lui (revue du 26/10, M3 : sans borne basse, un
    // humain de niveau 60 recevait des allies de niveau 30, que rien ne rattrape au lot 1). Les comptes de type 2
    // (addclass) ne sont pas dans la liste.
    QueryResult res = CharacterDatabase.Query(
        "SELECT c.guid, c.race, c.class, c.level, c.name FROM characters c"
        " WHERE c.account IN ({}) AND c.level <= {} AND c.level + {} >= {} AND c.deleteInfos_Account IS NULL"
        " AND NOT EXISTS (SELECT 1 FROM coa_compagnon cc WHERE cc.bot_guid = c.guid)"
        " AND NOT EXISTS (SELECT 1 FROM arena_team_member a WHERE a.guid = c.guid){}",
        listeComptes, uint32(niveau) + 2, sPlayerbotAIConfig.coaCompagnonsEcartBas, uint32(niveau), clauseReserve);

    std::vector<uint32> const gladiateurs = GladiateursDeLaConf();
    std::vector<Candidat> allies, visAVis;
    if (res)
    {
        std::unordered_set<uint32> registre;
        {
            std::shared_lock<std::shared_mutex> verrou(gVerrou);
            registre = gEnsemble;
            registre.insert(gEnsembleR.begin(), gEnsembleR.end());   // jamais un bot de reserve (revision du 26/10)
        }
        do
        {
            Field* f = res->Fetch();
            uint32 const guid = f[0].Get<uint32>();
            uint8 const race = f[1].Get<uint8>();
            uint8 const classe = f[2].Get<uint8>();
            uint8 const niv = f[3].Get<uint8>();
            std::string const nom = f[4].Get<std::string>();

            if (registre.count(guid) || NomExclu(nom) ||
                std::find(gladiateurs.begin(), gladiateurs.end(), guid) != gladiateurs.end())
                continue;
            if (sPlayerbotAIConfig.disableDeathKnightLogin && classe == CLASS_DEATH_KNIGHT)
                continue;
            // Mobilise par un banc (revue du 26/10, m4) : cible de banc de prime, membre d'essai du banc des soins.
            if (CoaDirecteur::CibleDeBanc(guid) || CoaBancSoin::MembreEngage(guid))
                continue;
            // Pas de mercenaire, ni allie ni vis-a-vis (§ 17 question 6 : « non au debut » ; revue du 26/10, m1).
            if (sPlayerbotAIConfig.IsMercenary(Guid(guid).GetRawValue()))
                continue;

            // Faction par la table des races (ChrRaces.dbc) : les races 21 et 22 ne sont pas celles d'IsAlliance.
            TeamId const sienne = Player::TeamIdForRace(race);
            if (sienne != TEAM_ALLIANCE && sienne != TEAM_HORDE)
                continue;

            Player* enJeu = ObjectAccessor::FindConnectedPlayer(Guid(guid));
            // Un bot en groupe, en champ de bataille ou gele par un banc (CoaGelBanc) est occupe : on ne le prend pas.
            if (enJeu &&
                (enJeu->GetGroup() || enJeu->InBattleground() || enJeu->InBattlegroundQueue() || CoaGelBanc(enJeu)))
                continue;

            Candidat c{guid, classe, niv, enJeu != nullptr, urand(0, 1000000)};
            if (sienne == camp)
            {
                if (besoinAllies)
                    allies.push_back(c);
            }
            else if (besoinVisAVis)
                visAVis.push_back(c);
        } while (res->NextRow());
    }

    // Preference (§ 5) : niveau dans [h - 3, h], sinon le plus proche ; chez les allies, une classe pas encore prise ;
    // un bot hors ligne (ne pas faire disparaitre un bot que le joueur voit) ; puis le hasard.
    auto cle = [niveau](Candidat const& c, bool classeDejaPrise)
    {
        bool const dansFenetre = c.niveau <= niveau && c.niveau + 3 >= niveau;
        uint32 const ecart = c.niveau > niveau ? c.niveau - niveau : niveau - c.niveau;
        return std::make_tuple(dansFenetre ? 0 : 1, classeDejaPrise ? 1 : 0, c.enLigne ? 1 : 0, ecart, c.alea);
    };

    auto choisir = [&](std::vector<Candidat>& pool, uint32 besoin, Camp campMembre, uint8& rang)
    {
        for (uint32 k = 0; k < besoin && !pool.empty(); ++k)
        {
            auto meilleur = std::min_element(pool.begin(), pool.end(),
                                             [&](Candidat const& a, Candidat const& b)
                                             {
                                                 bool const pa = campMembre == Camp::Allie && classesAlliees.count(a.classe);
                                                 bool const pb = campMembre == Camp::Allie && classesAlliees.count(b.classe);
                                                 return cle(a, pa) < cle(b, pb);
                                             });
            Candidat const elu = *meilleur;
            pool.erase(meilleur);

            ++rang;
            // Synchrone : la ligne doit exister avant le premier UPDATE de reveil (file asynchrone a plusieurs fils).
            CharacterDatabase.DirectExecute(
                "INSERT IGNORE INTO coa_compagnon (humain_guid, bot_guid, camp, rang, etat, origine, cree)"
                " VALUES ({}, {}, '{}', {}, 'dort', 'auto', UTC_TIMESTAMP())",
                humain, elu.guid, NomCamp(campMembre), uint32(rang));
            reg.push_back(Membre{elu.guid, campMembre, rang, Etat::Dort});
            EnsembleAjouter(elu.guid);
            if (campMembre == Camp::Allie)
                classesAlliees.insert(elu.classe);
            ajoutes.push_back(elu.guid);
        }
    };

    uint32 const avantAllies = ajoutes.size();
    choisir(allies, besoinAllies, Camp::Allie, rangAllie);
    uint32 const prisAllies = ajoutes.size() - avantAllies;
    choisir(visAVis, besoinVisAVis, Camp::VisAVis, rangVisAVis);
    uint32 const prisVisAVis = ajoutes.size() - avantAllies - prisAllies;

    uint32 const manque = (besoinAllies - prisAllies) + (besoinVisAVis - prisVisAVis);
    if (manque)
        seance.prochaineConstitution = maintenant + CONSTITUTION_REESSAI_S;

    LOG_INFO("playerbots", "coa compagnon: constitution humain={} niv={} allies={} vis_a_vis={} manque={}", NomDe(humain),
             uint32(niveau), prisAllies, prisVisAVis, manque);
    return ajoutes;
}

// Met en file les reveils des membres endormis de cet humain, dans la limite de MaxEveilles.
void Programmer(uint32 humain, time_t maintenant)
{
    auto it = gRegistres.find(humain);
    if (it == gRegistres.end())
        return;
    for (Membre const& m : it->second)
    {
        if (m.etat == Etat::Retire || gEveilles.count(m.bot) || gEnFile.count(m.bot))
            continue;
        // Revision du 26/10 (Allies = 1, VisAVis = 0) : un registre plus grand garde ses membres en trop endormis.
        if (m.rang > (m.camp == Camp::Allie ? sPlayerbotAIConfig.coaCompagnonsAllies : sPlayerbotAIConfig.coaCompagnonsVisAVis))
            continue;
        if (gEveilles.size() + gEnFile.size() >= sPlayerbotAIConfig.coaCompagnonsMaxEveilles)
        {
            if (maintenant >= gDernierAvisPlafond + RELEVE_S)
            {
                gDernierAvisPlafond = maintenant;
                LOG_INFO("playerbots", "coa compagnon: plafond humain={} eveilles={} en_file={} max={}", NomDe(humain),
                         uint32(gEveilles.size()), uint32(gEnFile.size()), sPlayerbotAIConfig.coaCompagnonsMaxEveilles);
            }
            return;
        }
        gFile.emplace_back(humain, m.bot);
        gEnFile.insert(m.bot);
    }
}

// Humain au seuil : complete son registre, programme ses reveils. Rend le nombre de bots ajoutes au registre.
uint32 Activer(uint32 humain, uint8 niveau, TeamId camp, Seance& seance, time_t maintenant)
{
    if (niveau < sPlayerbotAIConfig.coaCompagnonsSeuilNiveau)
        return 0;
    if (camp != TEAM_ALLIANCE && camp != TEAM_HORDE)
        return 0;

    std::vector<uint32> const ajoutes = Constituer(humain, niveau, camp, seance, maintenant);
    Programmer(humain, maintenant);

    // Un nouveau membre qui ne sera pas reveille tout de suite doit dormir des maintenant (« logout » pose), sinon le
    // tirage pourrait le prendre. Celui qui est EN JEU et en file n'est pas deconnecte pour etre reconnecte deux
    // secondes plus tard : son reveil renouvellera sa presence.
    for (uint32 bot : ajoutes)
    {
        bool const enFile = gEnFile.count(bot) != 0;
        if (enFile && ObjectAccessor::FindConnectedPlayer(Guid(bot)))
            continue;
        Coucher(humain, bot);
    }
    return ajoutes.size();
}

void Reveiller(time_t maintenant)
{
    uint32 const parTour = sPlayerbotAIConfig.coaCompagnonsEchelonnement ? 1 : REVEILS_PAR_SECONDE_MAX;
    for (uint32 n = 0; n < parTour && !gFile.empty() && maintenant >= gProchainReveil;)
    {
        auto const [humain, bot] = gFile.front();
        gFile.pop_front();
        gEnFile.erase(bot);

        auto s = gSeances.find(humain);
        if (s == gSeances.end() || (!s->second.present && !s->second.commande))
        {
            // Parti entre-temps : pas de reveil, et pas de delai consomme. Un membre neuf pris EN JEU (Activer ne l'a
            // pas endormi, il attendait ce reveil) doit quand meme quitter le jeu.
            if (!gEveilles.count(bot))
                Coucher(humain, bot);
            continue;
        }
        Membre* m = Trouver(humain, bot);
        if (!m || m->etat == Etat::Retire || gEveilles.count(bot))
            continue;
        CharacterCacheEntry const* e = sCharacterCache->GetCharacterCacheByGuid(Guid(bot));
        if (!e)
        {
            Retirer(humain, *m, "absent");
            continue;
        }

        sRandomPlayerbotMgr.CoaEnroler(Guid(bot), DureePresence());
        gEveilles[bot] = humain;
        gRetenus.erase(bot);
        m->etat = Etat::Eveille;
        CharacterDatabase.Execute(
            "UPDATE coa_compagnon SET etat = 'eveille', dernier_eveil = UTC_TIMESTAMP(), niveau_reveil = {}"
            " WHERE humain_guid = {} AND bot_guid = {}",
            uint32(e->Level), humain, bot);
        // niv=<a>-><b> : le lot 1 ne rattrape rien, a et b sont egaux ; le lot 3 y ecrira le rattrapage.
        LOG_INFO("playerbots", "coa compagnon: eveil humain={} bot={} camp={} niv={}->{}", NomDe(humain), e->Name,
                 NomCamp(m->camp), uint32(e->Level), uint32(e->Level));

        gProchainReveil = maintenant + sPlayerbotAIConfig.coaCompagnonsEchelonnement;
        ++n;
    }
}

void Renouveler(time_t maintenant)
{
    std::vector<std::pair<time_t, uint32>> ordre;
    ordre.reserve(gSeances.size());
    for (auto const& [humain, s] : gSeances)
        ordre.emplace_back(s.debut, humain);
    std::sort(ordre.begin(), ordre.end());   // les plus anciens d'abord : priorite sous MaxEveilles

    std::vector<uint32> echues;
    for (auto const& [debut, humain] : ordre)
    {
        Seance& s = gSeances[humain];
        if (s.present)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(Guid(humain));
            if (!p || !p->GetSession() || p->GetSession()->IsBot())
            {
                // Depart manque par le crochet (ne devrait pas arriver) : la grace part d'ici.
                s.present = false;
                s.depart = maintenant;
                LOG_INFO("playerbots", "coa compagnon: depart humain={} grace={} source=renouvellement", NomDe(humain),
                         sPlayerbotAIConfig.coaCompagnonsGrace);
            }
            else
                Activer(humain, p->GetLevel(), p->GetTeamId(), s, maintenant);
        }

        if (!s.present && maintenant >= s.depart + time_t(sPlayerbotAIConfig.coaCompagnonsGrace))
        {
            Endormir(humain, "grace", maintenant);
            echues.push_back(humain);
            continue;
        }

        // Relever d'abord, renouveler ensuite : CoaEnroler ne doit pas tourner pendant qu'on parcourt gEveilles.
        std::vector<uint32> aRenouveler;
        for (auto const& [bot, h] : gEveilles)
            if (h == humain)
                aRenouveler.push_back(bot);
        for (uint32 bot : aRenouveler)
            sRandomPlayerbotMgr.CoaEnroler(Guid(bot), DureePresence());   // reconnecte aussi un compagnon tombe
        uint32 const renouveles = aRenouveler.size();
        if (s.present && renouveles)
            CharacterDatabase.Execute(
                "UPDATE coa_compagnon SET secondes_ensemble = secondes_ensemble + {} WHERE humain_guid = {} AND etat = 'eveille'",
                sPlayerbotAIConfig.coaCompagnonsRenouvellement, humain);
    }
    for (uint32 humain : echues)
        gSeances.erase(humain);

    // Filet : un eveille dont l'humain n'a plus de seance (ne devrait pas arriver) s'endort.
    std::vector<std::pair<uint32, uint32>> orphelins;
    for (auto const& [bot, h] : gEveilles)
        if (!gSeances.count(h))
            orphelins.emplace_back(h, bot);
    for (auto const& [h, bot] : orphelins)
        EndormirBot(h, bot, "orphelin", maintenant);

    // Les endormis retenus en jeu par un groupe (revue du 26/10, M2) : redemander. Sortis du jeu (ProcessBot les
    // deconnecte quand le groupe les lache) ou reveilles entre-temps : oublies. Un groupe qui survit a ses humains
    // hors ligne ne les retient plus : CoaEndormir les deconnecte alors (et LogoutPlayerBot les sort du groupe).
    std::vector<std::pair<uint32, uint32>> retenus(gRetenus.begin(), gRetenus.end());
    for (auto const& [bot, h] : retenus)
    {
        if (gEveilles.count(bot) || gEnFile.count(bot) || !ObjectAccessor::FindConnectedPlayer(Guid(bot)))
        {
            gRetenus.erase(bot);
            continue;
        }
        if (Coucher(h, bot))
            LOG_INFO("playerbots", "coa compagnon: sommeil humain={} bot={} apres=0 motif=groupe_libere", NomDe(h),
                     NomDe(bot));
    }
}

void Relever(time_t maintenant)
{
    if (gSeances.empty() && gEveilles.empty() && gRetenus.empty())
        return;
    uint32 presents = 0;
    for (auto const& [h, s] : gSeances)
        if (s.present)
            ++presents;
    uint32 enRetard = 0, devant = 0;
    for (auto const& [bot, h] : gEveilles)
    {
        Player* b = ObjectAccessor::FindConnectedPlayer(Guid(bot));
        Player* p = ObjectAccessor::FindConnectedPlayer(Guid(h));
        if (!b || !p)
            continue;
        int32 const ecart = int32(p->GetLevel()) - int32(b->GetLevel());
        if (ecart > 2)
            ++enRetard;
        else if (ecart < -2)
            ++devant;
    }
    LOG_INFO("playerbots",
             "coa compagnon releve: humains={} seances={} eveilles={} en_file={} en_retard={} devant={} retenus={}",
             presents, uint32(gSeances.size()), uint32(gEveilles.size()), uint32(gEnFile.size()), enRetard, devant,
             uint32(gRetenus.size()));
    (void)maintenant;
}

// Resout un nom de personnage HUMAIN pour les commandes. Rend nullptr (et le dit) sinon.
CharacterCacheEntry const* HumainParNom(ChatHandler* handler, std::string const& nom, char const* commande)
{
    if (!Actif())
    {
        handler->PSendSysMessage("coa compagnon {}: inerte (AiPlayerbot.CoaCompagnons.Enable = 0, ou table coa_compagnon absente au demarrage).",
                                 commande);
        return nullptr;
    }
    if (nom.empty())
    {
        handler->PSendSysMessage("Usage: .coa compagnon {} <personnage humain>", commande);
        return nullptr;
    }
    std::string normalise = nom;
    normalizePlayerName(normalise);   // « euphorik » -> « Euphorik », comme les commandes du coeur
    CharacterCacheEntry const* e = sCharacterCache->GetCharacterCacheByName(normalise);
    if (!e)
    {
        handler->PSendSysMessage("coa compagnon {}: personnage {} introuvable.", commande, nom);
        return nullptr;
    }
    if (!CompteHumain(e->AccountId))
    {
        handler->PSendSysMessage("coa compagnon {}: {} est un bot, pas un humain.", commande, e->Name);
        return nullptr;
    }
    return e;
}

// =====================================================================================================================
// RESERVES PAR TRANCHE DE NIVEAU (revision du 26/10/2026, conception en tete et § 15 bis)
//
// Six tranches (1-10 ... 51-60, tranche = (niveau - 1) / 10), une reserve par tranche ET par faction, toujours les memes
// bots (table `coa_reserve`, ecrite ici seulement). Un humain de la tranche, quel que soit son camp, reveille les DEUX
// reserves de sa tranche ; elles s'endorment Grace secondes apres le depart du dernier humain de la tranche (releve des
// humains toutes les SCAN_S : un humain qui change de tranche en seance quitte l'ancienne et entre dans la nouvelle).
// Niveau FIGE : NO_XP_GAIN repose a chaque connexion (PlayerbotHolder::OnBotLogin, et ici au placement), exemptions
// du lot 1 par EstCompagnon. Constitution : niveaux etales dans la tranche, classes variees ; tranche trop maigre
// completee par des bots plus bas (montee au premier reveil, GiveLevel + recette de `.coa niveau`, jamais de baisse ni
// de changement de spe). Placement au reveil : pres d'un humain de la tranche de SA faction (600-1 200 yd, hors de vue,
// recherche de la preparation du Directeur), sinon dans une zone de son niveau (RandomTeleportGrindForLevel).
// Tout au FIL MONDE (crochets, OnUpdate, commandes, Init) ; EstReserve/EstCompagnon lus de tout fil sous gVerrou.
// =====================================================================================================================
constexpr uint8 NB_TRANCHES = 6;
constexpr time_t SCAN_S = 5;                    // releve des humains par tranche
constexpr time_t PLACEMENT_LIMITE_S = 120;      // un bot reveille qui n'est pas en jeu apres ce delai n'est pas place
constexpr uint32 PLACEMENTS_PAR_SECONDE = 2;    // ChercherPointCarte : jusqu'a 36 sondes de terrain par recherche
constexpr uint32 MONTEES_PAR_SECONDE = 1;       // la fabrique refait l'equipement : couteux
constexpr time_t REESSAI_BOT_S = 300;           // un bot sorti seul (connexion ratee, presence expiree) : pas avant

enum : uint8
{
    F_ALLIANCE = 0,
    F_HORDE = 1,
};

struct MembreR
{
    uint32 bot = 0;
    uint8 tranche = 0;
    uint8 faction = F_ALLIANCE;
    uint8 rang = 0;
    uint8 niveauCible = 0;
    Etat etat = Etat::Dort;
    bool montee = false;
};

struct Tranche
{
    bool eveillee = false;
    bool commande = false;       // eveillee par `.coa reserve eveiller` sans humain
    bool suspendue = false;      // endormie par commande alors qu'un humain y est : pas de reveil avant son depart
    time_t derniereVue = 0;      // dernier releve ou la tranche comptait un humain (ou la commande d'eveil)
    uint32 humains = 0;
    uint32 humainsF[2] = {0, 0};
    time_t prochaineConstitution[2] = {0, 0};
};

std::unordered_map<uint32, MembreR> gReserve;     // bot -> membre (retires compris)
Tranche gTranches[NB_TRANCHES];
std::unordered_set<uint32> gHumainsR;              // humains connectes connus (crochets), relus a chaque releve
std::unordered_set<uint32> gEveillesR;
std::deque<uint32> gFileR;
std::unordered_set<uint32> gEnFileR;
std::unordered_map<uint32, time_t> gAPlacer;       // bot reveille -> limite de placement
std::unordered_map<uint32, time_t> gReessaiR;      // bot -> pas de reveil avant
std::unordered_set<uint32> gRetenusR;              // endormis retenus en jeu par un groupe avec un humain
bool gReserveCharge = false;
bool gReserveInit = false;
bool gTableCompagnonPresente = false;
time_t gProchainScan = 0;
time_t gDernierAvisPlafondR = 0;

bool ReserveActive() { return gReserveFige.load(std::memory_order_relaxed) && gReserveCharge; }

// >= NB_TRANCHES : hors reserve (niveau 61 et plus).
uint8 TrancheDe(uint32 niveau) { return niveau ? uint8(std::min<uint32>((niveau - 1) / 10, 255)) : 0; }
uint32 BasDe(uint8 t) { return uint32(t) * 10 + 1; }
uint32 HautDe(uint8 t) { return uint32(t) * 10 + 10; }
char const* NomFaction(uint8 f) { return f == F_ALLIANCE ? "alliance" : "horde"; }
int FactionDe(TeamId t) { return t == TEAM_ALLIANCE ? F_ALLIANCE : (t == TEAM_HORDE ? F_HORDE : -1); }

void EnsembleRAjouter(uint32 bot)
{
    std::unique_lock<std::shared_mutex> verrou(gVerrou);
    gEnsembleR.insert(bot);
}

void EnsembleRRetirer(uint32 bot)
{
    std::unique_lock<std::shared_mutex> verrou(gVerrou);
    gEnsembleR.erase(bot);
}

bool EstReserveInterne(uint32 bot)
{
    std::shared_lock<std::shared_mutex> verrou(gVerrou);
    return gEnsembleR.count(bot) != 0;
}

// Endort un bot de reserve (« logout » long). Reste en jeu si un humain en ligne de son groupe le retient : suivi dans
// gRetenusR et redemande a chaque renouvellement. Rend vrai s'il est sorti (ou n'etait pas en jeu).
bool CoucherR(uint32 bot)
{
    if (sRandomPlayerbotMgr.CoaEndormir(Guid(bot)))
    {
        gRetenusR.erase(bot);
        return true;
    }
    if (gRetenusR.insert(bot).second)
        LOG_INFO("playerbots", "coa reserve: retenu bot={} raison=groupe_humain", NomDe(bot));
    return false;
}

void RetirerR(MembreR& m, char const* motif)
{
    if (m.etat == Etat::Retire)
        return;
    uint8 const avant = m.tranche;
    m.etat = Etat::Retire;
    gEveillesR.erase(m.bot);
    gEnFileR.erase(m.bot);   // l'entree de gFileR est sautee au depilement (membre retire)
    gAPlacer.erase(m.bot);
    gRetenusR.erase(m.bot);
    EnsembleRRetirer(m.bot);
    // Rendu a la vie ordinaire : deconnecte s'il le peut (« logout » long), puis « logout » leve, sans le connecter.
    // Retenu par un groupe : il reste en jeu comme un bot ordinaire, ProcessBot le deconnectera a l'echeance de « add ».
    sRandomPlayerbotMgr.CoaEndormir(Guid(m.bot));
    sRandomPlayerbotMgr.CoaLiberer(Guid(m.bot));
    CharacterDatabase.Execute("UPDATE coa_reserve SET etat = 'retire', motif_retrait = '{}' WHERE bot_guid = {}", motif,
                              m.bot);
    LOG_INFO("playerbots", "coa reserve: retrait tranche={} faction={} bot={} motif={}", uint32(avant),
             NomFaction(m.faction), NomDe(m.bot), motif);
}

void EndormirR(uint32 bot, char const* motif, time_t apres)
{
    CoucherR(bot);
    gEveillesR.erase(bot);
    gAPlacer.erase(bot);
    auto it = gReserve.find(bot);
    if (it != gReserve.end() && it->second.etat == Etat::Eveille)
        it->second.etat = Etat::Dort;
    CharacterDatabase.Execute("UPDATE coa_reserve SET etat = 'dort' WHERE bot_guid = {} AND etat = 'eveille'", bot);
    LOG_INFO("playerbots", "coa reserve: sommeil tranche={} faction={} bot={} apres={} motif={}",
             it != gReserve.end() ? uint32(it->second.tranche) : 0u,
             it != gReserve.end() ? NomFaction(it->second.faction) : "?", NomDe(bot), uint64(apres), motif);
}

// Endort les deux reserves de la tranche et oublie leurs reveils en attente. Rend le nombre de bots endormis.
uint32 EndormirTranche(uint8 t, char const* motif, time_t maintenant)
{
    Tranche& tr = gTranches[t];
    for (auto it = gFileR.begin(); it != gFileR.end();)
    {
        auto m = gReserve.find(*it);
        if (m == gReserve.end() || m->second.tranche == t)
        {
            gEnFileR.erase(*it);
            it = gFileR.erase(it);
        }
        else
            ++it;
    }
    time_t const apres = tr.derniereVue && maintenant > tr.derniereVue ? maintenant - tr.derniereVue : 0;
    std::vector<uint32> aEndormir;
    for (uint32 bot : gEveillesR)
    {
        auto m = gReserve.find(bot);
        if (m != gReserve.end() && m->second.tranche == t)
            aEndormir.push_back(bot);
    }
    for (uint32 bot : aEndormir)
        EndormirR(bot, motif, apres);
    bool const etaitEveillee = tr.eveillee;
    tr.eveillee = false;
    tr.commande = false;
    if (etaitEveillee || !aEndormir.empty())
        LOG_INFO("playerbots", "coa reserve: sommeil_tranche tranche={} niveaux={}-{} bots={} apres={} motif={}",
                 uint32(t), BasDe(t), HautDe(t), uint32(aEndormir.size()), uint64(apres), motif);
    return aEndormir.size();
}

uint32 ActifsDe(uint8 t, uint8 f)
{
    uint32 n = 0;
    for (auto const& [bot, m] : gReserve)
        if (m.tranche == t && m.faction == f && m.etat != Etat::Retire)
            ++n;
    return n;
}

// Niveaux voulus pour `n` places dans la tranche, etales (n = 8 sur 1-10 : 1 2 4 5 6 7 9 10).
std::vector<uint8> CiblesEtalees(uint8 t, uint32 n)
{
    std::vector<uint8> out;
    for (uint32 k = 0; k < n; ++k)
        out.push_back(uint8(BasDe(t) + ((2 * k + 1) * 10) / (2 * n)));
    return out;
}

struct ResultatR
{
    uint8 tranche = 0;
    uint8 faction = 0;
    uint32 voulus = 0;
    uint32 pris = 0;
    uint32 montes = 0;
    uint32 manque = 0;
    char const* refus = nullptr;
};

struct CandidatR
{
    uint32 guid;
    uint8 classe;
    uint8 niveau;
    uint8 faction;
    uint32 alea;
    bool pris;
};

// Complete les reserves demandees (tranche, faction). `force` (commande `constituer`) : jusqu'a ParTranche, delai de
// nouvel essai ignore ; sinon seulement une reserve sous SeuilConstitution, et pas avant prochaineConstitution.
// UNE requete synchrone (fil monde, comme la constitution du lot 1), seulement si une reserve a besoin de bots.
std::vector<ResultatR> ConstituerR(std::vector<std::pair<uint8, uint8>> const& voulues, time_t maintenant, bool force)
{
    struct Besoin
    {
        uint8 t, f;
        std::vector<uint8> cibles;
        std::unordered_set<uint8> classes;
        uint8 rang;
        ResultatR r;
    };
    std::vector<Besoin> besoins;
    std::vector<ResultatR> resultats;
    uint32 const parTranche = sPlayerbotAIConfig.coaReserveParTranche;
    uint32 const niveauMax = std::min<uint32>(NB_TRANCHES * 10, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));

    for (auto const& [t, f] : voulues)
    {
        if (t >= NB_TRANCHES || f > F_HORDE)
            continue;
        Besoin b{t, f, {}, {}, 0, {}};
        b.r.tranche = t;
        b.r.faction = f;
        std::vector<uint8> existants;
        for (auto const& [bot, m] : gReserve)
        {
            if (m.tranche != t || m.faction != f)
                continue;
            b.rang = std::max(b.rang, m.rang);
            if (m.etat == Etat::Retire)
                continue;
            existants.push_back(m.niveauCible);
            if (CharacterCacheEntry const* e = sCharacterCache->GetCharacterCacheByGuid(Guid(bot)))
                b.classes.insert(e->Class);
        }
        uint32 const actifs = existants.size();
        if (actifs >= parTranche || (!force && actifs >= sPlayerbotAIConfig.coaReserveSeuilConstitution))
            continue;
        if (!force && maintenant < gTranches[t].prochaineConstitution[f])
            continue;
        if (BasDe(t) > niveauMax)
        {
            b.r.refus = "tranche_au_dela_du_plafond";
            resultats.push_back(b.r);
            continue;
        }
        // Les places : ParTranche niveaux etales, moins celles que les membres en place occupent deja (au plus proche).
        b.cibles = CiblesEtalees(t, parTranche);
        for (uint8 niv : existants)
        {
            if (b.cibles.empty())
                break;
            auto plusProche = std::min_element(b.cibles.begin(), b.cibles.end(),
                                               [niv](uint8 a, uint8 c)
                                               { return std::abs(int(a) - int(niv)) < std::abs(int(c) - int(niv)); });
            b.cibles.erase(plusProche);
        }
        for (uint8& c : b.cibles)
            c = uint8(std::min<uint32>(c, niveauMax));
        b.r.voulus = b.cibles.size();
        besoins.push_back(std::move(b));
    }
    if (besoins.empty())
        return resultats;

    auto finir = [&](char const* refus)
    {
        for (Besoin& b : besoins)
        {
            b.r.manque = b.cibles.size();
            if (refus)
                b.r.refus = refus;
            if (b.r.manque)
                gTranches[b.t].prochaineConstitution[b.f] = maintenant + CONSTITUTION_REESSAI_S;
            LOG_INFO("playerbots", "coa reserve: constitution tranche={} faction={} voulus={} pris={} montes={} manque={}{}{}",
                     uint32(b.t), NomFaction(b.f), b.r.voulus, b.r.pris, b.r.montes, b.r.manque,
                     refus ? " refus=" : "", refus ? refus : "");
            resultats.push_back(b.r);
        }
        return resultats;
    };

    std::vector<uint32> const& comptes = sRandomPlayerbotMgr.CoaComptesAleatoires();
    if (comptes.empty())
        return finir("aucun_compte_de_bots");
    std::string listeComptes;
    for (uint32 c : comptes)
        listeComptes += (listeComptes.empty() ? "" : ",") + std::to_string(c);

    std::string clauseGuildes;
    std::vector<uint32> const guildes = sPlayerbotAIConfig.GuildesDeReserve();
    if (!guildes.empty())
    {
        std::string ids;
        for (uint32 id : guildes)
            ids += (ids.empty() ? "" : ",") + std::to_string(id);
        clauseGuildes = " AND NOT EXISTS (SELECT 1 FROM guild_member g WHERE g.guid = c.guid AND g.guildid IN (" + ids + "))";
    }
    // Un bot n'est jamais a la fois compagnon et reserve : toute ligne de coa_compagnon l'ecarte (retires compris).
    std::string const clauseCompagnon =
        gTableCompagnonPresente ? " AND NOT EXISTS (SELECT 1 FROM coa_compagnon cc WHERE cc.bot_guid = c.guid)" : "";

    // Hors ligne seulement (c.online = 0, puis FindConnectedPlayer) : un bot que le joueur voit ne disparait pas.
    QueryResult res = CharacterDatabase.Query(
        "SELECT c.guid, c.race, c.class, c.level, c.name FROM characters c"
        " WHERE c.account IN ({}) AND c.level BETWEEN 1 AND {} AND c.online = 0 AND c.deleteInfos_Account IS NULL"
        " AND NOT EXISTS (SELECT 1 FROM coa_reserve r WHERE r.bot_guid = c.guid){}"
        " AND NOT EXISTS (SELECT 1 FROM arena_team_member a WHERE a.guid = c.guid){}",
        listeComptes, niveauMax, clauseCompagnon, clauseGuildes);

    std::vector<CandidatR> pool;
    if (res)
    {
        std::unordered_set<uint32> pris;
        {
            std::shared_lock<std::shared_mutex> verrou(gVerrou);
            pris = gEnsemble;
            pris.insert(gEnsembleR.begin(), gEnsembleR.end());
        }
        std::vector<uint32> const gladiateurs = GladiateursDeLaConf();
        do
        {
            Field* f = res->Fetch();
            uint32 const guid = f[0].Get<uint32>();
            uint8 const race = f[1].Get<uint8>();
            uint8 const classe = f[2].Get<uint8>();
            uint8 const niv = f[3].Get<uint8>();
            std::string const nom = f[4].Get<std::string>();
            if (pris.count(guid) || gReserve.count(guid) || NomExclu(nom) ||
                std::find(gladiateurs.begin(), gladiateurs.end(), guid) != gladiateurs.end())
                continue;
            if (sPlayerbotAIConfig.disableDeathKnightLogin && classe == CLASS_DEATH_KNIGHT)
                continue;
            if (CoaDirecteur::CibleDeBanc(guid) || CoaBancSoin::MembreEngage(guid))
                continue;
            // Mercenaire : il gagne des niveaux en tuant (MercenaryRewards), contre le niveau fige ; et il attaque son camp.
            if (sPlayerbotAIConfig.IsMercenary(Guid(guid).GetRawValue()))
                continue;
            if (ObjectAccessor::FindConnectedPlayer(Guid(guid)))
                continue;
            int const faction = FactionDe(Player::TeamIdForRace(race));
            if (faction < 0)
                continue;
            pool.push_back(CandidatR{guid, classe, niv, uint8(faction), urand(0, 1000000), false});
        } while (res->NextRow());
    }

    auto prendre = [&](Besoin& b, CandidatR& c, uint8 cible, bool montee)
    {
        c.pris = true;
        ++b.rang;
        // Synchrone : la ligne doit exister avant le premier UPDATE de reveil (file asynchrone a plusieurs fils).
        CharacterDatabase.DirectExecute(
            "INSERT IGNORE INTO coa_reserve (bot_guid, tranche, faction, rang, etat, origine, niveau_cible, cree)"
            " VALUES ({}, {}, '{}', {}, 'dort', '{}', {}, UTC_TIMESTAMP())",
            c.guid, uint32(b.t), NomFaction(b.f), uint32(b.rang), montee ? "montee" : "naturel", uint32(cible));
        MembreR m;
        m.bot = c.guid;
        m.tranche = b.t;
        m.faction = b.f;
        m.rang = b.rang;
        m.niveauCible = cible;
        m.etat = Etat::Dort;
        m.montee = montee;
        gReserve[c.guid] = m;
        EnsembleRAjouter(c.guid);
        CoucherR(c.guid);   // hors ligne : « logout » long, hors du tirage des maintenant
        b.classes.insert(c.classe);
        ++b.r.pris;
        if (montee)
            ++b.r.montes;
    };

    // 1. Les bots deja dans la tranche : pour chaque niveau voulu, le plus proche, une classe pas encore prise d'abord.
    for (Besoin& b : besoins)
    {
        std::vector<uint8> restantes;
        for (uint8 cible : b.cibles)
        {
            CandidatR* meilleur = nullptr;
            // Un niveau d'ecart tolere pour varier les classes (les roles suivent la classe et la spe).
            auto cle = [&](CandidatR const& c)
            {
                int const ecart = std::abs(int(c.niveau) - int(cible));
                return std::make_tuple(std::max(0, ecart - 1), b.classes.count(c.classe) ? 1 : 0, ecart, c.alea);
            };
            for (CandidatR& c : pool)
                if (!c.pris && c.faction == b.f && TrancheDe(c.niveau) == b.t && (!meilleur || cle(c) < cle(*meilleur)))
                    meilleur = &c;
            if (meilleur)
                prendre(b, *meilleur, meilleur->niveau, false);
            else
                restantes.push_back(cible);
        }
        b.cibles = restantes;
    }
    // 2. Tranche trop maigre : MONTEE d'un bot plus bas (le plus haut d'abord, classe variee), au premier reveil.
    std::vector<Besoin*> ordre;
    for (Besoin& b : besoins)
        ordre.push_back(&b);
    std::sort(ordre.begin(), ordre.end(), [](Besoin const* a, Besoin const* c) { return a->t > c->t; });
    for (Besoin* b : ordre)
    {
        std::vector<uint8> restantes;
        for (uint8 cible : b->cibles)
        {
            CandidatR* meilleur = nullptr;
            auto cle = [&](CandidatR const& c)
            { return std::make_tuple(-int(c.niveau), b->classes.count(c.classe) ? 1 : 0, c.alea); };
            for (CandidatR& c : pool)
            {
                if (c.pris || c.faction != b->f || c.niveau >= BasDe(b->t))
                    continue;
                // Un chevalier de la mort ne monte que vers un niveau qu'il peut porter (55 et plus).
                if (c.classe == CLASS_DEATH_KNIGHT && cible < sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL))
                    continue;
                if (!meilleur || cle(c) < cle(*meilleur))
                    meilleur = &c;
            }
            if (meilleur)
                prendre(*b, *meilleur, cible, true);
            else
                restantes.push_back(cible);
        }
        b->cibles = restantes;
    }
    return finir(nullptr);
}

// Met en file les reveils des membres endormis de (t, f), dans la limite de Reserve.MaxEveilles.
void ProgrammerR(uint8 t, uint8 f, time_t maintenant)
{
    if (!gTranches[t].eveillee)
        return;
    std::vector<std::pair<uint8, uint32>> aReveiller;   // (rang, bot) : dans l'ordre des rangs
    for (auto const& [bot, m] : gReserve)
    {
        if (m.tranche != t || m.faction != f || m.etat == Etat::Retire || gEveillesR.count(bot) || gEnFileR.count(bot))
            continue;
        auto r = gReessaiR.find(bot);
        if (r != gReessaiR.end())
        {
            if (maintenant < r->second)
                continue;
            gReessaiR.erase(r);
        }
        aReveiller.emplace_back(m.rang, bot);
    }
    std::sort(aReveiller.begin(), aReveiller.end());
    for (auto const& [rang, bot] : aReveiller)
    {
        if (gEveillesR.size() + gEnFileR.size() >= sPlayerbotAIConfig.coaReserveMaxEveilles)
        {
            if (maintenant >= gDernierAvisPlafondR + RELEVE_S)
            {
                gDernierAvisPlafondR = maintenant;
                LOG_INFO("playerbots", "coa reserve: plafond tranche={} faction={} eveilles={} en_file={} max={}",
                         uint32(t), NomFaction(f), uint32(gEveillesR.size()), uint32(gEnFileR.size()),
                         sPlayerbotAIConfig.coaReserveMaxEveilles);
            }
            return;
        }
        gFileR.push_back(bot);
        gEnFileR.insert(bot);
    }
}

void EveillerTranche(uint8 t, char const* motif, time_t maintenant)
{
    Tranche& tr = gTranches[t];
    if (!tr.eveillee)
        LOG_INFO("playerbots", "coa reserve: reveil_tranche tranche={} niveaux={}-{} motif={} humains={} alliance={} horde={}",
                 uint32(t), BasDe(t), HautDe(t), motif, tr.humains, tr.humainsF[F_ALLIANCE], tr.humainsF[F_HORDE]);
    tr.eveillee = true;
    ConstituerR({{t, F_ALLIANCE}, {t, F_HORDE}}, maintenant, false);
    ProgrammerR(t, F_ALLIANCE, maintenant);
    ProgrammerR(t, F_HORDE, maintenant);
}

// Releve des humains en jeu par tranche, puis reveil, sommeil apres Grace, complement et reveils manquants.
void ScanTranches(time_t maintenant)
{
    uint32 avant[NB_TRANCHES];
    for (uint8 t = 0; t < NB_TRANCHES; ++t)
    {
        avant[t] = gTranches[t].humains;
        gTranches[t].humains = 0;
        gTranches[t].humainsF[0] = gTranches[t].humainsF[1] = 0;
    }
    for (auto it = gHumainsR.begin(); it != gHumainsR.end();)
    {
        Player* p = ObjectAccessor::FindConnectedPlayer(Guid(*it));
        if (!p || !p->GetSession() || p->GetSession()->IsBot())
        {
            it = gHumainsR.erase(it);   // depart manque par le crochet : il ne compte plus
            continue;
        }
        uint8 const t = TrancheDe(p->GetLevel());
        if (t < NB_TRANCHES)
        {
            ++gTranches[t].humains;
            int const f = FactionDe(p->GetTeamId());
            if (f >= 0)
                ++gTranches[t].humainsF[f];
        }
        ++it;
    }
    for (uint8 t = 0; t < NB_TRANCHES; ++t)
    {
        Tranche& tr = gTranches[t];
        if (tr.humains)
        {
            tr.derniereVue = maintenant;
            tr.commande = false;   // un humain la tient : la regle ordinaire reprend
            if (!tr.eveillee && !tr.suspendue)
                EveillerTranche(t, "humain", maintenant);
        }
        else
        {
            tr.suspendue = false;
            if (avant[t] && tr.eveillee)
                LOG_INFO("playerbots", "coa reserve: depart_tranche tranche={} grace={}", uint32(t),
                         sPlayerbotAIConfig.coaCompagnonsGrace);
        }
        if (!tr.eveillee)
            continue;
        if (!tr.humains && maintenant >= tr.derniereVue + time_t(sPlayerbotAIConfig.coaCompagnonsGrace))
        {
            EndormirTranche(t, tr.commande ? "commande_echue" : "grace", maintenant);
            continue;
        }
        // Eveillee : complement (membre retire, bot supprime) et reveils manquants (plafond libere, bot sorti).
        ConstituerR({{t, F_ALLIANCE}, {t, F_HORDE}}, maintenant, false);
        ProgrammerR(t, F_ALLIANCE, maintenant);
        ProgrammerR(t, F_HORDE, maintenant);
    }
}

void ReveillerR(time_t maintenant)
{
    uint32 const parTour = sPlayerbotAIConfig.coaCompagnonsEchelonnement ? 1 : REVEILS_PAR_SECONDE_MAX;
    for (uint32 n = 0; n < parTour && !gFileR.empty() && maintenant >= gProchainReveil;)
    {
        uint32 const bot = gFileR.front();
        gFileR.pop_front();
        gEnFileR.erase(bot);
        auto it = gReserve.find(bot);
        if (it == gReserve.end() || it->second.etat == Etat::Retire || gEveillesR.count(bot))
            continue;
        MembreR& m = it->second;
        if (!gTranches[m.tranche].eveillee)
            continue;
        CharacterCacheEntry const* e = sCharacterCache->GetCharacterCacheByGuid(Guid(bot));
        if (!e)
        {
            RetirerR(m, "absent");
            continue;
        }
        // Jamais de baisse : un bot sorti de sa tranche par le haut (ce qui ne devrait pas arriver) la quitte.
        if (e->Level > HautDe(m.tranche))
        {
            RetirerR(m, "hors_tranche");
            continue;
        }
        sRandomPlayerbotMgr.CoaEnroler(Guid(bot), DureePresence());
        gEveillesR.insert(bot);
        gRetenusR.erase(bot);
        gAPlacer[bot] = maintenant + PLACEMENT_LIMITE_S;
        m.etat = Etat::Eveille;
        CharacterDatabase.Execute(
            "UPDATE coa_reserve SET etat = 'eveille', dernier_eveil = UTC_TIMESTAMP(), niveau_reveil = {} WHERE bot_guid = {}",
            uint32(e->Level), bot);
        LOG_INFO("playerbots", "coa reserve: eveil tranche={} faction={} bot={} niv={} cible={}", uint32(m.tranche),
                 NomFaction(m.faction), e->Name, uint32(e->Level), uint32(m.niveauCible));
        gProchainReveil = maintenant + sPlayerbotAIConfig.coaCompagnonsEchelonnement;
        ++n;
    }
}

// La recette de `.coa niveau` (CoaDuelCommandScript.cpp) : GiveLevel, puis competences, sorts de classe, sorts
// disponibles, equipement, munitions, familier. Ni InitTalentsTree ni Randomize : la spe ne change pas. Les points de
// talent du nouveau niveau sont places par l'IA du bot (paquet de montee -> AutoMaintenanceOnLevelupAction ->
// ApplyCoaTalents / AutoPickTalents, comme a toute montee) ; sa teleportation de niveau est refusee (EstCompagnon).
void Monter(Player* p, MembreR& m)
{
    uint32 const avant = p->GetLevel();
    uint32 const cible = m.niveauCible;
    if (p->IsMounted())
        p->Dismount();
    p->GiveLevel(uint8(cible));
    PlayerbotFactory fabrique(p, cible, 0);
    fabrique.InitSkills();
    fabrique.InitClassSpells();
    fabrique.InitAvailableSpells();
    fabrique.InitEquipment(false);
    fabrique.InitAmmo();
    fabrique.InitPet();
    // Ce que AiFactory fait a chaque connexion (la sienne a eu lieu a l'ancien niveau) : specialisation tiree par le
    // hachage du guid (deja posee : inchangee ; posee seulement si le bot passe 10), puis les points du nouveau niveau.
    EnsureCoaSpecialization(p);
    ApplyCoaTalents(p);
    p->SaveToDB(false, false);
    CharacterDatabase.Execute("UPDATE coa_reserve SET monte_le = UTC_TIMESTAMP() WHERE bot_guid = {}", m.bot);
    LOG_INFO("playerbots", "coa reserve: montee tranche={} faction={} bot={} niv={}->{} sorts={}", uint32(m.tranche),
             NomFaction(m.faction), p->GetName(), avant, uint32(p->GetLevel()), uint32(p->GetSpellMap().size()));
}

void PlacerBot(Player* p, MembreR const& m)
{
    // Pres d'un humain de la tranche de SA faction, en monde ouvert.
    std::vector<Player*> humains;
    for (uint32 h : gHumainsR)
    {
        Player* hp = ObjectAccessor::FindConnectedPlayer(Guid(h));
        if (!hp || !hp->IsInWorld() || hp->IsBeingTeleported() || TrancheDe(hp->GetLevel()) != m.tranche ||
            FactionDe(hp->GetTeamId()) != int(m.faction))
            continue;
        Map* carte = hp->GetMap();
        if (!carte || carte->Instanceable())
            continue;
        humains.push_back(hp);
    }
    if (!humains.empty())
    {
        Player* hp = humains[urand(0, uint32(humains.size() - 1))];
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint32 essais = 0;
        if (CoaDirecteur::PointHorsDeVue(hp, p, p->GetLevel(), x, y, z, essais))
        {
            if (CoaDirecteur::Sauter(p, WorldLocation(hp->GetMapId(), x, y, z + 0.5f, p->GetOrientation())))
            {
                float const dx = x - hp->GetPositionX(), dy = y - hp->GetPositionY();
                LOG_INFO("playerbots", "coa reserve: placement tranche={} faction={} bot={} mode=pres humain={} carte={}"
                         " distance={:.0f} essais={}", uint32(m.tranche), NomFaction(m.faction), p->GetName(),
                         hp->GetName(), hp->GetMapId(), std::sqrt(dx * dx + dy * dy), essais);
                return;
            }
        }
        LOG_INFO("playerbots", "coa reserve: placement tranche={} faction={} bot={} mode=pres_echoue humain={} essais={}",
                 uint32(m.tranche), NomFaction(m.faction), p->GetName(), hp->GetName(), essais);
    }
    // L'autre faction (ou aucun point sur) : une zone de son niveau. RandomTeleport refuse un bot de reserve, sauf
    // celui que ce fil place ici (PlacementAutorise).
    uint32 const carteAvant = p->GetMapId();
    tPlacement = m.bot;
    sRandomPlayerbotMgr.RandomTeleportGrindForLevel(p);
    tPlacement = 0;
    LOG_INFO("playerbots", "coa reserve: placement tranche={} faction={} bot={} mode=niveau carte_avant={} teleporte={}",
             uint32(m.tranche), NomFaction(m.faction), p->GetName(), carteAvant, p->IsBeingTeleported() ? 1 : 0);
}

// Les bots reveilles, une fois en jeu : niveau fige, controle de faction (GetTeamId), montee, placement.
void Placer(time_t maintenant)
{
    if (gAPlacer.empty())
        return;
    std::vector<std::pair<uint32, time_t>> aPlacer(gAPlacer.begin(), gAPlacer.end());
    uint32 places = 0, montees = 0;
    for (auto const& [bot, limite] : aPlacer)
    {
        if (places >= PLACEMENTS_PAR_SECONDE)
            break;
        auto it = gReserve.find(bot);
        if (it == gReserve.end() || it->second.etat != Etat::Eveille || !gEveillesR.count(bot))
        {
            gAPlacer.erase(bot);
            continue;
        }
        MembreR& m = it->second;
        Player* p = ObjectAccessor::FindConnectedPlayer(Guid(bot));
        if (!p || !p->IsInWorld() || p->IsBeingTeleported() || p->IsDuringRemoveFromWorld())
        {
            if (maintenant >= limite)
            {
                gAPlacer.erase(bot);
                LOG_INFO("playerbots", "coa reserve: placement tranche={} faction={} bot={} mode=abandon raison=hors_monde",
                         uint32(m.tranche), NomFaction(m.faction), NomDe(bot));
            }
            continue;
        }
        // Faction du JEU (races 21 et 22 du PTR) : la constitution l'a lue dans ChrRaces.dbc.
        if (FactionDe(p->GetTeamId()) != int(m.faction))
        {
            LOG_INFO("playerbots", "coa reserve: faction_ecart bot={} constitution={} jeu={}", p->GetName(),
                     NomFaction(m.faction), uint32(p->GetTeamId()));
            RetirerR(m, "faction");
            continue;
        }
        if (p->GetLevel() > HautDe(m.tranche))
        {
            RetirerR(m, "hors_tranche");
            continue;
        }
        bool const aMonter = p->GetLevel() < m.niveauCible;
        if (aMonter && montees >= MONTEES_PAR_SECONDE)
            continue;
        // Mort a sa connexion (il s'etait endormi fantome) : releve sur place, personne ne le voit, avant de le
        // deplacer -- sinon son cadavre resterait a l'autre bout du monde.
        if (!p->IsAlive())
        {
            p->ResurrectPlayer(1.0f);
            p->SpawnCorpseBones();
        }
        if (aMonter)
        {
            ++montees;
            Monter(p, m);
        }
        p->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);   // ceinture : PlayerbotHolder::OnBotLogin le repose deja
        ++places;
        gAPlacer.erase(bot);
        PlacerBot(p, m);
    }
}

void RenouvelerR(time_t maintenant)
{
    std::vector<uint32> eveilles(gEveillesR.begin(), gEveillesR.end());
    for (uint32 bot : eveilles)
        sRandomPlayerbotMgr.CoaEnroler(Guid(bot), DureePresence());   // reconnecte aussi un bot tombe
    std::vector<uint32> retenus(gRetenusR.begin(), gRetenusR.end());
    for (uint32 bot : retenus)
    {
        if (gEveillesR.count(bot) || gEnFileR.count(bot) || !ObjectAccessor::FindConnectedPlayer(Guid(bot)))
        {
            gRetenusR.erase(bot);
            continue;
        }
        if (CoucherR(bot))
            LOG_INFO("playerbots", "coa reserve: sommeil bot={} apres=0 motif=groupe_libere", NomDe(bot));
    }
    (void)maintenant;
}

void ReleverR()
{
    uint32 tranches = 0, humains = 0;
    for (Tranche const& tr : gTranches)
    {
        tranches += tr.eveillee ? 1 : 0;
        humains += tr.humains;
    }
    if (!tranches && gEveillesR.empty() && gRetenusR.empty())
        return;
    std::string detail;
    for (uint8 t = 0; t < NB_TRANCHES; ++t)
        if (gTranches[t].eveillee)
            detail += (detail.empty() ? "" : ",") + std::to_string(t);
    LOG_INFO("playerbots",
             "coa reserve releve: tranches_eveillees={} ({}) humains={} eveilles={} en_file={} a_placer={} retenus={}",
             tranches, detail.empty() ? "-" : detail, humains, uint32(gEveillesR.size()), uint32(gEnFileR.size()),
             uint32(gAPlacer.size()), uint32(gRetenusR.size()));
}

void ChargerReserve()
{
    bool const confEnable = sPlayerbotAIConfig.coaReserveEnable;
    if (!gReserveInit)
    {
        gReserveInit = true;
        gReserveFige.store(confEnable, std::memory_order_relaxed);
    }
    else
    {
        bool const fige = gReserveFige.load(std::memory_order_relaxed);
        if (gReserveCharge || !fige)
        {
            if (gReserveCharge || confEnable)
                LOG_INFO("playerbots", "coa reserve: rechargement ignore enable_fige={} enable_conf={} reserve_gardee={}",
                         fige ? 1 : 0, confEnable ? 1 : 0, gReserveCharge ? 1 : 0);
            return;
        }
    }
    if (!gReserveFige.load(std::memory_order_relaxed))
        return;   // inerte : pas une requete

    gReserve.clear();
    gHumainsR.clear();
    gEveillesR.clear();
    gFileR.clear();
    gEnFileR.clear();
    gAPlacer.clear();
    gReessaiR.clear();
    gRetenusR.clear();
    for (Tranche& tr : gTranches)
        tr = Tranche();
    {
        std::unique_lock<std::shared_mutex> verrou(gVerrou);
        gEnsembleR.clear();
    }

    if (!CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'coa_reserve'"))
    {
        LOG_ERROR("playerbots", "coa reserve: inerte raison=table_absente -- table coa_reserve absente de la base "
                                "personnages (appliquer data/sql/characters/updates/2026_10_26_00_coa_reserve.sql, puis "
                                "recharger la conf ou redemarrer)");
        return;
    }
    gTableCompagnonPresente = CharacterDatabase.Query(
        "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'coa_compagnon'") != nullptr;

    uint32 membres = 0, retires = 0, eveillesAvant = 0, montees = 0;
    if (QueryResult res = CharacterDatabase.Query(
            "SELECT r.bot_guid, r.tranche, r.faction, r.rang, r.etat, r.niveau_cible, r.origine, b.guid, b.level"
            " FROM coa_reserve r LEFT JOIN characters b ON b.guid = r.bot_guid AND b.deleteInfos_Account IS NULL"))
    {
        do
        {
            Field* f = res->Fetch();
            MembreR m;
            m.bot = f[0].Get<uint32>();
            m.tranche = f[1].Get<uint8>();
            m.faction = f[2].Get<std::string>() == "horde" ? F_HORDE : F_ALLIANCE;
            m.rang = f[3].Get<uint8>();
            std::string const etat = f[4].Get<std::string>();
            m.niveauCible = f[5].Get<uint8>();
            m.montee = f[6].Get<std::string>() == "montee";
            bool const botPresent = !f[7].IsNull();
            uint32 const niveau = botPresent ? f[8].Get<uint32>() : 0;

            char const* motif = nullptr;
            if (etat == "retire")
                m.etat = Etat::Retire;
            else if (!botPresent)
                motif = "absent";
            else if (m.tranche >= NB_TRANCHES)
                motif = "tranche_invalide";
            else if (niveau > HautDe(m.tranche))
                motif = "hors_tranche";
            if (motif)
            {
                m.etat = Etat::Retire;
                if (botPresent)
                    sRandomPlayerbotMgr.CoaLiberer(Guid(m.bot));
                CharacterDatabase.DirectExecute(
                    "UPDATE coa_reserve SET etat = 'retire', motif_retrait = '{}' WHERE bot_guid = {}", motif, m.bot);
            }
            else if (m.etat != Etat::Retire)
            {
                if (etat == "eveille")
                    ++eveillesAvant;
                if (niveau < m.niveauCible)
                    ++montees;
                m.etat = Etat::Dort;
                EnsembleRAjouter(m.bot);
                CoucherR(m.bot);   // « logout » long sur toute la reserve, avant le premier tirage
                ++membres;
            }
            if (m.etat == Etat::Retire)
                ++retires;
            gReserve[m.bot] = m;
        } while (res->NextRow());
    }
    CharacterDatabase.DirectExecute("UPDATE coa_reserve SET etat = 'dort' WHERE etat = 'eveille'");

    gReserveCharge = true;
    gProchainScan = 0;
    // Nouvel essai apres une table absente (recharge de conf) : des humains sont peut-etre deja en jeu.
    for (Player* p : sRandomPlayerbotMgr.GetPlayers())
        if (p && p->GetSession() && !p->GetSession()->IsBot() && CompteHumain(p->GetSession()->GetAccountId()))
            gHumainsR.insert(p->GetGUID().GetCounter());
    LOG_INFO("playerbots",
             "coa reserve: charge membres={} retires={} montees_en_attente={} eveilles_a_l_arret={} par_tranche={} "
             "max_eveilles={} (tous endormis)",
             membres, retires, montees, eveillesAvant, sPlayerbotAIConfig.coaReserveParTranche,
             sPlayerbotAIConfig.coaReserveMaxEveilles);
}

// « 3 » ou « 31-40 » -> tranche 3. Faux si illisible.
bool LireTranche(std::string const& arg, uint8& t)
{
    if (arg.empty())
        return false;
    size_t const tiret = arg.find('-');
    std::string const premier = arg.substr(0, tiret);
    if (premier.empty() || premier.size() > 3 || premier.find_first_not_of("0123456789") != std::string::npos)
        return false;
    uint32 const a = std::stoul(premier);
    if (tiret == std::string::npos)
    {
        if (a >= NB_TRANCHES)
            return false;
        t = uint8(a);
        return true;
    }
    std::string const second = arg.substr(tiret + 1);
    if (second.empty() || second.size() > 3 || second.find_first_not_of("0123456789") != std::string::npos)
        return false;
    uint32 const b = std::stoul(second);
    if (!a || (a - 1) % 10 || b != a + 9 || (a - 1) / 10 >= NB_TRANCHES)
        return false;
    t = uint8((a - 1) / 10);
    return true;
}

bool ReserveCommandePossible(ChatHandler* handler, char const* commande)
{
    if (ReserveActive())
        return true;
    handler->PSendSysMessage("coa reserve {}: inerte (AiPlayerbot.CoaCompagnons.Reserve.Enable = 0 au demarrage, ou "
                             "table coa_reserve absente).", commande);
    return false;
}
}  // namespace

namespace CoaCompagnons
{
bool EstCompagnon(uint32 botGuid)
{
    bool const compagnons = gEnableFige.load(std::memory_order_relaxed);
    bool const reserve = gReserveFige.load(std::memory_order_relaxed);
    if (!compagnons && !reserve)
        return false;
    std::shared_lock<std::shared_mutex> verrou(gVerrou);
    return (compagnons && gEnsemble.count(botGuid)) || (reserve && gEnsembleR.count(botGuid));
}

bool EstReserve(uint32 botGuid)
{
    if (!gReserveFige.load(std::memory_order_relaxed))
        return false;
    std::shared_lock<std::shared_mutex> verrou(gVerrou);
    return gEnsembleR.count(botGuid) != 0;
}

bool PlacementAutorise(uint32 botGuid) { return botGuid && tPlacement == botGuid; }

void ChargerEtEndormir()
{
    // Revision du 26/10 : la reserve d'abord, pour que le chargement des compagnons ecarte un bot deja en reserve.
    ChargerReserve();

    // Revue du 26/10 (M1) : RandomPlayerbotMgr::Init repasse ici a CHAQUE recharge de la conf (`.reload config` puis
    // `playerbots rndbot reload`, `.playerbots bot reload` -> PlayerbotAIConfig::Initialize). Tout vider alors
    // endormait et deconnectait les compagnons des humains en jeu, sans rien pour les reveiller. Donc :
    //  - Enable est fige a la premiere lecture ;
    //  - registre deja charge : on le garde tel quel ;
    //  - seul cas rejoue : Enable fige a 1 et table absente au premier passage (migration appliquee depuis).
    bool const confEnable = sPlayerbotAIConfig.coaCompagnonsEnable;
    if (!gInitialise)
    {
        gInitialise = true;
        gEnableFige.store(confEnable, std::memory_order_relaxed);
    }
    else
    {
        bool const fige = gEnableFige.load(std::memory_order_relaxed);
        if (gCharge || !fige)
        {
            if (gCharge || confEnable)
                LOG_INFO("playerbots",
                         "coa compagnon: rechargement ignore enable_fige={} enable_conf={} registre_garde={} "
                         "(Enable ne change qu'au redemarrage)",
                         fige ? 1 : 0, confEnable ? 1 : 0, gCharge ? 1 : 0);
            return;
        }
    }

    if (!gEnableFige.load(std::memory_order_relaxed))
        return;   // inerte : pas une requete, pas une ligne (le Live tourne sans la table)

    // Premier passage, ou nouvel essai apres une table absente : l'etat est vide (rien n'a tourne sans gCharge).
    gRegistres.clear();
    gSeances.clear();
    gEveilles.clear();
    gFile.clear();
    gEnFile.clear();
    gRetenus.clear();
    {
        std::unique_lock<std::shared_mutex> verrou(gVerrou);
        gEnsemble.clear();
    }

    if (!CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'coa_compagnon'"))
    {
        // Premier mot « inerte » : un genre que lit exploitation/directeur/compagnons.py (revue du 26/10, m6).
        LOG_ERROR("playerbots", "coa compagnon: inerte raison=table_absente -- table coa_compagnon absente de la base "
                                "personnages (appliquer data/sql/characters/updates/2026_10_25_00_coa_compagnon.sql, "
                                "puis recharger la conf ou redemarrer)");
        return;
    }

    // Humain supprime (ou detache de son compte) : ses bots retournent a la vie ordinaire, la ligne disparait.
    uint32 purges = 0;
    if (QueryResult orphelins = CharacterDatabase.Query(
            "SELECT cc.bot_guid FROM coa_compagnon cc LEFT JOIN characters h ON h.guid = cc.humain_guid"
            " WHERE h.guid IS NULL OR h.deleteInfos_Account IS NOT NULL"))
    {
        do
        {
            sRandomPlayerbotMgr.CoaLiberer(Guid(orphelins->Fetch()[0].Get<uint32>()));
            ++purges;
        } while (orphelins->NextRow());
        CharacterDatabase.DirectExecute(
            "DELETE cc FROM coa_compagnon cc LEFT JOIN characters h ON h.guid = cc.humain_guid"
            " WHERE h.guid IS NULL OR h.deleteInfos_Account IS NOT NULL");
    }

    uint32 humains = 0, compagnons = 0, retires = 0, eveillesAvant = 0;
    if (QueryResult res = CharacterDatabase.Query(
            "SELECT cc.humain_guid, cc.bot_guid, cc.camp, cc.rang, cc.etat, b.guid FROM coa_compagnon cc"
            " LEFT JOIN characters b ON b.guid = cc.bot_guid AND b.deleteInfos_Account IS NULL"))
    {
        do
        {
            Field* f = res->Fetch();
            uint32 const humain = f[0].Get<uint32>();
            Membre m;
            m.bot = f[1].Get<uint32>();
            m.camp = f[2].Get<std::string>() == "vis_a_vis" ? Camp::VisAVis : Camp::Allie;
            m.rang = f[3].Get<uint8>();
            std::string const etat = f[4].Get<std::string>();
            bool const botPresent = !f[5].IsNull();

            std::vector<Membre>& reg = gRegistres[humain];
            if (reg.empty())
                ++humains;

            if (etat == "retire")
                m.etat = Etat::Retire;
            else if (!botPresent)
            {
                m.etat = Etat::Retire;
                CharacterDatabase.DirectExecute(
                    "UPDATE coa_compagnon SET etat = 'retire', motif_retrait = 'absent' WHERE humain_guid = {} AND bot_guid = {}",
                    humain, m.bot);
            }
            else if (EstReserveInterne(m.bot))
            {
                // Revision du 26/10 : un bot n'est jamais a la fois compagnon et reserve ; la reserve le garde.
                m.etat = Etat::Retire;
                CharacterDatabase.DirectExecute(
                    "UPDATE coa_compagnon SET etat = 'retire', motif_retrait = 'reserve' WHERE humain_guid = {} AND bot_guid = {}",
                    humain, m.bot);
            }
            else
            {
                if (etat == "eveille")
                    ++eveillesAvant;
                m.etat = Etat::Dort;
                // « logout » long sur TOUT le registre, avant le premier tirage (§ 6.4 de la conception).
                EnsembleAjouter(m.bot);
                Coucher(humain, m.bot);
                ++compagnons;
            }
            if (m.etat == Etat::Retire)
                ++retires;
            reg.push_back(m);
        } while (res->NextRow());
    }
    CharacterDatabase.DirectExecute("UPDATE coa_compagnon SET etat = 'dort' WHERE etat = 'eveille'");

    gCharge = true;
    LOG_INFO("playerbots",
             "coa compagnon: charge humains={} compagnons={} retires={} purges={} eveilles_a_l_arret={} (tous endormis)",
             humains, compagnons, retires, purges, eveillesAvant);
}

void HumainConnecte(Player* humain)
{
    if (!humain || !humain->GetSession())
        return;
    // Revision du 26/10 : la reserve de sa tranche (releve des la seconde suivante).
    if (ReserveActive() && !humain->GetSession()->IsBot() && CompteHumain(humain->GetSession()->GetAccountId()))
    {
        gHumainsR.insert(humain->GetGUID().GetCounter());
        gProchainScan = 0;
    }
    if (!Actif())
        return;
    time_t const maintenant = Maintenant();
    uint32 const h = humain->GetGUID().GetCounter();
    uint32 const compte = humain->GetSession()->GetAccountId();

    // Changement de personnage sur le meme compte : l'ancien registre s'endort tout de suite (pas de grace).
    std::vector<uint32> anciens;
    for (auto const& [autre, s] : gSeances)
        if (autre != h && s.compte == compte && !s.present)
            anciens.push_back(autre);
    for (uint32 autre : anciens)
    {
        Endormir(autre, "changement", maintenant);
        gSeances.erase(autre);
    }

    Seance& s = gSeances[h];
    bool const retour = s.debut != 0 && !s.present;
    s.compte = compte;
    s.present = true;
    s.depart = 0;
    s.commande = false;
    if (!s.debut)
        s.debut = maintenant;

    uint32 eveilles = 0;
    for (auto const& [bot, hh] : gEveilles)
        if (hh == h)
            ++eveilles;
    LOG_INFO("playerbots", "coa compagnon: arrivee humain={} niv={} retour_en_grace={} eveilles={}", humain->GetName(),
             uint32(humain->GetLevel()), retour ? 1 : 0, eveilles);

    Activer(h, humain->GetLevel(), humain->GetTeamId(), s, maintenant);
}

void HumainDeconnecte(Player* humain)
{
    if (!humain)
        return;
    // Revision du 26/10 : il ne compte plus dans sa tranche (encore « connecte » pendant ce crochet : retire a la main).
    if (ReserveActive() && gHumainsR.erase(humain->GetGUID().GetCounter()))
        gProchainScan = 0;
    if (!Actif())
        return;
    auto it = gSeances.find(humain->GetGUID().GetCounter());
    if (it == gSeances.end() || !it->second.present)
        return;
    it->second.present = false;
    it->second.commande = false;
    it->second.depart = Maintenant();
    LOG_INFO("playerbots", "coa compagnon: depart humain={} grace={}", humain->GetName(),
             sPlayerbotAIConfig.coaCompagnonsGrace);
}

void BotSorti(uint32 botGuid)
{
    // Revision du 26/10 : un bot de reserve sorti sans nous (presence expiree, connexion ratee) redevient « dort » ; sa
    // tranche, si elle est eveillee, le reprogramme apres REESSAI_BOT_S (pas de boucle de connexions ratees).
    if (ReserveActive() && gEveillesR.erase(botGuid))
    {
        gAPlacer.erase(botGuid);
        gReessaiR[botGuid] = Maintenant() + REESSAI_BOT_S;
        auto r = gReserve.find(botGuid);
        if (r != gReserve.end() && r->second.etat == Etat::Eveille)
            r->second.etat = Etat::Dort;
        CharacterDatabase.Execute("UPDATE coa_reserve SET etat = 'dort' WHERE bot_guid = {} AND etat = 'eveille'",
                                  botGuid);
        LOG_INFO("playerbots", "coa reserve: sommeil tranche={} faction={} bot={} apres=0 motif=sortie",
                 r != gReserve.end() ? uint32(r->second.tranche) : 0u,
                 r != gReserve.end() ? NomFaction(r->second.faction) : "?", NomDe(botGuid));
        return;
    }
    if (!Actif())
        return;
    auto it = gEveilles.find(botGuid);
    if (it == gEveilles.end())
        return;
    uint32 const humain = it->second;
    gEveilles.erase(it);
    if (Membre* m = Trouver(humain, botGuid))
        if (m->etat == Etat::Eveille)
            m->etat = Etat::Dort;
    CharacterDatabase.Execute(
        "UPDATE coa_compagnon SET etat = 'dort' WHERE humain_guid = {} AND bot_guid = {} AND etat = 'eveille'", humain,
        botGuid);
    LOG_INFO("playerbots", "coa compagnon: sommeil humain={} bot={} apres=0 motif=sortie", NomDe(humain), NomDe(botGuid));
}

void MettreAJour(uint32 diff)
{
    bool const compagnons = Actif();
    bool const reserve = ReserveActive();
    if (!compagnons && !reserve)
        return;
    gCumulMs += diff;
    if (gCumulMs < 1000)
        return;
    gCumulMs = 0;

    time_t const maintenant = Maintenant();
    if (!gProchainRenouvellement)
    {
        gProchainRenouvellement = maintenant + sPlayerbotAIConfig.coaCompagnonsRenouvellement;
        gProchainReleve = maintenant + RELEVE_S;
    }

    if (compagnons)
        Reveiller(maintenant);
    if (reserve)
    {
        if (maintenant >= gProchainScan)
        {
            gProchainScan = maintenant + SCAN_S;
            ScanTranches(maintenant);
        }
        ReveillerR(maintenant);   // meme echelonnement que les compagnons (gProchainReveil partage)
        Placer(maintenant);
    }

    if (maintenant >= gProchainRenouvellement)
    {
        gProchainRenouvellement = maintenant + sPlayerbotAIConfig.coaCompagnonsRenouvellement;
        if (compagnons)
            Renouveler(maintenant);
        if (reserve)
            RenouvelerR(maintenant);
    }
    if (maintenant >= gProchainReleve)
    {
        gProchainReleve = maintenant + RELEVE_S;
        if (compagnons)
            Relever(maintenant);
        if (reserve)
            ReleverR();
    }
}

bool CommandeListe(ChatHandler* handler, std::string const& nom)
{
    CharacterCacheEntry const* e = HumainParNom(handler, nom, "liste");
    if (!e)
        return false;
    uint32 const h = e->Guid.GetCounter();
    time_t const maintenant = Maintenant();

    std::string seance = "aucune";
    auto s = gSeances.find(h);
    if (s != gSeances.end())
    {
        if (s->second.present)
            seance = "en jeu";
        else
        {
            time_t const fin = s->second.depart + time_t(sPlayerbotAIConfig.coaCompagnonsGrace);
            seance = std::string(s->second.commande ? "par commande, " : "en grace, ") + "sommeil dans " +
                     std::to_string(fin > maintenant ? uint64(fin - maintenant) : 0) + " s";
        }
    }
    handler->PSendSysMessage("coa compagnon liste: {} (guid {}) niv {}, seance : {}", e->Name, h, uint32(e->Level), seance);

    auto r = gRegistres.find(h);
    if (r == gRegistres.end() || r->second.empty())
    {
        handler->PSendSysMessage("  registre vide (constitue a la premiere connexion au niveau {} ou plus).",
                                 sPlayerbotAIConfig.coaCompagnonsSeuilNiveau);
        return true;
    }
    for (Membre const& m : r->second)
    {
        CharacterCacheEntry const* b = sCharacterCache->GetCharacterCacheByGuid(Guid(m.bot));
        Player* enJeu = ObjectAccessor::FindConnectedPlayer(Guid(m.bot));
        // Faction : celle du JEU (GetTeamId) quand il est en jeu, sinon celle de sa race (ChrRaces.dbc).
        TeamId const faction = enJeu ? enJeu->GetTeamId() : (b ? Player::TeamIdForRace(b->Race) : TEAM_NEUTRAL);
        handler->PSendSysMessage("  {} (guid {}) {} {} : {}, niv {}, faction {}{}, {}{}", b ? b->Name : "?", m.bot,
                                 NomCamp(m.camp), uint32(m.rang), NomEtat(m.etat), b ? uint32(b->Level) : 0,
                                 faction == TEAM_ALLIANCE ? "A" : (faction == TEAM_HORDE ? "H" : "?"),
                                 enJeu ? " (GetTeamId)" : " (race)", enJeu ? "en jeu" : "hors jeu",
                                 gEnFile.count(m.bot) ? ", reveil en file" : "");
    }
    return true;
}

bool CommandeEveiller(ChatHandler* handler, std::string const& nom)
{
    CharacterCacheEntry const* e = HumainParNom(handler, nom, "eveiller");
    if (!e)
        return false;
    uint32 const h = e->Guid.GetCounter();
    time_t const maintenant = Maintenant();

    if (e->Level < sPlayerbotAIConfig.coaCompagnonsSeuilNiveau)
    {
        handler->PSendSysMessage("coa compagnon eveiller: {} est niveau {}, sous le seuil {}.", e->Name, uint32(e->Level),
                                 sPlayerbotAIConfig.coaCompagnonsSeuilNiveau);
        return false;
    }

    Player* enJeu = ObjectAccessor::FindConnectedPlayer(e->Guid);
    if (enJeu && enJeu->GetSession() && !enJeu->GetSession()->IsBot())
    {
        HumainConnecte(enJeu);
        handler->PSendSysMessage("coa compagnon eveiller: {} est en jeu ; reveils en file : {}.", e->Name,
                                 uint32(gEnFile.size()));
        return true;
    }

    // Humain absent : seance « par commande », qui suit la regle d'un depart a l'instant (sommeil apres Grace).
    Seance& s = gSeances[h];
    s.compte = e->AccountId;
    s.present = false;
    s.commande = true;
    s.depart = maintenant;
    if (!s.debut)
        s.debut = maintenant;
    uint32 const ajoutes = Activer(h, e->Level, Player::TeamIdForRace(e->Race), s, maintenant);

    uint32 enFile = 0;
    for (auto const& [hh, bot] : gFile)
        if (hh == h)
            ++enFile;
    handler->PSendSysMessage(
        "coa compagnon eveiller: {} hors jeu ; {} ajoute(s) au registre, {} reveil(s) en file ; sommeil dans {} s.",
        e->Name, ajoutes, enFile, sPlayerbotAIConfig.coaCompagnonsGrace);
    return true;
}

bool CommandeEndormir(ChatHandler* handler, std::string const& nom)
{
    CharacterCacheEntry const* e = HumainParNom(handler, nom, "endormir");
    if (!e)
        return false;
    uint32 const h = e->Guid.GetCounter();
    uint32 const n = Endormir(h, "commande", Maintenant());
    gSeances.erase(h);
    uint32 retenus = 0;
    for (auto const& [bot, hh] : gRetenus)
        if (hh == h)
            ++retenus;
    handler->PSendSysMessage("coa compagnon endormir: {} ; {} compagnon(s) endormi(s), dont {} retenu(s) en jeu par un "
                             "groupe (deconnectes des qu'aucun humain du groupe n'est en ligne). Ils se reveilleront a "
                             "sa prochaine connexion.",
                             e->Name, n, retenus);
    return true;
}
// ------------------------------------------------------------------ commandes de la reserve (revision du 26/10)

bool CommandeReserveListe(ChatHandler* handler, std::string const& arg)
{
    if (!ReserveCommandePossible(handler, "liste"))
        return false;
    time_t const maintenant = Maintenant();
    auto etatTranche = [&](uint8 t)
    {
        Tranche const& tr = gTranches[t];
        if (!tr.eveillee)
            return std::string(tr.suspendue ? "endormie (suspendue par commande)" : "endormie");
        if (tr.humains)
            return std::string("eveillee");
        time_t const fin = tr.derniereVue + time_t(sPlayerbotAIConfig.coaCompagnonsGrace);
        return std::string(tr.commande ? "eveillee par commande" : "eveillee, en grace") + ", sommeil dans " +
               std::to_string(fin > maintenant ? uint64(fin - maintenant) : 0) + " s";
    };

    if (arg.empty())
    {
        handler->PSendSysMessage("coa reserve liste: {} par tranche et par faction, {} eveille(s) / max {}, {} en file, "
                                 "{} a placer, {} retenu(s).", sPlayerbotAIConfig.coaReserveParTranche,
                                 uint32(gEveillesR.size()), sPlayerbotAIConfig.coaReserveMaxEveilles,
                                 uint32(gEnFileR.size()), uint32(gAPlacer.size()), uint32(gRetenusR.size()));
        for (uint8 t = 0; t < NB_TRANCHES; ++t)
        {
            uint32 total[2] = {0, 0}, eveilles[2] = {0, 0}, montees[2] = {0, 0}, retires = 0;
            for (auto const& [bot, m] : gReserve)
            {
                if (m.tranche != t)
                    continue;
                if (m.etat == Etat::Retire)
                {
                    ++retires;
                    continue;
                }
                ++total[m.faction];
                if (gEveillesR.count(bot))
                    ++eveilles[m.faction];
                CharacterCacheEntry const* e = sCharacterCache->GetCharacterCacheByGuid(Guid(bot));
                if (e && e->Level < m.niveauCible)
                    ++montees[m.faction];
            }
            Tranche const& tr = gTranches[t];
            handler->PSendSysMessage("  tranche {} ({}-{}) : {} ; humains {} (A {} / H {}) ; alliance {}/{} eveilles, "
                                     "horde {}/{} eveilles ; montees en attente {} ; retires {}",
                                     uint32(t), BasDe(t), HautDe(t), etatTranche(t), tr.humains,
                                     tr.humainsF[F_ALLIANCE], tr.humainsF[F_HORDE], eveilles[F_ALLIANCE],
                                     total[F_ALLIANCE], eveilles[F_HORDE], total[F_HORDE],
                                     montees[F_ALLIANCE] + montees[F_HORDE], retires);
        }
        return true;
    }

    uint8 t = 0;
    if (!LireTranche(arg, t))
    {
        handler->PSendSysMessage("coa reserve liste: tranche illisible « {} » (0 a 5, ou 1-10 ... 51-60).", arg);
        return false;
    }
    handler->PSendSysMessage("coa reserve liste: tranche {} ({}-{}) : {} ; humains {}.", uint32(t), BasDe(t), HautDe(t),
                             etatTranche(t), gTranches[t].humains);
    std::vector<MembreR const*> membres;
    for (auto const& [bot, m] : gReserve)
        if (m.tranche == t)
            membres.push_back(&m);
    std::sort(membres.begin(), membres.end(), [](MembreR const* a, MembreR const* b)
              { return std::make_pair(a->faction, a->rang) < std::make_pair(b->faction, b->rang); });
    if (membres.empty())
        handler->PSendSysMessage("  reserve vide (constituee au premier reveil, ou par .coa reserve constituer).");
    for (MembreR const* m : membres)
    {
        CharacterCacheEntry const* e = sCharacterCache->GetCharacterCacheByGuid(Guid(m->bot));
        Player* enJeu = ObjectAccessor::FindConnectedPlayer(Guid(m->bot));
        uint32 const niveau = enJeu ? enJeu->GetLevel() : (e ? uint32(e->Level) : 0);
        int const factionJeu = enJeu ? FactionDe(enJeu->GetTeamId()) : -1;
        handler->PSendSysMessage("  {} (guid {}) {} {} : {}, niv {} (cible {}{}), {}{}{}{}", e ? e->Name : "?", m->bot,
                                 NomFaction(m->faction), uint32(m->rang), NomEtat(m->etat), niveau,
                                 uint32(m->niveauCible), m->montee ? ", montee" : "",
                                 enJeu ? "en jeu" : "hors jeu",
                                 enJeu ? (factionJeu == int(m->faction) ? " (GetTeamId conforme)" : " (GetTeamId DIFFERENT)") : "",
                                 gEnFileR.count(m->bot) ? ", reveil en file" : "",
                                 gAPlacer.count(m->bot) ? ", placement en attente" : "");
    }
    return true;
}

bool CommandeReserveEveiller(ChatHandler* handler, std::string const& arg)
{
    if (!ReserveCommandePossible(handler, "eveiller"))
        return false;
    uint8 t = 0;
    if (!LireTranche(arg, t))
    {
        handler->PSendSysMessage("Usage: .coa reserve eveiller <tranche 0-5 ou 1-10 ... 51-60>");
        return false;
    }
    time_t const maintenant = Maintenant();
    Tranche& tr = gTranches[t];
    tr.suspendue = false;
    tr.derniereVue = maintenant;   // sans humain, la tranche s'endort Grace secondes apres cette commande
    bool const deja = tr.eveillee;
    if (!tr.humains)
        tr.commande = true;
    EveillerTranche(t, "commande", maintenant);
    uint32 enFile = 0;
    for (uint32 bot : gFileR)
    {
        auto m = gReserve.find(bot);
        if (m != gReserve.end() && m->second.tranche == t)
            ++enFile;
    }
    handler->PSendSysMessage("coa reserve eveiller: tranche {} ({}-{}) {} ; alliance {} membre(s), horde {} ; {} reveil(s) "
                             "en file ; {}.", uint32(t), BasDe(t), HautDe(t), deja ? "deja eveillee" : "eveillee",
                             ActifsDe(t, F_ALLIANCE), ActifsDe(t, F_HORDE), enFile,
                             tr.humains ? std::string("un humain la tient") :
                                          "sommeil dans " + std::to_string(sPlayerbotAIConfig.coaCompagnonsGrace) + " s");
    return true;
}

bool CommandeReserveEndormir(ChatHandler* handler, std::string const& arg)
{
    if (!ReserveCommandePossible(handler, "endormir"))
        return false;
    uint8 t = 0;
    if (!LireTranche(arg, t))
    {
        handler->PSendSysMessage("Usage: .coa reserve endormir <tranche 0-5 ou 1-10 ... 51-60>");
        return false;
    }
    Tranche& tr = gTranches[t];
    uint32 const n = EndormirTranche(t, "commande", Maintenant());
    if (tr.humains)
        tr.suspendue = true;
    uint32 retenus = 0;
    for (uint32 bot : gRetenusR)
    {
        auto m = gReserve.find(bot);
        if (m != gReserve.end() && m->second.tranche == t)
            ++retenus;
    }
    handler->PSendSysMessage("coa reserve endormir: tranche {} ({}-{}) ; {} bot(s) endormi(s), dont {} retenu(s) en jeu par "
                             "un groupe ; {}.", uint32(t), BasDe(t), HautDe(t), n, retenus,
                             tr.humains ? "suspendue tant qu'un humain y reste (.coa reserve eveiller pour reprendre)"
                                        : "elle se reveillera au prochain humain de la tranche");
    return true;
}

bool CommandeReserveConstituer(ChatHandler* handler)
{
    if (!ReserveCommandePossible(handler, "constituer"))
        return false;
    time_t const maintenant = Maintenant();
    std::vector<std::pair<uint8, uint8>> toutes;
    for (uint8 t = 0; t < NB_TRANCHES; ++t)
        for (uint8 f : {uint8(F_ALLIANCE), uint8(F_HORDE)})
            toutes.emplace_back(t, f);
    std::vector<ResultatR> const res = ConstituerR(toutes, maintenant, true);
    if (res.empty())
        handler->PSendSysMessage("coa reserve constituer: toutes les reserves sont completes ({} par tranche et par faction).",
                                 sPlayerbotAIConfig.coaReserveParTranche);
    for (ResultatR const& r : res)
        handler->PSendSysMessage("coa reserve constituer: tranche {} ({}-{}) {} : voulus {}, pris {} (dont {} a monter), "
                                 "manque {}{}{}", uint32(r.tranche), BasDe(r.tranche), HautDe(r.tranche),
                                 NomFaction(r.faction), r.voulus, r.pris, r.montes, r.manque, r.refus ? ", refus " : "",
                                 r.refus ? r.refus : "");
    // Une tranche eveillee reveille ses nouveaux membres tout de suite.
    for (uint8 t = 0; t < NB_TRANCHES; ++t)
        if (gTranches[t].eveillee)
        {
            ProgrammerR(t, F_ALLIANCE, maintenant);
            ProgrammerR(t, F_HORDE, maintenant);
        }
    return true;
}
}  // namespace CoaCompagnons
