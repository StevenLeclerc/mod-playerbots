/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaLayaEtat.h"
#include "CoaLayaOracle.h"
#include "CoaAiObjectContext.h"
#include "CoaChasse.h"
#include "CoaRegistreCombat.h"

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
#include "mod-ascension-compat/src/AscensionSpecialization.h"

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
    KIND_CONTROL    = 0x1000,  // stuns, fears, polymorphs... never aimed at a group member
    KIND_STANCE     = 0x2000,  // a form or stance on the caster that never expires
    KIND_STEALTH    = 0x4000,  // hides the caster: hunted, not worn
    KIND_SELF_HEAL  = 0x8000   // heals the caster and nobody else
};

/*
 * Class abilities, indexed by class id, ordered by required level.
 *
 * Most CoA abilities are scripted (dummy or script effects), so what mod-ascension-compat says
 * a class can learn is the only reliable list of what it can cast.
 */
struct CoaAbility
{
    uint32 spellId;
    uint8 requiredLevel;
    uint16 kind;
    uint32 dispelMask;
    uint32 firstSpellId;  // first rank: ranks of one spell replace each other
};

struct ClassKit
{
    std::vector<CoaAbility> abilities;
    uint16 kinds = 0;  // every kind some ability of the class has
};

// Ally targets: pet, the nearest ally, party and raid areas, ally or any unit, chain heal.
// Named from SharedDefines.h rather than written as numbers: the four NEARBY and CONE forms
// (3, 4, 58, 59) were missing from the list while they were bare numbers.
bool IsAllyTarget(uint32 target)
{
    switch (target)
    {
        case TARGET_UNIT_NEARBY_ALLY:           // 3
        case TARGET_UNIT_NEARBY_PARTY:          // 4
        case TARGET_UNIT_PET:                   // 5
        case TARGET_UNIT_CASTER_AREA_PARTY:     // 20
        case TARGET_UNIT_TARGET_ALLY:           // 21
        case TARGET_UNIT_TARGET_ANY:            // 25
        case TARGET_UNIT_SRC_AREA_ALLY:         // 30
        case TARGET_UNIT_DEST_AREA_ALLY:        // 31
        case TARGET_UNIT_SRC_AREA_PARTY:        // 33
        case TARGET_UNIT_DEST_AREA_PARTY:       // 34
        case TARGET_UNIT_TARGET_PARTY:          // 35
        case TARGET_UNIT_LASTTARGET_AREA_PARTY: // 37
        case TARGET_UNIT_TARGET_CHAINHEAL_ALLY: // 45
        case TARGET_UNIT_CASTER_AREA_RAID:      // 56
        case TARGET_UNIT_TARGET_RAID:           // 57
        case TARGET_UNIT_NEARBY_RAID:           // 58
        case TARGET_UNIT_CONE_ALLY:             // 59
        case TARGET_UNIT_TARGET_AREA_RAID_CLASS:// 61
            return true;
        default:
            return false;
    }
}

// Of those, the ones that reach several allies at once. The NEARBY forms (3, 4, 58) pick a single
// unit and are left out, as SpellInfo.cpp's target table has them under TARGET_SELECT_CATEGORY_NEARBY.
bool IsAllyAreaTarget(uint32 target)
{
    switch (target)
    {
        case TARGET_UNIT_CASTER_AREA_PARTY:     // 20
        case TARGET_UNIT_SRC_AREA_ALLY:         // 30
        case TARGET_UNIT_DEST_AREA_ALLY:        // 31
        case TARGET_UNIT_SRC_AREA_PARTY:        // 33
        case TARGET_UNIT_DEST_AREA_PARTY:       // 34
        case TARGET_UNIT_LASTTARGET_AREA_PARTY: // 37
        case TARGET_UNIT_TARGET_CHAINHEAL_ALLY: // 45
        case TARGET_UNIT_CASTER_AREA_RAID:      // 56
        case TARGET_UNIT_CONE_ALLY:             // 59
        case TARGET_UNIT_TARGET_AREA_RAID_CLASS:// 61
            return true;
        default:
            return false;
    }
}

// Aimed at one chosen ally, who may be someone else than the caster.
bool IsAllyCastTarget(uint32 target)
{
    return target == TARGET_UNIT_TARGET_ALLY || target == TARGET_UNIT_TARGET_ANY ||
           target == TARGET_UNIT_TARGET_PARTY || target == TARGET_UNIT_TARGET_CHAINHEAL_ALLY ||
           target == TARGET_UNIT_TARGET_RAID;
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

/*
 * A stance, form or aspect: every effect is an aura on the caster and it never runs out.
 * The bot takes one out of combat and keeps it; it has no place in the damage rotation.
 * Several CoA abilities are gated behind one through CasterAuraSpell (Beetle Form 803183
 * carries 64 of them, Spider Form 800841 another 42), so they cannot simply be ignored.
 */
bool IsStance(SpellInfo const* info)
{
    if (info->GetMaxDuration() > 0)
        return false;

    bool aura = false;
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (!effect.IsEffect())
            continue;

        // A summon, a teleport, a trade skill or an item alongside the aura: not a stance.
        if (!IsAuraEffect(effect) || effect.ApplyAuraName == SPELL_AURA_NONE ||
            effect.TargetA.GetTarget() != TARGET_UNIT_CASTER)
            return false;

        aura = true;
    }

    return aura;
}

// What a spell does, looking two levels into the spells it triggers: CoA abilities often
// carry their heal, taunt or aura in a triggered spell.
void Classify(SpellInfo const* info, CoaAbility& ability, uint8 depth = 0)
{
    constexpr int32 LongAura = 5 * MINUTE * IN_MILLISECONDS;
    int32 const duration = info->GetMaxDuration();

    // Only the ability itself: a spell it triggers is not the stance the bot stands in.
    if (!depth && IsStance(info))
        ability.kind |= KIND_STANCE;

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
        // A heal aimed at the caster alone (TARGET_UNIT_CASTER) is still a heal. Left out, such a
        // spell came out of Classify with kind == 0 and was then refused by every action - IsAttack
        // returns false on kind == 0, and the heal and defensive actions ask for KIND_HEAL. A
        // Barbarian, which has no healing specialization at all, was left with no heal whatsoever.
        // KIND_SELF_HEAL marks it so the party heals below do not offer it to somebody else.
        if (heal && (ally || self))
        {
            ability.kind |= KIND_HEAL;
            if (!ally)
                ability.kind |= KIND_SELF_HEAL;
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

        // La furtivite se reconnait a son aura. Elle ressemble a une posture — aucune duree,
        // sur le lanceur seul — mais elle ne doit surtout pas etre portee en permanence :
        // Underwalk (800797) ralentit celui qui la porte, et neuf bots la gardaient en
        // continu, se trainant toute la journee pour rien. C'est une arme de chasse.
        if (aura && self && effect.ApplyAuraName == SPELL_AURA_MOD_STEALTH)
            ability.kind |= KIND_STEALTH;

        // Stances and forms (no duration) are left out: two of them would take turns forever.
        if (aura && (self || ally) && !heal && info->IsPositive() && duration >= LongAura &&
            effect.ApplyAuraName != SPELL_AURA_MOD_SHAPESHIFT)
            ability.kind |= KIND_BUFF;

        // A proc rider is not part of the cast: "when you hit, deal nature damage" describes
        // the aura, not the spell that applies it. Following it made a weapon poison or a
        // damage-proc buff look like a damage spell and sent it to the damage rotation, where
        // it was reapplied over and over (Blight Venom 805776, Temporal Resilience 680389).
        // Triggers the cast itself fires are still followed.
        bool const proc = aura && (effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL ||
                                   effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE ||
                                   effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_DAMAGE ||
                                   effect.ApplyAuraName == SPELL_AURA_ADD_TARGET_TRIGGER);

        if (effect.TriggerSpell && !proc && depth < 2)
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
        std::unordered_map<uint8, std::unordered_map<uint32, size_t>> known;

        auto add = [&byClass, &known](uint8 classId, AscensionClassAbility const& learnable)
        {
            ClassKit& kit = byClass[classId];
            auto const [itr, inserted] = known[classId].try_emplace(learnable.SpellId, kit.abilities.size());
            if (!inserted)
            {
                // Listed again as a higher rank of another spell: remember the link.
                if (learnable.FirstSpellId != learnable.SpellId)
                    kit.abilities[itr->second].firstSpellId = learnable.FirstSpellId;
                return false;
            }

            CoaAbility ability = { learnable.SpellId, learnable.RequiredLevel, 0, 0, learnable.FirstSpellId };
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(learnable.SpellId))
                Classify(info, ability);

            kit.abilities.push_back(ability);
            kit.kinds |= ability.kind;
            return true;
        };

        // Class grants, Character Advancement entries (a bot only knows those of its own
        // specialization, which HasSpell sorts out at run time) and higher ranks. Ids below
        // 100000 are vanilla weapon skills and Auto Attack.
        uint32 count = 0, ranks = 0;
        for (uint8 classId = 1; classId < MAX_CLASSES; ++classId)
        {
            if (!IsAscensionCustomClassId(classId))
                continue;

            for (AscensionClassAbility const& learnable : GetAscensionClassAbilities(classId))
            {
                if (learnable.SpellId < 100000 || !add(classId, learnable))
                    continue;

                ++count;
                ranks += learnable.SpellId != learnable.FirstSpellId;
            }
        }

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

        LOG_INFO("playerbots", "coa: {} abilities ({} higher ranks) from mod-ascension-compat, {} classes",
                 count, ranks, byClass.size());
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

// How long a bot's kit is trusted when neither its level nor the size of its spell book has
// moved. It only has to be short next to a level up, not next to a tick.
constexpr time_t CoaKitLifeSeconds = 5;

/*
 * The abilities the bot has reached and really knows, unfiltered, held by its context
 * between ticks. See CoaKnownAbility and CoaAiObjectContext::knownKit.
 *
 * The three tests hoisted here - required level, Player::HasSpell, and the SpellInfo lookup
 * that drops passives - do not depend on what the caller is looking for, so filtering the
 * result afterwards gives exactly the list the per-call walk used to build.
 */
std::vector<CoaKnownAbility> const& KnownKit(PlayerbotAI* botAI, Player* bot)
{
    auto* const coaContext = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext());
    time_t const now = time(nullptr);
    uint8 const level = bot->GetLevel();
    uint32 const spellCount = uint32(bot->GetSpellMap().size());

    if (coaContext->knownKitBuilt && coaContext->knownKitLevel == level &&
        coaContext->knownKitSpells == spellCount && now - coaContext->knownKitBuilt < CoaKitLifeSeconds)
        return coaContext->knownKit;

    coaContext->knownKit.clear();
    coaContext->knownKitBuilt = now;
    coaContext->knownKitLevel = level;
    coaContext->knownKitSpells = spellCount;

    auto const& all = ClassAbilities();
    auto const found = all.find(bot->getClass());
    if (found == all.end())
        return coaContext->knownKit;

    for (CoaAbility const& ability : found->second.abilities)
    {
        if (ability.requiredLevel > level || !bot->HasSpell(ability.spellId))
            continue;

        // Class grants include passives (e.g. 552011 Resilient Constitution).
        SpellInfo const* info = sSpellMgr->GetSpellInfo(ability.spellId);
        if (!info || info->IsPassive())
            continue;

        coaContext->knownKit.push_back({ info, ability.kind, ability.dispelMask, ability.firstSpellId });
    }

    return coaContext->knownKit;
}

// Active abilities the bot has reached and actually knows, for which `wanted(kind)` is true.
template <typename Filter>
std::vector<Usable> KnownAbilities(PlayerbotAI* botAI, Player* bot, Filter wanted)
{
    std::vector<Usable> usable;
    // First rank of each entry of `usable`, at the same index: which ability a row stands for.
    // A linear scan rather than a hash map - the filtered lists hold a handful of spells, and
    // this runs several times per bot and per tick.
    std::vector<uint32> firsts;

    // A bot can still know the lower ranks of a spell: only cast the highest one it has reached.
    // Abilities are ordered by required level, so a later rank replaces an earlier one.
    for (CoaKnownAbility const& ability : KnownKit(botAI, bot))
    {
        if (!wanted(ability.kind))
            continue;

        auto const at = std::find(firsts.begin(), firsts.end(), ability.firstSpellId);
        if (at == firsts.end())
        {
            firsts.push_back(ability.firstSpellId);
            usable.push_back({ ability.info, ability.kind, ability.dispelMask });
        }
        else
            usable[size_t(at - firsts.begin())] = { ability.info, ability.kind, ability.dispelMask };
    }

    return usable;
}

bool IsFriendlyDispel(uint16 kind)
{
    return (kind & KIND_DISPEL) && !(kind & (KIND_CONTROL | KIND_HOSTILE));
}

// Whether a heal can land on somebody else than the bot. A spell that also aims at a chosen ally
// (KIND_ALLY_CAST) or at an area of allies (KIND_GROUP_HEAL) still can, even when one of its
// effects only heals the caster.
bool CanHealOther(uint16 kind)
{
    return !(kind & KIND_SELF_HEAL) || (kind & (KIND_ALLY_CAST | KIND_GROUP_HEAL));
}

SpellInfo const* EffectiveSpell(Player* bot, SpellInfo const* info);

// Whether a spell is still set aside. The bench is keyed by the id that was actually checked,
// which is the replacement's id when a proc or a talent has swapped the listed spell (see
// EffectiveSpell), so both ids have to be consulted before a spell is tried.
bool IsBenched(std::unordered_map<uint32, time_t> const& benched, uint32 spellId, time_t now)
{
    auto const bench = benched.find(spellId);
    return bench != benched.end() && bench->second > now;
}

// Whether the bot knows an ability for which `wanted(kind)` is true and that is off cooldown:
// actions only run when they have something to cast, which keeps the usage counters honest.
template <typename Filter>
bool HasReadyAbility(PlayerbotAI* botAI, Player* bot, Filter wanted)
{
    auto const& benched = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->benchedSpells;
    time_t const now = time(nullptr);

    for (Usable const& spell : KnownAbilities(botAI, bot, wanted))
    {
        // What the bot would really cast, as in CastFirst: the cooldown and the bench of a
        // swapped spell are the replacement's. Reading only the listed id had this answer yes
        // on a spell CastFirst then refuses to try, and the action ran for nothing.
        SpellInfo const* const info = EffectiveSpell(bot, spell.info);
        if (!bot->HasSpellCooldown(info->Id) && !IsBenched(benched, spell.info->Id, now) &&
            !IsBenched(benched, info->Id, now))
            return true;
    }

    return false;
}

// Abilities worth using in the damage rotation.
bool IsAttack(uint16 kind, bool tank)
{
    // An ability none of the tests above recognised is a summon, a stance, a permanent self
    // aura, a trade skill or an item: over the whole CoA catalogue, 645 active abilities end
    // up here and not one of them carries a target the caster could aim at an enemy. Trying
    // them on the target only spent global cooldowns and, for the permanent ones, reapplied
    // the same aura for ever (Bushcraft 800267, Serpent Ward 500960, Tower Formation 800317).
    if (!kind)
        return false;

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
 * The spell the bot will really cast.
 *
 * Ascension swaps a spell for another while a proc is up - Malefic Wrath becomes Malefic Arrow,
 * Reclamation becomes Spirit Volley - and PlayerbotAI::CastSpell follows the swap
 * (PlayerbotAI.cpp:3718). Everything the caller decides about the cast has to be decided on the
 * replacement: its cost, its range, its cooldown, its cast time, and the name it is blamed under
 * when it fails. The cast itself is still asked for under the base id, or the proc is not consumed.
 */
SpellInfo const* EffectiveSpell(Player* bot, SpellInfo const* info)
{
    uint32 const replacement = bot->GetTemporarySpellReplacement(info->Id);
    if (replacement && replacement != info->Id)
        if (SpellInfo const* swapped = sSpellMgr->GetSpellInfo(replacement))
            return swapped;

    return info;
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
    // The check has to bear on the spell that will be prepared, replacement included: checking the
    // base spell meant checking the wrong cost and the wrong range, so the check said yes and
    // prepare() said no - which the caller read as "refused for a reason we cannot see" and
    // answered by benching a spell that was never at fault. The resource pre-check below is the
    // clearest case: it compared the cost of a base spell to the resource, while the replacement
    // the proc grants is often free.
    info = EffectiveSpell(bot, info);

    ObjectGuid const oldSel = bot->GetTarget();

    // Spell::CheckCast reads m_powerCost, and that is only worked out in Spell::prepare: a check run
    // on its own therefore sees a spell that costs nothing and lets every spell through, however
    // empty the bot's mana or custom resource is. The cast then fails with SPELL_FAILED_NO_POWER -
    // 186 of the 210 refusals measured on 18/09. So pay for it here, before asking.
    if (info->PowerType < MAX_POWERS && info->PowerType != POWER_HEALTH)
    {
        int32 const cost = info->CalcPowerCost(bot, info->GetSchoolMask());
        if (cost > 0 && bot->GetPower(Powers(info->PowerType)) < cost)
            return SPELL_FAILED_NO_POWER;
    }

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

// The usage kinds a bot drops its attack cast for. Until now this followed "was a usage kind
// passed at all", so the actions that passed none - area attack, defensive, interrupt - never
// dropped anything, which is the opposite of what the comment below promises. Attack and area
// attack are left out on purpose: an attack must not cut off another attack.
bool PreemptsAttackCast(uint8 usage);

// A bot busy casting an attack drops it for a heal, dispel, defensive, taunt or interrupt. A heal
// or buff in progress is kept, or heals would keep cutting each other off.
void DropAttackCast(Player* bot)
{
    for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
        if (Spell* current = bot->GetCurrentSpell(type))
            if (!current->GetSpellInfo()->IsPositive())
                bot->InterruptSpell(type, false);
}

// Whether the bot is holding its mana back for healing rather than spending it on damage.
// A healer keeps the larger share; any other bot that knows a heal keeps a smaller cushion, so
// that it can still patch itself up; a bot with no heal at all never holds anything back, as it
// would stop fighting for nothing. Both shares are settings, 0 turning the reserve off.
bool SavingManaForHeals(PlayerbotAI* botAI, Player* bot)
{
    if (bot->getPowerType() != POWER_MANA)
        return false;

    uint32 const reserve = GetCoaRole(bot) == CoaRole::Heal ? sPlayerbotAIConfig.coaHealerManaReserve
                                                            : sPlayerbotAIConfig.coaCasterManaReserve;
    if (!reserve || bot->GetPowerPct(POWER_MANA) >= float(reserve))
        return false;

    return !KnownAbilities(botAI, bot, [](uint16 kind)
        { return (kind & (KIND_HEAL | KIND_HOT)) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); }).empty();
}

// Removes what costs mana, for a bot that is keeping the rest of its mana for healing.
void DropManaSpells(Player* bot, std::vector<Usable>& spells)
{
    spells.erase(std::remove_if(spells.begin(), spells.end(), [bot](Usable const& spell)
        { return spell.info->PowerType == POWER_MANA && spell.info->CalcPowerCost(bot, spell.info->GetSchoolMask()) > 0; }),
        spells.end());
}

// Retire les sorts que le coeur REFUSERA de toute facon, sur cette cible et a cette
// distance. Mesure du 2026-09-22, ligne « coa usage since start » du module :
// attack 44834/124990, soit 36 % de reussite et 80 156 refus. Sur les dix sorts les
// plus refuses, deux causes se corrigent ici :
//
//   TOO_CLOSE          6 554  dont Darkslayer (680256) 5 341 a lui seul — « Fire a dark
//                             shot... Ranged Weapon Damage », un tir a portee minimale
//                             que le bot reessaie au corps a corps, tick apres tick
//   AFFECTING_COMBAT   1 068  Flay Corpse (801042), un sort hors combat propose dans la
//                             rotation d'attaque
//
// Les autres causes du releve ne sont PAS filtrees ici, et c'est deliberé : STUNNED
// (2 804) et NO_POWER (2 109) sont des refus legitimes — le bot est etourdi ou a sec,
// il n'y a rien a corriger ; NOT_MOUNTED (1 872, Soul Capture et Scythe Rush) n'est pas
// compris, et filtrer sur une cause qu'on n'explique pas reviendrait a deviner.
//
// RESERVE : ce top dix ne couvre que 15 831 des 80 156 refus, soit environ 20 %. Le
// gain attendu porte sur cette part-la, pas sur la totalite.
//
// POURQUOI ICI ET PAS DANS KnownAbilities : cette derniere est mise en cache sur le
// niveau et la taille du livre de sorts, alors que la distance change a chaque tick.
// Y porter le filtre empoisonnerait le cache.
//
// La portee MAXIMALE n'est volontairement pas filtree : un sort hors d'atteinte parce
// que le bot est trop LOIN se corrige en s'approchant, et le retirer de la liste priverait
// le moteur de la raison de le faire. Seul le « trop pres » est sans recours pour le bot.
void DropUncastableHere(Player* bot, Unit* target, std::vector<Usable>& spells)
{
    if (!bot || !target)
        return;

    bool const inCombat = bot->IsInCombat();

    spells.erase(std::remove_if(spells.begin(), spells.end(),
        [bot, target, inCombat](Usable const& spell)
        {
            SpellInfo const* info = spell.info;
            if (!info)
                return true;

            // SpellInfo::CanBeUsedInCombat() est !HasAttribute(SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL),
            // lu dans SpellInfo.cpp. C'est exactement ce que le coeur teste avant de rendre
            // SPELL_FAILED_AFFECTING_COMBAT.
            if (inCombat && !info->CanBeUsedInCombat())
                return true;

            // Le test de portee minimale est RECOPIE de Spell::CheckRange (Spell.cpp), pas
            // approche : le coeur en a DEUX branches, et une premiere redaction qui n'avait
            // gardé que la seconde aurait manque le poste dominant — Darkslayer est un sort
            // SPELL_RANGE_RANGED, donc il passe par la premiere.
            if (!info->RangeEntry)
                return false;
            // RangeEntry->ID == 1 : le coeur rend SPELL_CAST_OK sans rien verifier.
            if (info->RangeEntry->ID == 1)
                return false;

            float const minRange = bot->GetSpellMinRangeForTarget(target, info);

            // EGALITE, pas masque : SpellRangeFlag (Spell.h) est une enumeration de valeurs
            // — DEFAULT 0, MELEE 1, RANGED 2 — et non des bits. Le coeur ecrit lui-meme
            // `range_type == SPELL_RANGE_RANGED`.
            if (info->RangeEntry->Flags == SPELL_RANGE_RANGED && !bot->IgnoresSpellMinRange(info))
            {
                // Pour un sort a distance, le coeur ajoute la portee de melee au minimum.
                float const minRangeCombined = minRange + bot->GetMeleeRange(target);
                return bot->IsWithinRange(target, minRangeCombined);
            }

            return minRange > 0.0f && bot->IsWithinCombatRange(target, minRange);
        }),
        spells.end());

    // Deuxieme passe, ajoutee le 2026-09-22 apres mesure : deux causes de plus de la
    // meme famille « ne peut jamais reussir dans cet etat ». Elles sont separees de la
    // portee parce qu'elles ne dependent pas de la distance et se lisent d'un coup d'oeil.
    //
    //   ONLY_STEALTHED     3 324   Arachnophobia (804970) lance a decouvert
    //   UNIT_NOT_INFRONT   4 448   Bloodmoon Blast (501608) lance dans le dos de la cible
    //
    // CE QUI REND LE FILTRE D'ORIENTATION UTILE MALGRE SON CARACTERE PASSAGER. Un bot mal
    // oriente le sera encore une fraction de seconde : on pourrait croire qu'il suffit
    // d'attendre le tick suivant. Mais CastFirst MET AU BAN 8 SECONDES tout sort dont le
    // lancement est refuse. Un simple quart de tour coutait donc huit secondes de rotation.
    // Ecarter le sort pour ce tick le laisse disponible au suivant : c'est exactement
    // l'inverse.
    spells.erase(std::remove_if(spells.begin(), spells.end(),
        [bot, target](Usable const& spell)
        {
            SpellInfo const* info = spell.info;

            // Spell.cpp : HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) && !HasStealthAura().
            if (info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) && !bot->HasStealthAura())
                return true;

            // Spell.cpp, recopie sans le IsPlayer() qui est vrai par construction ici :
            // (FacingCasterFlags & SPELL_FACING_FLAG_INFRONT) && !HasInArc(M_PI, target)
            // && !IsWithinBoundaryRadius(target). Le dernier terme compte : au contact,
            // le coeur ne reclame pas l'orientation, et l'oublier ferait ecarter a tort
            // les sorts de melee.
            return (info->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT)
                && !bot->HasInArc(static_cast<float>(M_PI), target)
                && !bot->IsWithinBoundaryRadius(target);
        }),
        spells.end());
}

// Puts the cheapest heals first. Low on mana a bot would otherwise keep offering its biggest heal,
// be turned down for want of power and heal nobody, while a small heal was still within reach.
void CheapestFirst(Player* bot, std::vector<Usable>& spells)
{
    std::stable_sort(spells.begin(), spells.end(), [bot](Usable const& a, Usable const& b)
    {
        return a.info->CalcPowerCost(bot, a.info->GetSchoolMask()) <
               b.info->CalcPowerCost(bot, b.info->GetSchoolMask());
    });
}

/*
 * Priorite de « attack enemy player », recopiee de AttackEnemyPlayersStrategy.cpp:13.
 *
 * L'amont ne l'exporte sous aucun nom : la valeur est forcement dupliquee ici, mais elle
 * l'est sous un nom, si bien qu'un grep relie les deux endroits. Le commentaire seul ne le
 * faisait pas : une mise a jour amont portant 55 a 60 aurait remis l'embuscade derriere
 * l'attaque, sans conflit de fusion, sans erreur et sans ligne de journal.
 */
constexpr float AttackEnemyPlayerPriority = 55.0f;

// L'embuscade passe juste devant : les deux actions sont armees par le meme declencheur
// (« enemy player near » et « coa ambush » lisent tous deux « enemy player target »), au
// meme tick. Voir CoaBuffStrategy::InitTriggers.
constexpr float AmbushPriority = AttackEnemyPlayerPriority + 1.0f;

/*
 * Delai avant de proposer a nouveau un buff dont la duree ne dit rien.
 *
 * Une posture est permanente : IsStance exige GetMaxDuration() <= 0, donc la garde de
 * duree de CoaBuffAction calculait 0/1000*9/10 = 0 (ou -1/1000 = 0), c'est-a-dire une
 * entree deja expiree au tick suivant. Pour une posture dont l'aura atterrit sous un
 * autre id que celui du sort - le cas meme qui justifie l'existence de `recent` - le bot
 * la relancait a chaque passage de « coa buff », soit un temps de recharge global par
 * tick hors combat. Trente secondes suffisent a fermer la boucle sans retarder une
 * reprise legitime.
 */
constexpr time_t StanceRecastSeconds = 30;

constexpr time_t SpellBenchSeconds = 20;
constexpr time_t RefusedBenchSeconds = 8;
constexpr time_t NoPowerBenchSeconds = 5;

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
        // What the bot will really cast: a proc or a talent may have replaced this spell (see
        // EffectiveSpell). Cooldowns, cast time, the bench and the failure log all go by the
        // replacement; only the cast itself keeps the id the rotation lists, because that is the
        // id the lists hold and the one PlayerbotAI::CastSpell has to be given for the proc to be
        // consumed.
        SpellInfo const* const info = EffectiveSpell(bot, spell.info);

        // A spell on cooldown would only fail with SPELL_FAILED_NOT_READY.
        if (bot->HasSpellCooldown(info->Id))
            continue;

        // The global cooldown blocks every spell alike: try again on a later tick.
        if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
            return nullptr;

        // The bench is posted under the id that was checked, so a failure that belongs to a
        // replacement never sidelines the listed spell: a proc lasting two seconds used to take
        // the bot's permanent ability out of the rotation for twenty, which is the very injustice
        // this bench was written to prevent. Both ids are read, and a spent entry is dropped so
        // the table does not grow for ever.
        bool stillBenched = false;
        for (uint32 const benchedId : { spell.info->Id, info->Id })
        {
            auto const bench = benched.find(benchedId);
            if (bench == benched.end())
                continue;
            if (bench->second > now)
                stillBenched = true;
            else
                benched.erase(bench);
        }
        if (stillBenched)
            continue;

        SpellCastResult const check = StrictCheck(bot, info, target);
        if (check == SPELL_CAST_OK)
        {
            // PlayerbotAI::CastSpell refuses a spell with a cast time while the bot moves: stop
            // now and cast it on a later tick, once standing still.
            if (bot->isMoving() && info->CalcCastTime(bot))
            {
                bot->StopMoving();
                if (usage != 255)
                    RecordFailure(usage, info->Id, FAILURE_MOVING);
                continue;
            }

            if (PreemptsAttackCast(usage))
                DropAttackCast(bot);

            bool const sitting = !bot->IsStandState();
            bool const casting = bot->IsNonMeleeSpellCast(false, true, true);
            if (botAI->CastSpell(spell.info->Id, target))
                return spell.info;

            // Refused for a reason the check cannot see (CoA spell scripts check their own
            // resources when the cast is prepared): leave it aside briefly so the next ability
            // of the list gets its turn instead of this one failing every tick.
            if (!sitting && !casting)
                benched[info->Id] = now + RefusedBenchSeconds;

            if (usage != 255)
                RecordFailure(usage, info->Id, sitting ? FAILURE_SITTING : casting ? FAILURE_CASTING : FAILURE_REFUSED);
            continue;
        }
        else if (IsLastingFailure(check))
            benched[info->Id] = now + SpellBenchSeconds;
        // Out of mana, energy or rage: asking again on the very next tick changes nothing, and
        // with the spell set aside the action reports itself useless, so the bot does something
        // it can afford instead of spending its ticks being turned down.
        else if (check == SPELL_FAILED_NO_POWER)
            benched[info->Id] = now + NoPowerBenchSeconds;

        if (usage != 255)
            RecordFailure(usage, info->Id, uint16(check));
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
    USAGE_DEFENSIVE, USAGE_DISPEL, USAGE_INTERRUPT, USAGE_BUFF, USAGE_STEALTH,
    USAGE_POSTE_PRIS, USAGE_POSTE_ATTEINT,
    USAGE_TRAQUE, USAGE_RETOUR, USAGE_DECROCHAGE,
    USAGE_POSTE_CATALOGUE,
    USAGE_POSTE_AUBERGE, USAGE_POSTE_VOLERIE, USAGE_POSTE_CIMETIERE, USAGE_POSTE_ROUTE,
    USAGE_POSTE_PEUPLE,
    USAGE_MAX
};

// Les cinq derniers ne comptent aucun sort : ce sont les jalons de la chasse
// (Ai/Coa/CoaChasse.h). « poste pris » monte quand un poste de guet est retenu,
// « poste atteint » quand le bot y arrive ; l'ecart entre les deux est la mesure
// du voyage, et c'est la seule preuve que le lot 2 tourne.
//
// Le lot 3 en ajoute trois. « traque » monte a chaque passage GUET -> TRAQUE,
// « retour » a chaque poste RETROUVE apres une traque, « decrochage » a chaque
// rupture de combat PvE. Ce qu'ils disent, lus ensemble : traque contre poste
// atteint donne le rendement d'un poste ; retour contre traque, la part des
// traques qui se terminent sans que le bot soit mort ou parti ailleurs.
//
// CE QU'ILS NE DISENT PAS, et l'en-tete de ReportUsage le dit deja : ils sont
// GLOBAUX AU PROCESSUS (piege 36). Ils prouvent qu'une traque a eu lieu, jamais
// qu'un bot donne a traque.
//
// Le lot 4 en ajoute un dernier. « poste catalogue » monte en MEME TEMPS que
// « poste pris », et seulement quand le poste vient de la table
// playerbots_coa_poste_guet (Ai/Coa/CoaPostes.h). L'ecart entre les deux est
// donc exactement le nombre de postes tires par le repli du lot 2, hubs ou
// cellules de grind -- et c'est la seule facon de savoir, sans ouvrir la base,
// si le catalogue a ete applique et s'il sert.
//
// ET LA CLASSE DU POSTE, en quatre compteurs de plus : « poste auberge »,
// « poste volerie », « poste cimetiere », « poste route ». Leur somme vaut
// « poste catalogue ».
//
// POURQUOI L'AGREGE NE SUFFISAIT PAS. Il monte pareil pour une auberge, dont le
// dossier mesure l'enrichissement a x18, et pour un carrefour, mesure a x1 --
// c'est-a-dire au niveau du temoin. La metrique de preuve que le lot 4 se donne
// est « la part des rencontres a moins de N yd d'un poste catalogue » : si elle
// baisse, seul le detail par classe permet de dire si c'est le catalogue qui ne
// sert pas ou le tirage qui prend des carrefours a la place des auberges. Sans
// eux, la mesure que ce lot devait rendre possible etait precisement celle qu'il
// rendait impossible.
//
// Le lot 5 en ajoute un seul : « poste peuple ». Il monte quand le poste
// FINALEMENT RETENU -- catalogue, hub ou cellule de grind -- est dans un bloc
// 2x2 ou l'index des proies (CoaIndexProies, Bot/MercenaryRewards.h) avait vu
// au moins une proie NON MERCENAIRE dont le palier n'est pas categoriquement
// refuse par le filtre de niveau de ce bot.
//
// SON DENOMINATEUR N'EST PAS « poste pris », et le dire ainsi etait faux. Ce
// dernier monte pour TOUS les postes retenus (RetenirUnPoste, Ai/Coa/CoaChasse.h),
// y compris les deux replis de sortie de lieu sans PvP -- qui ne sont
// DELIBEREMENT pas ponderes, un bot en sanctuaire cherchant la sortie et non la
// meilleure chasse, et qui ne peuvent donc jamais faire monter « poste
// peuple ». Le ratio se lit contre les TIRAGES ORDINAIRES, c'est-a-dire
// « poste pris » moins les replis : catalogue ordinaire, hubs ordinaires et
// cellule de grind ordinaire, les trois chemins qui consultent l'index. Les
// trois y sont, depuis que le troisieme a ete ajoute -- il manquait, et c'est
// aujourd'hui le plus frequent, le catalogue n'etant pas applique en base.
//
// CE QU'IL NE DIT PAS, et ce sont les reserves qui comptent :
//   - il compte ce que l'index CROYAIT il y a moins de 30 s, pas ce que le bot
//     a trouve en arrivant plusieurs minutes plus tard ;
//   - « attaquable » y est un raccourci. L'index ignore le quota
//     MaxAttackersPerTarget (EnemyPlayerValue.cpp:217) : trois proies deja
//     engagees chacune par un attaquant comptent pour trois et n'en valent
//     aucune. La treve de TruceAfterDeathSec, elle, est filtree a l'ecriture
//     (CoaIndexProies::Compter) et ne fausse donc plus ce compte ;
//   - il ne compte QUE les proies non mercenaires. C'est ce qui le rend
//     falsifiable : avec les mercenaires dedans, il montait parce que la chasse
//     y avait envoye des chasseurs, et nul n'aurait pu dire si c'etait l'index
//     qui avait raison ou la boucle qui se refermait sur elle-meme.
// La preuve que le lot sert, c'est « traque » contre « poste atteint » -- le
// rendement d'un poste -- et ce compteur-ci ne sert qu'a savoir si l'index
// avait de la matiere a donner.
//
// AVEC AiPlayerbot.WildPvp.IndexProies = 0 -- OU WildPvp.Enabled = 0 -- IL
// RESTE A ZERO, par construction : CoaChasseAction::IndexProiesActif exige les
// deux, et ProiesAutour rend alors 0 partout. Un compteur a zero alors que les
// deux interrupteurs sont a 1 veut donc dire que l'index est vide, ou qu'il ne
// contient que des mercenaires -- et la ligne « [CoA index] » de CoaBots.log,
// qui publie « proies N (dont mercenaires N) », dit alors laquelle des deux.
//
// ILS SONT ADDITIFS pour tools/dashboard/serveur_dashboard.py comme pour la vue
// mercenaires du panneau : ce sont des noms nouveaux en minuscules et espaces,
// et aucun nom existant ne bouge.
//
// IL EST AJOUTE EN FIN D'ENUM, ET C'EST VOULU. Les compteurs sont lus par
// position nulle part -- ni RecordUsage ni ReportUsage n'indexent autrement que
// par la valeur nommee -- mais UsageNames est un tableau de taille USAGE_MAX :
// un ajout au milieu decalerait silencieusement tous les noms suivants si les
// deux listes n'etaient pas modifiees ensemble. En fin de liste, l'erreur
// devient une erreur de compilation.
//
// LES NOMS SONT UN CONTRAT. tools/dashboard/serveur_dashboard.py:52 lit cette
// ligne avec COUNT_RE = ([a-z ]+?) (\d+)/(\d+) : minuscules et espaces, rien
// d'autre. Pas d'accent, pas de chiffre, pas de tiret.
constexpr char const* UsageNames[USAGE_MAX] =
{
    "attack", "aoe", "heal", "group heal", "hot", "taunt", "defensive", "dispel", "interrupt", "buff",
    "stealth", "poste pris", "poste atteint", "traque", "retour", "decrochage",
    "poste catalogue",
    "poste auberge", "poste volerie", "poste cimetiere", "poste route",
    "poste peuple"
};

bool PreemptsAttackCast(uint8 usage)
{
    switch (usage)
    {
        case USAGE_HEAL: case USAGE_GROUP_HEAL: case USAGE_HOT:
        case USAGE_TAUNT: case USAGE_DEFENSIVE: case USAGE_DISPEL: case USAGE_INTERRUPT:
            return true;
        default:
            return false;
    }
}

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

    // L'oracle Laya ne se prouve que par ses compteurs. « vetos » est le
    // chiffre qui compte : combien de fois le modele a change le cours des
    // choses. A zero, on a monte un pont reseau pour rien, et il faut pouvoir
    // le constater plutot que de le supposer.
    if (sPlayerbotAIConfig.layaEnabled)
    {
        LOG_INFO("playerbots.coa", "coa laya: {}", CoaLayaOracle::Instance().Compteurs());
        // Le tirage par rencontre se verifie ici, et nulle part ailleurs : la
        // part pilotee des rencontres OUVERTES doit valoir le seuil de la conf.
        // Si elle le vaut et que la part pilotee des rencontres JOURNALISEES ne
        // le vaut pas, la selection est en aval, et ces compteurs disent
        // laquelle. Voir l'en-tete de CoaRegistreCombat.h.
        LOG_INFO("playerbots.coa", "coa registre: {}", CoaRegistreCombat::Instance().Compteurs());
    }

    std::lock_guard<std::mutex> guard(UsageSpellsLock);
    // Attacks and heals list more spells: they show which rank of each spell the bots cast.
    for (UsageKind kind : { USAGE_DISPEL, USAGE_INTERRUPT, USAGE_ATTACK, USAGE_HEAL })
    {
        std::vector<std::pair<uint32, uint32>> top(UsageSpells[kind].begin(), UsageSpells[kind].end());
        std::sort(top.begin(), top.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        size_t const shown = kind == USAGE_ATTACK || kind == USAGE_HEAL ? 40 : 15;
        std::string spells;
        for (size_t i = 0; i < top.size() && i < shown; ++i)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(top[i].first);
            spells += Acore::StringFormat("{}{} ({}) x{}", i ? ", " : "", info ? info->SpellName[0] : "?",
                                          top[i].first, top[i].second);
        }
        LOG_INFO("playerbots.coa", "coa {} spells: {}", UsageNames[kind], spells.empty() ? "none yet" : spells);
    }

    // Why a cast fails: spell (id) reason xcount. Reasons are SpellCastResult values,
    // "refused" when CastSpell turned down a spell the check accepted, "nothing" when no
    // ability was left to try. Kinds that record no failure print nothing.
    for (UsageKind kind : { USAGE_HEAL, USAGE_GROUP_HEAL, USAGE_HOT, USAGE_TAUNT, USAGE_DISPEL,
                            USAGE_ATTACK, USAGE_AOE, USAGE_DEFENSIVE, USAGE_INTERRUPT, USAGE_BUFF,
                            USAGE_STEALTH })
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
        if (kind == USAGE_DISPEL || kind == USAGE_INTERRUPT || kind == USAGE_ATTACK || kind == USAGE_HEAL)
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

// Whether the unit already carries a buff of this spell category.
//
// Spell.dbc field 1 groups buffs that displace each other: the four Venomancer
// combat venoms are all category 55, the three Primalist boons 2336. Casting a
// second one silently drops the first, so a "cast what is missing" pass never
// settles - one of them is always missing.
bool HasBuffOfCategory(Unit* unit, uint32 category, uint32 exceptSpellId)
{
    if (!category)
        return false;

    for (auto const& [auraId, application] : unit->GetAppliedAuras())
    {
        if (auraId == exceptSpellId || !application->IsPositive())
            continue;

        SpellInfo const* auraInfo = application->GetBase()->GetSpellInfo();
        if (auraInfo && auraInfo->GetCategory() == category)
            return true;
    }

    return false;
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

/*
 * L'ORACLE CHOISIT LE SORT — et le tourniquet reste le repli exact.
 *
 * Ce que remplace cette fonction : `std::rotate`, qui fait tourner la liste des
 * sorts utilisables pour repartir apres le dernier qui a marche. Ce tourniquet
 * n'est pas une decision, c'est un tour de role. Il garantit que le bot use
 * tout son arsenal, rien de plus.
 *
 * Pourquoi ce point d'accroche plutot que le multiplicateur. Deux journees de
 * mesure sur le veto n'ont rien donne (P-085, P-089) : un multiplicateur est
 * appele APRES que le moteur a choisi, il ne peut que refuser, et refuser dans
 * une file deja triee n'ouvre aucun choix. Ici le choix existe vraiment — cinq
 * a huit sorts lancables, dans un ordre que personne n'a jamais optimise — et
 * les libelles sont des noms de sorts, que le modele lit.
 *
 * POURQUOI C'EST SANS RISQUE. On ne fait que REORDONNER un vecteur que
 * `CastFirst` parcourt de toute facon en entier : il saute ce qui est en
 * recharge, au ban ou impossible, et lance le premier qui passe. Le modele
 * decide donc par quoi on essaie, jamais ce qui est permis. S'il se trompe, le
 * cout est d'essayer un sort avant un autre.
 *
 * LE REPLI. Pas de reponse fraiche, oracle injoignable, bot non elite, option
 * eteinte, rencontre tiree hors du bras pilote : la fonction rend `false` et
 * l'appelant joue le tourniquet d'origine, bit pour bit. La demande part pour
 * le tour SUIVANT — attendre la reponse ici bloquerait le thread monde.
 *
 * LE TIRAGE AU SORT PAR RENCONTRE. Le registre decide a l'ouverture de chaque
 * combat si l'oracle a le droit de le piloter (voir Ai/Coa/CoaRegistreCombat.h
 * pour le biais que cela supprime). Sur le bras non pilote on sort ICI, avant
 * meme d'emettre la demande : charger l'oracle pour une reponse qu'on
 * n'utilisera pas fausserait sa cadence et son cout sans rien apporter.
 */
bool OrdonnerParLaya(PlayerbotAI* botAI, Player* bot, Unit* target, std::vector<Usable>& usable)
{
    if (!sPlayerbotAIConfig.layaSorts || usable.size() < 2)
        return false;
    uint64 const guid = bot->GetGUID().GetRawValue();
    if (!sPlayerbotAIConfig.IsLayaElite(guid))
        return false;

    // A 100 — le defaut — on n'interroge PAS le registre : ni verrou pris, ni
    // comportement change. C'est ce qui garantit que la greffe est inerte tant
    // qu'on ne l'arme pas, y compris pour les sorts lances avant que le coeur
    // n'ait marque le bot en combat, pour lesquels aucune rencontre n'est
    // encore ouverte.
    //
    // En dessous, une rencontre non ouverte vaut « non pilotee » : le
    // tourniquet reprend la main. Le cas se produit pour les incantations
    // emises avant l'entree en combat ; il dilue le bras pilote sans le
    // melanger au temoin, donc il sous-estime l'effet au lieu de le fabriquer.
    if (sPlayerbotAIConfig.layaTirageAuSort < 100 &&
        !CoaRegistreCombat::Instance().EstPilotee(guid))
        return false;

    CoaLayaOracle& oracle = CoaLayaOracle::Instance();
    if (!oracle.Actif())
        return false;

    // Ne pas consulter quand rien ne peut partir : CanCastSpell refuse tout
    // sort sous UNIT_STATE_LOST_CONTROL (etourdi, confus, en fuite, en saut, en
    // charge). Meme raison que dans le multiplicateur.
    if (!bot->IsAlive() || bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
    {
        oracle.CompterMuet();
        return false;
    }

    // 1. Demander, pour le tour suivant. La cadence est celle de la conf.
    if (oracle.PeutRedemander(guid, CoaLayaOracle::CANAL_SORT))
    {
        std::vector<std::pair<std::string, float>> lot;
        std::vector<std::string> descriptions;
        lot.reserve(usable.size());
        descriptions.reserve(usable.size());
        for (Usable const& sort : usable)
        {
            if (!sort.info || !sort.info->SpellName[0])
                continue;
            std::string const nom = sort.info->SpellName[0];
            // La description ne pretend rien savoir de plus que le coeur : le
            // nom du sort, que le modele lit, et la nature que le catalogue CoA
            // lui a deja reconnue. Inventer un effet serait une regle de plus a
            // maintenir, et fausse le jour ou un sort est retouche.
            std::string nature;
            if (sort.kind & KIND_AOE)       nature += " Degats de zone.";
            if (sort.kind & KIND_DAMAGE)    nature += " Degats.";
            if (sort.kind & KIND_INTERRUPT) nature += " Interrompt l'incantation.";
            if (sort.kind & KIND_CONTROL)   nature += " Neutralise la cible.";
            if (sort.kind & KIND_TAUNT)     nature += " Force la cible a m'attaquer.";
            if (sort.kind & KIND_DEFENSIVE) nature += " Reduit les degats subis.";
            if (nature.empty())             nature = " Attaque.";

            // Les trois faits que le moteur connait deja sur chaque sort et
            // qu'il ne disait pas : incantation, portee, recharge. Ils ne sont
            // pas decoratifs, ce sont les conditions d'emploi elles-memes — un
            // sort a incantation ne part pas d'un bot en mouvement, et l'etat
            // dit maintenant au modele s'il se deplace et a quelle distance est
            // sa cible. Sans ces trois-la, cette moitie de l'etat ne sert a rien.
            char faits[128];
            uint32 const msIncantation = sort.info->CalcCastTime(bot);
            if (msIncantation >= 100)
                snprintf(faits, sizeof(faits), " Incantation %.1f s.", msIncantation / 1000.0f);
            else
                snprintf(faits, sizeof(faits), " Instantane.");
            nature += faits;

            float const portee = sort.info->GetMaxRange(false, bot);
            if (portee >= 6.0f)
                snprintf(faits, sizeof(faits), " Portee %d m.", int(portee));
            else
                snprintf(faits, sizeof(faits), " Au contact.");
            nature += faits;

            uint32 const msRecharge = std::max(sort.info->RecoveryTime, sort.info->CategoryRecoveryTime);
            if (msRecharge >= 1500)
            {
                snprintf(faits, sizeof(faits), " Recharge %u s.", msRecharge / 1000u);
                nature += faits;
            }

            lot.emplace_back(nom, 0.0f);
            descriptions.push_back(nom + "." + nature);
        }
        if (lot.size() >= 2)
        {
            // Meme description que le canal action, construite au meme endroit
            // (CoaLayaEtat.h). Celle qui vivait ici ecrivait « Ressource: 0% »
            // a toute classe sans mana et « Cible a 0% de vie, a 0 metres »
            // quand il n'y avait pas de cible : deux faits faux, servis au
            // modele a chaque tour, sur le canal meme qu'on cherchait a mesurer.
            std::string const etat = CoaDecrireEtatLaya(botAI, bot, target);
            // Pas de question annexe sur ce canal : elle doublerait le cout
            // d'inference (27 ms contre 14 mesures sur M1) sans rien apporter
            // au choix du sort.
            oracle.Demander(guid, CoaLayaOracle::CANAL_SORT, etat, lot, descriptions, false);
        }
    }

    // 2. Lire le cache, et rien que le cache.
    float const meilleure = oracle.MeilleureProbabilite(guid, CoaLayaOracle::CANAL_SORT);
    if (meilleure <= 0.0f)
        return false;

    // RELEVER LES PROBABILITES UNE FOIS, PUIS TRIER SUR CE RELEVE.
    //
    // Les interroger depuis le comparateur serait un defaut grave et pas une
    // maladresse : le thread receveur ecrit le cache a tout moment, donc deux
    // comparaisons du meme couple pourraient rendre des reponses differentes.
    // Un comparateur incoherent, dans std::sort, n'est pas une approximation :
    // c'est un comportement indefini, qui deborde du conteneur. Le releve
    // prealable supprime la question, et economise au passage une prise de
    // verrou par comparaison.
    std::vector<std::pair<float, Usable>> classees;
    classees.reserve(usable.size());
    for (Usable const& sort : usable)
    {
        float const p = sort.info && sort.info->SpellName[0]
            ? oracle.Probabilite(guid, CoaLayaOracle::CANAL_SORT, sort.info->SpellName[0])
            : -1.0f;
        // Un sort absent de la reponse garde -1 : il part en fin de liste sans
        // etre retire. CastFirst pourra toujours l'atteindre si tout le reste
        // echoue, ce qui garde l'arsenal complet accessible.
        classees.emplace_back(p, sort);
    }
    std::stable_sort(classees.begin(), classees.end(),
        [](auto const& a, auto const& b) { return a.first > b.first; });
    for (size_t i = 0; i < classees.size(); ++i)
        usable[i] = classees[i].second;
    oracle.CompterChoix();
    return true;
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

        // A bot that spends its last mana on damage has nothing left when someone drops, and
        // measured on 18/09 that is the usual state: in 99% of the casts turned down for want of
        // power the bot sat below 10% mana. Under its reserve it attacks with what costs nothing
        // (and its weapon), keeping the rest for heals.
        bool const saveMana = SavingManaForHeals(botAI, bot);

        std::vector<Usable> usable = KnownAbilities(botAI, bot, [tank](uint16 kind) { return IsAttack(kind, tank); });
        if (usable.empty())
            return false;

        if (saveMana)
            DropManaSpells(bot, usable);
        // Retire ce que le coeur refuserait ici et maintenant — trop pres, ou hors combat
        // seulement. Apres DropManaSpells, pour ne pas payer le calcul de portee sur des
        // sorts qu'on vient d'ecarter.
        DropUncastableHere(bot, target, usable);
        if (usable.empty())
            return RecordUsage(USAGE_ATTACK, nullptr);

        // L'oracle ordonne la liste s'il a une reponse fraiche ; sinon on joue
        // le tourniquet d'origine, inchange. Voir OrdonnerParLaya.
        size_t start = 0;
        if (!OrdonnerParLaya(botAI, bot, target, usable))
        {
            // Rotate through the abilities, starting after the last one that went off, so a
            // bot uses its whole kit instead of spamming the first ability that works.
            start = next % usable.size();
            std::rotate(usable.begin(), usable.begin() + start, usable.end());
        }

        // The cast goes through CastFirst rather than through a second loop of its own. The one
        // written here judged every refusal by PlayerbotAI::CastSpell to be the spell's fault and
        // benched it for a minute after three of them - while CastSpell also refuses for reasons
        // that pass in a tick and have nothing to do with the spell: a cast time while the bot is
        // moving, a bot still sitting, a cast already in progress. A ranged bot, which moves for
        // the whole fight, lost its main spell for a minute in under a second, and the count never
        // decayed, so three refusals hours apart did the same. CastFirst handles the three cases,
        // benches for 8 seconds and says why in the failure log.
        SpellInfo const* const cast = CastFirst(botAI, bot, usable, target, USAGE_ATTACK);
        if (cast)
        {
            auto const itr = std::find_if(usable.begin(), usable.end(),
                [cast](Usable const& spell) { return spell.info == cast; });
            if (itr != usable.end())
                next = start + size_t(std::distance(usable.begin(), itr)) + 1;
        }

        return RecordUsage(USAGE_ATTACK, cast);
    }

    bool isUseful() override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        return target && target->IsAlive();
    }

private:
    // Index into the ability list of the next spell to try, kept from one tick to the next.
    size_t next = 0;
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

        std::vector<Usable> spells = KnownAbilities(botAI, bot, [](uint16 kind)
            { return (kind & KIND_AOE) && (kind & (KIND_DAMAGE | KIND_HOSTILE)); });
        if (SavingManaForHeals(botAI, bot))
            DropManaSpells(bot, spells);

        return RecordUsage(USAGE_AOE, CastFirst(botAI, bot, spells, target, USAGE_AOE));
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
                spells = KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_GROUP_HEAL) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); });
                break;
            case Mode::OverTime:
            {
                spells = KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_HOT) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); });
                ObjectGuid const caster = bot->GetGUID();
                spells.erase(std::remove_if(spells.begin(), spells.end(),
                    [target, caster](Usable const& spell) { return target->HasAura(spell.info->Id, caster); }),
                    spells.end());
                break;
            }
            default:
                // Single target heals first, direct ones before those over time; area heals last.
                spells = KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_HEAL) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); });
                std::stable_sort(spells.begin(), spells.end(), [](Usable const& a, Usable const& b)
                {
                    auto rank = [](uint16 kind) { return ((kind & KIND_GROUP_HEAL) ? 2 : 0) + ((kind & KIND_HOT) ? 1 : 0); };
                    return rank(a.kind) < rank(b.kind);
                });
                break;
        }

        // Since a heal aimed at the caster alone is classified as a heal (KIND_SELF_HEAL), it has
        // to be kept out of the list when the wounded member is somebody else: the strict check
        // would turn it down for bad targets and bench it for 20 seconds, blaming a spell that was
        // simply asked the impossible.
        if (target != bot)
            spells.erase(std::remove_if(spells.begin(), spells.end(),
                [](Usable const& spell) { return !CanHealOther(spell.kind); }), spells.end());

        if (SavingManaForHeals(botAI, bot))
            CheapestFirst(bot, spells);

        UsageKind const usage = mode == Mode::Group ? USAGE_GROUP_HEAL : mode == Mode::OverTime ? USAGE_HOT : USAGE_HEAL;
        if (spells.empty())
            RecordFailure(usage, 0, FAILURE_NOTHING);

        return RecordUsage(usage, CastFirst(botAI, bot, spells, target, usage));
    }

    bool isUseful() override
    {
        Unit* target = AI_VALUE(Unit*, "party member to heal");
        uint16 const wanted = mode == Mode::Group ? KIND_GROUP_HEAL : mode == Mode::OverTime ? KIND_HOT : KIND_HEAL;
        // Same rule as Execute: a caster-only heal makes this action useful for the bot itself,
        // never for another member, or the action would run every tick and cast nothing.
        bool const onSelf = target == bot;
        return target && target->IsAlive() && target->GetHealthPct() < sPlayerbotAIConfig.almostFullHealth &&
               ClassHas(bot, wanted) &&
               HasReadyAbility(botAI, bot,[wanted, onSelf](uint16 kind)
                   { return (kind & wanted) && !(kind & (KIND_CONTROL | KIND_HOSTILE)) && (onSelf || CanHealOther(kind)); });
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
            KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_TAUNT) != 0; }), target, USAGE_TAUNT));
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
            CastFirst(botAI, bot, KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_DEFENSIVE) != 0; }), bot,
                      USAGE_DEFENSIVE);
        if (!cast)
        {
            std::vector<Usable> heals = KnownAbilities(botAI, bot, [](uint16 kind)
                { return (kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE)); });
            if (SavingManaForHeals(botAI, bot))
                CheapestFirst(bot, heals);

            cast = CastFirst(botAI, bot, heals, bot, USAGE_DEFENSIVE);
        }

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
        std::vector<Usable> const spells = KnownAbilities(botAI, bot, IsFriendlyDispel);
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
            CastFirst(botAI, bot, KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_INTERRUPT) != 0; }), caster,
                      USAGE_INTERRUPT));
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
        // Chaque sortie compte un passage, meme celles qui ne lancent rien. Sans cela
        // `cast` et `tried` n'etaient incrementes que par le meme appel, avec le meme sort
        // non nul : le releve affichait « buff 4971/4971 », c'est-a-dire 100 % de reussite
        // par construction, et un systeme de buffs en panne restait indemontrable. Meme
        // patron que CoaStealthAction. RecordUsage rend le pointeur recu, donc nullptr,
        // donc false.
        if (bot->IsInCombat() || bot->IsMounted() || bot->IsInFlight() || !bot->IsAlive())
            return RecordUsage(USAGE_BUFF, nullptr);

        std::vector<Usable> const spells = KnownAbilities(botAI, bot, [](uint16 kind)
            { return (kind & (KIND_BUFF | KIND_STANCE)) && !(kind & KIND_STEALTH) &&
                     !(kind & (KIND_HOSTILE | KIND_DAMAGE | KIND_TAUNT | KIND_HEAL | KIND_CONTROL)); });
        if (spells.empty())
        {
            RecordFailure(USAGE_BUFF, 0, FAILURE_NOTHING);
            return RecordUsage(USAGE_BUFF, nullptr);
        }

        // A stance replaces the one the bot is in, and CoA classes have several of them
        // (five Boons, twenty-two Runic Tattoos): take one only while standing in none, or
        // two of them would take turns for ever.
        bool const inStance = std::any_of(spells.begin(), spells.end(), [this](Usable const& spell)
            { return (spell.kind & KIND_STANCE) && bot->HasAura(spell.info->Id); });

        time_t const now = time(nullptr);
        if (recent.size() > 64)
            for (auto itr = recent.begin(); itr != recent.end();)
                itr = itr->second <= now ? recent.erase(itr) : std::next(itr);

        for (Usable const& spell : spells)
            for (Player* member : NearbyGroup(bot))
            {
                if (member != bot && !(spell.kind & KIND_ALLY_CAST))
                    continue;

                // A stance is the bot's own, and only when it stands in none.
                if ((spell.kind & KIND_STANCE) && (member != bot || inStance))
                    continue;

                if (member->HasAura(spell.info->Id))
                    continue;

                // One buff per category: several of a displacing group would
                // chase each other forever. See HasBuffOfCategory.
                if (HasBuffOfCategory(member, spell.info->GetCategory(), spell.info->Id))
                    continue;

                // The aura may come from a triggered spell under another id: do not recast
                // it on the same member before it would have run out.
                auto const key = std::make_pair(member->GetGUID(), spell.info->Id);
                auto const found = recent.find(key);
                if (found != recent.end() && found->second > now)
                    continue;

                // Buff and stealth cast on their own rather than through CastFirst - they pick
                // their target themselves and are never benched - so they record their own
                // failures. Without this, "coa usage" could show a buff that never lands and the
                // failure log had nothing at all to say about why.
                SpellCastResult const check = StrictCheck(bot, spell.info, member);
                if (check != SPELL_CAST_OK)
                {
                    RecordFailure(USAGE_BUFF, spell.info->Id, uint16(check));
                    continue;
                }

                if (botAI->CastSpell(spell.info->Id, member))
                {
                    // Une posture, et tout buff sans duree, rend 0 ou -1 : la garde
                    // expirait au tick suivant et ne gardait rien. Voir StanceRecastSeconds.
                    int32 const duration = spell.info->GetMaxDuration();
                    recent[key] = now + (duration > 0 ? time_t(duration / IN_MILLISECONDS * 9 / 10)
                                                      : StanceRecastSeconds);
                    return RecordUsage(USAGE_BUFF, spell.info);
                }

                RecordFailure(USAGE_BUFF, spell.info->Id, FAILURE_REFUSED);
            }

        // Rien n'a ete lance : ni faute de sort connu, ni faute de cible, mais parce que
        // tout ce qui etait a portee tenait deja. Compte quand meme, sans quoi le releve
        // ne saurait pas distinguer « rien a faire » de « tout a echoue ».
        return RecordUsage(USAGE_BUFF, nullptr);
    }

    bool isUseful() override { return ClassHas(bot, KIND_BUFF | KIND_STANCE); }

private:
    std::map<std::pair<ObjectGuid, uint32>, time_t> recent;
};

/*
 * Embuscade. Un mercenaire qui reperе une proie se fond dans le decor AVANT d'etre vu,
 * s'approche, et frappe. Sans cela il charge a decouvert et n'est qu'un monstre de plus.
 *
 * La furtivite n'est PAS portee en permanence : elle ralentit (Underwalk) et n'a de sens
 * qu'a l'approche. Le bot la prend quand une cible valable est en vue, et la perd
 * naturellement en attaquant.
 */
class CoaStealthAction : public Action
{
public:
    CoaStealthAction(PlayerbotAI* botAI) : Action(botAI, "coa stealth") {}

    // Secondes d'attente apres un echec. Sans ce repli, une action prioritaire
    // qui echoue a chaque tick confisquerait le tour a « attack enemy player »
    // et rendrait le mercenaire inoffensif : il resterait plante devant sa proie
    // a rater sa furtivite. Au premier echec on se tait, l'attaque reprend la
    // main, et on retentera a la prochaine rencontre.
    static constexpr time_t RepliSecondes = 10;

    bool Execute(Event /*event*/) override
    {
        if (bot->IsInCombat() || bot->IsMounted() || bot->IsInFlight() || !bot->IsAlive())
            return false;
        if (bot->HasAuraType(SPELL_AURA_MOD_STEALTH))
            return false;

        std::vector<Usable> const spells = KnownAbilities(botAI, bot, [](uint16 kind)
            { return (kind & KIND_STEALTH) && !(kind & (KIND_HOSTILE | KIND_DAMAGE)); });

        for (Usable const& spell : spells)
        {
            // Comme le buff, l'embuscade lance elle-meme : elle consigne donc elle-meme ses
            // echecs, sans quoi « stealth 0/N » ne dit jamais QUELLE condition refuse.
            SpellCastResult const check = StrictCheck(bot, spell.info, bot);
            if (check != SPELL_CAST_OK)
            {
                RecordFailure(USAGE_STEALTH, spell.info->Id, uint16(check));
                continue;
            }

            if (botAI->CastSpell(spell.info->Id, bot))
                return RecordUsage(USAGE_STEALTH, spell.info);

            RecordFailure(USAGE_STEALTH, spell.info->Id, FAILURE_REFUSED);
        }

        // Echec : on se met en retrait pour laisser l'attaque passer.
        prochainEssai = time(nullptr) + RepliSecondes;

        // Compter l'ESSAI meme sans lancement, comme le fait CoaAttackAction :
        // RecordUsage incremente `tried` a chaque appel et `cast` seulement sur un
        // sort non nul. Ne l'appeler qu'en cas de succes rendait le compteur aveugle
        // — « stealth 0/0 » ne disait alors pas si l'action n'avait jamais tourne ou
        // si elle echouait a chaque fois, ce qui est precisement la question.
        RecordUsage(USAGE_STEALTH, nullptr);
        return false;
    }

    // Le repli est teste ICI et non dans Execute : une action jugee inutile est
    // ecartee par le moteur, qui passe alors a « attack enemy player ». La
    // tester dans Execute la laisserait gagner le tour pour ne rien faire.
    bool isUseful() override
    {
        return ClassHas(bot, KIND_STEALTH) && time(nullptr) >= prochainEssai;
    }

private:
    time_t prochainEssai = 0;   // propre a ce bot : une instance d'action par bot
};

/*
 * Un mercenaire, hors combat, a decouvert, avec une proie en vue.
 *
 * Une sonde de diagnostic a vecu ici : sept compteurs globaux (vu, sans classe, pas
 * mercenaire, en combat, deja cache, sans proie, arme) et une ligne « coa ambush probe »
 * toutes les dix minutes. Elle a repondu a la question qu'elle posait - le declencheur
 * s'arme bien, et l'action etait evincee par « attack enemy player » - ce qui a donne
 * AmbushPriority. La question fermee, la sonde est retiree le 21/09/2026 : ces compteurs
 * n'etaient plus un outil, ils n'etaient qu'un reliquat qu'une revue devait a nouveau
 * identifier. La rouvrir, c'est relire ce paragraphe, pas deviner.
 */
class CoaAmbushTrigger : public Trigger
{
public:
    CoaAmbushTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa ambush") {}

    bool IsActive() override
    {
        if (!sPlayerbotAIConfig.wildPvpStealthAmbush)
            return false;
        if (!ClassHas(bot, KIND_STEALTH))
            return false;
        if (!sPlayerbotAIConfig.IsMercenary(bot->GetGUID().GetRawValue()))
            return false;
        if (bot->IsInCombat() || !bot->IsAlive() || bot->IsMounted() || bot->IsInFlight())
            return false;
        if (bot->HasAuraType(SPELL_AURA_MOD_STEALTH))
            return false;

        // « enemy player target » porte deja toutes les regles de cible : camp, zones
        // interdites, detection. Ne pas en ecrire une seconde.
        //
        // PIEGE PAYE (P-035) : le nom enregistre est « enemy player target ».
        // « enemy player » n'est que l'argument par defaut du constructeur de
        // EnemyPlayerValue, enregistre nulle part. Or AI_VALUE vaut
        // context->GetValue<type>(name)->Get() : sur un nom inconnu, GetValue rend
        // nullptr et la fleche dereference zero. Segfault deterministe des que les
        // bots tournent. D'ou le garde ci-dessous plutot que AI_VALUE : un nom faux
        // rendra desormais « pas de proie », jamais un plantage.
        Value<Unit*>* proie = context->GetValue<Unit*>("enemy player target");
        if (!proie || !proie->Get())
            return false;

        return true;
    }
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

        std::vector<Usable> const spells = KnownAbilities(botAI, bot, IsFriendlyDispel);
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

    /*
     * ACTION_NORMAL vaut 10, et Engine.cpp ecarte toute action de pertinence < 100 quand le
     * bot passe en mode minimal (AiPlayerbot.BotActiveAlone) : le bot reste alors en combat,
     * cible selectionnee, et ne fait rien, avec « PUSH:coa attack - 10.000000 | no actions
     * executed » a chaque tick. Consigne en P-066 (incidents du projet) et referme en
     * EXPLOITATION, pas dans le code : BotActiveAlone est a 100, donc plus aucun bot ne passe
     * en mode minimal.
     *
     * Volontairement laisse a ACTION_NORMAL : monter la rotation au-dessus de 100 la ferait
     * aussi passer devant des comportements amont qui comptent sur 10, ce qui est un
     * arbitrage de jeu, pas un correctif. Si BotActiveAlone redescend un jour, c'est ici
     * qu'il faut revenir.
     */
    std::vector<NextAction> getDefaultActions() override
    {
        return { NextAction("coa attack", ACTION_NORMAL) };
    }

    // L'oracle de decision Laya, pour les seuls mercenaires d'elite et
    // seulement si la conf l'arme. Herite par CoaTankStrategy et
    // CoaHealStrategy : les trois roles en beneficient sans duplication.
    //
    // Le multiplicateur ne bloque jamais le thread monde : il note les
    // candidats, emet au plus une demande par LayaPeriodMs, et lit un cache.
    // Voir Ai/Coa/CoaLayaOracle.h.
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override
    {
        CombatStrategy::InitMultipliers(multipliers);
        if (sPlayerbotAIConfig.layaVeto
            && sPlayerbotAIConfig.IsLayaElite(botAI->GetBot()->GetGUID().GetRawValue()))
            multipliers.push_back(new CoaLayaMultiplier(botAI));
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
        // AmbushPriority, et non ACTION_NORMAL + 6 : « attack enemy player » est arme par le
        // MEME declencheur (EnemyPlayerNear rend AI_VALUE(Unit*, "enemy player
        // target"), PvpTriggers.cpp:16) a la priorite 55,0
        // (AttackEnemyPlayersStrategy.cpp:13). A 16 la furtivite etait
        // systematiquement evincee : mesure en jeu, declencheur arme 2 fois,
        // action executee 0 fois. Le bot degainait avant d'avoir pu se cacher,
        // et le mode attaque interdit ensuite la furtivite.
        //
        // Ne touche QUE la rencontre d'un joueur : le farm sur les creatures
        // passe par « attack anything » a 4,0 (GrindingStrategy.cpp:25), sur un
        // declencheur different, et n'est donc pas ralenti.
        triggers.push_back(new TriggerNode("coa ambush", { NextAction("coa stealth", AmbushPriority) }));
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

/*
 * « coa chasse » A SA PROPRE FABRIQUE, ET ELLE NE SUPPORTE PAS LES FRERES.
 *
 * CE QUE LA PLACER DANS CoaStrategyFactoryInternal CASSAIT. Cette fabrique-la
 * est construite en NamedObjectContext<Strategy>(false, true) (juste au-dessus) :
 * le second parametre est supportsSiblings (NamedObjectContext.h:90). Or
 * Engine::addStrategy demande GetSiblingStrategy(nom) et RETIRE du moteur chaque
 * frere avant de poser la nouvelle (Engine.cpp:365-379), et GetSiblings rend
 * supports() du contexte entier moins le nom demande, pour le PREMIER contexte
 * qui supporte les freres et qui porte ce nom (NamedObjectContext.h:213-229).
 * Poser « coa chasse » comme 6e cle du groupe faisait donc retirer ses cinq
 * freres -- dont « coa buff », posee sur le MEME moteur non-combat cent lignes
 * plus haut dans AddDefaultNonCombatStrategies (AiFactory.cpp:640-642, contre
 * :745). Le mercenaire perdait ses buffs hors combat ET le declencheur
 * « coa ambush » -> « coa stealth » (InitTriggers de CoaBuffStrategy, seul
 * endroit du module qui arme « coa stealth »), sans erreur ni journal : un
 * simple LogAction « S:-coa buff », invisible hors debug.
 *
 * L'exclusivite mutuelle voulue reste entiere entre « coa », « coa ranged »,
 * « coa tank » et « coa heal » : elles n'ont pas bouge de fabrique.
 *
 * GetSiblings saute les contextes dont IsSupportsSiblings() est faux
 * (NamedObjectContext.h:215-217) : construite par defaut, celle-ci rend donc un
 * ensemble vide pour « coa chasse », et rien n'est plus retire.
 *
 * PREUVE ATTENDUE APRES COMPILATION : la commande « strategy » du port 8888 sur
 * un mercenaire de classe CoA hors combat doit rendre A LA FOIS « coa buff » et
 * « coa chasse ».
 */
class CoaChasseStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    CoaChasseStrategyFactoryInternal()
    {
        creators["coa chasse"] = &CoaChasseStrategyFactoryInternal::coa_chasse;
    }

private:
    static Strategy* coa_chasse(PlayerbotAI* botAI) { return new CoaChasseStrategy(botAI); }
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
        creators["coa stealth"] = &CoaActionFactoryInternal::coa_stealth;
        creators["coa chasse"] = &CoaActionFactoryInternal::coa_chasse;
        creators["coa decrochage"] = &CoaActionFactoryInternal::coa_decrochage;
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
    static Action* coa_stealth(PlayerbotAI* botAI) { return new CoaStealthAction(botAI); }
    static Action* coa_chasse(PlayerbotAI* botAI) { return new CoaChasseAction(botAI); }
    // Lot 3. Elle est dans la MEME fabrique que les autres actions, et non dans
    // une fabrique a part comme « coa chasse » cote strategies : le mecanisme
    // des freres (Engine::addStrategy -> GetSiblingStrategy, Engine.cpp:365-379)
    // ne concerne QUE les strategies. Les fabriques d'actions ne retirent rien.
    static Action* coa_decrochage(PlayerbotAI* botAI) { return new CoaDecrochageAction(botAI); }
};

class CoaTriggerFactoryInternal : public NamedObjectContext<Trigger>
{
public:
    CoaTriggerFactoryInternal()
    {
        creators["coa dispel"] = &CoaTriggerFactoryInternal::coa_dispel;
        creators["coa enemy casting"] = &CoaTriggerFactoryInternal::coa_enemy_casting;
        creators["coa ambush"] = &CoaTriggerFactoryInternal::coa_ambush;
    }

private:
    static Trigger* coa_dispel(PlayerbotAI* botAI) { return new CoaDispelTrigger(botAI); }
    static Trigger* coa_enemy_casting(PlayerbotAI* botAI) { return new CoaEnemyCastingTrigger(botAI); }
    static Trigger* coa_ambush(PlayerbotAI* botAI) { return new CoaAmbushTrigger(botAI); }
};

}  // namespace

/*
 * Le pont entre la chasse et les compteurs UsageKind, declare dans
 * Ai/Coa/CoaChasse.h. Il est ICI et pas la-bas parce que l'enum USAGE_* et
 * RecordUsage vivent dans l'espace de noms anonyme ouvert plus haut dans ce
 * fichier : un en-tete inclus avant lui ne peut pas les nommer. Une fonction
 * definie a la portee du fichier, elle, les voit.
 *
 * RecordUsage incremente `tried` a chaque appel et `cast` seulement sur un sort
 * non nul (CoaAiObjectContext.cpp, corps de RecordUsage). Aucun sort n'est lance
 * ici : ces deux compteurs ne renseignent donc que la colonne `tried`, et la
 * ligne de journal dira « poste pris 0/N ». C'est N qui compte.
 */
void CoaChasseComptePostePris() { RecordUsage(USAGE_POSTE_PRIS, nullptr); }
void CoaChasseComptePosteAtteint() { RecordUsage(USAGE_POSTE_ATTEINT, nullptr); }
void CoaChasseCompteTraque() { RecordUsage(USAGE_TRAQUE, nullptr); }
void CoaChasseCompteRetour() { RecordUsage(USAGE_RETOUR, nullptr); }
void CoaChasseCompteDecrochage() { RecordUsage(USAGE_DECROCHAGE, nullptr); }
void CoaChasseComptePosteCatalogue() { RecordUsage(USAGE_POSTE_CATALOGUE, nullptr); }
void CoaChasseComptePostePeuple() { RecordUsage(USAGE_POSTE_PEUPLE, nullptr); }

/*
 * La classe du poste tire. `classe` est un CoaSourcePoste (Ai/Coa/CoaPostes.h) :
 * l'enum y est declare, mais l'aiguillage est ECRIT A LA MAIN plutot que calcule
 * par « USAGE_POSTE_AUBERGE + classe ». Une addition sur deux enums qui vivent
 * dans deux fichiers different rendrait toute insertion dans l'un silencieuse
 * dans l'autre ; un switch sans default force le compilateur a signaler une
 * valeur nouvelle.
 *
 * COA_POSTE_SOURCES est la valeur « aucune classe », celle que
 * TirerPosteCatalogue pose quand rien n'est retenu : elle ne compte rien.
 */
void CoaChasseComptePosteClasse(uint8 classe)
{
    switch (classe)
    {
        case COA_POSTE_AUBERGE:   RecordUsage(USAGE_POSTE_AUBERGE, nullptr);   break;
        case COA_POSTE_VOLERIE:   RecordUsage(USAGE_POSTE_VOLERIE, nullptr);   break;
        case COA_POSTE_CIMETIERE: RecordUsage(USAGE_POSTE_CIMETIERE, nullptr); break;
        case COA_POSTE_ROUTE:     RecordUsage(USAGE_POSTE_ROUTE, nullptr);     break;
        default:                  break;
    }
}

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
    // Contexte SEPARE, et non une cle de plus dans le precedent : voir l'en-tete
    // de CoaChasseStrategyFactoryInternal. SharedNamedObjectContextList::Add
    // fusionne les createurs dans une seule table (NamedObjectContext.h:148-153),
    // donc « coa chasse » reste trouvable par create() ; seule GetSiblings, qui
    // parcourt les contextes un par un, voit la difference.
    strategyContexts.Add(new CoaChasseStrategyFactoryInternal());
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
