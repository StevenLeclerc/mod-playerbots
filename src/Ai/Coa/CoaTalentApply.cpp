#include "CoaTalentApply.h"

#include "mod-ascension-compat/src/AscensionSpecialization.h"
#include "CoaTalentPlan.h"
#include "Log.h"
#include "Player.h"
#include "SpellMgr.h"

namespace
{
    uint32 LearnFromPlan(Player* bot, uint32 const* ids, uint16 count)
    {
        uint32 learned = 0;
        for (uint16 i = 0; i < count; ++i)
        {
            uint32 const spellId = ids[i];
            if (!spellId || bot->HasSpell(spellId))
                continue;

            if (!sSpellMgr->GetSpellInfo(spellId))
                continue;

            // learnSpell bypasses the ClassMask that CoA spells otherwise fail
            // against. The second parameter is "temporary", not "dependent" -
            // true would lose the spell on logout.
            bot->learnSpell(spellId, false);
            ++learned;
        }
        return learned;
    }
}

void ApplyCoaTalentPlan(Player* bot)
{
    if (!bot)
        return;

    // Since 15 Sep 2026 there is one plan per SPEC rather than one per class.
    // If the bot already has a specialization, that spec's plan applies -
    // before, it got the talents of the class default even when it had picked
    // something else.
    uint32 const active = GetAscensionActiveSpecialization(bot);
    CoaTalentPlan const* plan =
        active ? GetCoaTalentPlanForSpec(bot->getClass(), active) : nullptr;

    if (!plan)
    {
        // The first plan of a class is the one the rotation is written for. It
        // applies while no spec is set, and as the fallback for a spec we do
        // not know.
        plan = GetCoaTalentPlan(bot->getClass());
        if (!plan)
            return;

        // Without a specialization set, the compat module grants neither the
        // automatic talent entries nor the taught abilities.
        if (!active)
            SwitchAscensionSpecialization(bot, plan->specId);
    }

    uint8 const level = bot->GetLevel();
    uint16 fromClassTree = CoaClassTreePoints(level);
    uint16 fromSpecTree = CoaSpecTreePoints(level);
    if (fromClassTree > plan->classTreeCount)
        fromClassTree = plan->classTreeCount;
    if (fromSpecTree > plan->specTreeCount)
        fromSpecTree = plan->specTreeCount;

    uint32 const learned = LearnFromPlan(bot, plan->classTree, fromClassTree) +
                           LearnFromPlan(bot, plan->specTree, fromSpecTree);

    // The "playerbots" logger does not reach Server.log in this installation -
    // checked on 15 Sep 2026 while bots were demonstrably learning spells.
    // "module.ascension_compat" does write, so that is where we write: without
    // this line there is no way to tell whether the plan ran.
    if (learned)
        LOG_INFO("module.ascension_compat",
                 "CoA talents for {} (class {}, level {}): {} points in the class tree, "
                 "{} in the spec tree, {} spells learned",
                 bot->GetName(), uint32(bot->getClass()), uint32(level),
                 uint32(fromClassTree), uint32(fromSpecTree), learned);
}
