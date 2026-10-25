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
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "WorldSession.h"

#include <algorithm>
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
bool gCharge = false;                                         // table lue au demarrage
uint32 gCumulMs = 0;
time_t gProchainReveil = 0;
time_t gProchainRenouvellement = 0;
time_t gProchainReleve = 0;
time_t gDernierAvisPlafond = 0;

// --- lu par tout fil (EstCompagnon), ecrit au fil monde seul
std::shared_mutex gVerrou;
std::unordered_set<uint32> gEnsemble;   // bots au registre, NON retires

time_t Maintenant() { return GameTime::GetGameTime().count(); }

bool Actif() { return sPlayerbotAIConfig.coaCompagnonsEnable && gCharge; }

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
    sRandomPlayerbotMgr.CoaEndormir(Guid(bot));
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
            sRandomPlayerbotMgr.CoaEndormir(Guid(bot));

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

    // Jamais au-dessus de humain + 2 (§ 5) ; les comptes de type 2 (addclass) ne sont pas dans la liste.
    QueryResult res = CharacterDatabase.Query(
        "SELECT c.guid, c.race, c.class, c.level, c.name FROM characters c"
        " WHERE c.account IN ({}) AND c.level <= {} AND c.deleteInfos_Account IS NULL"
        " AND NOT EXISTS (SELECT 1 FROM coa_compagnon cc WHERE cc.bot_guid = c.guid)"
        " AND NOT EXISTS (SELECT 1 FROM arena_team_member a WHERE a.guid = c.guid){}",
        listeComptes, uint32(niveau) + 2, clauseReserve);

    std::vector<uint32> const gladiateurs = GladiateursDeLaConf();
    std::vector<Candidat> allies, visAVis;
    if (res)
    {
        std::unordered_set<uint32> registre;
        {
            std::shared_lock<std::shared_mutex> verrou(gVerrou);
            registre = gEnsemble;
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

            // Faction par la table des races (ChrRaces.dbc) : les races 21 et 22 ne sont pas celles d'IsAlliance.
            TeamId const sienne = Player::TeamIdForRace(race);
            if (sienne != TEAM_ALLIANCE && sienne != TEAM_HORDE)
                continue;

            Player* enJeu = ObjectAccessor::FindConnectedPlayer(Guid(guid));
            // Un bot en groupe ou en champ de bataille est occupe : on ne le prend pas.
            if (enJeu && (enJeu->GetGroup() || enJeu->InBattleground() || enJeu->InBattlegroundQueue()))
                continue;

            Candidat c{guid, classe, niv, enJeu != nullptr, urand(0, 1000000)};
            if (sienne == camp)
            {
                if (besoinAllies && !sPlayerbotAIConfig.IsMercenary(Guid(guid).GetRawValue()))
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
        sRandomPlayerbotMgr.CoaEndormir(Guid(bot));
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
                sRandomPlayerbotMgr.CoaEndormir(Guid(bot));
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
}

void Relever(time_t maintenant)
{
    if (gSeances.empty() && gEveilles.empty())
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
    LOG_INFO("playerbots", "coa compagnon releve: humains={} seances={} eveilles={} en_file={} en_retard={} devant={}",
             presents, uint32(gSeances.size()), uint32(gEveilles.size()), uint32(gEnFile.size()), enRetard, devant);
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
}  // namespace

namespace CoaCompagnons
{
bool EstCompagnon(uint32 botGuid)
{
    if (!sPlayerbotAIConfig.coaCompagnonsEnable)
        return false;
    std::shared_lock<std::shared_mutex> verrou(gVerrou);
    return gEnsemble.count(botGuid) != 0;
}

void ChargerEtEndormir()
{
    gCharge = false;
    gRegistres.clear();
    gSeances.clear();
    gEveilles.clear();
    gFile.clear();
    gEnFile.clear();
    {
        std::unique_lock<std::shared_mutex> verrou(gVerrou);
        gEnsemble.clear();
    }

    if (!sPlayerbotAIConfig.coaCompagnonsEnable)
        return;   // inerte : pas une requete, pas une ligne (le Live tourne sans la table)

    if (!CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'coa_compagnon'"))
    {
        LOG_ERROR("playerbots", "coa compagnon: table coa_compagnon absente de la base personnages -- compagnons INERTES "
                                "(appliquer data/sql/characters/updates/2026_10_25_00_coa_compagnon.sql)");
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
            else
            {
                if (etat == "eveille")
                    ++eveillesAvant;
                m.etat = Etat::Dort;
                // « logout » long sur TOUT le registre, avant le premier tirage (§ 6.4 de la conception).
                sRandomPlayerbotMgr.CoaEndormir(Guid(m.bot));
                EnsembleAjouter(m.bot);
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
    if (!Actif() || !humain || !humain->GetSession())
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
    if (!Actif() || !humain)
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
    if (!Actif())
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

    Reveiller(maintenant);

    if (maintenant >= gProchainRenouvellement)
    {
        gProchainRenouvellement = maintenant + sPlayerbotAIConfig.coaCompagnonsRenouvellement;
        Renouveler(maintenant);
    }
    if (maintenant >= gProchainReleve)
    {
        gProchainReleve = maintenant + RELEVE_S;
        Relever(maintenant);
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
    handler->PSendSysMessage("coa compagnon endormir: {} ; {} compagnon(s) endormi(s). Ils se reveilleront a sa prochaine connexion.",
                             e->Name, n);
    return true;
}
}  // namespace CoaCompagnons
