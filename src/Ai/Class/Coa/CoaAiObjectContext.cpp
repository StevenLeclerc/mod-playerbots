/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaAiObjectContext.h"

#include "Action.h"
#include "CoaSpecialization.h"
#include "CombatStrategy.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "NamedObjectContext.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Strategy.h"
#include "Trigger.h"
// Data only (constexpr arrays).
#include "mod-ascension-compat/src/AscensionCoATalentData.h"
#include "mod-ascension-compat/src/AscensionCustomClassData.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <ctime>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

// What an ability does, read from its effects. An ability can do several of these.
enum AbilityKind : uint16
{
    KIND_HEAL       = 0x0001,  // heals an ally
    KIND_TAUNT      = 0x0002,
    KIND_AOE        = 0x0004,  // hits enemies in an area
    KIND_BUFF       = 0x0008,  // long positive aura on the caster or allies
    KIND_DEFENSIVE  = 0x0010,  // short damage reduction, absorb or avoidance on the caster
    KIND_HOSTILE    = 0x0020,  // aimed at an enemy
    KIND_DAMAGE     = 0x0040,
    KIND_GROUP_HEAL = 0x0080,  // heals allies in an area
    KIND_HOT        = 0x0100,  // heal over time
    KIND_ALLY_CAST  = 0x0200,  // can be cast on another group member
    KIND_DISPEL     = 0x0400,  // removes harmful auras from allies
    KIND_INTERRUPT  = 0x0800,
    KIND_CONTROL    = 0x1000   // stuns, fears, polymorphs... never aimed at a group member
};

/*
 * Class abilities, indexed by class id, ordered by required level.
 *
 * Most CoA abilities are scripted (dummy or script effects), so the class ability table
 * and the specialization data are the only reliable lists of what a class can cast.
 */
struct CoaAbility
{
    uint32 spellId;
    uint8 requiredLevel;
    uint16 kind;
    uint32 dispelMask;
};

struct ClassKit
{
    std::vector<CoaAbility> abilities;
    uint16 kinds = 0;  // every kind some ability of the class has
};

// Ally targets: pet, party and raid areas, ally or any unit, chain heal.
bool IsAllyTarget(uint32 target)
{
    switch (target)
    {
        case 5: case 20: case 21: case 25: case 30: case 31: case 33:
        case 34: case 35: case 37: case 45: case 56: case 57: case 61:
            return true;
        default:
            return false;
    }
}

bool IsAllyAreaTarget(uint32 target)
{
    switch (target)
    {
        case 20: case 30: case 31: case 33: case 34: case 37: case 45: case 56: case 61:
            return true;
        default:
            return false;
    }
}

// Aimed at one chosen ally, who may be someone else than the caster.
bool IsAllyCastTarget(uint32 target)
{
    return target == TARGET_UNIT_TARGET_ALLY || target == TARGET_UNIT_TARGET_ANY || target == 35 ||
           target == TARGET_UNIT_TARGET_CHAINHEAL_ALLY || target == TARGET_UNIT_TARGET_RAID;
}

bool IsEnemyAreaTarget(uint32 target)
{
    switch (target)
    {
        case TARGET_UNIT_SRC_AREA_ENEMY: case TARGET_UNIT_DEST_AREA_ENEMY: case TARGET_UNIT_CONE_ENEMY_24:
        case TARGET_DEST_DYNOBJ_ENEMY: case TARGET_UNIT_CONE_ENEMY_54: case TARGET_UNIT_CONE_ENEMY_104:
            return true;
        default:
            return false;
    }
}

bool IsEnemyTarget(uint32 target)
{
    return target == TARGET_UNIT_TARGET_ENEMY || target == TARGET_DEST_TARGET_ENEMY || IsEnemyAreaTarget(target);
}

bool IsAuraEffect(SpellEffectInfo const& effect)
{
    return effect.Effect == SPELL_EFFECT_APPLY_AURA || effect.Effect == SPELL_EFFECT_APPLY_AREA_AURA_PARTY ||
           effect.Effect == SPELL_EFFECT_APPLY_AREA_AURA_RAID;
}

bool IsDamage(SpellEffectInfo const& effect)
{
    switch (effect.Effect)
    {
        case SPELL_EFFECT_SCHOOL_DAMAGE: case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL: case SPELL_EFFECT_WEAPON_DAMAGE:
        case SPELL_EFFECT_NORMALIZED_WEAPON_DMG: case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
        case SPELL_EFFECT_HEALTH_LEECH: case SPELL_EFFECT_POWER_BURN:
            return true;
        default:
            break;
    }

    return IsAuraEffect(effect) &&
           (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE || effect.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH ||
            effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE_PERCENT);
}

bool IsControlAura(SpellEffectInfo const& effect)
{
    switch (effect.ApplyAuraName)
    {
        case SPELL_AURA_MOD_CONFUSE: case SPELL_AURA_MOD_FEAR: case SPELL_AURA_MOD_STUN: case SPELL_AURA_MOD_ROOT:
        case SPELL_AURA_MOD_SILENCE: case SPELL_AURA_MOD_PACIFY: case SPELL_AURA_TRANSFORM:
        case SPELL_AURA_MOD_PACIFY_SILENCE:
            return true;
        default:
            return false;
    }
}

bool IsDefensiveAura(SpellEffectInfo const& effect)
{
    switch (effect.ApplyAuraName)
    {
        case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
            return effect.BasePoints < 0;
        case SPELL_AURA_SCHOOL_ABSORB: case SPELL_AURA_MOD_PARRY_PERCENT: case SPELL_AURA_MOD_DODGE_PERCENT:
        case SPELL_AURA_MOD_BLOCK_PERCENT: case SPELL_AURA_MOD_INCREASE_HEALTH:
        case SPELL_AURA_MOD_INCREASE_HEALTH_PERCENT: case SPELL_AURA_SCHOOL_IMMUNITY:
            return true;
        default:
            return false;
    }
}

// What a spell does, looking two levels into the spells it triggers: CoA abilities often
// carry their heal, taunt or aura in a triggered spell.
void Classify(SpellInfo const* info, CoaAbility& ability, uint8 depth = 0)
{
    constexpr int32 LongAura = 5 * MINUTE * IN_MILLISECONDS;
    int32 const duration = info->GetMaxDuration();

    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (!effect.IsEffect())
            continue;

        uint32 const targetA = effect.TargetA.GetTarget();
        uint32 const targetB = effect.TargetB.GetTarget();
        bool const aura = IsAuraEffect(effect);
        bool const self = targetA == TARGET_UNIT_CASTER;
        bool const ally = IsAllyTarget(targetA) || IsAllyTarget(targetB);
        bool const enemy = IsEnemyTarget(targetA) || IsEnemyTarget(targetB);

        if (IsAllyCastTarget(targetA))
            ability.kind |= KIND_ALLY_CAST;

        if (enemy)
        {
            ability.kind |= KIND_HOSTILE;
            if (IsEnemyAreaTarget(targetA) || IsEnemyAreaTarget(targetB))
                ability.kind |= KIND_AOE;
        }

        if (IsDamage(effect))
            ability.kind |= KIND_DAMAGE;

        // Some CoA crowd control also cleanses (Babify, Knockout): a bot must not "dispel" a
        // group member by stunning or transforming it.
        if (aura && !self && IsControlAura(effect))
            ability.kind |= KIND_CONTROL;

        bool const periodicHeal = aura && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL;
        bool const heal = effect.Effect == SPELL_EFFECT_HEAL || effect.Effect == SPELL_EFFECT_HEAL_PCT ||
                          effect.Effect == SPELL_EFFECT_HEAL_MAX_HEALTH || periodicHeal;
        if (heal && ally)
        {
            ability.kind |= KIND_HEAL;
            if (IsAllyAreaTarget(targetA) || IsAllyAreaTarget(targetB))
                ability.kind |= KIND_GROUP_HEAL;
            if (periodicHeal)
                ability.kind |= KIND_HOT;
        }

        if (effect.Effect == SPELL_EFFECT_ATTACK_ME || (aura && effect.ApplyAuraName == SPELL_AURA_MOD_TAUNT))
            ability.kind |= KIND_TAUNT;

        if (effect.Effect == SPELL_EFFECT_DISPEL && (ally || self))
        {
            ability.kind |= KIND_DISPEL;
            ability.dispelMask |= SpellInfo::GetDispelMask(DispelType(effect.MiscValue));
        }

        if (effect.Effect == SPELL_EFFECT_INTERRUPT_CAST ||
            (aura && enemy && effect.ApplyAuraName == SPELL_AURA_MOD_SILENCE))
            ability.kind |= KIND_INTERRUPT;

        if (aura && self && IsDefensiveAura(effect) && duration > 0 && duration < LongAura)
            ability.kind |= KIND_DEFENSIVE;

        // Stances and forms (no duration) are left out: two of them would take turns forever.
        if (aura && (self || ally) && !heal && info->IsPositive() && duration >= LongAura &&
            effect.ApplyAuraName != SPELL_AURA_MOD_SHAPESHIFT)
            ability.kind |= KIND_BUFF;

        if (effect.TriggerSpell && depth < 2)
            if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                Classify(triggered, ability, depth + 1);
    }
}

std::unordered_map<uint8, ClassKit> const& ClassAbilities()
{
    // Magic static: loaded once, thread safe, on the first bot that needs it.
    static std::unordered_map<uint8, ClassKit> const abilities = []
    {
        std::unordered_map<uint8, ClassKit> byClass;
        std::unordered_map<uint8, std::unordered_set<uint32>> known;

        auto add = [&byClass, &known](uint8 classId, uint32 spellId, uint8 requiredLevel)
        {
            if (!known[classId].insert(spellId).second)
                return false;

            CoaAbility ability = { spellId, requiredLevel, 0, 0 };
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                Classify(info, ability);

            ClassKit& kit = byClass[classId];
            kit.abilities.push_back(ability);
            kit.kinds |= ability.kind;
            return true;
        };

        // Flags 8706 and 10754 mark passives (weapon skills, ratings, Auto Shot).
        QueryResult result = WorldDatabase.Query(
            "SELECT `class`, `spell_id`, `required_level` FROM `ascension_custom_class_spell` "
            "WHERE `flags` NOT IN (8706, 10754) ORDER BY `class`, `required_level`");

        uint32 count = 0;
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                count += add(fields[0].Get<uint8>(), fields[1].Get<uint32>(), fields[2].Get<uint8>());
            } while (result->NextRow());
        }

        // The table still lists the previous class layout for several classes (13, 15, 19,
        // 22, 27, 31 share no id with what players are actually granted). The ids
        // mod-ascension-compat really teaches are the authority, so merge them in.
        // Ids below 100000 are vanilla weapon skills and Auto Attack.
        uint32 merged = 0;
        for (AscensionCompatData::ClassSpell const& grant : AscensionCompatData::ClassSpells)
            if (grant.SpellId >= 100000)
                merged += add(grant.ClassId, grant.SpellId, grant.RequiredLevel);

        // Specialization abilities: a bot only knows those of its own specialization, which
        // HasSpell sorts out at run time.
        uint32 specialization = 0;
        for (AscensionCompatData::CoATalentEntry const& entry : AscensionCompatData::CoATalentEntries)
            for (uint32 spellId : entry.SpellIds)
                if (spellId)
                    specialization += add(entry.ClassId, spellId, entry.RequiredLevel);

        uint32 heals = 0, aoe = 0, buffs = 0, defensives = 0, dispels = 0, interrupts = 0;
        for (auto& [classId, kit] : byClass)
        {
            std::stable_sort(kit.abilities.begin(), kit.abilities.end(),
                [](CoaAbility const& a, CoaAbility const& b) { return a.requiredLevel < b.requiredLevel; });

            for (CoaAbility const& ability : kit.abilities)
            {
                heals += (ability.kind & KIND_HEAL) != 0;
                aoe += (ability.kind & KIND_AOE) != 0;
                buffs += (ability.kind & KIND_BUFF) != 0;
                defensives += (ability.kind & KIND_DEFENSIVE) != 0;
                dispels += (ability.kind & KIND_DISPEL) != 0;
                interrupts += (ability.kind & KIND_INTERRUPT) != 0;
            }
        }

        LOG_INFO("playerbots", "coa: {} abilities from the table, {} granted by mod-ascension-compat, "
                 "{} from specializations, {} classes", count, merged, specialization, byClass.size());
        LOG_INFO("playerbots", "coa: {} heals, {} area attacks, {} buffs, {} defensives, {} dispels, {} interrupts",
                 heals, aoe, buffs, defensives, dispels, interrupts);
        return byClass;
    }();

    return abilities;
}

// Whether the bot's class has any ability of that kind: a cheap test before looking at spells.
bool ClassHas(Player* bot, uint16 kind)
{
    auto const& all = ClassAbilities();
    auto const found = all.find(bot->getClass());
    return found != all.end() && (found->second.kinds & kind);
}

struct Usable
{
    SpellInfo const* info;
    uint16 kind;
    uint32 dispelMask;
};

// Active abilities the bot has reached and actually knows, for which `wanted(kind)` is true.
template <typename Filter>
std::vector<Usable> KnownAbilities(Player* bot, Filter wanted)
{
    std::vector<Usable> usable;

    auto const& all = ClassAbilities();
    auto const found = all.find(bot->getClass());
    if (found == all.end())
        return usable;

    for (CoaAbility const& ability : found->second.abilities)
    {
        if (!wanted(ability.kind) || ability.requiredLevel > bot->GetLevel() || !bot->HasSpell(ability.spellId))
            continue;

        // Merged grants include passives (e.g. 552011 Resilient Constitution).
        SpellInfo const* info = sSpellMgr->GetSpellInfo(ability.spellId);
        if (info && !info->IsPassive())
            usable.push_back({ info, ability.kind, ability.dispelMask });
    }

    return usable;
}

bool IsFriendlyDispel(uint16 kind)
{
    return (kind & KIND_DISPEL) && !(kind & (KIND_CONTROL | KIND_HOSTILE));
}

// Whether the bot knows an ability for which `wanted(kind)` is true and that is off cooldown:
// actions only run when they have something to cast, which keeps the usage counters honest.
template <typename Filter>
bool HasReadyAbility(PlayerbotAI* botAI, Player* bot, Filter wanted)
{
    auto const& benched = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->benchedSpells;
    time_t const now = time(nullptr);

    for (Usable const& spell : KnownAbilities(bot, wanted))
    {
        auto const bench = benched.find(spell.info->Id);
        if (!bot->HasSpellCooldown(spell.info->Id) && (bench == benched.end() || bench->second <= now))
            return true;
    }

    return false;
}

// Abilities worth using in the damage rotation.
bool IsAttack(uint16 kind, bool tank)
{
    // Scripted abilities carry nothing to go by: try them.
    if (!kind)
        return true;

    // Heals, buffs, defensives and dispels have their own actions.
    if (!(kind & (KIND_HOSTILE | KIND_DAMAGE)))
        return false;

    if (kind & KIND_DAMAGE)
        return true;

    // A taunt that deals no damage pulls aggro off the tank: only tanks use it. Interrupts
    // are kept for enemy casts.
    if (kind & KIND_TAUNT)
        return tank;

    return !(kind & KIND_INTERRUPT);
}

/*
 * The check CastSpell will actually face.
 *
 * PlayerbotAI::CanCastSpell checks with TRIGGERED_IGNORE_POWER_AND_REAGENT_COST and
 * accepts OUT_OF_RANGE, MOVING and NOT_INFRONT as success, whereas CastSpell prepares
 * with TRIGGERED_NONE. A spell can therefore pass the first and be refused by the second.
 */
SpellCastResult StrictCheck(Player* bot, SpellInfo const* info, Unit* target)
{
    ObjectGuid const oldSel = bot->GetTarget();

    Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
    spell->m_targets.SetUnitTarget(target);
    // Ground-targeted spells: CastSpell aims them at the target's position.
    if (info->Targets & TARGET_FLAG_DEST_LOCATION)
        spell->m_targets.SetDst(*target);
    SpellCastResult const result = spell->CheckCast(true);
    delete spell;

    bot->SetSelection(oldSel);
    return result;
}

constexpr uint16 FAILURE_REFUSED = 1000;  // passed the strict check, refused by PlayerbotAI::CastSpell
constexpr uint16 FAILURE_NOTHING = 1001;  // nothing left to cast (e.g. every heal over time already on the target)
constexpr uint16 FAILURE_MOVING = 1002;   // cast time while moving: the bot stops and casts on a later tick
constexpr uint16 FAILURE_SITTING = 1003;  // refused while sitting (eating, drinking): the bot stands up first
constexpr uint16 FAILURE_CASTING = 1004;  // refused while still casting a heal or buff

// A bot busy casting an attack drops it for a heal, dispel, defensive, taunt or interrupt. A heal
// or buff in progress is kept, or heals would keep cutting each other off.
void DropAttackCast(Player* bot)
{
    for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
        if (Spell* current = bot->GetCurrentSpell(type))
            if (!current->GetSpellInfo()->IsPositive())
                bot->InterruptSpell(type, false);
}
constexpr time_t SpellBenchSeconds = 20;
constexpr time_t RefusedBenchSeconds = 8;

// Failures that will not clear up on the next tick: wrong target or state for this spell.
bool IsLastingFailure(SpellCastResult result)
{
    switch (result)
    {
        case SPELL_FAILED_BAD_IMPLICIT_TARGETS: case SPELL_FAILED_BAD_TARGETS: case SPELL_FAILED_CASTER_AURASTATE:
        case SPELL_FAILED_NOT_SHAPESHIFT: case SPELL_FAILED_ONLY_SHAPESHIFT: case SPELL_FAILED_TARGET_AURASTATE:
        case SPELL_FAILED_EQUIPPED_ITEM_CLASS: case SPELL_FAILED_REAGENTS: case SPELL_FAILED_TOTEMS:
            return true;
        default:
            return false;
    }
}
void RecordFailure(uint8 kind, uint32 spellId, uint16 reason);

// Casts the first ability of the list that passes the strict check on the target. Returns it,
// or nullptr when none went off. With a usage kind, why each ability failed is counted.
SpellInfo const* CastFirst(PlayerbotAI* botAI, Player* bot, std::vector<Usable> const& spells, Unit* target,
                           uint8 usage = 255)
{
    time_t const now = time(nullptr);
    auto& benched = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->benchedSpells;

    for (Usable const& spell : spells)
    {
        // A spell on cooldown would only fail with SPELL_FAILED_NOT_READY.
        if (bot->HasSpellCooldown(spell.info->Id))
            continue;

        // The global cooldown blocks every spell alike: try again on a later tick.
        if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(spell.info))
            return nullptr;

        auto const bench = benched.find(spell.info->Id);
        if (bench != benched.end())
        {
            if (bench->second > now)
                continue;
            benched.erase(bench);
        }

        SpellCastResult const check = StrictCheck(bot, spell.info, target);
        if (check == SPELL_CAST_OK)
        {
            // PlayerbotAI::CastSpell refuses a spell with a cast time while the bot moves: stop
            // now and cast it on a later tick, once standing still.
            if (bot->isMoving() && spell.info->CalcCastTime(bot))
            {
                bot->StopMoving();
                if (usage != 255)
                    RecordFailure(usage, spell.info->Id, FAILURE_MOVING);
                continue;
            }

            if (usage != 255)
                DropAttackCast(bot);

            bool const sitting = !bot->IsStandState();
            bool const casting = bot->IsNonMeleeSpellCast(false, true, true);
            if (botAI->CastSpell(spell.info->Id, target))
                return spell.info;

            // Refused for a reason the check cannot see (CoA spell scripts check their own
            // resources when the cast is prepared): leave it aside briefly so the next ability
            // of the list gets its turn instead of this one failing every tick.
            if (!sitting && !casting)
                benched[spell.info->Id] = now + RefusedBenchSeconds;

            if (usage != 255)
                RecordFailure(usage, spell.info->Id, sitting ? FAILURE_SITTING : casting ? FAILURE_CASTING : FAILURE_REFUSED);
            continue;
        }
        else if (IsLastingFailure(check))
            benched[spell.info->Id] = now + SpellBenchSeconds;

        if (usage != 255)
            RecordFailure(usage, spell.info->Id, uint16(check));
    }

    return nullptr;
}

/*
 * Usage counters of the CoA actions, for all bots, written every 10 minutes to the
 * "playerbots.coa" logger (CoaBots.log): how often each action was tried (its trigger fired)
 * and how often it actually cast something, with the most cast interrupts and dispels.
 */
enum UsageKind : uint8
{
    USAGE_ATTACK, USAGE_AOE, USAGE_HEAL, USAGE_GROUP_HEAL, USAGE_HOT, USAGE_TAUNT,
    USAGE_DEFENSIVE, USAGE_DISPEL, USAGE_INTERRUPT, USAGE_BUFF, USAGE_MAX
};

constexpr char const* UsageNames[USAGE_MAX] =
{
    "attack", "aoe", "heal", "group heal", "hot", "taunt", "defensive", "dispel", "interrupt", "buff"
};

struct UsageCounter
{
    std::atomic<uint64> tried{ 0 };
    std::atomic<uint64> cast{ 0 };
};

std::array<UsageCounter, USAGE_MAX> Usage;
std::mutex UsageSpellsLock;
std::map<uint32, uint32> UsageSpells[USAGE_MAX];  // spell id -> casts, dispels and interrupts only
std::atomic<time_t> UsageLastReport{ 0 };
// (spell id, reason) -> failures, for the kinds whose casts are diagnosed
std::map<std::pair<uint32, uint16>, uint32> UsageFailures[USAGE_MAX];

void RecordFailure(uint8 kind, uint32 spellId, uint16 reason)
{
    if (kind >= USAGE_MAX)
        return;

    std::lock_guard<std::mutex> guard(UsageSpellsLock);
    ++UsageFailures[kind][{ spellId, reason }];
}

void ReportUsage(time_t now)
{
    std::string line;
    for (uint8 kind = 0; kind < USAGE_MAX; ++kind)
        line += Acore::StringFormat("{}{} {}/{}", kind ? ", " : "", UsageNames[kind],
                                    Usage[kind].cast.load(), Usage[kind].tried.load());
    LOG_INFO("playerbots.coa", "coa usage since start (cast/tried): {}", line);

    std::lock_guard<std::mutex> guard(UsageSpellsLock);
    for (UsageKind kind : { USAGE_DISPEL, USAGE_INTERRUPT })
    {
        std::vector<std::pair<uint32, uint32>> top(UsageSpells[kind].begin(), UsageSpells[kind].end());
        std::sort(top.begin(), top.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        std::string spells;
        for (size_t i = 0; i < top.size() && i < 15; ++i)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(top[i].first);
            spells += Acore::StringFormat("{}{} ({}) x{}", i ? ", " : "", info ? info->SpellName[0] : "?",
                                          top[i].first, top[i].second);
        }
        LOG_INFO("playerbots.coa", "coa {} spells: {}", UsageNames[kind], spells.empty() ? "none yet" : spells);
    }

    // Why heals and taunts fail: spell (id) reason xcount. Reasons are SpellCastResult values,
    // "refused" when CastSpell turned down a spell the check accepted, "nothing" when no
    // ability was left to try.
    for (UsageKind kind : { USAGE_HEAL, USAGE_GROUP_HEAL, USAGE_HOT, USAGE_TAUNT, USAGE_DISPEL })
    {
        std::vector<std::pair<std::pair<uint32, uint16>, uint32>> top(UsageFailures[kind].begin(), UsageFailures[kind].end());
        if (top.empty())
            continue;

        std::sort(top.begin(), top.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        std::string failures;
        for (size_t i = 0; i < top.size() && i < 10; ++i)
        {
            auto const [spellId, reason] = top[i].first;
            SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            std::string const why = reason == FAILURE_REFUSED ? "refused"
                                  : reason == FAILURE_NOTHING ? "nothing"
                                  : reason == FAILURE_MOVING ? "moving"
                                  : reason == FAILURE_SITTING ? "sitting"
                                  : reason == FAILURE_CASTING ? "casting"
                                  : std::to_string(reason);
            failures += Acore::StringFormat("{}{} ({}) {} x{}", i ? ", " : "", info ? info->SpellName[0] : "-",
                                            spellId, why, top[i].second);
        }
        LOG_INFO("playerbots.coa", "coa {} failures: {}", UsageNames[kind], failures);
    }
    (void)now;
}

// Counts one run of an action and, when `spell` is set, the cast it made.
SpellInfo const* RecordUsage(UsageKind kind, SpellInfo const* spell)
{
    ++Usage[kind].tried;
    if (spell)
    {
        ++Usage[kind].cast;
        if (kind == USAGE_DISPEL || kind == USAGE_INTERRUPT)
        {
            std::lock_guard<std::mutex> guard(UsageSpellsLock);
            ++UsageSpells[kind][spell->Id];
        }
    }

    time_t const now = time(nullptr);
    time_t last = UsageLastReport.load();
    if (!last)
        UsageLastReport.compare_exchange_strong(last, now);
    else if (now - last >= 10 * MINUTE && UsageLastReport.compare_exchange_strong(last, now))
        ReportUsage(now);

    return spell;
}

// The bot and the living group members near it, the bot first.
std::vector<Player*> NearbyGroup(Player* bot)
{
    std::vector<Player*> members = { bot };
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsInWorld() && member->IsAlive() &&
                member->GetMapId() == bot->GetMapId() && bot->IsWithinDistInMap(member, 40.0f))
                members.push_back(member);
        }

    return members;
}

// Whether the unit carries a harmful aura one of the dispel types in `mask` removes.
bool HasDispellable(Unit* unit, uint32 mask)
{
    for (auto const& [auraId, application] : unit->GetAppliedAuras())
    {
        if (application->IsPositive())
            continue;

        SpellInfo const* auraInfo = application->GetBase()->GetSpellInfo();
        if (auraInfo->Dispel && (mask & (1 << auraInfo->Dispel)))
            return true;
    }

    return false;
}

bool IsInterruptibleCast(Unit* unit)
{
    if (Spell* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        if (spell->getState() == SPELL_STATE_PREPARING &&
            (spell->GetSpellInfo()->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT))
            return true;

    if (Spell* spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        if (spell->getState() == SPELL_STATE_CASTING &&
            (spell->GetSpellInfo()->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT))
            return true;

    return false;
}

// The enemy to interrupt: the current target when it casts, else an attacker that does.
Unit* FindCaster(PlayerbotAI* botAI, Player* bot)
{
    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (target && target->IsAlive() && IsInterruptibleCast(target))
        return target;

    for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* attacker = botAI->GetUnit(guid);
        if (attacker && attacker->IsAlive() && bot->IsWithinDistInMap(attacker, 30.0f) && IsInterruptibleCast(attacker))
            return attacker;
    }

    return nullptr;
}

class CoaAttackAction : public Action
{
public:
    CoaAttackAction(PlayerbotAI* botAI) : Action(botAI, "coa attack") {}

    bool Execute(Event /*event*/) override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (!target || !target->IsAlive())
            return false;

        // Playerbots only starts the melee swing for melee bots. A ranged CoA bot caught in melee
        // (a level 1 Ranger: its shot has a minimum range) would then stand there doing nothing and
        // die, so swing the weapon while the target is in reach; spells still go first when they can.
        if (bot->IsWithinMeleeRange(target) && (bot->GetVictim() != target || !bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING)))
            bot->Attack(target, true);

        bool const tank = GetCoaRole(bot) == CoaRole::Tank;
        std::vector<Usable> const usable = KnownAbilities(bot, [tank](uint16 kind) { return IsAttack(kind, tank); });
        if (usable.empty())
            return false;

        time_t const now = time(nullptr);

        // Rotate through the abilities, starting after the last one that went off, so a
        // bot uses its whole kit instead of spamming the first ability that works.
        for (size_t i = 0; i < usable.size(); ++i)
        {
            size_t const index = (next + i) % usable.size();
            SpellInfo const* info = usable[index].info;

            Strikes& strikes = failures[info->Id];
            if (strikes.benchedUntil > now)
                continue;

            if (StrictCheck(bot, info, target) != SPELL_CAST_OK)
                continue;

            if (botAI->CastSpell(info->Id, target))
            {
                strikes.count = 0;
                next = index + 1;
                RecordUsage(USAGE_ATTACK, info);
                return true;
            }

            // Passed the strict check yet refused: something we cannot see from here.
            // Stop insisting for a while rather than burning every tick on it.
            if (++strikes.count >= MaxStrikes)
            {
                strikes.count = 0;
                strikes.benchedUntil = now + BenchSeconds;
            }
        }

        RecordUsage(USAGE_ATTACK, nullptr);
        return false;
    }

    bool isUseful() override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        return target && target->IsAlive();
    }

private:
    static constexpr uint8 MaxStrikes = 3;
    static constexpr time_t BenchSeconds = 60;

    struct Strikes
    {
        uint8 count = 0;
        time_t benchedUntil = 0;
    };

    size_t next = 0;
    std::unordered_map<uint32, Strikes> failures;
};

// Area attacks on the current target, when several enemies are around.
class CoaAoeAction : public Action
{
public:
    CoaAoeAction(PlayerbotAI* botAI) : Action(botAI, "coa aoe") {}

    bool Execute(Event /*event*/) override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (!target || !target->IsAlive())
            return false;

        return RecordUsage(USAGE_AOE, CastFirst(botAI, bot, KnownAbilities(bot, [](uint16 kind)
            { return (kind & KIND_AOE) && (kind & (KIND_DAMAGE | KIND_HOSTILE)); }), target));
    }

    bool isUseful() override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        return target && target->IsAlive() && ClassHas(bot, KIND_AOE) &&
               HasReadyAbility(botAI, bot,[](uint16 kind) { return (kind & KIND_AOE) && (kind & (KIND_DAMAGE | KIND_HOSTILE)); });
    }
};

// Heals the most wounded party member (the bot included): a direct heal, an area heal, or a
// heal over time it does not already carry from this bot.
class CoaHealAction : public Action
{
public:
    enum class Mode : uint8
    {
        Direct,
        Group,
        OverTime
    };

    CoaHealAction(PlayerbotAI* botAI, std::string const name, Mode mode) : Action(botAI, name), mode(mode) {}

    bool Execute(Event /*event*/) override
    {
        Unit* target = AI_VALUE(Unit*, "party member to heal");
        if (!target || !target->IsAlive())
            return false;

        std::vector<Usable> spells;
        switch (mode)
        {
            case Mode::Group:
                spells = KnownAbilities(bot, [](uint16 kind) { return (kind & KIND_GROUP_HEAL) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); });
                break;
            case Mode::OverTime:
            {
                spells = KnownAbilities(bot, [](uint16 kind) { return (kind & KIND_HOT) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); });
                ObjectGuid const caster = bot->GetGUID();
                spells.erase(std::remove_if(spells.begin(), spells.end(),
                    [target, caster](Usable const& spell) { return target->HasAura(spell.info->Id, caster); }),
                    spells.end());
                break;
            }
            default:
                // Single target heals first, direct ones before those over time; area heals last.
                spells = KnownAbilities(bot, [](uint16 kind) { return (kind & KIND_HEAL) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); });
                std::stable_sort(spells.begin(), spells.end(), [](Usable const& a, Usable const& b)
                {
                    auto rank = [](uint16 kind) { return ((kind & KIND_GROUP_HEAL) ? 2 : 0) + ((kind & KIND_HOT) ? 1 : 0); };
                    return rank(a.kind) < rank(b.kind);
                });
                break;
        }

        UsageKind const usage = mode == Mode::Group ? USAGE_GROUP_HEAL : mode == Mode::OverTime ? USAGE_HOT : USAGE_HEAL;
        if (spells.empty())
            RecordFailure(usage, 0, FAILURE_NOTHING);

        return RecordUsage(usage, CastFirst(botAI, bot, spells, target, usage));
    }

    bool isUseful() override
    {
        Unit* target = AI_VALUE(Unit*, "party member to heal");
        uint16 const wanted = mode == Mode::Group ? KIND_GROUP_HEAL : mode == Mode::OverTime ? KIND_HOT : KIND_HEAL;
        return target && target->IsAlive() && target->GetHealthPct() < sPlayerbotAIConfig.almostFullHealth &&
               ClassHas(bot, wanted) &&
               HasReadyAbility(botAI, bot,[wanted](uint16 kind) { return (kind & wanted) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); });
    }

private:
    Mode mode;
};

// Takes the current target back when it attacks someone else.
class CoaTauntAction : public Action
{
public:
    CoaTauntAction(PlayerbotAI* botAI) : Action(botAI, "coa taunt") {}

    bool Execute(Event /*event*/) override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (!target || !target->IsAlive())
            return false;

        return RecordUsage(USAGE_TAUNT, CastFirst(botAI, bot,
            KnownAbilities(bot, [](uint16 kind) { return (kind & KIND_TAUNT) != 0; }), target, USAGE_TAUNT));
    }

    bool isUseful() override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        return target && target->IsAlive() && target->GetVictim() && target->GetVictim() != bot &&
               ClassHas(bot, KIND_TAUNT) && HasReadyAbility(botAI, bot,[](uint16 kind) { return (kind & KIND_TAUNT) != 0; });
    }
};

// When hurt: a defensive cooldown, or failing that a heal on itself.
class CoaDefensiveAction : public Action
{
public:
    CoaDefensiveAction(PlayerbotAI* botAI) : Action(botAI, "coa defensive") {}

    bool Execute(Event /*event*/) override
    {
        SpellInfo const* cast =
            CastFirst(botAI, bot, KnownAbilities(bot, [](uint16 kind) { return (kind & KIND_DEFENSIVE) != 0; }), bot);
        if (!cast)
            cast = CastFirst(botAI, bot, KnownAbilities(bot, [](uint16 kind)
                { return (kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE)); }), bot);

        return RecordUsage(USAGE_DEFENSIVE, cast);
    }

    bool isUseful() override
    {
        return bot->IsAlive() && ClassHas(bot, KIND_DEFENSIVE | KIND_HEAL) && HasReadyAbility(botAI, bot,[](uint16 kind)
            { return (kind & KIND_DEFENSIVE) || ((kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE))); });
    }
};

// Removes a harmful magic, curse, disease or poison effect from the bot or a group member.
class CoaDispelAction : public Action
{
public:
    CoaDispelAction(PlayerbotAI* botAI) : Action(botAI, "coa dispel") {}

    bool Execute(Event /*event*/) override
    {
        std::vector<Usable> const spells = KnownAbilities(bot, IsFriendlyDispel);
        if (spells.empty())
            return false;

        for (Player* member : NearbyGroup(bot))
            for (Usable const& spell : spells)
            {
                // Some cleanses only work on the caster.
                if (member != bot && !(spell.kind & KIND_ALLY_CAST))
                    continue;

                if (!HasDispellable(member, spell.dispelMask))
                    continue;

                if (SpellInfo const* cast = CastFirst(botAI, bot, { spell }, member, USAGE_DISPEL))
                    return RecordUsage(USAGE_DISPEL, cast);
            }

        return RecordUsage(USAGE_DISPEL, nullptr);
    }

    bool isUseful() override { return ClassHas(bot, KIND_DISPEL) && HasReadyAbility(botAI, bot,IsFriendlyDispel); }
};

// Kicks the cast of the current target or of an attacker.
class CoaInterruptAction : public Action
{
public:
    CoaInterruptAction(PlayerbotAI* botAI) : Action(botAI, "coa interrupt") {}

    bool Execute(Event /*event*/) override
    {
        Unit* caster = FindCaster(botAI, bot);
        if (!caster)
            return false;

        return RecordUsage(USAGE_INTERRUPT,
            CastFirst(botAI, bot, KnownAbilities(bot, [](uint16 kind) { return (kind & KIND_INTERRUPT) != 0; }), caster));
    }

    bool isUseful() override
    {
        return ClassHas(bot, KIND_INTERRUPT) && HasReadyAbility(botAI, bot,[](uint16 kind) { return (kind & KIND_INTERRUPT) != 0; });
    }
};

// Out of combat: long buffs on the bot and its group.
class CoaBuffAction : public Action
{
public:
    CoaBuffAction(PlayerbotAI* botAI) : Action(botAI, "coa buff") {}

    bool Execute(Event /*event*/) override
    {
        if (bot->IsInCombat() || bot->IsMounted() || bot->IsInFlight() || !bot->IsAlive())
            return false;

        std::vector<Usable> const spells = KnownAbilities(bot, [](uint16 kind)
            { return (kind & KIND_BUFF) && !(kind & (KIND_HOSTILE | KIND_DAMAGE | KIND_TAUNT | KIND_HEAL | KIND_CONTROL)); });
        if (spells.empty())
            return false;

        time_t const now = time(nullptr);
        if (recent.size() > 64)
            for (auto itr = recent.begin(); itr != recent.end();)
                itr = itr->second <= now ? recent.erase(itr) : std::next(itr);

        for (Usable const& spell : spells)
            for (Player* member : NearbyGroup(bot))
            {
                if (member != bot && !(spell.kind & KIND_ALLY_CAST))
                    continue;

                if (member->HasAura(spell.info->Id))
                    continue;

                // The aura may come from a triggered spell under another id: do not recast
                // it on the same member before it would have run out.
                auto const key = std::make_pair(member->GetGUID(), spell.info->Id);
                auto const found = recent.find(key);
                if (found != recent.end() && found->second > now)
                    continue;

                if (StrictCheck(bot, spell.info, member) == SPELL_CAST_OK && botAI->CastSpell(spell.info->Id, member))
                {
                    recent[key] = now + time_t(spell.info->GetMaxDuration() / IN_MILLISECONDS * 9 / 10);
                    return RecordUsage(USAGE_BUFF, spell.info);
                }
            }

        return false;
    }

    bool isUseful() override { return ClassHas(bot, KIND_BUFF); }

private:
    std::map<std::pair<ObjectGuid, uint32>, time_t> recent;
};

// A group member, the bot included, carries something the bot knows how to dispel.
class CoaDispelTrigger : public Trigger
{
public:
    CoaDispelTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa dispel") {}

    bool IsActive() override
    {
        if (!ClassHas(bot, KIND_DISPEL))
            return false;

        std::vector<Usable> const spells = KnownAbilities(bot, IsFriendlyDispel);
        for (Player* member : NearbyGroup(bot))
            for (Usable const& spell : spells)
                if ((member == bot || (spell.kind & KIND_ALLY_CAST)) && HasDispellable(member, spell.dispelMask))
                    return true;

        return false;
    }
};

// An enemy near the bot is casting something that can be interrupted.
class CoaEnemyCastingTrigger : public Trigger
{
public:
    CoaEnemyCastingTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa enemy casting") {}

    bool IsActive() override
    {
        return ClassHas(bot, KIND_INTERRUPT) && FindCaster(botAI, bot) &&
               HasReadyAbility(botAI, bot,[](uint16 kind) { return (kind & KIND_INTERRUPT) != 0; });
    }
};

/*
 * Damage dealer: use the class abilities every tick, area attacks on packs, interrupts,
 * dispels, and a defensive when hurt. Melee specializations close in; ranged ones keep the
 * "reach spell" distance of CombatStrategy.
 */
class CoaCombatStrategy : public CombatStrategy
{
public:
    CoaCombatStrategy(PlayerbotAI* botAI, bool ranged = false) : CombatStrategy(botAI), ranged(ranged) {}

    std::string const getName() override { return ranged ? "coa ranged" : "coa"; }
    uint32 GetType() const override
    {
        return CombatStrategy::GetType() | STRATEGY_TYPE_DPS | (ranged ? STRATEGY_TYPE_RANGED : 0);
    }

    std::vector<NextAction> getDefaultActions() override
    {
        return { NextAction("coa attack", ACTION_NORMAL) };
    }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override
    {
        // "invalid target" -> "drop target" is what hands the bot back to the non-combat
        // engine once its target dies. Without it the bot stays in combat mode, facing the
        // corpse, and never loots, quests or moves again.
        CombatStrategy::InitTriggers(triggers);

        // Most CoA melee abilities fail with SPELL_FAILED_OUT_OF_RANGE until the bot closes
        // in. Same trigger and priority as MeleeCombatStrategy.
        if (!ranged)
            triggers.push_back(new TriggerNode("enemy out of melee", { NextAction("reach melee", ACTION_HIGH + 1) }));

        triggers.push_back(new TriggerNode("coa enemy casting", { NextAction("coa interrupt", InterruptPriority()) }));
        triggers.push_back(new TriggerNode("coa dispel", { NextAction("coa dispel", DispelPriority()) }));
        triggers.push_back(new TriggerNode("medium aoe", { NextAction("coa aoe", ACTION_HIGH + 2) }));
        triggers.push_back(new TriggerNode("low health", { NextAction("coa defensive", ACTION_HIGH + 8) }));
    }

protected:
    virtual float InterruptPriority() { return ACTION_INTERRUPT; }
    virtual float DispelPriority() { return ACTION_NORMAL + 5; }

    bool ranged;
};

// Tank specializations: taunt back whatever turns on someone else, area threat on two
// enemies, defensives from medium health.
class CoaTankStrategy : public CoaCombatStrategy
{
public:
    CoaTankStrategy(PlayerbotAI* botAI) : CoaCombatStrategy(botAI) {}

    std::string const getName() override { return "coa tank"; }
    uint32 GetType() const override { return STRATEGY_TYPE_COMBAT | STRATEGY_TYPE_TANK | STRATEGY_TYPE_MELEE; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override
    {
        CoaCombatStrategy::InitTriggers(triggers);
        triggers.push_back(new TriggerNode("lose aggro", { NextAction("coa taunt", ACTION_HIGH + 5) }));
        triggers.push_back(new TriggerNode("light aoe", { NextAction("coa aoe", ACTION_HIGH + 3) }));
        triggers.push_back(new TriggerNode("medium health", { NextAction("coa defensive", ACTION_HIGH + 6) }));
    }
};

/*
 * Healer specializations, from range. By urgency: move in range, critical heal, area heal
 * when several members are hurt, heal the low, then heals over time and dispels, and only
 * then attack. Interrupts wait behind the heals.
 */
class CoaHealStrategy : public CoaCombatStrategy
{
public:
    CoaHealStrategy(PlayerbotAI* botAI) : CoaCombatStrategy(botAI, true) {}

    std::string const getName() override { return "coa heal"; }
    uint32 GetType() const override { return STRATEGY_TYPE_COMBAT | STRATEGY_TYPE_HEAL | STRATEGY_TYPE_RANGED; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override
    {
        CoaCombatStrategy::InitTriggers(triggers);

        // A wounded member out of healing range or line of sight: close in first, above
        // healing (which would fail the range check) and attacking.
        triggers.push_back(new TriggerNode("party member to heal out of spell range",
                                           { NextAction("reach party member to heal", ACTION_CRITICAL_HEAL + 5) }));
        triggers.push_back(new TriggerNode("party member critical health",
                                           { NextAction("coa heal", ACTION_CRITICAL_HEAL + 4) }));
        triggers.push_back(new TriggerNode("medium aoe heal",
                                           { NextAction("coa group heal", ACTION_CRITICAL_HEAL + 3) }));
        triggers.push_back(new TriggerNode("party member low health",
                                           { NextAction("coa heal", ACTION_CRITICAL_HEAL + 2) }));
        triggers.push_back(new TriggerNode("party member medium health",
                                           { NextAction("coa hot", ACTION_CRITICAL_HEAL + 1),
                                             NextAction("coa heal", ACTION_CRITICAL_HEAL) }));
        triggers.push_back(new TriggerNode("party member almost full health",
                                           { NextAction("coa hot", ACTION_MEDIUM_HEAL) }));
    }

protected:
    float InterruptPriority() override { return ACTION_MEDIUM_HEAL + 7; }
    float DispelPriority() override { return ACTION_MEDIUM_HEAL + 6; }
};

// Out of combat: keep the long buffs up on the bot and its group.
class CoaBuffStrategy : public Strategy
{
public:
    CoaBuffStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "coa buff"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override
    {
        triggers.push_back(new TriggerNode("often", { NextAction("coa buff", ACTION_NORMAL + 5) }));
    }
};

class CoaStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    CoaStrategyFactoryInternal() : NamedObjectContext<Strategy>(false, true)
    {
        creators["coa"] = &CoaStrategyFactoryInternal::coa;
        creators["coa ranged"] = &CoaStrategyFactoryInternal::coa_ranged;
        creators["coa tank"] = &CoaStrategyFactoryInternal::coa_tank;
        creators["coa heal"] = &CoaStrategyFactoryInternal::coa_heal;
        creators["coa buff"] = &CoaStrategyFactoryInternal::coa_buff;
    }

private:
    static Strategy* coa(PlayerbotAI* botAI) { return new CoaCombatStrategy(botAI); }
    static Strategy* coa_ranged(PlayerbotAI* botAI) { return new CoaCombatStrategy(botAI, true); }
    static Strategy* coa_tank(PlayerbotAI* botAI) { return new CoaTankStrategy(botAI); }
    static Strategy* coa_heal(PlayerbotAI* botAI) { return new CoaHealStrategy(botAI); }
    static Strategy* coa_buff(PlayerbotAI* botAI) { return new CoaBuffStrategy(botAI); }
};

class CoaActionFactoryInternal : public NamedObjectContext<Action>
{
public:
    CoaActionFactoryInternal()
    {
        creators["coa attack"] = &CoaActionFactoryInternal::coa_attack;
        creators["coa aoe"] = &CoaActionFactoryInternal::coa_aoe;
        creators["coa heal"] = &CoaActionFactoryInternal::coa_heal;
        creators["coa group heal"] = &CoaActionFactoryInternal::coa_group_heal;
        creators["coa hot"] = &CoaActionFactoryInternal::coa_hot;
        creators["coa taunt"] = &CoaActionFactoryInternal::coa_taunt;
        creators["coa defensive"] = &CoaActionFactoryInternal::coa_defensive;
        creators["coa dispel"] = &CoaActionFactoryInternal::coa_dispel;
        creators["coa interrupt"] = &CoaActionFactoryInternal::coa_interrupt;
        creators["coa buff"] = &CoaActionFactoryInternal::coa_buff;
    }

private:
    static Action* coa_attack(PlayerbotAI* botAI) { return new CoaAttackAction(botAI); }
    static Action* coa_aoe(PlayerbotAI* botAI) { return new CoaAoeAction(botAI); }
    static Action* coa_heal(PlayerbotAI* botAI)
    {
        return new CoaHealAction(botAI, "coa heal", CoaHealAction::Mode::Direct);
    }
    static Action* coa_group_heal(PlayerbotAI* botAI)
    {
        return new CoaHealAction(botAI, "coa group heal", CoaHealAction::Mode::Group);
    }
    static Action* coa_hot(PlayerbotAI* botAI) { return new CoaHealAction(botAI, "coa hot", CoaHealAction::Mode::OverTime); }
    static Action* coa_taunt(PlayerbotAI* botAI) { return new CoaTauntAction(botAI); }
    static Action* coa_defensive(PlayerbotAI* botAI) { return new CoaDefensiveAction(botAI); }
    static Action* coa_dispel(PlayerbotAI* botAI) { return new CoaDispelAction(botAI); }
    static Action* coa_interrupt(PlayerbotAI* botAI) { return new CoaInterruptAction(botAI); }
    static Action* coa_buff(PlayerbotAI* botAI) { return new CoaBuffAction(botAI); }
};

class CoaTriggerFactoryInternal : public NamedObjectContext<Trigger>
{
public:
    CoaTriggerFactoryInternal()
    {
        creators["coa dispel"] = &CoaTriggerFactoryInternal::coa_dispel;
        creators["coa enemy casting"] = &CoaTriggerFactoryInternal::coa_enemy_casting;
    }

private:
    static Trigger* coa_dispel(PlayerbotAI* botAI) { return new CoaDispelTrigger(botAI); }
    static Trigger* coa_enemy_casting(PlayerbotAI* botAI) { return new CoaEnemyCastingTrigger(botAI); }
};

}  // namespace

SharedNamedObjectContextList<Strategy> CoaAiObjectContext::sharedStrategyContexts;
SharedNamedObjectContextList<Action> CoaAiObjectContext::sharedActionContexts;
SharedNamedObjectContextList<Trigger> CoaAiObjectContext::sharedTriggerContexts;
SharedNamedObjectContextList<UntypedValue> CoaAiObjectContext::sharedValueContexts;

CoaAiObjectContext::CoaAiObjectContext(PlayerbotAI* botAI)
    : AiObjectContext(botAI, sharedStrategyContexts, sharedActionContexts,
                      sharedTriggerContexts, sharedValueContexts)
{
}

void CoaAiObjectContext::BuildSharedContexts()
{
    BuildSharedStrategyContexts(sharedStrategyContexts);
    BuildSharedActionContexts(sharedActionContexts);
    BuildSharedTriggerContexts(sharedTriggerContexts);
    BuildSharedValueContexts(sharedValueContexts);
}

void CoaAiObjectContext::BuildSharedStrategyContexts(SharedNamedObjectContextList<Strategy>& strategyContexts)
{
    AiObjectContext::BuildSharedStrategyContexts(strategyContexts);
    strategyContexts.Add(new CoaStrategyFactoryInternal());
}

void CoaAiObjectContext::BuildSharedActionContexts(SharedNamedObjectContextList<Action>& actionContexts)
{
    AiObjectContext::BuildSharedActionContexts(actionContexts);
    actionContexts.Add(new CoaActionFactoryInternal());
}

void CoaAiObjectContext::BuildSharedTriggerContexts(SharedNamedObjectContextList<Trigger>& triggerContexts)
{
    AiObjectContext::BuildSharedTriggerContexts(triggerContexts);
    triggerContexts.Add(new CoaTriggerFactoryInternal());
}

void CoaAiObjectContext::BuildSharedValueContexts(SharedNamedObjectContextList<UntypedValue>& valueContexts)
{
    AiObjectContext::BuildSharedValueContexts(valueContexts);
}
