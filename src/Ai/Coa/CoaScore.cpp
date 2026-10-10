/*
 * Conquest of Azeroth -- score d'efficacite des bots, lot L1. Voir CoaScore.h.
 */

#include "CoaScore.h"

#include "Config.h"
#include "Creature.h"
#include "DataMap.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "Unit.h"
#include "Ai/Class/Coa/CoaSpecialization.h"
#include "mod-ascension-compat/src/AscensionSpecialization.h"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace
{

char const* const CLE = "coa_score";

// Bornes de CoaRegistreCombat (2 s, 10 min) : une rencontre plus breve ne mesure rien et n'ecrit pas
// de ligne ; une rencontre de plus de 10 min (bot coince en combat) est fermee de force, `enlisee=1`.
constexpr uint32 DUREE_MIN_MS = 2000;
constexpr uint32 DUREE_MAX_MS = 10 * 60 * 1000;

// Les compteurs d'UNE rencontre, portes par le Player du bot. Touches seulement par le fil de sa carte.
struct CoaScoreRencontre : DataMap::Base
{
    bool ouverte = false;
    bool mort = false;
    uint32 debutMs = 0;
    uint32 spe = 0;
    uint8 role = 0;
    uint8 table = 0;
    bool tirage = false;
    uint8 niveau = 0;
    uint8 pvDebut = 100;
    uint8 pvMin = 100;
    uint64 degatsJoueur = 0;
    uint64 degatsCreature = 0;
    uint64 subis = 0;
    uint64 soins = 0;
    uint32 victimesCreature = 0;
    uint32 victimesJoueur = 0;
};

// Rencontres ouvertes, tous fils confondus : a zero, OnDamage et OnHeal sortent sur ce seul chargement.
std::atomic<uint32> ouvertes{0};
std::atomic<bool> actif{false};
std::atomic<int64> prochaineLecture{0};

int64 MaintenantMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Actif()
{
    // L'option est relue au plus une fois par seconde : relire la conf a chaque tick de chaque joueur
    // couterait une recherche de chaine par joueur et par tick. Course benigne entre fils : au pire
    // deux relectures dans la meme seconde. Horloge monotone : pas de debordement a gerer.
    int64 const maintenant = MaintenantMs();
    if (maintenant >= prochaineLecture.load(std::memory_order_relaxed))
    {
        prochaineLecture.store(maintenant + 1000, std::memory_order_relaxed);
        actif.store(sConfigMgr->GetOption<bool>("AiPlayerbot.Coa.Score.Enable", false, false),
                    std::memory_order_relaxed);
    }
    return actif.load(std::memory_order_relaxed);
}

// La rencontre ouverte du bot, ou nullptr. Le test de classe passe avant la recherche par chaine.
CoaScoreRencontre* RencontreOuverte(Player* p)
{
    if (!p || !IsAscensionCustomClassId(p->getClass()))
        return nullptr;
    CoaScoreRencontre* r = p->CustomData.Get<CoaScoreRencontre>(CLE);
    return r && r->ouverte ? r : nullptr;
}

void Ouvrir(Player* bot, CoaScoreRencontre& r)
{
    r = CoaScoreRencontre();
    r.ouverte = true;
    r.debutMs = getMSTime();
    r.niveau = uint8(bot->GetLevel());
    r.pvDebut = r.pvMin = uint8(bot->GetHealthPct());
    r.spe = GetAscensionActiveSpecialization(bot);
    r.role = uint8(GetCoaRole(bot));
    // Table EFFECTIVE de ce bot (sous l'experience BuildSource = 3, celle que son guid a tiree), et
    // si l'experience est en cours : ce sont les deux colonnes dont l'analyse a besoin.
    r.table = uint8(GetCoaBuildTable(bot));
    r.tirage = sConfigMgr->GetOption<uint32>("AiPlayerbot.Coa.BuildSource", 0, false) == 3;
    ouvertes.fetch_add(1, std::memory_order_relaxed);
}

void Fermer(Player* bot, CoaScoreRencontre& r)
{
    r.ouverte = false;
    ouvertes.fetch_sub(1, std::memory_order_relaxed);
    uint32 const duree = getMSTimeDiff(r.debutMs, getMSTime());
    if (duree < DUREE_MIN_MS)
        return;
    uint64 const guid = bot->GetGUID().GetRawValue();
    // Champs nommes, comme `coa rencontre:` : une colonne ajoutee demain ne casse pas la lecture.
    LOG_INFO("playerbots.coa",
             "coa score: guid={} classe={} spe={} role={} niveau={} table={} tirage={} mercenaire={} elite={} "
             "duree_ms={} degats_joueur={} degats_creature={} soins={} subis={} victimes_creature={} "
             "victimes_joueur={} pv_debut={} pv_min={} mort={} enlisee={}",
             guid, uint32(bot->getClass()), r.spe, uint32(r.role), uint32(r.niveau), uint32(r.table), r.tirage ? 1 : 0,
             sPlayerbotAIConfig.IsMercenary(guid) ? 1 : 0, sPlayerbotAIConfig.IsLayaElite(guid) ? 1 : 0,
             duree, r.degatsJoueur, r.degatsCreature, r.soins, r.subis,
             r.victimesCreature, r.victimesJoueur, uint32(r.pvDebut), uint32(r.pvMin), r.mort ? 1 : 0,
             duree >= DUREE_MAX_MS ? 1 : 0);
}

class coa_score_playerscript : public PlayerScript
{
public:
    coa_score_playerscript()
        : PlayerScript("coa_score_playerscript",
                       {PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_PVP_KILL,
                        PLAYERHOOK_ON_CREATURE_KILL, PLAYERHOOK_ON_CREATURE_KILLED_BY_PET, PLAYERHOOK_ON_LOGOUT})
    {
    }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        if (!player || !IsAscensionCustomClassId(player->getClass()))
            return;

        CoaScoreRencontre* r = player->CustomData.Get<CoaScoreRencontre>(CLE);
        bool const enCombat = player->IsInCombat() && player->IsAlive();
        if (r && r->ouverte)
        {
            if (enCombat && getMSTimeDiff(r->debutMs, getMSTime()) < DUREE_MAX_MS)
            {
                r->pvMin = std::min<uint8>(r->pvMin, uint8(player->GetHealthPct()));
                return;
            }
            // LA MORT SE LIT ICI, pas dans OnPlayerJustDied : le coeur appelle OnPlayerUpdate
            // (PlayerUpdates.cpp:315) AVANT KillPlayer (:324), seul appelant de OnPlayerJustDied
            // (Player.cpp:4822). Au tick qui suit la mort, la rencontre se ferme d'abord ; le crochet
            // de mort arriverait trop tard et `mort` vaudrait toujours 0 (revue du 10/10).
            if (!player->IsAlive())
            {
                r->mort = true;
                r->pvMin = 0;
            }
            Fermer(player, *r);           // sortie de combat, mort, ou rencontre enlisee
            return;
        }
        // Ouverture : seulement si l'option est armee, et seulement pour un bot aleatoire CoA.
        if (!enCombat || !Actif() || !sRandomPlayerbotMgr.IsRandomBot(player))
            return;
        if (!r)
            r = player->CustomData.GetDefault<CoaScoreRencontre>(CLE);
        Ouvrir(player, *r);
    }

    // Un bot deconnecte en plein combat ne repasserait jamais par OnPlayerUpdate : sans cette
    // fermeture, sa rencontre resterait ouverte et `ouvertes` ne redescendrait jamais. Appele par
    // WorldSession::LogoutPlayer, dans le fil monde, hors de toute mise a jour de carte.
    void OnPlayerLogout(Player* player) override
    {
        if (CoaScoreRencontre* r = RencontreOuverte(player))
            Fermer(player, *r);
    }

    void OnPlayerPVPKill(Player* killer, Player* /*killed*/) override
    {
        if (CoaScoreRencontre* r = RencontreOuverte(killer))
            ++r->victimesJoueur;
    }

    void OnPlayerCreatureKill(Player* killer, Creature* /*killed*/) override
    {
        if (CoaScoreRencontre* r = RencontreOuverte(killer))
            ++r->victimesCreature;
    }

    // Le coeur appelle ce crochet avec victim->ToCreature() (Unit.cpp:14837) : nullptr quand le
    // familier a tue un JOUEUR. Ce cas n'est pas compte (limite connue : les victimes JcJ d'un
    // familier manquent a `victimes_joueur`).
    void OnPlayerCreatureKilledByPet(Player* owner, Creature* killed) override
    {
        if (!killed)
            return;
        if (CoaScoreRencontre* r = RencontreOuverte(owner))
            ++r->victimesCreature;
    }
};

class coa_score_unitscript : public UnitScript
{
public:
    coa_score_unitscript() : UnitScript("coa_score_unitscript", true, {UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL}) {}

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        // Le cas commun -- score eteint, ou aucun bot en rencontre -- sort ici.
        if (!damage || !ouvertes.load(std::memory_order_relaxed))
            return;

        if (attacker && attacker != victim)
            if (CoaScoreRencontre* r = RencontreOuverte(attacker->GetCharmerOrOwnerPlayerOrPlayerItself()))
            {
                // Familier et totem comptent pour leur maitre. Une victime controlee par un joueur
                // (joueur, familier de joueur) est un combat JcJ.
                if (victim && victim->GetCharmerOrOwnerPlayerOrPlayerItself())
                    r->degatsJoueur += damage;
                else
                    r->degatsCreature += damage;
            }

        if (victim && victim->IsPlayer())
            if (CoaScoreRencontre* r = RencontreOuverte(victim->ToPlayer()))
                r->subis += damage;
    }

    void OnHeal(Unit* healer, Unit* /*receiver*/, uint32& gain) override
    {
        // `gain` est le soin EFFECTIF : DealHeal l'a deja applique par ModifyHealth (Unit.cpp:8469).
        if (!gain || !healer || !ouvertes.load(std::memory_order_relaxed))
            return;
        if (CoaScoreRencontre* r = RencontreOuverte(healer->GetCharmerOrOwnerPlayerOrPlayerItself()))
            r->soins += gain;
    }
};

}  // namespace

void AddSC_coa_score()
{
    new coa_score_playerscript();
    new coa_score_unitscript();
}
