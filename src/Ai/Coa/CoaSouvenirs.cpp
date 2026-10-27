/*
 * Conquest of Azeroth -- souvenirs du compagnon (27/10/2026). Voir CoaSouvenirs.h.
 *
 * Demande du commanditaire : « une petite memoire des evenements (quand le joueur est mort, les quetes, les moments
 * un peu compliques, le temps passe a miner...), pour que le compagnon, de temps en temps, lui fasse des messages
 * vraiment lies aux activites qu'ils ont faites ensemble ou qu'il a pu voir ».
 *
 * CE QUI EST NOTE (genre, objet), pour tout humain, qu'il ait deja un compagnon ou non :
 *   mort      la creature qui l'a tue (vide : JcJ, chute, noyade) ; hors champ de bataille et arene
 *   quete     le titre de la quete finie
 *   danger    l'adversaire : vie sous DANGER_PCT en combat, puis sorti du combat VIVANT (au plus un par 10 min)
 *   recolte   minage / herboristerie / depecage : RECOLTE_N recoltes en RECOLTE_FENETRE_S (au plus une par 30 min)
 *   exploit   une creature elite, rare ou boss, pas grise pour lui
 *   niveau    le niveau pris
 * « ensemble » : son compagnon eveille etait en jeu, sur la meme carte, a moins de ENSEMBLE_YD. Lu au fil monde, au
 * plus une seconde apres l'evenement (les crochets tournent dans les fils de carte : ils ne font que mettre en file).
 *
 * CE QUI EST DIT : un chuchotement du compagnon eveille, PremiereParole secondes apres son reveil, puis toutes les
 * IntervalleMin a IntervalleMax secondes ; jamais si l'un des deux est en combat ou mort (nouvel essai 30 s plus tard).
 * Le souvenir choisi n'a jamais ete dit ; d'abord les recents (moins de 12 h) vecus ensemble, puis les recents vus de
 * loin (« on m'a dit que... »), puis les anciens (« tu te souviens ? », les niveaux exceptes), le plus recent d'abord
 * et pas deux fois de suite le meme genre quand on a le choix. Aucun souvenir disponible : nouvel essai 10 min plus
 * tard. Les phrases sont ecrites ici, en francais ; les noms propres (quete, creature, zone) restent ceux du jeu.
 *
 * BASE. Table `coa_compagnon_souvenir` (personnages) : une insertion asynchrone par souvenir, une mise a jour par
 * parole (dit_le), une lecture synchrone a l'arrivee de l'humain (ses 40 derniers non dits de moins de 14 jours), une
 * purge des lignes de plus de 30 jours au demarrage. La table absente : tout est inerte, dit une fois au journal.
 */

#include "CoaSouvenirs.h"

#include "CoaCompagnons.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "Random.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
enum class Genre : uint8
{
    Mort,
    Quete,
    Danger,
    Recolte,
    Exploit,
    Niveau,
};

char const* NomGenre(Genre g)
{
    switch (g)
    {
        case Genre::Mort:    return "mort";
        case Genre::Quete:   return "quete";
        case Genre::Danger:  return "danger";
        case Genre::Recolte: return "recolte";
        case Genre::Exploit: return "exploit";
        case Genre::Niveau:  return "niveau";
    }
    return "?";
}

bool GenreDe(std::string const& nom, Genre& g)
{
    for (Genre x : {Genre::Mort, Genre::Quete, Genre::Danger, Genre::Recolte, Genre::Exploit, Genre::Niveau})
        if (nom == NomGenre(x))
        {
            g = x;
            return true;
        }
    return false;
}

constexpr float DANGER_PCT = 20.0f;
constexpr time_t DANGER_ESPACEMENT_S = 600;
constexpr uint32 RECOLTE_N = 8;
constexpr time_t RECOLTE_FENETRE_S = 1200;
constexpr time_t RECOLTE_ESPACEMENT_S = 1800;
constexpr float ENSEMBLE_YD = 80.0f;
constexpr time_t DOUBLON_S = 600;            // meme genre et meme objet a moins de 10 min : un seul souvenir
constexpr time_t ANCIEN_S = 12 * 3600;
constexpr time_t MORT_DELAI_S = 1;           // le nom du tueur arrive par un second crochet, dans un ordre non garanti
constexpr time_t REESSAI_COMBAT_S = 30;
constexpr time_t REESSAI_VIDE_S = 600;
constexpr size_t MEMOIRE_MAX = 40;
// Plafond de la file entre fils de carte et fil monde : si MettreAJour ne tourne plus (compagnons inertes, Enable = 0
// pendant que l'option des souvenirs reste a 1), les crochets ne la font pas grossir sans fin.
constexpr size_t FILE_MAX = 2000;

struct Evenement
{
    uint32 humain = 0;
    Genre genre = Genre::Quete;
    std::string objet;
    uint32 zone = 0;
};

struct MortEnCours
{
    time_t quand = 0;
    uint32 zone = 0;
    std::string tueur;
    bool morte = false;   // OnPlayerJustDied vu (le tueur seul ne suffit pas : il ne dit pas la mort)
};

struct RecolteEnCours
{
    time_t debut = 0;
    uint32 n = 0;
    time_t prochaine = 0;
};

// --- partage entre fils de carte et fil monde, sous gVerrou
std::mutex gVerrou;
std::vector<Evenement> gFile;
std::unordered_map<uint32, MortEnCours> gMorts;                    // humain -> mort en cours
std::unordered_map<uint64, RecolteEnCours> gRecoltes;              // (humain << 32 | competence)

// --- fil monde seul
struct Souvenir
{
    Genre genre = Genre::Quete;
    std::string objet;
    uint32 zone = 0;
    bool ensemble = false;
    time_t quand = 0;
    bool dit = false;
};

struct Memoire
{
    std::deque<Souvenir> souvenirs;   // le plus recent devant
    bool present = false;
    uint32 compagnon = 0;             // compagnon eveille vu au dernier tour (un changement = un reveil)
    time_t prochaineParole = 0;
    Genre dernierGenre = Genre::Niveau;
    bool aParle = false;
    bool enDanger = false;
    std::string adversaire;
    time_t dernierDanger = 0;
};

std::unordered_map<uint32, Memoire> gMemoires;
int gTable = 0;   // 0 : pas encore lue, 1 : presente, 2 : absente (inerte)

time_t Maintenant() { return GameTime::GetGameTime().count(); }
ObjectGuid Guid(uint32 bas) { return ObjectGuid::Create<HighGuid::Player>(bas); }

bool TableOk()
{
    if (gTable == 0)
    {
        gTable = CharacterDatabase.Query("SHOW TABLES LIKE 'coa_compagnon_souvenir'") ? 1 : 2;
        if (gTable == 1)
            CharacterDatabase.Execute(
                "DELETE FROM coa_compagnon_souvenir WHERE quand < UTC_TIMESTAMP() - INTERVAL 30 DAY");
        LOG_INFO("playerbots", "coa compagnon: souvenirs table={}",
                 gTable == 1 ? "presente" : "absente (inerte : appliquer 2026_10_27_00_coa_compagnon_souvenir.sql)");
    }
    return gTable == 1;
}

void Pousser(Player* humain, Genre genre, std::string objet)
{
    Evenement e;
    e.humain = humain->GetGUID().GetCounter();
    e.genre = genre;
    e.objet = std::move(objet);
    e.zone = humain->GetZoneId();
    std::lock_guard<std::mutex> garde(gVerrou);
    if (gFile.size() < FILE_MAX)
        gFile.push_back(std::move(e));
}

bool HorsMondeOuvert(Player* p) { return p->InBattleground() || p->InArena(); }

std::string Echapper(std::string s)
{
    CharacterDatabase.EscapeString(s);
    return s;
}

void Charger(uint32 humain, Memoire& m, time_t maintenant)
{
    QueryResult r = CharacterDatabase.Query(
        "SELECT genre, objet, zone, ensemble, TIMESTAMPDIFF(SECOND, quand, UTC_TIMESTAMP()) FROM coa_compagnon_souvenir"
        " WHERE humain_guid = {} AND dit_le IS NULL AND quand > UTC_TIMESTAMP() - INTERVAL 14 DAY"
        " ORDER BY id DESC LIMIT {}",
        humain, uint32(MEMOIRE_MAX));
    m.souvenirs.clear();
    if (!r)
        return;
    do
    {
        Field* f = r->Fetch();
        Souvenir s;
        if (!GenreDe(f[0].Get<std::string>(), s.genre))
            continue;
        s.objet = f[1].Get<std::string>();
        s.zone = f[2].Get<uint32>();
        s.ensemble = f[3].Get<uint8>() != 0;
        s.quand = maintenant - std::max<int64>(0, f[4].Get<int64>());
        m.souvenirs.push_back(std::move(s));   // ORDER BY id DESC : le plus recent d'abord
    } while (r->NextRow());
}

// Le compagnon eveille de cet humain, EN JEU, a cote de lui.
bool Ensemble(uint32 humain)
{
    uint32 const bot = CoaCompagnons::CompagnonEveille(humain);
    if (!bot)
        return false;
    Player* b = ObjectAccessor::FindConnectedPlayer(Guid(bot));
    Player* p = ObjectAccessor::FindConnectedPlayer(Guid(humain));
    return b && p && b->IsInWorld() && p->IsInWorld() && b->GetMapId() == p->GetMapId() &&
           b->GetInstanceId() == p->GetInstanceId() && b->GetDistance(p) <= ENSEMBLE_YD;
}

void Noter(Evenement const& e, time_t maintenant)
{
    auto it = gMemoires.find(e.humain);
    if (it == gMemoires.end())
    {
        it = gMemoires.emplace(e.humain, Memoire()).first;
        Charger(e.humain, it->second, maintenant);
    }
    Memoire& m = it->second;
    for (Souvenir const& s : m.souvenirs)
        if (s.genre == e.genre && s.objet == e.objet && maintenant - s.quand < DOUBLON_S)
            return;

    Souvenir s;
    s.genre = e.genre;
    s.objet = e.objet;
    s.zone = e.zone;
    s.ensemble = Ensemble(e.humain);
    s.quand = maintenant;
    m.souvenirs.push_front(s);
    while (m.souvenirs.size() > MEMOIRE_MAX)
        m.souvenirs.pop_back();

    CharacterDatabase.Execute(
        "INSERT INTO coa_compagnon_souvenir (humain_guid, bot_guid, genre, objet, zone, ensemble, quand)"
        " VALUES ({}, {}, '{}', '{}', {}, {}, UTC_TIMESTAMP())",
        e.humain, s.ensemble ? CoaCompagnons::CompagnonEveille(e.humain) : 0u, NomGenre(e.genre), Echapper(e.objet),
        e.zone, s.ensemble ? 1 : 0);
    LOG_INFO("playerbots", "coa compagnon: souvenir humain={} genre={} objet={} zone={} ensemble={}", e.humain,
             NomGenre(e.genre), e.objet, e.zone, s.ensemble ? 1 : 0);
}

// ---------------------------------------------------------------- les phrases
// {j} nom de l'humain, {o} objet, {z} zone, {v} le verbe de la recolte, {e} « e » si l'humain est une femme. Le
// compagnon ne s'accorde jamais lui-meme (son genre n'est pas celui de l'humain) : aucune phrase ne le demande.
using Phrases = std::vector<char const*>;

Phrases const MORT_ENSEMBLE = {
    "Je t'ai vu tomber face à {o}. J'ai bien cru te perdre, {j}.",
    "{o} t'a mis{e} à terre sous mes yeux... La prochaine fois, je frappe le premier.",
    "Pardon pour tout à l'heure, contre {o}. J'aurais dû être plus rapide.",
};
Phrases const MORT_ENSEMBLE_SANS = {
    "Je t'ai vu tomber tout à l'heure. Ça m'a glacé le sang, {j}.",
    "Tu es tombé{e} sous mes yeux... Ne me refais pas ça, d'accord ?",
};
Phrases const MORT_SEUL = {
    "On m'a raconté que {o} avait eu raison de toi, du côté de {z}. Tu tiens le coup ?",
    "{o}, hein ? Si j'avais été là... La prochaine fois, appelle-moi.",
};
Phrases const MORT_SEUL_SANS = {
    "Il paraît que tu es tombé{e} du côté de {z}. Fais attention à toi, {j}.",
};
Phrases const MORT_ANCIEN = {
    "Tu te souviens de {o} ? J'y repense encore, parfois.",
    "Je n'ai pas oublié {o}, tu sais. Un jour, on lui rendra la monnaie de sa pièce.",
};
Phrases const MORT_ANCIEN_SANS = {
    "Tu te souviens de ce jour où tu es tombé{e}, du côté de {z} ? On en a fait, du chemin, depuis.",
};
Phrases const QUETE_ENSEMBLE = {
    "« {o} », c'est fait. On forme une belle équipe, toi et moi.",
    "« {o} » : bouclé, et à deux. Quelle est la suite, {j} ?",
    "Je garderai un bon souvenir de « {o} ». Beau travail.",
};
Phrases const QUETE_SEUL = {
    "J'ai entendu dire que tu avais bouclé « {o} ». Bien joué, {j} !",
    "Alors, « {o} », c'est réglé ? Tu me raconteras.",
    "« {o} », déjà ? Tu ne chômes pas, {j}.",
};
Phrases const QUETE_ANCIEN = {
    "Tu te souviens de « {o} » ? Ça paraît loin, maintenant.",
    "Je repensais à « {o} », l'autre jour. Le bon temps.",
};
Phrases const DANGER_ENSEMBLE = {
    "Contre {o}, tout à l'heure... j'ai bien cru qu'on y restait.",
    "Tu as vu comme {o} nous a malmenés ? On s'en est sortis de justesse.",
    "Ouf, {o}... Encore un peu et c'était fini. Reprends ton souffle, {j}.",
};
Phrases const DANGER_ENSEMBLE_SANS = {
    "Tout à l'heure, on a eu chaud. Reprends ton souffle, {j}.",
};
Phrases const DANGER_SEUL = {
    "Il paraît que {o} t'a donné du fil à retordre. Tu t'en es sorti{e}, c'est l'essentiel.",
    "On m'a dit que tu avais frôlé la mort contre {o}. Pense à m'appeler, la prochaine fois.",
};
Phrases const DANGER_SEUL_SANS = {
    "On m'a dit que tu avais frôlé la mort du côté de {z}. Prudence, {j}.",
};
Phrases const DANGER_ANCIEN = {
    "Je repense parfois à {o}. On a eu chaud, ce jour-là.",
};
Phrases const DANGER_ANCIEN_SANS = {
    "Tu te souviens de cette fois où tu as frôlé la mort, du côté de {z} ?",
};
Phrases const RECOLTE_ENSEMBLE = {
    "On en a passé, du temps à {v} du côté de {z}. J'en ai encore mal au dos !",
    "Tout ce temps à {v} à {z}... Tu comptes ouvrir boutique, {j} ?",
};
Phrases const RECOLTE_SEUL = {
    "On m'a dit que tu passais des heures à {v} du côté de {z}. Ça rapporte ?",
    "Toujours à {v} du côté de {z} ? Garde-m'en un peu !",
};
Phrases const RECOLTE_ANCIEN = {
    "Tu te souviens de nos heures à {v} du côté de {z} ?",
};
Phrases const RECOLTE_ANCIEN_SEUL = {
    "Tu vas encore {v} du côté de {z}, ces temps-ci ?",
};
Phrases const EXPLOIT_ENSEMBLE = {
    "{o} ne s'attendait pas à nous deux. Quel combat !",
    "On a fait tomber {o} ensemble. Ça, on le racontera.",
};
Phrases const EXPLOIT_SEUL = {
    "Tu as abattu {o} ? Il faudra que tu me racontes ça !",
    "La nouvelle court déjà : {o} est tombé, et c'est toi. Bravo, {j}.",
};
Phrases const EXPLOIT_ANCIEN = {
    "On parle encore de {o}, tu sais.",
    "Tu te souviens de {o} ? Quelle bataille, ce jour-là.",
};
Phrases const NIVEAU_ENSEMBLE = {
    "Niveau {o} ! Tu progresses vite, {j}.",
    "Niveau {o}... On ne t'arrête plus.",
};
Phrases const NIVEAU_SEUL = {
    "Niveau {o}, à ce qu'on m'a dit ! Félicitations, {j}.",
};
Phrases const AUCUNE = {};

Phrases const& PhrasesDe(Souvenir const& s, bool ancien)
{
    bool const sans = s.objet.empty();
    switch (s.genre)
    {
        case Genre::Mort:
            if (ancien)
                return sans ? MORT_ANCIEN_SANS : MORT_ANCIEN;
            if (s.ensemble)
                return sans ? MORT_ENSEMBLE_SANS : MORT_ENSEMBLE;
            return sans ? MORT_SEUL_SANS : MORT_SEUL;
        case Genre::Quete:
            return ancien ? QUETE_ANCIEN : (s.ensemble ? QUETE_ENSEMBLE : QUETE_SEUL);
        case Genre::Danger:
            if (ancien)
                return sans ? DANGER_ANCIEN_SANS : DANGER_ANCIEN;
            if (s.ensemble)
                return sans ? DANGER_ENSEMBLE_SANS : DANGER_ENSEMBLE;
            return sans ? DANGER_SEUL_SANS : DANGER_SEUL;
        case Genre::Recolte:
            if (ancien)
                return s.ensemble ? RECOLTE_ANCIEN : RECOLTE_ANCIEN_SEUL;
            return s.ensemble ? RECOLTE_ENSEMBLE : RECOLTE_SEUL;
        case Genre::Exploit:
            return ancien ? EXPLOIT_ANCIEN : (s.ensemble ? EXPLOIT_ENSEMBLE : EXPLOIT_SEUL);
        case Genre::Niveau:
            return ancien ? AUCUNE : (s.ensemble ? NIVEAU_ENSEMBLE : NIVEAU_SEUL);
    }
    return AUCUNE;
}

char const* VerbeRecolte(std::string const& objet)
{
    if (objet == "minage")
        return "miner";
    if (objet == "herboristerie")
        return "cueillir des herbes";
    if (objet == "depecage")
        return "dépecer";
    return "récolter";
}

std::string NomZone(uint32 zone)
{
    AreaTableEntry const* a = zone ? sAreaTableStore.LookupEntry(zone) : nullptr;
    std::string const nom = a ? PlayerbotAI::GetLocalizedAreaName(a) : "";
    return nom.empty() ? "là-bas" : nom;
}

std::string Composer(char const* modele, Souvenir const& s, Player* humain)
{
    std::string sortie;
    for (char const* c = modele; *c; ++c)
    {
        if (*c == '{' && c[1] && c[2] == '}')
        {
            switch (c[1])
            {
                case 'j': sortie += humain->GetName(); break;
                case 'o': sortie += s.objet; break;
                case 'z': sortie += NomZone(s.zone); break;
                case 'v': sortie += VerbeRecolte(s.objet); break;
                case 'e': if (humain->getGender() == GENDER_FEMALE) sortie += 'e'; break;
                default: sortie.append(c, 3); break;
            }
            c += 2;
            continue;
        }
        sortie += *c;
    }
    return sortie;
}

// Le souvenir a dire, ou nullptr. Recents ensemble, puis recents vus de loin, puis anciens ; le plus recent d'abord ;
// pas le meme genre que la derniere parole quand un autre se presente dans la meme classe.
Souvenir* Choisir(Memoire& m, time_t maintenant, bool& ancien)
{
    Souvenir* meilleur = nullptr;
    int meilleurScore = 0;
    bool meilleurAncien = false;
    for (Souvenir& s : m.souvenirs)   // du plus recent au plus ancien
    {
        if (s.dit)
            continue;
        bool const vieux = maintenant - s.quand >= ANCIEN_S;
        if (PhrasesDe(s, vieux).empty())
            continue;
        int score = vieux ? 100 : (s.ensemble ? 300 : 200);
        if (m.aParle && s.genre == m.dernierGenre)
            score -= 50;
        // le plus recent l'emporte a score egal : seul un score strictement meilleur remplace
        if (score > meilleurScore)
        {
            meilleur = &s;
            meilleurScore = score;
            meilleurAncien = vieux;
        }
    }
    ancien = meilleurAncien;
    return meilleur;
}

void Parler(uint32 humain, Memoire& m, time_t maintenant)
{
    uint32 const bot = CoaCompagnons::CompagnonEveille(humain);
    if (bot != m.compagnon)
    {
        // Reveil (ou changement) du compagnon : sa premiere parole attend PremiereParole.
        m.compagnon = bot;
        m.prochaineParole = maintenant + sPlayerbotAIConfig.coaSouvenirsPremiereParole;
        return;
    }
    if (!bot || maintenant < m.prochaineParole)
        return;
    Player* b = ObjectAccessor::FindConnectedPlayer(Guid(bot));
    Player* p = ObjectAccessor::FindConnectedPlayer(Guid(humain));
    if (!b || !p || !b->IsInWorld() || !p->IsInWorld())
        return;   // connexion en cours : seconde suivante
    PlayerbotAI* ia = GET_PLAYERBOT_AI(b);
    if (!ia)
        return;
    if (p->IsInCombat() || b->IsInCombat() || !p->IsAlive() || !b->IsAlive())
    {
        m.prochaineParole = maintenant + REESSAI_COMBAT_S;
        return;
    }
    bool ancien = false;
    Souvenir* s = Choisir(m, maintenant, ancien);
    if (!s)
    {
        m.prochaineParole = maintenant + REESSAI_VIDE_S;
        return;
    }
    Phrases const& phrases = PhrasesDe(*s, ancien);
    std::string const texte = Composer(phrases[urand(0, uint32(phrases.size() - 1))], *s, p);
    if (!ia->Whisper(texte, p->GetName()))
    {
        m.prochaineParole = maintenant + REESSAI_COMBAT_S;
        return;
    }
    // Dit : celui-ci et ses doublons (meme genre, meme objet) ne reviendront pas.
    for (Souvenir& autre : m.souvenirs)
        if (autre.genre == s->genre && autre.objet == s->objet)
            autre.dit = true;
    CharacterDatabase.Execute(
        "UPDATE coa_compagnon_souvenir SET dit_le = UTC_TIMESTAMP() WHERE humain_guid = {} AND genre = '{}'"
        " AND objet = '{}' AND dit_le IS NULL",
        humain, NomGenre(s->genre), Echapper(s->objet));
    LOG_INFO("playerbots", "coa compagnon: parole humain={} bot={} genre={} objet={} ensemble={} ancien={} age_s={} texte={}",
             p->GetName(), b->GetName(), NomGenre(s->genre), s->objet, s->ensemble ? 1 : 0, ancien ? 1 : 0,
             uint64(maintenant - s->quand), texte);
    m.aParle = true;
    m.dernierGenre = s->genre;
    m.prochaineParole = maintenant + urand(sPlayerbotAIConfig.coaSouvenirsIntervalleMin,
                                           sPlayerbotAIConfig.coaSouvenirsIntervalleMax);
}

// Vie sous DANGER_PCT en combat, puis sorti du combat vivant : un souvenir « danger ».
void Surveiller(uint32 humain, Memoire& m, time_t maintenant, std::vector<Evenement>& sortie)
{
    Player* p = ObjectAccessor::FindConnectedPlayer(Guid(humain));
    if (!p || !p->IsInWorld() || HorsMondeOuvert(p) || p->duel)
    {
        m.enDanger = false;
        return;
    }
    if (!p->IsAlive())
    {
        m.enDanger = false;   // la mort a son propre souvenir
        return;
    }
    if (p->IsInCombat())
    {
        if (p->GetHealthPct() <= DANGER_PCT)
        {
            if (!m.enDanger)
                m.adversaire.clear();
            m.enDanger = true;
            if (m.adversaire.empty())
            {
                Unit* v = p->GetVictim();
                if (!v && !p->getAttackers().empty())
                    v = *p->getAttackers().begin();
                if (v)
                    m.adversaire = v->GetName();
            }
        }
        return;
    }
    if (m.enDanger)
    {
        m.enDanger = false;
        if (maintenant >= m.dernierDanger + DANGER_ESPACEMENT_S)
        {
            m.dernierDanger = maintenant;
            Evenement e;
            e.humain = humain;
            e.genre = Genre::Danger;
            e.objet = m.adversaire;
            e.zone = p->GetZoneId();
            sortie.push_back(std::move(e));
        }
    }
}
}  // namespace

namespace CoaSouvenirs
{
void Mort(Player* humain)
{
    if (HorsMondeOuvert(humain))
        return;
    std::lock_guard<std::mutex> garde(gVerrou);
    if (gMorts.size() >= FILE_MAX)
        return;
    MortEnCours& m = gMorts[humain->GetGUID().GetCounter()];
    if (!m.quand)
        m.quand = Maintenant();
    m.zone = humain->GetZoneId();
    m.morte = true;
}

void TueParCreature(Creature* tueur, Player* humain)
{
    if (!tueur || HorsMondeOuvert(humain))
        return;
    std::lock_guard<std::mutex> garde(gVerrou);
    if (gMorts.size() >= FILE_MAX)
        return;
    MortEnCours& m = gMorts[humain->GetGUID().GetCounter()];
    if (!m.quand)
        m.quand = Maintenant();
    m.zone = humain->GetZoneId();
    m.tueur = tueur->GetName();
}

void QueteFinie(Player* humain, Quest const* quete)
{
    if (quete && !quete->GetTitle().empty())
        Pousser(humain, Genre::Quete, quete->GetTitle());
}

void Recolte(Player* humain, uint32 competence)
{
    char const* objet = competence == SKILL_MINING ? "minage"
                        : competence == SKILL_HERBALISM ? "herboristerie"
                        : competence == SKILL_SKINNING ? "depecage"
                                                       : nullptr;
    if (!objet)
        return;
    time_t const maintenant = Maintenant();
    {
        std::lock_guard<std::mutex> garde(gVerrou);
        RecolteEnCours& r = gRecoltes[(uint64(humain->GetGUID().GetCounter()) << 32) | competence];
        if (maintenant < r.prochaine)
            return;
        if (maintenant - r.debut > RECOLTE_FENETRE_S)
        {
            r.debut = maintenant;
            r.n = 0;
        }
        if (++r.n < RECOLTE_N)
            return;
        r.n = 0;
        r.debut = 0;
        r.prochaine = maintenant + RECOLTE_ESPACEMENT_S;
    }
    Pousser(humain, Genre::Recolte, objet);
}

void CreatureTuee(Player* humain, Creature* victime)
{
    if (!victime || HorsMondeOuvert(humain))
        return;
    CreatureTemplate const* t = victime->GetCreatureTemplate();
    if (!t || t->rank == CREATURE_ELITE_NORMAL)
        return;
    if (victime->GetLevel() + 5 < humain->GetLevel())   // grise pour lui : pas un exploit
        return;
    Pousser(humain, Genre::Exploit, victime->GetName());
}

void NiveauPris(Player* humain) { Pousser(humain, Genre::Niveau, std::to_string(humain->GetLevel())); }

void Arrivee(Player* humain, time_t maintenant)
{
    if (!sPlayerbotAIConfig.coaSouvenirsEnable || !TableOk())
        return;
    uint32 const h = humain->GetGUID().GetCounter();
    auto it = gMemoires.find(h);
    if (it == gMemoires.end())
    {
        it = gMemoires.emplace(h, Memoire()).first;
        Charger(h, it->second, maintenant);
    }
    it->second.present = true;
    it->second.enDanger = false;
}

void Depart(uint32 humainGuid)
{
    auto it = gMemoires.find(humainGuid);
    if (it == gMemoires.end())
        return;
    it->second.present = false;
    it->second.enDanger = false;
    it->second.compagnon = 0;
}

void MettreAJour(time_t maintenant)
{
    if (!sPlayerbotAIConfig.coaSouvenirsEnable)
        return;
    if (!TableOk())
    {
        std::lock_guard<std::mutex> garde(gVerrou);   // table absente : rien a noter, la file se vide
        gFile.clear();
        gMorts.clear();
        return;
    }
    std::vector<Evenement> arrivees;
    {
        std::lock_guard<std::mutex> garde(gVerrou);
        arrivees.swap(gFile);
        for (auto it = gMorts.begin(); it != gMorts.end();)
        {
            if (maintenant - it->second.quand < MORT_DELAI_S + 1)
            {
                ++it;
                continue;
            }
            if (it->second.morte)
            {
                Evenement e;
                e.humain = it->first;
                e.genre = Genre::Mort;
                e.objet = it->second.tueur;
                e.zone = it->second.zone;
                arrivees.push_back(std::move(e));
            }
            it = gMorts.erase(it);
        }
    }
    for (auto& [h, m] : gMemoires)
        if (m.present)
            Surveiller(h, m, maintenant, arrivees);
    for (Evenement const& e : arrivees)
        Noter(e, maintenant);
    for (auto& [h, m] : gMemoires)
        if (m.present)
            Parler(h, m, maintenant);
}
}  // namespace CoaSouvenirs
