/*
 * Conquest of Azeroth -- des bots relevent les joueurs HUMAINS morts. Conception et fils : CoaRelever.h.
 *
 * Faits du coeur sur lesquels ce fichier repose (core-ptr, lus le 27/10/2026) :
 *  - KillPlayer pose DeathState::Corpse (Player.cpp:4806) ; la liberation (BuildPlayerRepop) ne le change pas ; un
 *    fantome qui se reconnecte passe en DeathState::Dead (PlayerStorage.cpp:5558-5561). Les effets 18 et 113 ne
 *    testent que IsAlive / IsInWorld / isResurrectRequested (SpellEffects.cpp:563-587, 4974-5004) : les deux etats
 *    sont relevables.
 *  - Cible cadavre : Spell::SelectEffectTypeImplicitTargets remplace le cadavre par son proprietaire et l'ajoute SANS
 *    revalider (Spell.cpp:2050-2061, AddUnitTarget(..., false)) ; Spell::CheckRange ne teste ni portee ni distance sur
 *    un cadavre (Spell.cpp:7188-7256) : la distance au cadavre est donc bornee ICI.
 *  - L'humain accepte ou refuse la fenetre (HandleResurrectResponseOpcode, MiscHandler.cpp:761-782) ; un refus vide la
 *    demande, d'ou le Delai entre deux essais. Accepte, il est teleporte pres du lanceur (ResurectUsingRequestData).
 */

#include "CoaRelever.h"

#include "CoaArene1v1.h"
#include "CoaBancSoin.h"
#include "CoaCompagnons.h"
#include "CoaDirecteur.h"
#include "CoaGelBanc.h"
#include "Corpse.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace
{
// Horloge monotone : pas de retour a zero de getMSTime (49 jours) dans les comparaisons de delais.
uint64 MaintenantMs()
{
    return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

// Un bot garde le corps au plus ce temps (approche + incantation) ; au-dela, il l'abandonne et le Delai court. Le
// compagnon vient de plus loin (PorteeCompagnon, jusqu'a 100 yards) : il a plus de temps.
constexpr uint64 RESERVATION_MS = 30000;
constexpr uint64 RESERVATION_COMPAGNON_MS = 45000;
// Approches abandonnees ou lancers rates avant qu'un bot renonce a CETTE mort : un etranger vite, le compagnon tard
// (un combat pendant l'approche fait echoir la reservation sans que le corps soit inaccessible).
constexpr uint8 RATES_MAX_ETRANGER = 2;
constexpr uint8 RATES_MAX_COMPAGNON = 4;
// Lancers partis sur une meme mort : au-dela, l'humain a refuse (ou ne repond pas) ; on cesse de le solliciter.
constexpr uint8 LANCERS_MAX = 2;
// Sort a recharge plus longue (resurrection de combat : Rebirth 20484, Call of The Scourge 801792) : reserve au
// compagnon, un etranger ne le grille pas hors combat.
constexpr uint32 RECHARGE_LONGUE_MS = 60000;
// Mort JcJ (OnPlayerPVPKill precede OnPlayerJustDied : Unit::Kill pose JustDied, KillPlayer vient au tick suivant de
// Player::Update) : la marque est consommee par l'inscription si elle a moins de ce temps.
constexpr uint64 MARQUE_JCJ_MS = 10000;
// Liste des sorts de resurrection d'un bot : relue au plus tous les ...
constexpr uint64 CACHE_SORTS_MS = 60000;
// Quand l'humain a un compagnon eveille, les autres bots lui laissent ce temps apres la mort.
constexpr uint64 ATTENTE_COMPAGNON_MS = 15000;
// Un echec de lancer (bot qui se releve, incantation refusee) : nouvel essai plus tot que le Delai.
constexpr uint64 REESSAI_ECHEC_MS = 10000;
// Une inscription plus vieille est purgee (le cadavre a de toute facon disparu ou ete rejoint).
constexpr uint64 AGE_MAX_MS = 2ull * 3600ull * 1000ull;
// Journal « releve » : l'essai d'un bot est cite s'il date de moins de ce temps (supposition : c'est lui).
constexpr uint64 JOURNAL_RELEVE_MS = 120000;

struct Entree
{
    ObjectGuid guid;
    std::string nom;            // de l'humain, pour le journal (lu a l'inscription, sur son fil)
    uint64 mortMs = 0;
    uint32 compagnon = 0;       // compagnon eveille (guid bas), recopie par le fil monde ; 0 sans compagnon
    bool compagnonConnu = false;  // le fil monde a renseigne `compagnon` au moins une fois
    bool jcj = false;           // tue par un joueur (ou un bot) : personne ne le releve devant son tueur
    bool clos = false;          // LANCERS_MAX atteint sans resultat : plus aucun essai pour cette mort
    int8 permise = -1;          // OnPlayerCanResurrect, lu une fois par mort (-1 : pas encore lu)
    uint8 lancers = 0;
    uint32 lanceur = 0;         // bot qui tient la reservation (guid bas), 0 si libre
    std::string lanceurNom;
    uint64 reserveJusqua = 0;
    uint64 prochainEssai = 0;
    uint32 dernierLanceur = 0;  // dernier bot dont le lancer est parti
    std::string dernierLanceurNom;
    uint32 dernierSort = 0;
    uint64 dernierEssaiMs = 0;
    std::unordered_map<uint32, uint8> rates;  // par bot : approches abandonnees + lancers rates sur cette mort
    std::vector<uint32> annonces;              // bots dont l'« approche » est deja au journal pour cette mort
};

std::mutex gVerrou;
std::unordered_map<uint32, Entree> gMorts;  // cle : guid bas de l'humain
std::unordered_map<uint32, uint64> gJcJ;    // humain tue par un joueur -> date, en attente de son inscription
std::atomic<uint32> gNombre{0};             // gMorts.size(), lu sans verrou par le declencheur
uint32 gCumulMs = 0;                        // fil monde seul

// Ce bot a-t-il renonce a cette mort ? Sous gVerrou.
bool Renonce(Entree const& e, uint32 botLow)
{
    auto it = e.rates.find(botLow);
    if (it == e.rates.end())
        return false;
    return it->second >= (botLow == e.compagnon ? RATES_MAX_COMPAGNON : RATES_MAX_ETRANGER);
}

void Inscrire(Player* humain)
{
    if (!humain)
        return;
    std::lock_guard<std::mutex> verrou(gVerrou);
    Entree& e = gMorts[humain->GetGUID().GetCounter()];
    e = Entree();
    e.guid = humain->GetGUID();
    e.nom = humain->GetName();
    e.mortMs = MaintenantMs();
    auto jcj = gJcJ.find(humain->GetGUID().GetCounter());
    if (jcj != gJcJ.end())
    {
        e.jcj = e.mortMs - jcj->second < MARQUE_JCJ_MS;
        gJcJ.erase(jcj);
    }
    gNombre.store(static_cast<uint32>(gMorts.size()));
}

// Ce que le bot va faire, s'il peut.
struct Choix
{
    Player* humain = nullptr;
    Corpse* cadavre = nullptr;      // non nul : humain libere, vise par son cadavre
    SpellInfo const* sort = nullptr;
    float distance = 0.0f;          // bot -> corps (ou cadavre)
    float approche = 0.0f;          // distance a laquelle lancer
    bool compagnon = false;
};

struct Candidat
{
    ObjectGuid guid;
    bool compagnon;
    int8 permise;
};

// Les bots qui ne doivent pas lever le petit doigt : combat, champ de bataille, duel, banc, vol, JcJ sauvage.
bool BotDisponible(PlayerbotAI* botAI, Player* bot)
{
    if (!bot || !botAI || !bot->IsInWorld() || !bot->IsAlive() || bot->IsBeingTeleported())
        return false;
    if (bot->IsInCombat() || bot->InBattleground() || bot->InArena() || bot->duel)
        return false;
    // En vol (monture volante, forme de vol) ou sur un taxi : PlayerbotAI::CastSpell refuse, et demonter en l'air
    // ferait tomber le bot.
    if (bot->IsInFlight() || bot->IsFlying() || bot->GetVehicle())
        return false;
    if (bot->IsNonMeleeSpellCast(false) || bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
        return false;
    // Mercenaire : pas de soutien en JcJ sauvage. PAS le drapeau FFA du bot : avec WildPvp.AllBotsAreTargets, il est pose
    // sur tout bot sans maitre de niveau WildPvp.MinLevel ou plus hors sanctuaire (PlayerbotAI.cpp:425-439), compagnons
    // compris ; l'ecarter rendait la fonction muette au-dessus du niveau 20. C'est l'humain FFA qui est ecarte (Choisir).
    if (sPlayerbotAIConfig.IsMercenary(bot->GetGUID().GetRawValue()))
        return false;
    uint32 const botLow = bot->GetGUID().GetCounter();
    if (CoaGelBanc(bot) || CoaBancSoin::MembreEngage(botLow) || CoaArene1v1::GladiateurEngage(botLow))
        return false;
    return true;
}

// Sort accepte par CheckCast, avec les refus qu'une approche, un demontage ou un arret leveront.
bool CheckCastPassable(SpellCastResult r)
{
    switch (r)
    {
        case SPELL_CAST_OK:
        case SPELL_FAILED_OUT_OF_RANGE:
        case SPELL_FAILED_LINE_OF_SIGHT:
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_NOT_MOUNTED:
        case SPELL_FAILED_NOT_READY:
        case SPELL_FAILED_TRY_AGAIN:
            return true;
        default:
            return false;
    }
}

// Meme forme que PlayerbotAI::CanCastSpell (PlayerbotAI.cpp:3699-3709), avec la cible cadavre en plus.
SpellCastResult Verifier(Player* bot, SpellInfo const* info, Player* humain, Corpse* cadavre)
{
    Unit* ancienneSelection = bot->GetSelectedUnit();
    Spell* spell = new Spell(bot, info, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
    if (cadavre)
        spell->m_targets.SetCorpseTarget(cadavre);
    else
        spell->m_targets.SetUnitTarget(humain);
    SpellCastResult const r = spell->CheckCast(true);
    delete spell;
    if (ancienneSelection)
        bot->SetSelection(ancienneSelection->GetGUID());
    return r;
}

// Ce que les effets d'un sort disent de son usage pour relever un autre joueur.
struct Analyse
{
    bool explicite = false;         // un effet 18/113 a cible explicite (TargetA nul)
    bool nouveauExplicite = false;  // dont un 113 (seul a passer par le cadavre d'un fantome)
    bool zone = false;              // resurrection de zone autour du lanceur
    float rayon = 0.0f;
};

// Faux si le sort ne releve pas un autre joueur. Ne depend que du DBC (et du rayon modifie par le bot).
bool Analyser(SpellInfo const* info, Player* bot, Analyse& a)
{
    a = Analyse();
    if (!info || info->IsPassive() || !(info->Targets & TARGET_FLAG_CORPSE_ALLY))
        return false;
    bool soi = false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& eff = info->Effects[i];
        if (eff.Effect == SPELL_EFFECT_SELF_RESURRECT)
            soi = true;
        if (eff.Effect != SPELL_EFFECT_RESURRECT && eff.Effect != SPELL_EFFECT_RESURRECT_NEW)
            continue;
        // TargetA nul : cible explicite (Resurrection 2006, Revivify 801790...). Defibrillate 802175 : 22/30,
        // resurrection de zone autour du lanceur. Tout autre ciblage (Resynchronize 804495 : 22/33, groupe) est
        // ecarte.
        if (eff.TargetA.GetTarget() == 0)
        {
            a.explicite = true;
            if (eff.Effect == SPELL_EFFECT_RESURRECT_NEW)
                a.nouveauExplicite = true;
        }
        else if (eff.TargetA.GetTarget() == TARGET_SRC_CASTER && eff.TargetB.GetTarget() == TARGET_UNIT_SRC_AREA_ALLY)
        {
            a.zone = true;
            a.rayon = std::max(a.rayon, eff.CalcRadius(bot));
        }
    }
    // SELF_RESURRECT (94) : Reborn from Ash 804231, une auto-resurrection.
    return !soi && (a.explicite || a.zone);
}

// Cache par bot de ses sorts de resurrection (ids), relu au plus tous les CACHE_SORTS_MS : sans lui, chaque bot proche
// d'un mort (guerrier, voleur...) parcourait tout son livre chaque seconde pour ne rien trouver. Un bot qui apprend un
// sort ou change de specialisation attend au plus CACHE_SORTS_MS. Taille bornee par le nombre de bots.
struct CacheSorts
{
    uint64 jusqua = 0;
    std::vector<uint32> ids;
};
std::mutex gVerrouCache;
std::unordered_map<uint32, CacheSorts> gCache;

std::vector<uint32> SortsDeRelever(Player* bot)
{
    uint32 const botLow = bot->GetGUID().GetCounter();
    uint64 const maintenant = MaintenantMs();
    {
        std::lock_guard<std::mutex> verrou(gVerrouCache);
        auto it = gCache.find(botLow);
        if (it != gCache.end() && maintenant < it->second.jusqua)
            return it->second.ids;
    }
    // Hors verrou : le livre du bot ne se lit que sur son fil (celui-ci).
    std::vector<uint32> ids;
    Analyse a;
    for (auto const& [id, ps] : bot->GetSpellMap())
    {
        if (!ps || !bot->HasSpell(id))   // HasSpell : ni retire, ni rang inactif, ni autre specialisation
            continue;
        if (Analyser(sSpellMgr->GetSpellInfo(id), bot, a))
            ids.push_back(id);
    }
    std::lock_guard<std::mutex> verrou(gVerrouCache);
    CacheSorts& c = gCache[botLow];
    c.jusqua = maintenant + CACHE_SORTS_MS;
    c.ids = ids;
    return ids;
}

// Le meilleur sort de resurrection du livre du bot pour ce corps, lu par ses effets ; nullptr s'il n'en a aucun
// d'utilisable maintenant. `approche` recoit la distance de lancer.
SpellInfo const* ChoisirSort(Player* bot, Player* humain, Corpse* cadavre, bool compagnon, std::vector<uint32> const& sorts,
                             float& approche)
{
    struct Option
    {
        SpellInfo const* info;
        float approche;
        std::tuple<int, uint32, uint32> rang;   // sans recharge d'abord, puis niveau de sort, puis id
    };
    std::vector<Option> options;

    for (uint32 id : sorts)
    {
        if (!bot->HasSpell(id))   // desappris depuis la mise en cache
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
        Analyse an;
        if (!Analyser(info, bot, an))
            continue;
        bool const explicite = an.explicite;
        float const rayon = an.rayon;
        // Fantome : seule la cible cadavre d'un effet 113 explicite passe (Spell::CheckEffectTarget).
        if (cadavre && !an.nouveauExplicite)
            continue;

        float a = 0.0f;
        if (explicite)
            a = std::clamp(info->GetMaxRange(true, bot) - 3.0f, 2.0f, 27.0f);
        else
        {
            if (rayon <= 2.0f)
                continue;
            a = std::clamp(rayon - 1.5f, 1.5f, 27.0f);
        }

        if (bot->HasSpellCooldown(id))
            continue;
        uint32 const recharge = std::max<uint32>(info->RecoveryTime, info->CategoryRecoveryTime);
        if (recharge > RECHARGE_LONGUE_MS && !compagnon)
            continue;
        // CheckCast est fait plus bas SANS cout (TRIGGERED_IGNORE_POWER_AND_REAGENT_COST) : cout et composants ici.
        if (info->PowerType < MAX_POWERS)
        {
            int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
            if (cout > 0 && static_cast<int32>(bot->GetPower(Powers(info->PowerType))) < cout)
                continue;
        }
        else if (info->PowerType == POWER_HEALTH)
        {
            // Cout en vie : le bot ne se tue pas pour relever.
            int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
            if (cout > 0 && static_cast<int64>(bot->GetHealth()) <= cout)
                continue;
        }
        bool composants = true;
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            if (info->Reagent[r] > 0 && !bot->HasItemCount(static_cast<uint32>(info->Reagent[r]), info->ReagentCount[r]))
                composants = false;
        if (!composants)
            continue;

        options.push_back({info, a, std::make_tuple(recharge ? 0 : 1, info->SpellLevel, info->Id)});
    }

    std::sort(options.begin(), options.end(), [](Option const& x, Option const& y) { return x.rang > y.rang; });
    for (Option const& o : options)
    {
        if (CheckCastPassable(Verifier(bot, o.info, humain, cadavre)))
        {
            approche = o.approche;
            return o.info;
        }
    }
    return nullptr;
}

// Un humain du registre que CE bot peut relever maintenant. Fil de carte du bot.
bool Choisir(PlayerbotAI* botAI, Player* bot, Choix& choix)
{
    uint32 const botLow = bot->GetGUID().GetCounter();
    uint64 const maintenant = MaintenantMs();
    bool const etrangers = sPlayerbotAIConfig.coaReleverEtrangers;
    // Sans Etrangers, seul un compagnon (ou bot de reserve) peut agir : les autres sortent sans prendre le verrou.
    if (!etrangers && !CoaCompagnons::EstCompagnon(botLow))
        return false;
    // Sans sort de resurrection (cache), rien a faire : pas de verrou, pas de lecture du livre.
    std::vector<uint32> const sorts = SortsDeRelever(bot);
    if (sorts.empty())
        return false;
    // Bot en mission du Directeur (convoi, assaut, garde) ou cible de banc : il ne la quitte que pour SON humain
    // (meme garde que CoaHotelDesVentes.cpp:858).
    bool const missionne = CoaDirecteur::AffectationDe(botLow, nullptr) || CoaDirecteur::CibleDeBanc(botLow);

    std::vector<Candidat> candidats;
    {
        std::lock_guard<std::mutex> verrou(gVerrou);
        for (auto& [low, e] : gMorts)
        {
            // Reservation echue sans lancer : le bot a abandonne (chemin impossible, combat...). Il y renonce apres
            // RATES_MAX_* fois (corps sous l'eau, sur une corniche...) ; le Delai court pour tous.
            if (e.lanceur && e.reserveJusqua <= maintenant)
            {
                uint32 const abandonneur = e.lanceur;
                ++e.rates[abandonneur];
                LOG_INFO("playerbots", "coa relever: bot={} humain={} sort=0 distance=0 resultat=approche abandonnee{}",
                         e.lanceurNom, e.nom, Renonce(e, abandonneur) ? " (renonce a cette mort)" : "");
                e.lanceur = 0;
                e.lanceurNom.clear();
                e.prochainEssai = maintenant + uint64(sPlayerbotAIConfig.coaReleverDelai) * 1000;
            }
            if (e.clos || e.jcj || e.permise == 0)
                continue;
            if (maintenant < e.prochainEssai)
                continue;
            // LANCERS_MAX fenetres envoyees, Delai ecoule, toujours mort : refus (ou absence). On cesse.
            if (e.lancers >= LANCERS_MAX)
            {
                e.clos = true;
                LOG_INFO("playerbots", "coa relever: bot={} humain={} sort={} distance=0 resultat=refus suppose ({} lancers)",
                         e.dernierLanceurNom, e.nom, e.dernierSort, e.lancers);
                continue;
            }
            if (Renonce(e, botLow))
                continue;
            bool const compagnon = e.compagnon && e.compagnon == botLow;
            if (!compagnon)
            {
                if (!etrangers || missionne)
                    continue;
                // Tant que le fil monde n'a pas dit s'il y a un compagnon (jusqu'a 1 s apres la mort), on le suppose.
                if ((!e.compagnonConnu || e.compagnon) && maintenant - e.mortMs < ATTENTE_COMPAGNON_MS)
                    continue;
            }
            // Un autre bot tient le corps ; seul le compagnon le lui reprend.
            if (e.lanceur && e.lanceur != botLow && !(compagnon && e.lanceur != e.compagnon))
                continue;
            candidats.push_back({e.guid, compagnon, e.permise});
        }
    }
    if (candidats.empty())
        return false;

    uint32 proie = 0;
    bool const chasseur = CoaDirecteur::ProieImposee(botLow, &proie);
    float const portee = sPlayerbotAIConfig.coaReleverPortee;
    float const porteeCompagnon = sPlayerbotAIConfig.coaReleverPorteeCompagnon;

    bool trouve = false;
    for (Candidat const& c : candidats)
    {
        // Meme carte que le bot, donc meme fil : seule lecture sure de l'humain (ObjectAccessor.cpp:244-256).
        Player* humain = ObjectAccessor::GetPlayer(*bot, c.guid);
        if (!humain || humain == bot || humain->IsAlive() || !humain->IsInWorld() || humain->IsGameMaster())
            continue;
        // Demande deja posee (fenetre ouverte chez l'humain, par un bot ou un joueur) ; aura qui interdit.
        if (humain->isResurrectRequested() || humain->HasPreventResurectionAura())
            continue;
        // Un module refuse sa resurrection (mod-coa-challenges : echec permadeath, CoA.Challenges.Scripts.cpp:942) :
        // ResurrectPlayer sortirait sans rien faire (Player.cpp:4717), la fenetre reviendrait sans fin. Lu UNE fois par
        // mort, ici et non a l'inscription : le defi note l'echec dans son propre OnPlayerJustDied (ordre des crochets
        // inconnu), et sa reponse coute une requete SYNCHRONE en base (CoA.Challenges.Lifecycle.cpp:605-617).
        if (c.permise < 0)
        {
            bool const permise = sScriptMgr->OnPlayerCanResurrect(humain);
            std::lock_guard<std::mutex> verrou(gVerrou);
            auto it = gMorts.find(humain->GetGUID().GetCounter());
            if (it != gMorts.end())
                it->second.permise = permise ? 1 : 0;
            if (!permise)
            {
                LOG_INFO("playerbots", "coa relever: bot={} humain={} sort=0 distance=0 resultat=resurrection interdite",
                         bot->GetName(), humain->GetName());
                continue;
            }
        }
        else if (c.permise == 0)
            continue;
        // Humain en JcJ libre (drapeau FFA) : le relever devant ses adversaires n'est pas un service.
        if (humain->IsFFAPvP())
            continue;
        if (humain->GetTeamId() != bot->GetTeamId() || bot->IsHostileTo(humain))
            continue;
        if (humain->InBattleground() || humain->InArena())
            continue;
        // Bot mene par un joueur : il ne releve que son maitre et les humains de son groupe.
        if (botAI->HasGameClientMaster() && botAI->GetMaster() != humain && !bot->IsInSameGroupWith(humain))
            continue;
        if (chasseur && proie == humain->GetGUID().GetCounter())
            continue;

        Corpse* cadavre = nullptr;
        if (humain->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        {
            cadavre = humain->GetCorpse();
            if (!cadavre || !cadavre->IsInWorld())
                continue;
        }
        WorldObject* lieu = cadavre ? static_cast<WorldObject*>(cadavre) : static_cast<WorldObject*>(humain);
        float const distance = bot->GetDistance(lieu);
        if (distance > (c.compagnon ? porteeCompagnon : portee))
            continue;
        // Le compagnon passe avant tout autre ; sinon le plus proche.
        if (trouve && (choix.compagnon > c.compagnon || (choix.compagnon == c.compagnon && choix.distance <= distance)))
            continue;

        float approche = 0.0f;
        SpellInfo const* sort = ChoisirSort(bot, humain, cadavre, c.compagnon, sorts, approche);
        if (!sort)
            continue;

        choix.humain = humain;
        choix.cadavre = cadavre;
        choix.sort = sort;
        choix.distance = distance;
        choix.approche = approche;
        choix.compagnon = c.compagnon;
        trouve = true;
    }
    return trouve;
}

// Pose (ou garde) la reservation de ce corps pour ce bot. Vrai si le bot la tient ; `nouvelle` : il la prend pour la
// premiere fois sur cette mort (journal « approche », une fois par bot et par mort).
bool Reserver(uint32 humainLow, Player* bot, bool compagnon, bool& nouvelle)
{
    nouvelle = false;
    uint32 const botLow = bot->GetGUID().GetCounter();
    uint64 const maintenant = MaintenantMs();
    std::lock_guard<std::mutex> verrou(gVerrou);
    auto it = gMorts.find(humainLow);
    if (it == gMorts.end())
        return false;
    Entree& e = it->second;
    if (e.clos || e.jcj || maintenant < e.prochainEssai || e.lancers >= LANCERS_MAX)
        return false;
    if (Renonce(e, botLow))
        return false;
    if (e.lanceur == botLow && e.reserveJusqua > maintenant)
        return true;   // garde l'echeance d'origine : une approche ne se prolonge pas
    if (e.lanceur && e.lanceur != botLow && e.reserveJusqua > maintenant && !(compagnon && e.lanceur != e.compagnon))
        return false;
    // Un bot ne tient qu'un corps : celui qu'il quitte pour un plus proche redevient libre tout de suite (sans Delai :
    // il n'a rien lance).
    for (auto& [autreLow, autre] : gMorts)
        if (autreLow != humainLow && autre.lanceur == botLow)
        {
            autre.lanceur = 0;
            autre.lanceurNom.clear();
            autre.reserveJusqua = 0;
        }
    e.lanceur = botLow;
    e.lanceurNom = bot->GetName();
    e.reserveJusqua = maintenant + (compagnon ? RESERVATION_COMPAGNON_MS : RESERVATION_MS);
    if (std::find(e.annonces.begin(), e.annonces.end(), botLow) == e.annonces.end())
    {
        e.annonces.push_back(botLow);
        nouvelle = true;
    }
    return true;
}

void ApresEssai(uint32 humainLow, Player* bot, uint32 sortId, bool lance)
{
    uint64 const maintenant = MaintenantMs();
    std::lock_guard<std::mutex> verrou(gVerrou);
    auto it = gMorts.find(humainLow);
    if (it == gMorts.end())
        return;
    Entree& e = it->second;
    e.lanceur = 0;
    e.lanceurNom.clear();
    e.reserveJusqua = 0;
    if (lance)
    {
        ++e.lancers;
        e.prochainEssai = maintenant + uint64(sPlayerbotAIConfig.coaReleverDelai) * 1000;
        e.dernierLanceur = bot->GetGUID().GetCounter();
        e.dernierLanceurNom = bot->GetName();
        e.dernierSort = sortId;
        e.dernierEssaiMs = maintenant;
    }
    else
    {
        // Echec persistant (ligne de vue vue autrement par le coeur, refus au prepare...) : ce bot y renonce apres
        // RATES_MAX_* echecs, au lieu de boucler toutes les 10 s.
        ++e.rates[bot->GetGUID().GetCounter()];
        e.prochainEssai = maintenant + std::min<uint64>(REESSAI_ECHEC_MS, uint64(sPlayerbotAIConfig.coaReleverDelai) * 1000);
    }
}
}  // namespace

namespace CoaRelever
{
void Mort(Player* humain)
{
    if (!sPlayerbotAIConfig.coaReleverEnable || !humain || !CoaCompagnons::EstHumain(humain))
        return;
    if (humain->InBattleground() || humain->InArena())
        return;
    Inscrire(humain);
}

void Releve(Player* humain)
{
    if (!humain || gNombre.load() == 0)
        return;
    uint32 const low = humain->GetGUID().GetCounter();
    uint64 const maintenant = MaintenantMs();
    std::lock_guard<std::mutex> verrou(gVerrou);
    auto it = gMorts.find(low);
    if (it == gMorts.end())
        return;
    Entree const& e = it->second;
    // Le coeur ne dit pas qui a leve l'humain (sort d'un bot, d'un joueur, gardien des ames) : un essai de bot de moins
    // de JOURNAL_RELEVE_MS est cite, c'est une SUPPOSITION que ce soit lui.
    if (e.dernierLanceur && maintenant - e.dernierEssaiMs < JOURNAL_RELEVE_MS)
        LOG_INFO("playerbots", "coa relever: bot={} humain={} sort={} distance=0 resultat=releve ({} s apres le lancer)",
                 e.dernierLanceurNom, e.nom, e.dernierSort, (maintenant - e.dernierEssaiMs) / 1000);
    gMorts.erase(it);
    gNombre.store(static_cast<uint32>(gMorts.size()));
}

void TueParJoueur(Player* tueur, Player* tue)
{
    if (!sPlayerbotAIConfig.coaReleverEnable || !tueur || !tue || tueur == tue || !CoaCompagnons::EstHumain(tue))
        return;
    uint32 const low = tue->GetGUID().GetCounter();
    uint64 const maintenant = MaintenantMs();
    std::lock_guard<std::mutex> verrou(gVerrou);
    auto it = gMorts.find(low);
    // Ordre normal : ce crochet (Unit::Kill) precede l'inscription (KillPlayer au tick suivant) ; par prudence, une
    // inscription toute fraiche est marquee directement.
    if (it != gMorts.end() && maintenant - it->second.mortMs < MARQUE_JCJ_MS)
        it->second.jcj = true;
    else
        gJcJ[low] = maintenant;
}

void HumainConnecte(Player* humain)
{
    if (!sPlayerbotAIConfig.coaReleverEnable || !humain || humain->IsAlive() || !CoaCompagnons::EstHumain(humain))
        return;
    Inscrire(humain);
}

void MettreAJour(uint32 diff)
{
    gCumulMs += diff;
    if (gCumulMs < 1000)
        return;
    gCumulMs = 0;
    if (gNombre.load() == 0)
    {
        // Purge des marques JcJ meme registre vide (un humain tue en champ de bataille n'est jamais inscrit).
        std::lock_guard<std::mutex> verrou(gVerrou);
        if (!gJcJ.empty())
        {
            uint64 const maintenant = MaintenantMs();
            for (auto it = gJcJ.begin(); it != gJcJ.end();)
                it = maintenant - it->second > MARQUE_JCJ_MS ? gJcJ.erase(it) : std::next(it);
        }
        return;
    }

    uint64 const maintenant = MaintenantMs();
    std::lock_guard<std::mutex> verrou(gVerrou);
    for (auto it = gMorts.begin(); it != gMorts.end();)
    {
        Entree& e = it->second;
        // Deconnecte (le pointeur n'est pas lu : l'humain est peut-etre sur une autre carte) ou trop vieux.
        if (!ObjectAccessor::FindConnectedPlayer(e.guid) || maintenant - e.mortMs > AGE_MAX_MS)
        {
            it = gMorts.erase(it);
            continue;
        }
        e.compagnon = CoaCompagnons::CompagnonEveille(it->first);
        e.compagnonConnu = true;
        ++it;
    }
    // Marques JcJ jamais consommees (mort en champ de bataille, option rallumee entre-temps...).
    for (auto it = gJcJ.begin(); it != gJcJ.end();)
        it = maintenant - it->second > MARQUE_JCJ_MS ? gJcJ.erase(it) : std::next(it);
    gNombre.store(static_cast<uint32>(gMorts.size()));
}
}  // namespace CoaRelever

void CoaReleverStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Juste sous la resurrection du pretre (ACTION_CRITICAL_HEAL + 10, PriestNonCombatStrategy.cpp:24-26).
    triggers.push_back(new TriggerNode("coa humain a relever", { NextAction("coa relever", ACTION_CRITICAL_HEAL + 9) }));
}

bool CoaHumainAReleverTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.coaReleverEnable || gNombre.load() == 0)
        return false;
    if (!BotDisponible(botAI, bot))
        return false;
    Choix choix;
    return Choisir(botAI, bot, choix);
}

bool CoaReleverAction::isUseful()
{
    return sPlayerbotAIConfig.coaReleverEnable && gNombre.load() != 0 && BotDisponible(botAI, bot);
}

bool CoaReleverAction::Execute(Event /*event*/)
{
    Choix choix;
    if (!Choisir(botAI, bot, choix))
        return false;

    Player* humain = choix.humain;
    uint32 const humainLow = humain->GetGUID().GetCounter();
    bool nouvelle = false;
    if (!Reserver(humainLow, bot, choix.compagnon, nouvelle))
        return false;

    WorldObject* lieu = choix.cadavre ? static_cast<WorldObject*>(choix.cadavre) : static_cast<WorldObject*>(humain);
    if (nouvelle)
        LOG_INFO("playerbots", "coa relever: bot={} humain={} sort={} distance={:.1f} resultat=approche{}{}",
                 bot->GetName(), humain->GetName(), choix.sort->Id, choix.distance,
                 choix.compagnon ? " (compagnon)" : "", choix.cadavre ? " (cadavre)" : "");

    // Approche : jusqu'a la distance de lancer, puis la ligne de vue (corps derriere un mur, au bas d'une pente).
    if (choix.distance > choix.approche + 1.0f)
        return MoveTo(lieu, std::max(1.0f, choix.approche - 1.0f));
    if (!bot->IsWithinLOSInMap(lieu))
        return MoveTo(lieu, 1.5f);
    if (bot->isMoving())
    {
        bot->StopMoving();
        return true;   // lancer au prochain passage (PlayerbotAI::CastSpell refuse un bot en mouvement)
    }
    // A terre (le vol est ecarte par BotDisponible) : demonter puis lancer dans le meme passage, sans laisser a
    // « check mount state » le temps de remonter.
    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    // Assis (repas, boisson) : PlayerbotAI::CastSpell se contenterait de le lever et d'echouer ; le lancer sur cadavre
    // n'a pas ce garde-fou.
    if (!bot->IsStandState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    bool lance = false;
    if (choix.cadavre)
    {
        // Fantome : le moteur des bots ne sait viser qu'une unite (PlayerbotAI::CastSpell) ; le coeur, lui, accepte
        // le cadavre et en deduit le proprietaire.
        bot->SetFacingToObject(choix.cadavre);
        SpellCastTargets cibles;
        cibles.SetCorpseTarget(choix.cadavre);
        lance = bot->CastSpell(cibles, choix.sort, nullptr, TRIGGERED_NONE) == SPELL_CAST_OK;
    }
    else
        lance = botAI->CastSpell(choix.sort->Id, humain);

    ApresEssai(humainLow, bot, choix.sort->Id, lance);
    LOG_INFO("playerbots", "coa relever: bot={} humain={} sort={} distance={:.1f} resultat={}{}{}", bot->GetName(),
             humain->GetName(), choix.sort->Id, bot->GetDistance(lieu), lance ? "lance" : "echec",
             choix.compagnon ? " (compagnon)" : "", choix.cadavre ? " (cadavre)" : "");
    return lance;
}
