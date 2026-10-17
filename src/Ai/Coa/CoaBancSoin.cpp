/*
 * Conquest of Azeroth -- banc grandeur nature des soins, lots S0 (traces) et S1 (commande de banc). Voir CoaBancSoin.h.
 *
 * Conception : coa-project/docs/CONCEPTION-banc-soigneurs.md, § 9 (interface fixee avant l'ecriture : options,
 * commande, epreuves, formats de trace -- contrat avec l'orchestrateur coa-project/outils/banc-soigneurs.py).
 * Recherche et pieges : coa-project/docs/RECHERCHE-banc-soigneurs-20261013.md (P-088, arguments inverses de
 * ModifyHealReceived, Preparer qui force une specialisation de degats, compteurs globaux).
 *
 * AUCUN FICHIER DU COEUR N'EST TOUCHE : tout passe par les points d'accroche existants (CommandScript, WorldScript,
 * UnitScript, AllSpellScript) et par les API publiques d'Unit (CalcAbsorbResist, DealDamage...).
 *
 * CE QUE FAIT UN ESSAI
 *
 *  1. `.coa soin banc` (FIL MONDE : console RA ou commande MJ) : controles, inscription des membres au registre (les
 *     gardes du module les tiennent des lors : specialisation, composition, activite, gestionnaire des bots
 *     aleatoires, arene), preparation (niveau, specialisation DECLAREE de tout role -- jamais celle de degats de
 *     l'arene --, set fixe si AiPlayerbot.Arene1v1.BancSetFixe, vie et ressources), groupe sans humain, puis
 *     teleportation dans l'enclos (une case de l'enclos par essai simultane, ECART_CASE_YD entre deux cases).
 *  2. Suivi (WorldScript::OnUpdate, FIL MONDE, une fois par seconde, APRES la mise a jour des cartes) : quand tous les
 *     membres sont arrives, la creature est invoquee (Map::SummonCreature, sans invocateur), rendue hostile (faction
 *     14), PACIFIEE (aucun coup blanc : toute la charge vient du moteur) et invulnerable (crochet DealDamage), et
 *     attaque la cible de l'epreuve ; tout le groupe est mis en combat avec elle (P-088 : sans combat, pas de soin).
 *     Ligne `coa soin debut:`.
 *  3. Chaque seconde : la charge (palier en % des PV max par seconde) frappe le ou les membres vises DEPUIS la
 *     creature, comme un coup blanc physique : reductions en % (MeleeDamageBonusTaken), absorptions
 *     (CalcAbsorbResist), DealDamage, declenchements « touche en melee ». L'ARMURE N'EST PAS APPLIQUEE (volontaire :
 *     la charge doit valoir le meme % des PV pour toutes les classes). Les coups d'urgence (A3, a 20 %) et de
 *     semonce (B2, a 35 %) sont EXACTS : ni reduction ni absorption, pour que la situation de depart soit la meme.
 *  4. Fin (temps, mort, arret, abandon) : creature retiree, groupe dissous, membres desinscrits, ressuscites,
 *     remis a plein, specialisation d'origine reposee, composition ordinaire relue, teleportes a leur position
 *     d'origine. Arret du serveur (WorldScript::OnShutdown) : la meme fin, plus la position d'origine ecrite en base.
 *
 * FILS (P-137)
 *
 *  - La table des essais n'est touchee QUE par le fil monde (commande, OnUpdate, OnShutdown sur le fil principal
 *    apres la boucle) : jamais pendant une mise a jour de carte.
 *  - Le REGISTRE (membres, creatures, bornes de la trace d'action, coups critiques) est lu par les crochets des fils
 *    de carte : sous `verrouRegistre`, derriere le compteur atomique `nbInscrits` (zero = aucun essai, sortie
 *    immediate de tout crochet et de toute garde). L'appartenance d'un bot a un essai (MembreEngage, chemin chaud de
 *    PlayerbotAI::AllowActive) se lit SANS verrou dans `places`, copie atomique ecrite sous le verrou.
 *  - Le soin en attente (ModifyHealReceived -> OnHeal) est `thread_local` : les deux crochets se suivent dans le
 *    meme appel, sur le meme fil.
 *
 * COUT HORS ESSAI (Banc = 0, le Live) : par crochet, une lecture atomique ; par seconde, un test de vecteur vide.
 *
 * CORRECTIONS DU BANC DU 17/10/2026 (coa-project/docs/DIAG-equilibrage-classes-20261017.md, B-1 a B-7 ; conception
 * § 10). Chacune a son option, et l'ancienne valeur rend le banc d'avant :
 *  B-1  la creature PREND les degats (AiPlayerbot.CoaSoin.CreatureInvulnerable = 0) : le vol de vie periodique, calcule
 *       sur le retour de DealDamage, n'est plus nul. Jamais tuee : un coup qui la tuerait est ramene a PV - 1, et elle
 *       est remise a plein chaque seconde (Avancer). Consequence : la rage gagnee en INFLIGEANT des degats redevient
 *       celle d'un vrai combat (le code de la rage n'est pas touche).
 *  B-2  recharges remises a zero (Player::RemoveAllSpellCooldown, charges comprises) a la preparation et au retour ;
 *       AUCUNE potion pendant l'essai (AiPlayerbot.CoaSoin.Potions = 0) : « healing potion », « mana potion » et
 *       « healthstone » refusees aux membres (UseItemAction::isPossible), sans toucher aux sacs.
 *  B-3  AiPlayerbot.CoaSoin.AttenteAvantCombat secondes hors combat dans l'enclos avant l'engagement (phase ATTENTE),
 *       avec « coa buff » dans la composition des membres (AiFactory) : les auras de preparation sont posees.
 *  B-4  traces : `coa soin action:` action=attack|aoe|taunt|dispel|interrupt (lancers hors soin, CastFirst),
 *       `coa soin liste:` (CoaIaDefensifs / CoaIaSoinsSurSoi, une ligne quand la liste change), `coa soin degat:`
 *       (degats subis d'une autre source que la creature), `coa soin aura:` (pose / levee des auras suivies, 572617 et
 *       toute aura payee en vie par seconde ; presentes a l'engagement), `lanceur=` et `sources=` sur
 *       `coa soin absorbe:` (le JOUEUR proprietaire : maitre d'un familier, d'un totem, d'une balise -- revue du 17/10).
 *  Revue du 17/10 : la ligne `coa soin debut:` porte le reglage de l'essai en fin de ligne (`invulnerable=`,
 *       `potions=`, `attente=`), la ligne `coa soin banc: lance` aussi ; table des creatures pleine : abandon nomme.
 *  B-7  `coa soin arbre:` a la preparation : specialisation d'origine, natif ou force, entrees de talent tenues.
 */

#include "CoaBancSoin.h"

#include "CoaArene1v1.h"
#include "CoaDirecteur.h"
#include "CoaSpecialization.h"
#include "mod-ascension-compat/src/AscensionSpecialization.h"

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "PositionValue.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "Timer.h"
#include "Unit.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{

// ------------------------------------------------------------------ constantes

constexpr uint32 MEMBRES_MAX = 5;                 // un groupe ordinaire
constexpr uint32 MAX_PLAFOND = 8;                 // AiPlayerbot.CoaSoin.Max, borne haute
constexpr float ECART_CASE_YD = 60.0f;            // entre deux essais simultanes (soins de zone : <= 40 yd)
constexpr float RAYON_MEMBRES_YD = 4.0f;          // les membres autour du centre de leur case
constexpr float CREATURE_ECART_YD = 6.0f;         // la creature a cote
constexpr uint32 ARRIVEE_MAX_MS = 30000;          // teleportation : au-dela, abandon
constexpr float ARRIVE_YD = 20.0f;                // arrive : a moins de 20 yd du centre de sa case
constexpr float ELOIGNE_YD = 80.0f;               // en cours : un membre vivant au-dela est perdu
constexpr uint32 HORS_MONDE_MAX_S = 10;           // en cours : hors du monde (teleportation lointaine) au-dela, perdu
constexpr uint32 URGENCE_PERIODE_S = 20;          // A3
constexpr float URGENCE_PCT = 20.0f;              // A3 : un membre tire au hasard ramene a 20 %
constexpr float SEMONCE_PCT = 35.0f;              // B2 : le bot ramene a 35 %
constexpr uint32 SEMONCE_T_S = 3;                 // B2 : le temps que le combat soit engage
constexpr uint32 ACTIONS_PAR_SECONDE = 40;        // borne de `coa soin action:` par essai
constexpr uint32 CRIT_PEREMPTION_MS = 10000;      // une note de coup critique non consommee est oubliee
constexpr uint32 DUREE_MAX_PLAFOND_S = 1800;      // plafond absolu de AiPlayerbot.CoaSoin.DureeMax
constexpr float MENACE_CIBLE = 1.0e7f;            // la creature ne lache jamais la cible de l'epreuve
constexpr uint32 FACTION_HOSTILE = 14;            // hostile a tous
constexpr uint32 ATTENTE_MAX_S = 60;              // B-3 : plafond de AiPlayerbot.CoaSoin.AttenteAvantCombat
constexpr uint32 AURAS_SUIVIES_MAX = 16;          // B-4 : AiPlayerbot.CoaSoin.AurasSuivies, au plus
// B-4 : debit des nouvelles lignes, par essai et par seconde (chaque canal a le sien : aucune ne prend la place des
// lignes `coa soin action:` des actions de soin, bornees a ACTIONS_PAR_SECONDE).
enum Canal : uint8 { CANAL_LANCER, CANAL_LISTE, CANAL_DEGAT, CANAL_AURA, CANAL_MAX };
constexpr uint32 DEBIT_CANAL[CANAL_MAX] = { 20, 10, 20, 20 };

enum class Epreuve : uint8 { A1, A2, A3, A4, A5, B1, B2 };
enum class Phase : uint8 { ARRIVEE, ATTENTE, COURS };

char const* NomEpreuve(Epreuve e)
{
    switch (e)
    {
        case Epreuve::A1: return "A1";
        case Epreuve::A2: return "A2";
        case Epreuve::A3: return "A3";
        case Epreuve::A4: return "A4";
        case Epreuve::A5: return "A5";
        case Epreuve::B1: return "B1";
        case Epreuve::B2: return "B2";
    }
    return "?";
}

bool LireEpreuve(std::string t, Epreuve& e)
{
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    static std::pair<char const*, Epreuve> const Table[] = {
        {"A1", Epreuve::A1}, {"A2", Epreuve::A2}, {"A3", Epreuve::A3}, {"A4", Epreuve::A4},
        {"A5", Epreuve::A5}, {"B1", Epreuve::B1}, {"B2", Epreuve::B2}};
    for (auto const& [nom, valeur] : Table)
        if (t == nom)
        {
            e = valeur;
            return true;
        }
    return false;
}

bool VoletB(Epreuve e) { return e == Epreuve::B1 || e == Epreuve::B2; }
bool Escalade(Epreuve e) { return e == Epreuve::A1 || e == Epreuve::A2 || e == Epreuve::A4 || e == Epreuve::B1; }

// ------------------------------------------------------------------ options (sConfigMgr, lues a chaque commande)

// Le § 9 ecrit `AiPlayerbot.CoaSoin.Palier.Depart = 2.0`, puis `.Pas`, `.DureePalier`, `.Fond`, `.Constant`,
// `.DureeMax` : le nom canonique est `AiPlayerbot.CoaSoin.Palier.<X>`, sauf DureeMax, canonique en
// `AiPlayerbot.CoaSoin.DureeMax` (accord avec l'orchestrateur, 13/10) ; l'autre ecriture est lue en repli, pour
// qu'une conf ecrite d'une maniere ou de l'autre marche.
float OptionReelle(char const* canonique, char const* repli, float defaut)
{
    float const absent = -1.0e9f;
    float v = sConfigMgr->GetOption<float>(canonique, absent, false);
    if (v <= absent / 2)
        v = sConfigMgr->GetOption<float>(repli, defaut, false);
    return v;
}

struct Parametres
{
    float depart = 2.0f;
    float pas = 1.0f;
    uint32 dureePalierS = 30;
    float fond = 1.0f;
    float constant = 3.0f;
    uint32 dureeMaxS = 300;
    uint32 niveau = 60;
    uint32 creature = 14830;
    uint32 max = 4;
    bool setFixe = false;
    std::string enclos;
    // Corrections du 17/10 (voir l'en-tete). Les valeurs par defaut sont les corrections ; l'ancienne valeur de chaque
    // option rend le banc d'avant (comparaison avec les rapports ia-A / ia-B).
    bool creatureInvulnerable = false;   // B-1 : 1 = degats a 0 comme avant
    bool potions = false;                // B-2 : 1 = potions et pierres de soin permises comme avant
    uint32 attenteS = 10;                // B-3 : 0 = engagement des l'arrivee comme avant
    std::vector<uint32> aurasSuivies;    // B-4 : ids d'aura tracees (pose / levee), en plus des auras payees en vie
};

// « 572617 680388 » ou « 572617,680388 » : ids > 0, au plus AURAS_SUIVIES_MAX ; le reste est ignore.
std::vector<uint32> LireIds(std::string const& texte)
{
    std::vector<uint32> ids;
    std::string mot;
    auto Pousser = [&]() {
        if (!mot.empty() && mot.size() <= 9 && ids.size() < AURAS_SUIVIES_MAX)
            if (uint32 const v = uint32(std::strtoul(mot.c_str(), nullptr, 10)))
                if (std::find(ids.begin(), ids.end(), v) == ids.end())
                    ids.push_back(v);
        mot.clear();
    };
    for (char c : texte)
    {
        if (c >= '0' && c <= '9')
            mot += c;
        else
            Pousser();
    }
    Pousser();
    return ids;
}

Parametres LireParametres()
{
    Parametres p;
    p.depart = std::clamp(OptionReelle("AiPlayerbot.CoaSoin.Palier.Depart", "AiPlayerbot.CoaSoin.Depart", 2.0f), 0.0f, 100.0f);
    p.pas = std::clamp(OptionReelle("AiPlayerbot.CoaSoin.Palier.Pas", "AiPlayerbot.CoaSoin.Pas", 1.0f), 0.0f, 100.0f);
    p.dureePalierS = uint32(std::clamp(
        OptionReelle("AiPlayerbot.CoaSoin.Palier.DureePalier", "AiPlayerbot.CoaSoin.DureePalier", 30.0f), 1.0f, 3600.0f));
    p.fond = std::clamp(OptionReelle("AiPlayerbot.CoaSoin.Palier.Fond", "AiPlayerbot.CoaSoin.Fond", 1.0f), 0.0f, 100.0f);
    p.constant =
        std::clamp(OptionReelle("AiPlayerbot.CoaSoin.Palier.Constant", "AiPlayerbot.CoaSoin.Constant", 3.0f), 0.0f, 100.0f);
    p.dureeMaxS = uint32(std::clamp(
        OptionReelle("AiPlayerbot.CoaSoin.DureeMax", "AiPlayerbot.CoaSoin.Palier.DureeMax", 300.0f), 10.0f,
        float(DUREE_MAX_PLAFOND_S)));
    p.niveau = sConfigMgr->GetOption<uint32>("AiPlayerbot.CoaSoin.Niveau", 60, false);
    p.creature = sConfigMgr->GetOption<uint32>("AiPlayerbot.CoaSoin.Creature", 14830, false);
    p.max = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("AiPlayerbot.CoaSoin.Max", 4, false), 1, MAX_PLAFOND);
    // Option du banc d'equilibrage d'Armageddon (lot 2) : absente au Live, donc fausse.
    p.setFixe = sConfigMgr->GetOption<bool>("AiPlayerbot.Arene1v1.BancSetFixe", false, false);
    p.enclos = sConfigMgr->GetOption<std::string>("AiPlayerbot.CoaSoin.Enclos", "", false);
    p.creatureInvulnerable = sConfigMgr->GetOption<bool>("AiPlayerbot.CoaSoin.CreatureInvulnerable", false, false);
    p.potions = sConfigMgr->GetOption<bool>("AiPlayerbot.CoaSoin.Potions", false, false);
    p.attenteS = std::min<uint32>(sConfigMgr->GetOption<uint32>("AiPlayerbot.CoaSoin.AttenteAvantCombat", 10, false),
                                  ATTENTE_MAX_S);
    // Immolation Aura 572617 (Slayer, I4) et Fetid Ward 680388 (Rime, B-3) : DIAG du 17/10, § 4.
    p.aurasSuivies = LireIds(sConfigMgr->GetOption<std::string>("AiPlayerbot.CoaSoin.AurasSuivies", "572617 680388", false));
    return p;
}

// ------------------------------------------------------------------ registre (tout fil, sous verrou)

std::mutex verrouRegistre;
std::unordered_map<uint32, uint32> membresEssai;      // guid bas du bot -> id d'essai
std::unordered_map<uint64, bool> creaturesBanc;       // ObjectGuid brut des creatures de banc -> invulnerable (B-1)
struct BorneAction
{
    std::string cle;
    uint32 ms = 0;
};
std::unordered_map<uint32, BorneAction> bornesAction;  // guid bas -> derniere ligne `coa soin action:`
struct DebitEssai
{
    uint32 seconde = 0;
    uint32 lignes = 0;
};
std::unordered_map<uint32, DebitEssai> debitsAction;   // id d'essai -> lignes `coa soin action:` de la seconde
struct NoteCrit
{
    bool crit = false;
    uint32 ms = 0;
};
std::map<std::pair<uint32, uint32>, NoteCrit> notesCrit;   // (guid bas de la cible, sort) -> coup critique
// B-4 : debit des canaux nouveaux, (id d'essai, canal) -> lignes de la seconde.
std::map<std::pair<uint32, uint8>, DebitEssai> debitsCanal;
// B-4 : derniere liste tracee, (guid bas, action) -> texte ; une ligne `coa soin liste:` seulement quand elle change.
std::map<std::pair<uint32, std::string>, std::string> dernieresListes;
// membresEssai.size() + creaturesBanc.size() : a zero, crochets et gardes sortent sur ce seul chargement.
std::atomic<uint32> nbInscrits{0};

// Ecrits par la commande (fil monde), lus par les fils de carte sans verrou. Valent pour TOUS les essais en cours : la
// conf du dernier `.coa soin banc` l'emporte (l'orchestrateur ne la change pas pendant un plan).
std::atomic<bool> potionsPermises{false};    // B-2
std::atomic<bool> buffsAvantCombat{false};   // B-3 : « coa buff » dans la composition des membres
std::array<std::atomic<uint32>, AURAS_SUIVIES_MAX> aurasSuivies{};   // B-4 : 0 = case libre

// Sous verrou. Vrai : une ligne de plus est permise sur ce canal pour cet essai, dans la seconde en cours.
bool DebitPermis(uint32 essai, Canal canal, uint32 maintenant)
{
    DebitEssai& d = debitsCanal[{essai, uint8(canal)}];
    uint32 const seconde = maintenant / 1000;
    if (d.seconde != seconde)
    {
        d.seconde = seconde;
        d.lignes = 0;
    }
    if (d.lignes >= DEBIT_CANAL[canal])
        return false;
    ++d.lignes;
    return true;
}

// Revue du 13/10 : copie SANS VERROU de membresEssai, lue par les gardes des fils de carte. Pendant un essai,
// MembreEngage est appele par PlayerbotAI::AllowActive pour CHAQUE bot du royaume a chaque mise a jour, depuis les
// MapUpdate.Threads : sous le verrou unique, ces fils se seraient disputes la meme ligne de cache des centaines de
// milliers de fois par seconde. Une case = (guid bas << 32) | id d'essai, 0 = libre ; ecrite sous verrouRegistre (fil
// monde), lue librement. Capacite : MAX_PLAFOND essais de MEMBRES_MAX membres au plus.
std::array<std::atomic<uint64>, MAX_PLAFOND * MEMBRES_MAX> places{};

// Relecture du 17/10 (cout par seconde) : copie SANS VERROU de creaturesBanc. Le crochet DealDamage la lit pour CHAQUE
// degat sur une creature invoquee du royaume (familiers, totems, gardiens) pendant un essai : sous le verrou unique, ces
// degats se seraient disputes la meme ligne de cache. Une case = ObjectGuid brut, 0 = libre ; `invulnerable` est ecrit
// AVANT la case (release) et lu apres elle (acquire). Ecrites sous verrouRegistre (fil monde). Une case par essai.
std::array<std::atomic<uint64>, MAX_PLAFOND> creaturesPlaces{};
std::array<std::atomic<bool>, MAX_PLAFOND> creaturesInvulnerables{};

void MajCompteur() { nbInscrits.store(uint32(membresEssai.size() + creaturesBanc.size()), std::memory_order_release); }

// Sous verrou. Une case par membre, jamais deplacee (un lecteur ne voit jamais un membre manquer le temps d'une
// recopie). Faux si la capacite est depassee (ne doit pas arriver : la commande borne les essais a MAX_PLAFOND et les
// membres a MEMBRES_MAX).
bool PlacerVerrouille(uint32 guid, uint32 essai)
{
    for (std::atomic<uint64>& place : places)
        if (!place.load(std::memory_order_relaxed))
        {
            place.store((uint64(guid) << 32) | essai, std::memory_order_release);
            return true;
        }
    return false;
}

void LibererVerrouille(uint32 guid, uint32 essai)
{
    uint64 const v = (uint64(guid) << 32) | essai;
    for (std::atomic<uint64>& place : places)
        if (place.load(std::memory_order_relaxed) == v)
            place.store(0, std::memory_order_release);
}

// TOUT FIL, sans verrou. 0 : pas membre.
uint32 EssaiDuMembre(uint32 guidLow)
{
    if (!guidLow || !nbInscrits.load(std::memory_order_acquire))
        return 0;
    for (std::atomic<uint64> const& place : places)
    {
        uint64 const v = place.load(std::memory_order_acquire);
        if (v && uint32(v >> 32) == guidLow)
            return uint32(v);
    }
    return 0;
}

// Sous verrou (la table qui fait foi). 0 : pas membre.
uint32 EssaiDuMembreVerrouille(uint32 guidLow)
{
    auto it = membresEssai.find(guidLow);
    return it == membresEssai.end() ? 0 : it->second;
}

uint32 EssaiDuJoueur(Unit const* u)
{
    if (!u || !nbInscrits.load(std::memory_order_acquire))
        return 0;
    Player const* p = u->IsPlayer() ? u->ToPlayer() : nullptr;
    if (!p)
        return 0;
    return EssaiDuMembre(p->GetGUID().GetCounter());
}

// Sous verrou : inscrit / retire une creature de banc dans la copie sans verrou.
// Faux si la table est pleine (inatteignable : une case par essai, MAX_PLAFOND essais au plus) ; l'appelant abandonne
// alors l'essai plutot que de laisser une creature hors table (ni protegee ni reconnue par les crochets).
bool PlacerCreatureVerrouille(uint64 brut, bool invulnerable)
{
    for (size_t i = 0; i < creaturesPlaces.size(); ++i)
        if (!creaturesPlaces[i].load(std::memory_order_relaxed))
        {
            creaturesInvulnerables[i].store(invulnerable, std::memory_order_relaxed);
            creaturesPlaces[i].store(brut, std::memory_order_release);
            return true;
        }
    return false;
}

void LibererCreatureVerrouille(uint64 brut)
{
    for (std::atomic<uint64>& place : creaturesPlaces)
        if (place.load(std::memory_order_relaxed) == brut)
            place.store(0, std::memory_order_release);
}

// TOUT FIL, sans verrou. -1 : pas une creature de banc ; 0 : creature de banc qui prend les degats (B-1) ;
// 1 : invulnerable (banc d'avant).
int ModeCreatureDeBanc(Unit const* u)
{
    if (!u || !nbInscrits.load(std::memory_order_acquire) || !u->IsCreature() || !u->IsSummon())
        return -1;
    uint64 const brut = u->GetGUID().GetRawValue();
    for (size_t i = 0; i < creaturesPlaces.size(); ++i)
        if (creaturesPlaces[i].load(std::memory_order_acquire) == brut)
            return creaturesInvulnerables[i].load(std::memory_order_relaxed) ? 1 : 0;
    return -1;
}

bool EstCreatureDeBanc(Unit const* u) { return ModeCreatureDeBanc(u) >= 0; }

// B-4 : une aura suivie -- dans AiPlayerbot.CoaSoin.AurasSuivies, ou payee en vie par seconde (PowerType POWER_HEALTH et
// ManaPerSecond, comme CoaIaPayeEnVieParSeconde, mais pour toute classe : I4 du 17/10). Sans verrou.
bool AuraSuivie(SpellInfo const* info)
{
    if (!info)
        return false;
    if (info->PowerType == POWER_HEALTH && (info->ManaPerSecond > 0 || info->ManaPerSecondPerLevel > 0))
        return true;
    for (std::atomic<uint32> const& id : aurasSuivies)
    {
        uint32 const v = id.load(std::memory_order_relaxed);
        if (!v)
            break;
        if (v == info->Id)
            return true;
    }
    return false;
}

// ------------------------------------------------------------------ soin en attente (thread_local)

// ModifyHealReceived (soin brut, sort) puis OnHeal (lanceur, cible, soin effectif) se suivent dans le meme appel :
// Unit::HealBySpell (Unit.cpp:8758) et AuraEffect::HandlePeriodicHealAurasTick (SpellAuraEffects.cpp:6828). L'ORDRE
// DES DEUX UNITES N'EST PAS FIABLE (R § 4.1) : HealBySpell passe (this, cible), et `this` est le lanceur pour
// Spell.cpp:2816 mais la CIBLE pour plusieurs scripts du module Ascension (AscensionChronomancerEvents.cpp:499) ; le
// chemin periodique passe (cible, lanceur). On ne garde donc que la PAIRE, comparee sans ordre a celle d'OnHeal, qui
// est toujours (lanceur, cible). Le drapeau periodique vient de ModifyPeriodicDamageAurasTick, appele juste avant
// ModifyHealReceived sur le chemin periodique (SpellAuraEffects.cpp:6827), avec les memes (cible, lanceur, sort).
struct SoinEnAttente
{
    Unit const* a = nullptr;
    Unit const* b = nullptr;
    uint32 brut = 0;
    uint32 sort = 0;
    bool periodique = false;
    bool valide = false;
};
thread_local SoinEnAttente t_soin;

struct TickEnAttente
{
    Unit const* cible = nullptr;
    Unit const* lanceur = nullptr;
    uint32 sort = 0;
    bool valide = false;
};
thread_local TickEnAttente t_tick;

// B-4 : le sort d'un degat de sort direct. Unit::CalculateSpellDamageTaken appelle ModifySpellDamageTaken (cible,
// attaquant, sort) avant que Unit::DealSpellDamage n'appelle DealDamage (attaquant, cible) sur le meme fil ; la note est
// consommee par le premier DealDamage de la MEME paire. Un sort calcule sans degat infliges (manque) laisse une note
// perimee : elle n'est lue que pour la meme paire, et la suivante l'ecrase. Le sort est donc « au mieux » : 0 si inconnu.
struct DegatEnAttente
{
    Unit const* cible = nullptr;
    Unit const* source = nullptr;
    uint32 sort = 0;
    bool valide = false;
};
thread_local DegatEnAttente t_degat;

// ------------------------------------------------------------------ essais (FIL MONDE seulement)

struct Membre
{
    uint32 guid = 0;
    std::string nom;
    uint8 classe = 0;
    uint32 speVoulue = 0;      // `nom:spe`, 0 : la courante
    uint32 spe = 0;            // posee par la preparation
    uint32 speOrigine = 0;
    uint8 role = 0;            // 0 dps, 1 tank, 2 soin (codage de `coa score:`)
    WorldLocation origine;
    uint32 zoneOrigine = 0;
    bool vivant = true;
    // Mort une fois pendant l'essai : il en sort (ni charge, ni position tenue, ni seconde mort comptee), meme si le
    // moteur des morts le releve a son corps.
    bool mort = false;
    uint32 horsMondeS = 0;
    uint32 niveauAvant = 0;
};

struct Essai
{
    uint32 id = 0;
    Epreuve epreuve = Epreuve::A1;
    uint8 caseEnclos = 0;
    Phase phase = Phase::ARRIVEE;
    bool fini = false;
    std::vector<Membre> membres;   // [0] soigneur (volet A) ou bot seul (volet B), [1] tank (volet A)
    uint32 cible = 0;              // index du membre que la creature attaque (et que l'escalade frappe en A1/A4/B1)
    ObjectGuid creature;
    uint32 groupe = 0;             // guid bas du groupe forme, 0 sans groupe (volet B)
    WorldLocation centre;          // centre de la case
    Parametres p;
    uint32 creeMs = 0;
    uint32 attenteMs = 0;          // B-3 : debut de la phase ATTENTE (tous arrives)
    uint32 debutMs = 0;
    uint32 dernierCoupMs = 0;
    uint32 prochaineUrgenceS = URGENCE_PERIODE_S;
    bool semonceFaite = false;
    uint32 morts = 0;
};

std::vector<std::unique_ptr<Essai>> essais;

// Retours differes : un membre rendu pendant une teleportation LOINTAINE (hors du monde) ne peut pas etre teleporte ;
// il est repris a chaque seconde jusqu'a RETOUR_MAX_MS, puis abandonne (journal). FIL MONDE.
constexpr uint32 RETOUR_MAX_MS = 60000;
struct RetourDiffere
{
    uint32 guid = 0;
    WorldLocation origine;
    uint32 depuisMs = 0;
};
std::vector<RetourDiffere> retours;

uint32 Ecoule(uint32 depuisMs) { return getMSTimeDiff(depuisMs, getMSTime()); }

Player* Trouver(uint32 guidLow)
{
    return ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guidLow));
}

// Ce que chaque mort en combat use (Unit::Kill, 10 % de durabilite) : un bot qui meurt d'essai en essai finirait en
// equipement brise, sans ses statistiques -- le banc mesurerait l'usure. Gratuit, comme un MJ.
// La position « stay » d'une commande de chat ramenerait le bot a son ancien point (ReturnToStayPositionTrigger,
// meme garde que le Directeur) : oubliee.
void Remettre(Player* p)
{
    p->DurabilityRepairAll(false, 0.0f, false);
    if (PlayerbotAI* ia = GET_PLAYERBOT_AI(p))
        ia->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get()["stay"].Reset();
}

// Vie et ressources : recalcul AVANT remplissage (CoaDuelCommandScript, RemettreAPlein) ; rage et puissance runique a
// zero (elles se gagnent au combat), le reste a plein ; mana a plein si le personnage en a. Les ressources propres
// aux classes CoA (Heat...) ne sont pas touchees (decision du 13/10 : ressource de depart jamais modifiee).
void RemettreAPlein(Player* p)
{
    p->UpdateAllStats();
    p->SetFullHealth();
    Powers const type = p->getPowerType();
    if (type == POWER_RAGE || type == POWER_RUNIC_POWER)
        p->SetPower(type, 0);
    else if (type < MAX_POWERS)
        p->SetPower(type, p->GetMaxPower(type));
    if (type != POWER_MANA && p->GetMaxPower(POWER_MANA) > 0)
        p->SetPower(POWER_MANA, p->GetMaxPower(POWER_MANA));
}

// B-2 (17/10) : aucune recharge heritee du monde ouvert (Ranger 5113 : Horn 17 s en retard, recharge laissee par l'essai
// precedent ; Runemaster 4862 : Granite Resolve absent). Player::RemoveAllSpellCooldown rend aussi les charges
// (balises du Tinker). A la preparation et au retour, PAS a l'engagement : ce que le bot pose pendant l'attente (B-3)
// garde sa recharge, comme un joueur qui se prepare.
void RemettreRecharges(Player* p)
{
    p->RemoveAllSpellCooldown();
}

// `res=TYPE:PCT` de `coa soin etat:` (0-100) : la mana si le personnage en a (tous les soins actifs CoA coutent de la
// mana, R § 2), sinon sa ressource principale.
std::string Ressource(Player* p)
{
    Powers type = p->GetMaxPower(POWER_MANA) > 0 ? POWER_MANA : p->getPowerType();
    char const* nom = type == POWER_MANA ? "mana" : type == POWER_RAGE ? "rage" : type == POWER_ENERGY ? "energie" : "autre";
    uint32 const max = type < MAX_POWERS ? p->GetMaxPower(type) : 0;
    uint32 const pct = max ? uint32(std::lround(100.0 * double(p->GetPower(type)) / double(max))) : 0;
    return std::string(nom) + ":" + std::to_string(std::min<uint32>(pct, 100));
}

uint32 PvPct(Player* p)
{
    if (!p->IsAlive())
        return 0;
    return std::min<uint32>(100, uint32(std::lround(p->GetHealthPct())));
}

std::string FormatPalier(float v)
{
    char tampon[32];
    std::snprintf(tampon, sizeof(tampon), "%.1f", double(v));
    return tampon;
}

// ------------------------------------------------------------------ set fixe (option d'Armageddon)

// Copie de la lecture d'Armageddon (core-armageddon, CoaArene1v1.cpp LireSet, lot 2) : la table n'existe qu'en base
// monde d'Armageddon ; ailleurs la requete echoue (une ligne d'erreur SQL) -- jamais tant que BancSetFixe = 0.
std::vector<std::pair<uint8, uint32>> LireSet(uint8 classe, uint32 spe)
{
    std::vector<std::pair<uint8, uint32>> pieces;
    if (!spe)
        return pieces;
    QueryResult r = WorldDatabase.Query("SELECT emplacement, objet FROM coa_armageddon_set WHERE camp = 0 AND palier = 0"
                                        " AND classe = {} AND spe = {} ORDER BY emplacement", uint32(classe), spe);
    if (!r)
        return pieces;
    do
    {
        Field* f = r->Fetch();
        pieces.emplace_back(f[0].Get<uint8>(), f[1].Get<uint32>());
    } while (r->NextRow());
    return pieces;
}

// Rend vrai si le set entier est porte. Rien n'est detruit si le set est vide.
bool PoserSet(Player* bot, uint32& attendus, uint32& poses)
{
    std::vector<std::pair<uint8, uint32>> const pieces = LireSet(bot->getClass(), GetAscensionActiveSpecialization(bot));
    attendus = uint32(pieces.size());
    poses = 0;
    if (pieces.empty())
        return false;
    PlayerbotFactory::DestroyEquippedGear(bot);
    for (auto const& [emplacement, objet] : pieces)
    {
        if (emplacement >= EQUIPMENT_SLOT_END || emplacement == EQUIPMENT_SLOT_TABARD || emplacement == EQUIPMENT_SLOT_BODY)
            continue;
        uint16 dest = 0;
        if (bot->CanEquipNewItem(emplacement, dest, objet, false) == EQUIP_ERR_OK && bot->EquipNewItem(dest, objet, true))
            ++poses;
    }
    PlayerbotFactory fabrique(bot, bot->GetLevel());
    fabrique.InitAmmo();
    return poses == attendus;
}

// ------------------------------------------------------------------ refus d'un membre (FIL MONDE)

char const* RefusMembre(Player* b, uint32 niveau)
{
    if (!b)
        return "introuvable";
    if (!b->IsInWorld() || b->IsBeingTeleported())
        return "en_teleportation";
    PlayerbotAI* ia = GET_PLAYERBOT_AI(b);
    if (!ia || !sRandomPlayerbotMgr.IsRandomBot(b))
        return "pas_bot_aleatoire";
    if (ia->GetMaster() || ia->HasGameClientMaster())
        return "a_un_maitre";
    if (!IsAscensionCustomClassId(b->getClass()))
        return "pas_classe_coa";
    uint32 const guid = b->GetGUID().GetCounter();
    if (CoaBancSoin::MembreEngage(guid))
        return "deja_en_essai";
    if (CoaArene1v1::GladiateurEngage(guid))
        return "arene";
    if (CoaDirecteur::AffectationDe(guid, nullptr) || CoaDirecteur::ChasseurDe(guid) || CoaDirecteur::CibleDeBanc(guid) ||
        CoaDirecteur::CibleImmobile(guid))
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
    if (b->GetLevel() > niveau)
        return "niveau_superieur";
    return nullptr;
}

// ------------------------------------------------------------------ fin d'un essai (FIL MONDE)

void Desinscrire(Essai const& e)
{
    std::lock_guard<std::mutex> garde(verrouRegistre);
    for (Membre const& m : e.membres)
    {
        auto it = membresEssai.find(m.guid);
        if (it != membresEssai.end() && it->second == e.id)
        {
            membresEssai.erase(it);
            LibererVerrouille(m.guid, e.id);
        }
        bornesAction.erase(m.guid);
        for (auto it = notesCrit.begin(); it != notesCrit.end();)
            it = it->first.first == m.guid ? notesCrit.erase(it) : std::next(it);
        for (auto it = dernieresListes.begin(); it != dernieresListes.end();)
            it = it->first.first == m.guid ? dernieresListes.erase(it) : std::next(it);
    }
    if (!e.creature.IsEmpty())
    {
        creaturesBanc.erase(e.creature.GetRawValue());
        LibererCreatureVerrouille(e.creature.GetRawValue());
    }
    debitsAction.erase(e.id);
    for (auto it = debitsCanal.begin(); it != debitsCanal.end();)
        it = it->first.first == e.id ? debitsCanal.erase(it) : std::next(it);
    MajCompteur();
}

// Le groupe de l'essai, s'il existe encore et si c'est bien le sien (chef parmi les membres).
Group* GroupeDe(Essai const& e)
{
    if (!e.groupe)
        return nullptr;
    Group* g = sGroupMgr->GetGroupByGUID(e.groupe);
    if (!g)
        return nullptr;
    ObjectGuid const chef = g->GetLeaderGUID();
    for (Membre const& m : e.membres)
        if (chef == ObjectGuid::Create<HighGuid::Player>(m.guid))
            return g;
    return nullptr;
}

// Le bot rendu a sa vie ordinaire. APRES Desinscrire (sinon les gardes tiendraient encore la composition du banc).
void Rendre(Player* p, Membre const& m, bool arretServeur)
{
    p->AttackStop();
    p->CombatStop(true);
    if (!p->IsAlive())
    {
        p->ResurrectPlayer(1.0f);
        p->SpawnCorpseBones();
    }
    if (m.speOrigine && GetAscensionActiveSpecialization(p) != m.speOrigine)
    {
        std::string refus;
        if (!CoaPoserSpecialisationDeclaree(p, m.speOrigine, refus))
            LOG_INFO("playerbots.coa", "coa soin preparation: guid={} action=spe_origine spe={} refus={}", m.guid,
                     m.speOrigine, refus);
    }
    Remettre(p);
    if (PlayerbotAI* ia = GET_PLAYERBOT_AI(p))
    {
        ia->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
        p->SetSelection(ObjectGuid::Empty);
        ia->ResetStrategies(false);
    }
    RemettreAPlein(p);
    RemettreRecharges(p);
    // Une teleportation PROCHE en cours est remplacee par celle-ci ; une LOINTAINE (hors du monde) ne peut pas l'etre :
    // retour differe (SuivreRetours).
    if (!p->IsInWorld() || !p->TeleportTo(m.origine))
        retours.push_back(RetourDiffere{m.guid, m.origine, getMSTime()});
    // Arret du serveur : la boucle monde est finie, la teleportation ne s'achevera pas. Une teleportation PROCHE est
    // sauvee a sa destination (_SaveCharacter, Player.cpp) ; une LOINTAINE ne l'est pas du tout (SaveToDB differee) :
    // la position d'origine est donc aussi ecrite directement.
    if (arretServeur)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        Player::SavePositionInDB(m.origine, uint16(m.zoneOrigine), p->GetGUID(), trans);
        CharacterDatabase.CommitTransaction(trans);
    }
}

// Idempotente (`fini`). `motif` : detail d'un abandon, sur une ligne `coa soin abandon:` a part (la ligne de fin garde
// le format du § 9).
void Terminer(Essai& e, char const* raison, char const* motif = nullptr, bool arretServeur = false)
{
    if (e.fini)
        return;
    e.fini = true;

    uint32 const dureeS = e.phase == Phase::COURS ? Ecoule(e.debutMs) / 1000 : 0;
    // palier_max (accord avec l'orchestrateur, 13/10) : le dernier palier COMPLETEMENT tenu, cible vivante a la fin de
    // ce palier ; 0 si aucun. Escalade : la cible vivait a chaque fin de palier ecoulee avant la fin (la mort termine
    // l'essai au tick qui la voit). Charge constante (A3, A5, B2) : le niveau de fond ou constant si l'essai est alle
    // a son terme (`temps`) sans mort, sinon 0.
    float palierMax = 0.0f;
    if (e.phase == Phase::COURS)
    {
        if (Escalade(e.epreuve))
        {
            uint32 const tenus = dureeS / e.p.dureePalierS;
            if (tenus)
                palierMax = e.p.depart + e.p.pas * float(tenus - 1);
        }
        else if (std::string(raison) == "temps" && !e.morts)
            palierMax = e.epreuve == Epreuve::A5 ? e.p.constant : e.p.fond;
    }

    if (motif)
        LOG_INFO("playerbots.coa", "coa soin abandon: essai={} motif={} phase={}", e.id, motif,
                 e.phase == Phase::COURS ? "cours" : e.phase == Phase::ATTENTE ? "attente" : "arrivee");
    LOG_INFO("playerbots.coa", "coa soin fin: essai={} raison={} duree_s={} palier_max={} morts={}", e.id, raison, dureeS,
             FormatPalier(palierMax), e.morts);

    Desinscrire(e);

    if (!e.creature.IsEmpty())
        if (Map* carte = sMapMgr->FindBaseNonInstanceMap(e.centre.GetMapId()))
            if (Creature* c = carte->GetCreature(e.creature))
            {
                c->CombatStop(true);
                c->DespawnOrUnsummon();
            }

    if (Group* g = GroupeDe(e))
        g->Disband();

    for (Membre const& m : e.membres)
    {
        if (Player* p = Trouver(m.guid))
            Rendre(p, m, arretServeur);
        else
        {
            // Hors ligne (deconnecte en plein essai) : sa sauvegarde de deconnexion l'a laisse dans l'enclos. La
            // position d'origine passe APRES elle dans la file asynchrone (CharacterDatabase.WorkerThreads = 1 sur les
            // trois royaumes, FIFO). Sa specialisation reste celle du banc : EnsureCoaSpecialization la rendra a son
            // role a la prochaine composition si le role differe.
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            Player::SavePositionInDB(m.origine, uint16(m.zoneOrigine), ObjectGuid::Create<HighGuid::Player>(m.guid), trans);
            CharacterDatabase.CommitTransaction(trans);
        }
    }
}

// Annulation AVANT la teleportation (preparation refusee) : aucune ligne `coa soin fin:` (aucun essai n'a ete annonce).
void Annuler(Essai& e)
{
    e.fini = true;
    Desinscrire(e);
    if (Group* g = GroupeDe(e))
        g->Disband();
    for (Membre const& m : e.membres)
        if (Player* p = Trouver(m.guid))
        {
            if (m.speOrigine && GetAscensionActiveSpecialization(p) != m.speOrigine)
            {
                std::string refus;
                CoaPoserSpecialisationDeclaree(p, m.speOrigine, refus);
            }
            if (PlayerbotAI* ia = GET_PLAYERBOT_AI(p))
                ia->ResetStrategies(false);
        }
}

// ------------------------------------------------------------------ positions

// Le centre de la case `k` : l'enclos decale de k * ECART_CASE_YD en x, au sol si la carte le dit (sinon la hauteur
// de l'enclos).
Position PointAuSol(Map* carte, float x, float y, float zRef)
{
    float z = zRef;
    if (carte)
    {
        float const sol = carte->GetHeight(x, y, zRef + 5.0f, true, 20.0f);
        if (sol > INVALID_HEIGHT && std::fabs(sol - zRef) < 15.0f)
            z = sol;
    }
    return Position(x, y, z + 0.5f, 0.0f);
}

bool LireEnclos(std::string const& texte, WorldLocation& lieu)
{
    std::istringstream flux(texte);
    uint32 carte = 0;
    float x = 0, y = 0, z = 0;
    if (!(flux >> carte >> x >> y >> z) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;
    std::string reste;
    if (flux >> reste)
        return false;
    lieu = WorldLocation(carte, x, y, z, 0.0f);
    return true;
}

// ------------------------------------------------------------------ charge (FIL MONDE)

void TracerCoup(Essai const& e, Player* p, uint32 montant, char const* type, float palier)
{
    LOG_INFO("playerbots.coa", "coa soin coup: essai={} cible={} montant={} type={} palier={} pv_pct_apres={}", e.id,
             p->GetGUID().GetCounter(), montant, type, FormatPalier(palier), PvPct(p));
}

// B-4 : le lanceur des absorptions. Les reserves d'absorption de la cible qui couvrent le physique (SCHOOL_ABSORB,
// MANA_SHIELD), relevees PAR VALEUR avant Unit::CalcAbsorbResist ; apres, la baisse de chacune est sa part. Une reserve
// videe est retiree de la liste (Remove AURA_REMOVE_BY_ENEMY_SPELL) : sa part est tout ce qu'elle avait. Les pointeurs
// d'avant ne sont que COMPARES, jamais lus (l'aura retiree n'est detruite qu'a la mise a jour suivante de l'unite).
// Une absorption ecrite par script (montant inchange, ou negatif) n'a pas de part mesurable : `lanceur` n'est alors
// donne que si une seule reserve couvrait le coup.
struct ReserveAbsorption
{
    AuraEffect const* effet = nullptr;
    ObjectGuid lanceur;
    uint32 sort = 0;
    uint8 index = 0;
    int32 montant = 0;
};

// Revue du 17/10 : le lanceur ATTRIBUE est le JOUEUR proprietaire (familier, totem, balise du Tinker : leur maitre),
// comme la ligne `coa soin:` (OnHeal). Le guid bas d'une creature n'est jamais ecrit : il pourrait egaler celui d'un
// joueur. Proprietaire introuvable (lanceur disparu, creature sans maitre joueur) : guid vide, donc 0 = indetermine.
ObjectGuid ProprietaireDeLaReserve(AuraEffect const* eff)
{
    ObjectGuid const g = eff->GetCasterGUID();
    if (g.IsPlayer())
        return g;
    if (Unit* lanceur = eff->GetCaster())
        if (Player* maitre = lanceur->GetCharmerOrOwnerPlayerOrPlayerItself())
            return maitre->GetGUID();
    return ObjectGuid::Empty;
}

std::vector<ReserveAbsorption> ReservesAbsorption(Player const* p)
{
    std::vector<ReserveAbsorption> out;
    for (AuraType const type : { SPELL_AURA_SCHOOL_ABSORB, SPELL_AURA_MANA_SHIELD })
        for (AuraEffect const* eff : p->GetAuraEffectsByType(type))
            if (eff && (eff->GetMiscValue() & SPELL_SCHOOL_MASK_NORMAL))
                out.push_back(ReserveAbsorption{eff, ProprietaireDeLaReserve(eff), eff->GetId(), eff->GetEffIndex(),
                                                eff->GetAmount()});
    return out;
}

// `lanceur` : guid bas du JOUEUR qui a pose l'absorption (le maitre d'un serviteur), 0 si indetermine ; `sort` : son sort, 0 si indetermine ; `sources` :
// « guid:sort:part;... » des reserves qui ont baisse, « - » si aucune.
void AttribuerAbsorption(Player const* p, std::vector<ReserveAbsorption> const& avant, uint32 absorbe, uint32& lanceur,
                         uint32& sort, std::string& sources)
{
    lanceur = 0;
    sort = 0;
    sources.clear();
    std::vector<ReserveAbsorption> const apres = ReservesAbsorption(p);
    uint64 total = 0;
    ObjectGuid seul;                 // vide aussi quand le proprietaire est introuvable : `aucune` dit si on a vu une part
    uint32 seulSort = 0;
    bool aucune = true;
    bool plusieurs = false;
    for (ReserveAbsorption const& r : avant)
    {
        auto it = std::find_if(apres.begin(), apres.end(), [&r](ReserveAbsorption const& x)
            { return x.effet == r.effet && x.sort == r.sort && x.index == r.index; });
        int64 const part = it == apres.end() ? std::max<int64>(r.montant, 0) : std::max<int64>(int64(r.montant) - it->montant, 0);
        if (part <= 0)
            continue;
        total += uint64(part);
        if (!sources.empty())
            sources += ";";
        sources += std::to_string(r.lanceur.GetCounter()) + ":" + std::to_string(r.sort) + ":" + std::to_string(part);
        if (aucune)
        {
            aucune = false;
            seul = r.lanceur;
            seulSort = r.sort;
        }
        else if (seul != r.lanceur)
            plusieurs = true;
        else if (seulSort != r.sort)
            seulSort = 0;
    }
    if (!aucune && !plusieurs && !seul.IsEmpty() && total + 1 >= absorbe)
    {
        lanceur = seul.GetCounter();
        sort = seulSort;
    }
    else if (aucune && avant.size() == 1)
    {
        lanceur = avant[0].lanceur.GetCounter();
        sort = avant[0].sort;
    }
    if (sources.empty())
        sources = "-";
}

// Un coup blanc physique de `pct` % des PV max depuis la creature : reductions en %, absorptions, degats,
// declenchements. Armure NON appliquee (voir l'en-tete). `montant` de la trace = degats INFLIGES (apres reductions
// et absorptions) ; l'absorbe est sur sa ligne.
void Frapper(Essai const& e, Creature* c, Player* p, float pct, char const* type, float palier)
{
    if (!p->IsAlive() || !p->IsInWorld())
        return;
    uint32 const brut = uint32(std::lround(double(p->GetMaxHealth()) * double(pct) / 100.0));
    if (!brut)
        return;
    if (p->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL))
    {
        TracerCoup(e, p, 0, type, palier);
        return;
    }
    uint32 const apresReduction = p->MeleeDamageBonusTaken(c, brut, BASE_ATTACK, nullptr, SPELL_SCHOOL_MASK_NORMAL);
    DamageInfo di(c, p, apresReduction, nullptr, SPELL_SCHOOL_MASK_NORMAL, DIRECT_DAMAGE);
    std::vector<ReserveAbsorption> const reserves = ReservesAbsorption(p);
    Unit::CalcAbsorbResist(di);
    uint32 const absorbe = di.GetAbsorb();
    // Lue tout de suite : un declenchement du coup (plus bas) pourrait reposer ou retirer une reserve.
    uint32 lanceurAbsorbe = 0, sortAbsorbe = 0;
    std::string sourcesAbsorbe;
    if (absorbe)
        AttribuerAbsorption(p, reserves, absorbe, lanceurAbsorbe, sortAbsorbe, sourcesAbsorbe);
    uint32 const resiste = di.GetResist();
    uint32 const inflige = di.GetDamage();
    uint32 infoCoup = HITINFO_AFFECTS_VICTIM;
    if (absorbe)
        infoCoup |= inflige ? HITINFO_PARTIAL_ABSORB : HITINFO_FULL_ABSORB;
    c->SendAttackStateUpdate(infoCoup, p, 1, SPELL_SCHOOL_MASK_NORMAL, inflige, absorbe, resiste, VICTIMSTATE_HIT, 0);
    CleanDamage propre(brut > apresReduction ? brut - apresReduction : 0, absorbe, BASE_ATTACK, MELEE_HIT_NORMAL);
    Unit::DealDamage(c, p, inflige, &propre, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    if (p->IsInWorld() && p->IsAlive())
        Unit::ProcSkillsAndAuras(c, p, PROC_FLAG_DONE_MELEE_AUTO_ATTACK,
                                 PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK | (inflige ? PROC_FLAG_TAKEN_DAMAGE : 0),
                                 PROC_EX_NORMAL_HIT | (absorbe ? PROC_EX_ABSORB : 0), inflige, BASE_ATTACK, nullptr,
                                 nullptr, -1, nullptr, &di);
    if (absorbe)
    {
        // `lanceur`, `sort` et `sources` APRES `montant` (B-4) : un lecteur qui ne connait que les deux premiers champs
        // lit la ligne comme avant.
        LOG_INFO("playerbots.coa", "coa soin absorbe: essai={} cible={} montant={} lanceur={} sort={} sources={}", e.id,
                 p->GetGUID().GetCounter(), absorbe, lanceurAbsorbe, sortAbsorbe, sourcesAbsorbe);
    }
    TracerCoup(e, p, inflige, type, palier);
}

// Coup EXACT qui ramene `p` a `pct` % de ses PV max (urgence A3, semonce B2) ; rien s'il y est deja.
void Ramener(Essai const& e, Creature* c, Player* p, float pct, char const* type, float palier)
{
    if (!p->IsAlive() || !p->IsInWorld())
        return;
    uint32 const visee = std::max<uint32>(1, uint32(double(p->GetMaxHealth()) * double(pct) / 100.0));
    uint32 montant = 0;
    if (p->GetHealth() > visee)
    {
        montant = p->GetHealth() - visee;
        c->SendAttackStateUpdate(HITINFO_AFFECTS_VICTIM, p, 1, SPELL_SCHOOL_MASK_NORMAL, montant, 0, 0, VICTIMSTATE_HIT, 0);
        montant = Unit::DealDamage(c, p, montant, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    }
    TracerCoup(e, p, montant, type, palier);
}

// Met `p` en combat avec la creature (P-088) : ce que fait AttackAction::Attack (comme EngagerChasseur du Directeur).
void Engager(Player* p, Creature* c)
{
    if (!p->IsAlive() || !c->IsAlive())
        return;
    c->SetInCombatWith(p);
    p->SetInCombatWith(c);
    c->AddThreat(p, 1.0f);
    if (PlayerbotAI* ia = GET_PLAYERBOT_AI(p))
    {
        if (p->IsMounted())
            p->RemoveAurasByType(SPELL_AURA_MOUNTED);
        p->SetSelection(c->GetGUID());
        ia->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(c);
        ia->ChangeEngine(BOT_STATE_COMBAT);
        p->Attack(c, !PlayerbotAI::IsRanged(p));
    }
}

void Demarrer(Essai& e)
{
    Player* cible = Trouver(e.membres[e.cible].guid);
    Map* carte = cible ? cible->GetMap() : nullptr;
    if (!carte)
    {
        Terminer(e, "abandon", "carte_absente");
        return;
    }
    Position const pos = PointAuSol(carte, e.centre.GetPositionX() + CREATURE_ECART_YD, e.centre.GetPositionY(),
                                    e.centre.GetPositionZ());
    TempSummon* c = carte->SummonCreature(e.p.creature, pos);
    if (!c)
    {
        Terminer(e, "abandon", "creature_refusee");
        return;
    }
    e.creature = c->GetGUID();
    bool place = false;
    {
        std::lock_guard<std::mutex> garde(verrouRegistre);
        creaturesBanc[e.creature.GetRawValue()] = e.p.creatureInvulnerable;
        place = PlacerCreatureVerrouille(e.creature.GetRawValue(), e.p.creatureInvulnerable);
        MajCompteur();
    }
    if (!place)
    {
        // Revue du 17/10 : jamais en silence. Terminer retire la creature du registre et la renvoie.
        LOG_ERROR("playerbots.coa", "coa soin: essai={} table des creatures de banc pleine ({} cases)", e.id,
                  uint32(MAX_PLAFOND));
        Terminer(e, "abandon", "table_creatures_pleine");
        return;
    }
    c->SetFaction(FACTION_HOSTILE);
    // PACIFIED : elle attaque (victime, poursuite, combat) mais ne frappe jamais (Unit::AttackerStateUpdate,
    // Unit.cpp:2808) ; Unit::Attack ne le refuse pas (Unit.cpp:7706, test mis en commentaire par le coeur).
    c->SetUnitFlag(UNIT_FLAG_PACIFIED);
    c->SetFullHealth();
    c->SetFacingToObject(cible);

    for (Membre& m : e.membres)
        if (Player* p = Trouver(m.guid))
        {
            RemettreAPlein(p);
            m.vivant = p->IsAlive();
        }
    for (Membre const& m : e.membres)
        if (Player* p = Trouver(m.guid))
            Engager(p, c);
    c->AddThreat(cible, MENACE_CIBLE);
    if (c->IsAIEnabled && c->AI())
        c->AI()->AttackStart(cible);
    else
        c->Attack(cible, true);

    e.phase = Phase::COURS;
    e.debutMs = getMSTime();
    e.dernierCoupMs = e.debutMs;

    std::string membres;
    for (Membre const& m : e.membres)
        membres += (membres.empty() ? "" : ",") + std::to_string(m.guid) + ":" + std::to_string(m.classe) + ":" +
                   std::to_string(m.spe) + ":" + std::to_string(m.role);
    float const palier = Escalade(e.epreuve) ? e.p.depart : e.epreuve == Epreuve::A5 ? e.p.constant : e.p.fond;
    // Revue du 17/10 : le REGLAGE de l'essai (B-1, B-2, B-3) en fin de ligne, apres `palier` (dernier champ du format
    // § 9, que le lecteur exige) : un lecteur d'avant les ignore, le rapport refuse de meler deux reglages.
    LOG_INFO("playerbots.coa", "coa soin debut: essai={} epreuve={} creature={} membres={} palier={} invulnerable={} "
             "potions={} attente={}", e.id, NomEpreuve(e.epreuve), e.creature.GetCounter(), membres,
             FormatPalier(palier), e.p.creatureInvulnerable ? 1 : 0, e.p.potions ? 1 : 0, e.p.attenteS);

    // B-3/B-4 : les auras suivies deja posees a l'engagement (Fetid Ward 680388 posee pendant l'attente ?). Apres la
    // ligne `debut`, pour que le lecteur les rattache a l'essai a t=0. Une ligne par aura, `etat=present`.
    for (Membre const& m : e.membres)
        if (Player* p = Trouver(m.guid))
            for (auto const& [id, app] : p->GetAppliedAuras())
            {
                Aura const* aura = app ? app->GetBase() : nullptr;
                if (aura && AuraSuivie(aura->GetSpellInfo()))
                    LOG_INFO("playerbots.coa", "coa soin aura: essai={} guid={} sort={} etat=present lanceur={} pile={} "
                             "mode=0", e.id, m.guid, id, aura->GetCasterGUID().GetCounter(), uint32(aura->GetStackAmount()));
            }
}

// Une seconde d'un essai en cours.
void Avancer(Essai& e)
{
    Map* carte = sMapMgr->FindBaseNonInstanceMap(e.centre.GetMapId());
    Creature* c = carte ? carte->GetCreature(e.creature) : nullptr;
    if (!c || !c->IsInWorld() || !c->IsAlive())
    {
        Terminer(e, "abandon", "creature_perdue");
        return;
    }
    Group* g = nullptr;
    if (e.groupe)
    {
        g = GroupeDe(e);
        if (!g)
        {
            Terminer(e, "abandon", "groupe_perdu");
            return;
        }
    }

    // Presence. Un mort n'est plus tenu a la position (le moteur des morts le releve au cimetiere).
    std::vector<Player*> joueurs(e.membres.size(), nullptr);
    for (size_t i = 0; i < e.membres.size(); ++i)
    {
        Membre& m = e.membres[i];
        Player* p = Trouver(m.guid);
        if (!p)
        {
            Terminer(e, "abandon", "membre_deconnecte");
            return;
        }
        // Un mort est sorti de l'essai : sa releve (fantome envoye au cimetiere, eventuellement par une teleportation)
        // ne doit pas faire abandonner l'essai des vivants (revue du 13/10). Sa ligne d'etat seulement, s'il est la.
        if (m.mort)
        {
            if (p->IsInWorld() && !p->IsBeingTeleported())
                joueurs[i] = p;
            continue;
        }
        if (!p->IsInWorld() || p->IsBeingTeleported())
        {
            if (++m.horsMondeS > HORS_MONDE_MAX_S)
            {
                Terminer(e, "abandon", "membre_hors_monde");
                return;
            }
            continue;
        }
        m.horsMondeS = 0;
        if (g && p->GetGroup() != g)
        {
            Terminer(e, "abandon", "groupe_perdu");
            return;
        }
        if (p->IsAlive() && (p->GetMapId() != e.centre.GetMapId() || p->GetExactDist2d(&e.centre) > ELOIGNE_YD))
        {
            Terminer(e, "abandon", "membre_eloigne");
            return;
        }
        joueurs[i] = p;
    }

    uint32 const maintenant = getMSTime();
    float const secondes = std::clamp(float(getMSTimeDiff(e.dernierCoupMs, maintenant)) / 1000.0f, 0.5f, 2.0f);
    e.dernierCoupMs = maintenant;
    uint32 const t = getMSTimeDiff(e.debutMs, maintenant) / 1000;

    // Plafond de duree : avant toute charge de la seconde.
    if (t >= e.p.dureeMaxS)
    {
        Terminer(e, "temps");
        return;
    }

    // La charge de la seconde.
    float palier = e.p.fond;
    if (Escalade(e.epreuve))
        palier = e.p.depart + e.p.pas * float(t / e.p.dureePalierS);
    else if (e.epreuve == Epreuve::A5)
        palier = e.p.constant;
    char const* const type = Escalade(e.epreuve) ? "escalade" : "fond";
    float const pct = palier * secondes;
    // Les membres encore dans l'essai (jamais morts), dans le monde.
    std::vector<Player*> actifs(joueurs.size(), nullptr);
    for (size_t i = 0; i < joueurs.size(); ++i)
        if (!e.membres[i].mort)
            actifs[i] = joueurs[i];
    switch (e.epreuve)
    {
        case Epreuve::A1:
        case Epreuve::A4:
        case Epreuve::B1:
        case Epreuve::B2:
            if (Player* p = actifs[e.cible])
                Frapper(e, c, p, pct, type, palier);
            break;
        default:   // A2, A3, A5 : tous les membres
            for (Player* p : actifs)
                if (p)
                    Frapper(e, c, p, pct, type, palier);
            break;
    }
    if (e.epreuve == Epreuve::A3 && t >= e.prochaineUrgenceS)
    {
        e.prochaineUrgenceS += URGENCE_PERIODE_S;
        std::vector<Player*> vivants;
        for (Player* p : actifs)
            if (p && p->IsAlive())
                vivants.push_back(p);
        if (!vivants.empty())
            Ramener(e, c, vivants[urand(0, uint32(vivants.size()) - 1)], URGENCE_PCT, "urgence", palier);
    }
    if (e.epreuve == Epreuve::B2 && !e.semonceFaite && t >= SEMONCE_T_S)
    {
        e.semonceFaite = true;
        if (Player* p = actifs[0])
            Ramener(e, c, p, SEMONCE_PCT, "semonce", palier);
    }

    // Morts (la charge vient de passer), puis l'etat de chaque membre.
    for (size_t i = 0; i < e.membres.size(); ++i)
    {
        Membre& m = e.membres[i];
        Player* p = joueurs[i] ? joueurs[i] : Trouver(m.guid);
        if (!p)
            continue;
        bool const vivant = p->IsAlive();
        if (!m.mort && !vivant)
        {
            m.mort = true;
            ++e.morts;
            LOG_INFO("playerbots.coa", "coa soin mort: essai={} guid={} t={}", e.id, m.guid, t);
        }
        m.vivant = vivant;
        LOG_INFO("playerbots.coa", "coa soin etat: essai={} t={} guid={} pv_pct={} res={} vivant={}", e.id, t, m.guid,
                 PvPct(p), Ressource(p), vivant ? 1 : 0);
    }

    // Fins par la mort.
    bool tousMorts = true;
    for (Membre const& m : e.membres)
        tousMorts = tousMorts && m.mort;
    switch (e.epreuve)
    {
        case Epreuve::A1:
        case Epreuve::A4:
        case Epreuve::B1:
        case Epreuve::B2:
            if (e.membres[e.cible].mort)
            {
                Terminer(e, "mort_cible");
                return;
            }
            break;
        case Epreuve::A2:
            if (e.morts)
            {
                Terminer(e, "mort_groupe");
                return;
            }
            break;
        default:   // A3, A5 : le soigneur mort, ou tout le monde
            if (tousMorts || e.membres[0].mort)
            {
                Terminer(e, "mort_groupe");
                return;
            }
            break;
    }

    // Le combat tenu : creature pleine, sur la cible, et chaque membre vivant en combat avec elle.
    c->SetFullHealth();
    Player* cible = actifs[e.cible];
    if (cible && cible->IsAlive() && !c->IsInEvadeMode())
    {
        if (c->GetVictim() != cible)
        {
            c->AddThreat(cible, MENACE_CIBLE);
            if (c->IsAIEnabled && c->AI())
                c->AI()->AttackStart(cible);
            else
                c->Attack(cible, true);
        }
        for (Player* p : actifs)
            if (p && p->IsAlive() && !p->IsInCombat())
                Engager(p, c);
    }
}

void SuivreRetours()
{
    for (auto it = retours.begin(); it != retours.end();)
    {
        Player* p = Trouver(it->guid);
        bool fait = !p;   // deconnecte : sa position de deconnexion vaut (hors enclos si la teleportation a abouti)
        if (p && p->IsInWorld() && !p->IsBeingTeleported() && !CoaBancSoin::MembreEngage(it->guid))
            fait = p->TeleportTo(it->origine);
        else if (p && CoaBancSoin::MembreEngage(it->guid))
            fait = true;  // repris par un nouvel essai : il en sera rendu a sa fin
        if (!fait && Ecoule(it->depuisMs) > RETOUR_MAX_MS)
        {
            LOG_INFO("playerbots.coa", "coa soin abandon: essai=0 motif=retour_expire guid={}", it->guid);
            fait = true;
        }
        it = fait ? retours.erase(it) : std::next(it);
    }
}

void Suivre()
{
    for (std::unique_ptr<Essai>& ptr : essais)
    {
        Essai& e = *ptr;
        if (e.fini)
            continue;
        if (e.phase == Phase::COURS)
        {
            Avancer(e);
            continue;
        }
        // ATTENTE (B-3) : tous arrives, hors combat dans l'enclos le temps que « coa buff » pose les auras de
        // preparation. Un membre perdu (deconnecte, hors du monde au-dela de HORS_MONDE_MAX_S, trop loin, mort)
        // abandonne l'essai ; un membre attaque (en combat) n'abrege pas l'attente : l'engagement le mettra de toute
        // facon en combat avec la creature.
        if (e.phase == Phase::ATTENTE)
        {
            for (Membre& m : e.membres)
            {
                Player* p = Trouver(m.guid);
                if (!p)
                {
                    Terminer(e, "abandon", "membre_deconnecte");
                    break;
                }
                if (!p->IsInWorld() || p->IsBeingTeleported())
                {
                    if (++m.horsMondeS > HORS_MONDE_MAX_S)
                    {
                        Terminer(e, "abandon", "membre_hors_monde");
                        break;
                    }
                    continue;
                }
                m.horsMondeS = 0;
                if (!p->IsAlive())
                {
                    Terminer(e, "abandon", "membre_mort_attente");
                    break;
                }
                if (p->GetMapId() != e.centre.GetMapId() || p->GetExactDist2d(&e.centre) > ELOIGNE_YD)
                {
                    Terminer(e, "abandon", "membre_eloigne");
                    break;
                }
            }
            if (e.fini)
                continue;
            bool presents = true;
            for (Membre const& m : e.membres)
                if (Player* p = Trouver(m.guid))
                    presents = presents && p->IsInWorld() && !p->IsBeingTeleported();
            if (presents && Ecoule(e.attenteMs) >= e.p.attenteS * 1000)
                Demarrer(e);
            continue;
        }
        // ARRIVEE : tous dans le monde, sur la carte de l'enclos, pres du centre de leur case.
        bool arrives = true;
        for (Membre const& m : e.membres)
        {
            Player* p = Trouver(m.guid);
            if (!p)
            {
                Terminer(e, "abandon", "membre_deconnecte");
                break;
            }
            if (!p->IsInWorld() || p->IsBeingTeleported() || p->GetMapId() != e.centre.GetMapId() ||
                p->GetExactDist2d(&e.centre) > ARRIVE_YD)
                arrives = false;
        }
        if (e.fini)
            continue;
        if (arrives && e.p.attenteS)
        {
            e.phase = Phase::ATTENTE;
            e.attenteMs = getMSTime();
        }
        else if (arrives)
            Demarrer(e);
        else if (Ecoule(e.creeMs) > ARRIVEE_MAX_MS)
            Terminer(e, "abandon", "arrivee_expiree");
    }
    essais.erase(std::remove_if(essais.begin(), essais.end(), [](std::unique_ptr<Essai> const& e) { return e->fini; }),
                 essais.end());
}

// ------------------------------------------------------------------ commande (FIL MONDE)

class CoaBancSoinCommandScript : public CommandScript
{
public:
    CoaBancSoinCommandScript() : CommandScript("CoaBancSoinCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable soinTable = {
            {"banc", HandleBancCommand, SEC_GAMEMASTER, Console::Yes},
            {"arret", HandleArretCommand, SEC_GAMEMASTER, Console::Yes},
        };
        static ChatCommandTable coaTable = {
            {"soin", soinTable},
        };
        static ChatCommandTable commandTable = {
            {"coa", coaTable},
        };
        return commandTable;
    }

    static std::vector<std::string> Mots(char const* args)
    {
        std::vector<std::string> mots;
        std::istringstream flux(args ? args : "");
        for (std::string mot; flux >> mot;)
            mots.push_back(mot);
        return mots;
    }

    // `essai` : un entier (accord avec l'orchestrateur, 13/10), 1 a 4294967295.
    static bool LireEssai(std::string const& t, uint32& id)
    {
        if (t.empty() || t.size() > 10 || !std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        unsigned long long const v = std::strtoull(t.c_str(), nullptr, 10);
        if (!v || v > 0xFFFFFFFFull)
            return false;
        id = uint32(v);
        return true;
    }

    /*
     * .coa soin banc <epreuve> <essai> <nom[:spe]> [<nom[:spe]> ...]
     * Reponse : succes « coa soin banc: lance essai=... » (sans les mots refus, ECHEC ni usage) ; refus
     * « coa soin banc: refus=<code> ... » ; syntaxe « Usage: ... » puis le refus. La ligne `coa soin debut:` suit des
     * l'arrivee de tous les membres (au plus ARRIVEE_MAX_MS = 30 s, sinon `coa soin fin: ... raison=abandon`).
     */
    static bool HandleBancCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const mots = Mots(args);
        uint32 id = 0;
        auto Refus = [&](std::string const& code, std::string const& detail = "") {
            handler->PSendSysMessage("coa soin banc: refus={} essai={}{}{}", code, id, detail.empty() ? "" : " ", detail);
            LOG_INFO("playerbots.coa", "coa soin banc: refus={} essai={}{}{}", code, id, detail.empty() ? "" : " ", detail);
            handler->SetSentErrorMessage(true);
            return false;
        };
        if (!sConfigMgr->GetOption<bool>("AiPlayerbot.CoaSoin.Banc", false, false))
            return Refus("desactive");

        Epreuve epreuve = Epreuve::A1;
        if (mots.size() < 3 || !LireEpreuve(mots[0], epreuve) || !LireEssai(mots[1], id))
        {
            handler->PSendSysMessage("Usage: .coa soin banc <A1|A2|A3|A4|A5|B1|B2> <essai> <nom[:spe]> [<nom[:spe]> ...]");
            return Refus("syntaxe");
        }
        size_t const nbMembres = mots.size() - 2;
        if (VoletB(epreuve) ? nbMembres != 1 : (nbMembres < 2 || nbMembres > MEMBRES_MAX))
            return Refus("membres_nombre", "attendus=" + std::string(VoletB(epreuve) ? "1" : "2-5"));

        Parametres const p = LireParametres();
        for (auto const& ptr : essais)
            if (!ptr->fini && ptr->id == id)
                return Refus("essai_existant");
        std::vector<bool> casesPrises(p.max, false);
        uint32 enCours = 0;
        for (auto const& ptr : essais)
            if (!ptr->fini)
            {
                ++enCours;
                if (ptr->caseEnclos < p.max)
                    casesPrises[ptr->caseEnclos] = true;
            }
        if (enCours >= p.max)
            return Refus("occupe", "essais=" + std::to_string(enCours) + " max=" + std::to_string(p.max));
        uint8 caseLibre = 0;
        while (caseLibre < p.max && casesPrises[caseLibre])
            ++caseLibre;
        if (caseLibre >= p.max)
            return Refus("occupe", "raison=cases");

        WorldLocation enclos;
        if (!LireEnclos(p.enclos, enclos))
            return Refus("enclos_invalide", "enclos=\"" + p.enclos + "\"");
        MapEntry const* entreeCarte = sMapStore.LookupEntry(enclos.GetMapId());
        if (!entreeCarte || entreeCarte->Instanceable())
            return Refus("enclos_instance", "carte=" + std::to_string(enclos.GetMapId()));
        if (!sObjectMgr->GetCreatureTemplate(p.creature))
            return Refus("creature_inconnue", "creature=" + std::to_string(p.creature));
        if (p.niveau < 10 || p.niveau > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
            return Refus("niveau_hors_bornes", "niveau=" + std::to_string(p.niveau));

        // Les membres : controles d'abord, rien n'est touche avant que tous passent.
        auto e = std::make_unique<Essai>();
        e->id = id;
        e->epreuve = epreuve;
        e->caseEnclos = caseLibre;
        e->p = p;
        e->cible = (epreuve == Epreuve::A1 || epreuve == Epreuve::A2 || epreuve == Epreuve::A3 || epreuve == Epreuve::A5)
                       ? 1 : 0;
        std::vector<Player*> joueurs;
        for (size_t i = 2; i < mots.size(); ++i)
        {
            std::string nom = mots[i];
            uint32 spe = 0;
            size_t const deux = nom.find(':');
            if (deux != std::string::npos)
            {
                std::string const texte = nom.substr(deux + 1);
                nom = nom.substr(0, deux);
                if (texte.empty() || texte.size() > 5 ||
                    !std::all_of(texte.begin(), texte.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    return Refus("syntaxe", "membre=" + mots[i]);
                spe = uint32(std::strtoul(texte.c_str(), nullptr, 10));
                if (!spe)
                    return Refus("syntaxe", "membre=" + mots[i]);
            }
            if (nom.empty())
                return Refus("syntaxe", "membre=" + mots[i]);
            Player* b = ObjectAccessor::FindPlayerByName(nom, true);
            if (char const* r = RefusMembre(b, p.niveau))
                return Refus(std::string("membre_") + r, "nom=" + nom);
            if (std::find(joueurs.begin(), joueurs.end(), b) != joueurs.end())
                return Refus("membre_double", "nom=" + nom);
            joueurs.push_back(b);
            Membre m;
            m.guid = b->GetGUID().GetCounter();
            m.nom = b->GetName();
            m.classe = b->getClass();
            m.speVoulue = spe;
            m.speOrigine = GetAscensionActiveSpecialization(b);
            m.origine = WorldLocation(b->GetMapId(), b->GetPositionX(), b->GetPositionY(), b->GetPositionZ(),
                                      b->GetOrientation());
            m.zoneOrigine = b->GetZoneId();
            m.niveauAvant = b->GetLevel();
            e->membres.push_back(m);
        }

        // Corrections du 17/10, lues par les fils de carte : avant l'inscription (et donc avant la composition relue
        // plus bas, qui lit buffsAvantCombat dans AiFactory).
        potionsPermises.store(p.potions, std::memory_order_relaxed);
        buffsAvantCombat.store(p.attenteS > 0, std::memory_order_relaxed);
        for (size_t i = 0; i < aurasSuivies.size(); ++i)
            aurasSuivies[i].store(i < p.aurasSuivies.size() ? p.aurasSuivies[i] : 0, std::memory_order_relaxed);

        // Inscription : les gardes (specialisation, composition, activite, gestionnaire des bots, arene) tiennent des
        // maintenant. RefusMembre a deja ecarte tout membre inscrit ailleurs.
        {
            std::lock_guard<std::mutex> garde(verrouRegistre);
            size_t posees = 0;
            for (Membre const& m : e->membres)
            {
                if (!PlacerVerrouille(m.guid, id))
                    break;
                membresEssai[m.guid] = id;
                ++posees;
            }
            if (posees != e->membres.size())
            {
                for (size_t i = 0; i < posees; ++i)
                {
                    membresEssai.erase(e->membres[i].guid);
                    LibererVerrouille(e->membres[i].guid, id);
                }
                MajCompteur();
                return Refus("registre_plein");
            }
            MajCompteur();
        }

        // Preparation, dans l'ordre donne.
        for (size_t i = 0; i < e->membres.size(); ++i)
        {
            Membre& m = e->membres[i];
            Player* b = joueurs[i];
            if (b->IsMounted())
                b->Dismount();
            if (b->GetLevel() < p.niveau)
            {
                PlayerbotFactory fabrique(b, p.niveau);
                fabrique.Randomize(false);
            }
            std::string refus;
            if (!CoaPoserSpecialisationDeclaree(b, m.speVoulue, refus))
            {
                Annuler(*e);
                return Refus("membre_" + refus, "nom=" + m.nom + " spe=" + std::to_string(m.speVoulue));
            }
            m.spe = GetAscensionActiveSpecialization(b);
            m.role = uint8(GetCoaRole(b));
            uint32 attendus = 0, poses = 0;
            if (p.setFixe && !PoserSet(b, attendus, poses))
            {
                Annuler(*e);
                return Refus("membre_set_incomplet", "nom=" + m.nom + " attendus=" + std::to_string(attendus) +
                                                         " poses=" + std::to_string(poses));
            }
            // Pas de re-randomisation entre la preparation et l'essai (meme geste que `.coa niveau` et l'arene).
            sRandomPlayerbotMgr.SetValue(b, "randomize", 1);
            Remettre(b);
            RemettreAPlein(b);
            RemettreRecharges(b);
            LOG_INFO("playerbots.coa", "coa soin preparation: essai={} guid={} nom={} classe={} niveau_avant={}"
                     " niveau_apres={} spe_origine={} spe={} role={} set_fixe={} set_attendus={} set_poses={} pv_max={}",
                     id, m.guid, m.nom, uint32(m.classe), m.niveauAvant, uint32(b->GetLevel()), m.speOrigine, m.spe,
                     uint32(m.role), p.setFixe ? 1 : 0, attendus, poses, uint32(b->GetMaxHealth()));
            // B-7 (17/10) : l'arbre PORTE pendant l'essai, lu ici et non apres le banc (le bot rendu reprend sa
            // specialisation d'origine). `natif=0` : la specialisation a ete imposee a un bot d'une autre. `entrees`
            // en dernier (la plus longue).
            {
                uint32 voulues = 0, manquantes = 0, enTrop = 0;
                std::string const entrees = CoaArbrePorte(b, voulues, manquantes, enTrop);
                LOG_INFO("playerbots.coa", "coa soin arbre: essai={} guid={} classe={} spe_origine={} spe={} natif={}"
                         " table={} voulues={} manquantes={} en_trop={} entrees={}", id, m.guid, uint32(m.classe),
                         m.speOrigine, m.spe, m.speOrigine == m.spe ? 1 : 0, GetCoaBuildTable(b), voulues, manquantes,
                         enTrop, entrees.empty() ? "-" : entrees);
            }
        }

        // Groupe sans humain (volet A) : le premier membre le mene. S'enregistre aupres du gestionnaire (sinon lignes
        // orphelines dans `group_member` au redemarrage) -- comme `.coa groupe`.
        if (e->membres.size() > 1)
        {
            Group* g = new Group();
            if (!g->Create(joueurs[0]))
            {
                delete g;
                Annuler(*e);
                return Refus("groupe_refuse");
            }
            sGroupMgr->AddGroup(g);
            e->groupe = g->GetGUID().GetCounter();
            for (size_t i = 1; i < joueurs.size(); ++i)
                if (!g->AddMember(joueurs[i]))
                {
                    Annuler(*e);
                    return Refus("groupe_refuse", "nom=" + e->membres[i].nom);
                }
            g->BroadcastGroupUpdate();
        }

        // Composition relue (groupe forme, specialisation posee, gardes armees), puis le soigneur verifie.
        for (Player* b : joueurs)
            if (PlayerbotAI* ia = GET_PLAYERBOT_AI(b))
                ia->ResetStrategies(false);
        if (!VoletB(epreuve))
        {
            PlayerbotAI* ia = GET_PLAYERBOT_AI(joueurs[0]);
            if (!ia || !ia->HasStrategy("coa heal", BOT_STATE_COMBAT))
            {
                Annuler(*e);
                return Refus("soigneur_sans_coa_heal", "nom=" + e->membres[0].nom + " spe=" +
                                                           std::to_string(e->membres[0].spe));
            }
        }

        // Teleportation dans la case.
        Map* carteEnclos = sMapMgr->CreateBaseMap(enclos.GetMapId());
        float const cx = enclos.GetPositionX() + ECART_CASE_YD * float(caseLibre);
        float const cy = enclos.GetPositionY();
        Position const centre = PointAuSol(carteEnclos, cx, cy, enclos.GetPositionZ());
        e->centre = WorldLocation(enclos.GetMapId(), centre.GetPositionX(), centre.GetPositionY(), centre.GetPositionZ(), 0.0f);
        for (size_t i = 0; i < joueurs.size(); ++i)
        {
            float const angle = float(i) * 2.0f * 3.14159265f / float(joueurs.size());
            Position const pos = PointAuSol(carteEnclos, cx - RAYON_MEMBRES_YD * std::cos(angle),
                                            cy + RAYON_MEMBRES_YD * std::sin(angle), centre.GetPositionZ() - 0.5f);
            // Face a la creature (a +x du centre).
            float const o = std::atan2(cy - pos.GetPositionY(), cx + CREATURE_ECART_YD - pos.GetPositionX());
            if (!joueurs[i]->TeleportTo(enclos.GetMapId(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                                        Position::NormalizeOrientation(o)))
            {
                // Rien n'a encore bouge pour ce membre ; les precedents sont rendus par Terminer (teleportation retour).
                e->creeMs = getMSTime();
                Terminer(*e, "abandon", "teleportation_refusee");
                return Refus("teleportation_refusee", "nom=" + e->membres[i].nom);
            }
        }

        e->creeMs = getMSTime();
        std::string noms;
        for (Membre const& m : e->membres)
            noms += (noms.empty() ? "" : ",") + m.nom;
        handler->PSendSysMessage("coa soin banc: lance essai={} epreuve={} case={} membres={}", id, NomEpreuve(epreuve),
                                 uint32(caseLibre), noms);
        LOG_INFO("playerbots.coa", "coa soin banc: lance essai={} epreuve={} case={} membres={} enclos={} "
                 "invulnerable={} potions={} attente={}", id, NomEpreuve(epreuve), uint32(caseLibre), noms, p.enclos,
                 p.creatureInvulnerable ? 1 : 0, p.potions ? 1 : 0, p.attenteS);
        essais.push_back(std::move(e));
        return true;
    }

    // .coa soin arret <essai>
    static bool HandleArretCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const mots = Mots(args);
        uint32 id = 0;
        if (mots.size() != 1 || !LireEssai(mots[0], id))
        {
            handler->PSendSysMessage("Usage: .coa soin arret <essai>");
            handler->PSendSysMessage("coa soin arret: refus=syntaxe");
            handler->SetSentErrorMessage(true);
            return false;
        }
        for (auto& ptr : essais)
            if (!ptr->fini && ptr->id == id)
            {
                Terminer(*ptr, "arret");
                // Retire ici (fil monde) : un second `arret` du meme essai dit `inconnu`.
                essais.erase(std::remove_if(essais.begin(), essais.end(),
                                            [](std::unique_ptr<Essai> const& x) { return x->fini; }),
                             essais.end());
                handler->PSendSysMessage("coa soin arret: essai={} arrete", id);
                return true;
            }
        handler->PSendSysMessage("coa soin arret: refus=inconnu essai={}", id);
        handler->SetSentErrorMessage(true);
        return false;
    }
};

// ------------------------------------------------------------------ suivi (FIL MONDE)

class CoaBancSoinWorldScript : public WorldScript
{
public:
    CoaBancSoinWorldScript() : WorldScript("CoaBancSoinWorldScript", {WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN}) {}

    void OnUpdate(uint32 diff) override
    {
        if (essais.empty() && retours.empty())
        {
            cumul = 0;
            return;
        }
        cumul += diff;
        if (cumul < 1000)
            return;
        cumul = 0;
        Suivre();
        SuivreRetours();
    }

    // Main.cpp : apres WorldUpdateLoop, AVANT KickAll (qui sauvegarde et deconnecte) -- joueurs encore la, cartes
    // arretees. Chaque essai finit en `abandon` (motif arret_serveur), position d'origine ecrite en base.
    void OnShutdown() override
    {
        for (auto& ptr : essais)
            Terminer(*ptr, "abandon", "arret_serveur", true);
        essais.clear();
        retours.clear();
    }

private:
    uint32 cumul = 0;
};

// ------------------------------------------------------------------ crochets d'unite (fils de carte)

class CoaBancSoinUnitScript : public UnitScript
{
public:
    CoaBancSoinUnitScript()
        : UnitScript("CoaBancSoinUnitScript", true,
                     {UNITHOOK_ON_HEAL, UNITHOOK_MODIFY_HEAL_RECEIVED, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
                      UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE})
    {
    }

    // Appele pour TOUT degat du serveur, sans liste de crochets (ScriptMgr::DealDamage) : hors essai, un chargement
    // atomique.
    //  - Creature de banc : invulnerable (banc d'avant, CreatureInvulnerable = 1), ou B-1 (17/10) : les degats passent,
    //    sauf ce qui la tuerait (ramene a PV - 1) ; Avancer la remet a plein chaque seconde. Le vol de vie periodique
    //    (HandlePeriodicHealthLeechAuraTick) soigne sur ce RETOUR : il n'est plus nul.
    //  - Membre d'essai frappe par autre chose que la creature de banc (B-4) : ligne `coa soin degat:`.
    uint32 DealDamage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType type) override
    {
        if (!damage || !victim || !nbInscrits.load(std::memory_order_relaxed))
            return damage;
        if (victim->IsCreature())
        {
            int const mode = ModeCreatureDeBanc(victim);
            if (mode == 1)
                return 0;
            if (mode == 0)
            {
                uint32 const pv = victim->GetHealth();
                return damage < pv ? damage : (pv > 1 ? pv - 1 : 0);
            }
            return damage;
        }
        if (victim->IsPlayer())
            TracerDegat(attacker, victim, damage, type);
        return damage;
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& /*damage*/, SpellInfo const* spellInfo) override
    {
        if (!nbInscrits.load(std::memory_order_relaxed) || !target || !target->IsPlayer())
            return;
        t_degat.cible = target;
        t_degat.source = attacker;
        t_degat.sort = spellInfo ? spellInfo->Id : 0;
        t_degat.valide = true;
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!nbInscrits.load(std::memory_order_relaxed) || !unit || !aura || !unit->IsPlayer())
            return;
        TracerAura(unit, aura->GetSpellInfo(), aura->GetCasterGUID(), aura->GetStackAmount(), "pose", 0);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* app, AuraRemoveMode mode) override
    {
        if (!nbInscrits.load(std::memory_order_relaxed) || !unit || !app || !unit->IsPlayer())
            return;
        Aura const* aura = app->GetBase();
        if (aura)
            TracerAura(unit, aura->GetSpellInfo(), aura->GetCasterGUID(), aura->GetStackAmount(), "levee", uint32(mode));
    }

private:
    static char const* NomDegat(DamageEffectType type)
    {
        switch (type)
        {
            case DIRECT_DAMAGE: return "melee";
            case SPELL_DIRECT_DAMAGE: return "sort";
            case DOT: return "periodique";
            case HEAL: return "soin";
            case NODAMAGE: return "sans";
            case SELF_DAMAGE: return "soi";
        }
        return "autre";
    }

    // Fil de carte. Une ligne par degat subi par un membre, sauf de la creature de banc (sa charge a ses lignes
    // `coup`), bornee a DEBIT_CANAL[CANAL_DEGAT] par essai et par seconde.
    static void TracerDegat(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType type)
    {
        uint32 const essai = EssaiDuJoueur(victim);
        if (!essai)
            return;
        // Le sort : note periodique (ModifyPeriodicDamageAurasTick, meme paire) ou de sort direct (ModifySpellDamageTaken).
        uint32 sort = 0;
        if (t_tick.valide && t_tick.cible == victim && t_tick.lanceur == attacker)
        {
            sort = t_tick.sort;
            t_tick.valide = false;
        }
        else if (t_degat.valide && t_degat.cible == victim && t_degat.source == attacker)
            sort = t_degat.sort;
        t_degat.valide = false;
        if (attacker && attacker->IsCreature() && EstCreatureDeBanc(attacker))
            return;
        char const* origine = "aucune";
        uint32 source = 0;
        if (attacker)
        {
            Player* proprio = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
            source = attacker->GetGUID().GetCounter();
            if (attacker == victim)
                origine = "soi";
            else if (proprio == victim)
                origine = "serviteur";
            else if (proprio && EssaiDuJoueur(proprio) == essai)
                origine = proprio == attacker ? "membre" : "serviteur_membre";
            else if (proprio)
                origine = "joueur";
            else if (attacker->IsCreature())
                origine = "creature";
            else
                origine = "autre";
        }
        uint32 const cible = victim->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> garde(verrouRegistre);
            if (!EssaiDuMembreVerrouille(cible) || !DebitPermis(essai, CANAL_DEGAT, getMSTime()))
                return;
        }
        LOG_INFO("playerbots.coa", "coa soin degat: essai={} cible={} source={} origine={} sort={} montant={} type={}"
                 " pv_pct_avant={}", essai, cible, source, origine, sort, damage, NomDegat(type),
                 std::min<uint32>(100, uint32(std::lround(victim->GetHealthPct()))));
    }

    // Fil de carte. Pose et levee d'une aura suivie (AuraSuivie) sur un membre.
    static void TracerAura(Unit* unit, SpellInfo const* info, ObjectGuid lanceur, uint8 pile, char const* etat,
                           uint32 mode)
    {
        if (!AuraSuivie(info))
            return;
        uint32 const essai = EssaiDuJoueur(unit);
        if (!essai)
            return;
        uint32 const guid = unit->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> garde(verrouRegistre);
            if (!EssaiDuMembreVerrouille(guid) || !DebitPermis(essai, CANAL_AURA, getMSTime()))
                return;
        }
        LOG_INFO("playerbots.coa", "coa soin aura: essai={} guid={} sort={} etat={} lanceur={} pile={} mode={}", essai, guid,
                 info->Id, etat, lanceur.GetCounter(), uint32(pile), mode);
    }

public:

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& /*damage*/, SpellInfo const* spellInfo) override
    {
        if (!nbInscrits.load(std::memory_order_relaxed))
            return;
        t_tick.cible = target;
        t_tick.lanceur = attacker;
        t_tick.sort = spellInfo ? spellInfo->Id : 0;
        t_tick.valide = true;
    }

    void ModifyHealReceived(Unit* u1, Unit* u2, uint32& heal, SpellInfo const* spellInfo) override
    {
        if (!nbInscrits.load(std::memory_order_relaxed))
            return;
        uint32 const sort = spellInfo ? spellInfo->Id : 0;
        // Chemin periodique : (cible, lanceur) dans les deux crochets, dans cet ordre, meme sort.
        bool const periodique = t_tick.valide && t_tick.sort == sort && t_tick.cible == u1 && t_tick.lanceur == u2;
        t_tick.valide = false;
        t_soin.a = u1;
        t_soin.b = u2;
        t_soin.brut = heal;
        t_soin.sort = sort;
        t_soin.periodique = periodique;
        t_soin.valide = true;
    }

    // `gain` : le soin EFFECTIF (DealHeal, apres ModifyHealth). Lanceur et cible dans le bon ordre.
    void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override
    {
        if (!nbInscrits.load(std::memory_order_relaxed))
        {
            t_soin.valide = false;
            return;
        }
        SoinEnAttente const attente = t_soin;
        t_soin.valide = false;
        Player* lanceurJoueur = healer ? healer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        uint32 essai = EssaiDuJoueur(receiver);
        if (!essai)
            essai = EssaiDuJoueur(lanceurJoueur);
        if (!essai)
            return;
        // La paire de ModifyHealReceived, sans ordre. Sinon (DealHeal appele hors de HealBySpell et du tick, par un
        // script) : le brut est le gain, le sort inconnu (0).
        bool const apparie = attente.valide && ((attente.a == healer && attente.b == receiver) ||
                                                (attente.a == receiver && attente.b == healer));
        uint32 const brut = apparie ? std::max(attente.brut, gain) : gain;
        uint32 const sort = apparie ? attente.sort : 0;
        bool const periodique = apparie && attente.periodique;
        bool crit = false;
        // Coup critique : connu des soins DIRECTS d'un sort (OnSpellCalculatedTarget) ; jamais pour un tick de soin
        // periodique ni pour un soin de script (le coeur ne le publie pas) : crit=0 alors.
        if (sort && !periodique && receiver)
        {
            std::lock_guard<std::mutex> garde(verrouRegistre);
            auto it = notesCrit.find({receiver->GetGUID().GetCounter(), sort});
            if (it != notesCrit.end())
            {
                crit = it->second.crit && getMSTimeDiff(it->second.ms, getMSTime()) <= CRIT_PEREMPTION_MS;
                notesCrit.erase(it);
            }
        }
        uint32 const lanceur = lanceurJoueur ? lanceurJoueur->GetGUID().GetCounter()
                                             : healer ? healer->GetGUID().GetCounter() : 0;
        LOG_INFO("playerbots.coa", "coa soin: essai={} lanceur={} cible={} sort={} brut={} effectif={} surplus={}"
                 " periodique={} crit={}", essai, lanceur, receiver ? receiver->GetGUID().GetCounter() : 0, sort, brut,
                 gain, brut - gain, periodique ? 1 : 0, crit ? 1 : 0);
    }
};

// Coup critique des soins directs : Spell::CalculateTargetInfo tire `crit` puis appelle ce crochet
// (Spell.cpp:8540-8541) ; le soin part plus tard (Spell::DoAllEffectOnTarget, Spell.cpp:2784). Note par (cible, sort),
// consommee par OnHeal.
class CoaBancSoinSpellScript : public AllSpellScript
{
public:
    CoaBancSoinSpellScript() : AllSpellScript("CoaBancSoinSpellScript", {ALLSPELLHOOK_ON_CALCULATED_TARGET}) {}

    void OnSpellCalculatedTarget(Spell* spell, Unit* target, TargetInfo& targetInfo) override
    {
        if (!nbInscrits.load(std::memory_order_relaxed) || !spell || !target || !target->IsPlayer())
            return;
        SpellInfo const* info = spell->GetSpellInfo();
        if (!info || !(info->HasEffect(SPELL_EFFECT_HEAL) || info->HasEffect(SPELL_EFFECT_HEAL_PCT) ||
                       info->HasEffect(SPELL_EFFECT_HEAL_MAX_HEALTH) || info->HasEffect(SPELL_EFFECT_HEAL_MECHANICAL)))
            return;
        uint32 const guid = target->GetGUID().GetCounter();
        if (!EssaiDuMembre(guid))   // sans verrou : ce crochet voit TOUS les soins directs du royaume
            return;
        std::lock_guard<std::mutex> garde(verrouRegistre);
        uint32 const maintenant = getMSTime();
        // Borne : les notes perimees partent (un soin manque, une cible morte avant l'impact).
        if (notesCrit.size() > 256)
            for (auto it = notesCrit.begin(); it != notesCrit.end();)
                it = getMSTimeDiff(it->second.ms, maintenant) > CRIT_PEREMPTION_MS ? notesCrit.erase(it) : std::next(it);
        notesCrit[{guid, info->Id}] = NoteCrit{bool(targetInfo.crit), maintenant};
    }
};

}  // namespace

// ------------------------------------------------------------------ API

bool CoaBancSoin::MembreEngage(uint32 guidLow)
{
    return EssaiDuMembre(guidLow) != 0;   // aucun essai : un chargement atomique ; sinon une lecture sans verrou
}

void CoaBancSoin::TracerAction(Player* bot, char const* action, char const* resultat, uint32 sort, uint32 code)
{
    if (!bot || !nbInscrits.load(std::memory_order_acquire))
        return;
    uint32 const guid = bot->GetGUID().GetCounter();
    uint32 const maintenant = getMSTime();
    uint32 essai = 0;
    {
        std::lock_guard<std::mutex> garde(verrouRegistre);
        essai = EssaiDuMembreVerrouille(guid);
        if (!essai)
            return;
        std::string cle = std::string(action) + "|" + resultat + "|" + std::to_string(sort) + "|" + std::to_string(code);
        BorneAction& borne = bornesAction[guid];
        if (borne.cle == cle && borne.ms && getMSTimeDiff(borne.ms, maintenant) < 1000)
            return;
        DebitEssai& debit = debitsAction[essai];
        uint32 const seconde = maintenant / 1000;
        if (debit.seconde != seconde)
        {
            debit.seconde = seconde;
            debit.lignes = 0;
        }
        if (debit.lignes >= ACTIONS_PAR_SECONDE)
            return;
        ++debit.lignes;
        borne.cle = std::move(cle);
        borne.ms = maintenant ? maintenant : 1;
    }
    LOG_INFO("playerbots.coa", "coa soin action: essai={} guid={} action={} resultat={} sort={} code={}", essai, guid,
             action, resultat, sort, code);
}

bool CoaBancSoin::PotionsInterdites(uint32 guidLow)
{
    return EssaiDuMembre(guidLow) != 0 && !potionsPermises.load(std::memory_order_relaxed);
}

bool CoaBancSoin::BuffsAvantCombat()
{
    return buffsAvantCombat.load(std::memory_order_relaxed);
}

void CoaBancSoin::TracerLancer(Player* bot, char const* action, uint32 sort)
{
    if (!bot || !sort || !nbInscrits.load(std::memory_order_acquire))
        return;
    uint32 const guid = bot->GetGUID().GetCounter();
    uint32 essai = 0;
    {
        std::lock_guard<std::mutex> garde(verrouRegistre);
        essai = EssaiDuMembreVerrouille(guid);
        if (!essai || !DebitPermis(essai, CANAL_LANCER, getMSTime()))
            return;
    }
    // Meme format que les lignes des actions de soin (TracerAction) : le lecteur les distingue par `action`.
    LOG_INFO("playerbots.coa", "coa soin action: essai={} guid={} action={} resultat=lance sort={} code=0", essai, guid,
             action, sort);
}

void CoaBancSoin::TracerListe(Player* bot, char const* action, std::string const& defensifs, std::string const& soins)
{
    if (!bot || !nbInscrits.load(std::memory_order_acquire))
        return;
    uint32 const guid = bot->GetGUID().GetCounter();
    uint32 essai = 0;
    std::string const contenu = defensifs + "|" + soins;
    {
        std::lock_guard<std::mutex> garde(verrouRegistre);
        essai = EssaiDuMembreVerrouille(guid);
        if (!essai)
            return;
        std::string& derniere = dernieresListes[{guid, std::string(action)}];
        if (derniere == contenu || !DebitPermis(essai, CANAL_LISTE, getMSTime()))
            return;
        derniere = contenu;
    }
    LOG_INFO("playerbots.coa", "coa soin liste: essai={} guid={} action={} pv_pct={} defensifs={} soins={}", essai, guid,
             action, std::min<uint32>(100, uint32(std::lround(bot->GetHealthPct()))), defensifs.empty() ? "-" : defensifs,
             soins.empty() ? "-" : soins);
}

void AddSC_coa_banc_soin()
{
    new CoaBancSoinCommandScript();
    new CoaBancSoinWorldScript();
    new CoaBancSoinUnitScript();
    new CoaBancSoinSpellScript();
}
