/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COAAIOBJECTCONTEXT_H
#define _PLAYERBOT_COAAIOBJECTCONTEXT_H

#include "AiObjectContext.h"

#include <ctime>
#include <unordered_map>
#include <vector>

class PlayerbotAI;
class SpellInfo;

/*
 * One ability of a bot's kit: an ability of its class that it has reached and really
 * knows, kept between ticks by CoaAiObjectContext below.
 *
 * `firstSpellId` is the first rank of the spell, which is what tells two ranks of the
 * same ability apart from two different abilities: only the highest rank reached is
 * ever cast.
 */
struct CoaKnownAbility
{
    SpellInfo const* info;
    uint16 kind;
    uint32 dispelMask;
    uint32 firstSpellId;
};

/*
 * Fallback combat context for Conquest of Azeroth custom classes (ids 12 and above).
 *
 * mod-playerbots ships one hand written context per vanilla class. A character whose
 * class id is outside 1-11 falls through to the plain AiObjectContext, which carries
 * movement, questing and social behaviour but no combat rotation at all: such bots
 * greet, follow and hand in quests, then stand still in a fight.
 *
 * Rather than writing one context per custom class (each vanilla one is roughly two
 * thousand lines), this context rotates through the class abilities listed in
 * ascension_custom_class_spell that the bot has learned. It therefore covers every
 * custom class at once, including any the server author adds later, at the cost of
 * not knowing class specific rotations.
 */
class CoaAiObjectContext : public AiObjectContext
{
public:
    CoaAiObjectContext(PlayerbotAI* botAI);

    static void BuildSharedContexts();
    static void BuildSharedStrategyContexts(SharedNamedObjectContextList<Strategy>& strategyContexts);
    static void BuildSharedActionContexts(SharedNamedObjectContextList<Action>& actionContexts);
    static void BuildSharedTriggerContexts(SharedNamedObjectContextList<Trigger>& triggerContexts);
    static void BuildSharedValueContexts(SharedNamedObjectContextList<UntypedValue>& valueContexts);

    static SharedNamedObjectContextList<Strategy> sharedStrategyContexts;
    static SharedNamedObjectContextList<Action> sharedActionContexts;
    static SharedNamedObjectContextList<Trigger> sharedTriggerContexts;
    static SharedNamedObjectContextList<UntypedValue> sharedValueContexts;

    // Spells this bot set aside after a failure that will not clear on the next tick (wrong
    // target state, shapeshift...), with the time they may be tried again. One per bot, only
    // touched by the bot's own AI update, so no locking.
    std::unordered_map<uint32, time_t> benchedSpells;

    // The bot's kit: the abilities of its class it has reached and knows, without any
    // filter on what they do.
    //
    // Ten actions and triggers ask for a filtered view of this list on every tick of
    // every bot (isUseful of the attack, area, three heals, taunt, defensive and dispel
    // actions, the two triggers, then Execute). Building it walks the ~350 abilities of
    // the class with one Player::HasSpell and one SpellMgr::GetSpellInfo each, so it was
    // ~3500 lookups per bot and per tick, all giving the same answer. It is now built
    // once and filtered.
    //
    // Rebuilt when the bot's level or the size of its spell book changes, and in any
    // case after CoaKitLifeSeconds, so that a change neither of those two sees (a spell
    // learned and another unlearned between two ticks) cannot be missed for long.
    //
    // Like benchedSpells: one per bot, only touched by that bot's own AI update, so no
    // locking.
    std::vector<CoaKnownAbility> knownKit;
    time_t knownKitBuilt = 0;   // 0 while the kit has never been built
    uint32 knownKitSpells = 0;  // size of the spell book when it was built
    uint8 knownKitLevel = 0;
};

#endif
