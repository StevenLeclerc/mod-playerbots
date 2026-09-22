/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MULTIPLIER_H
#define PLAYERBOTS_MULTIPLIER_H

#include "AiObject.h"

#include <list>

class Action;
class ActionBasket;
class PlayerbotAI;

class Multiplier : public AiNamedObject
{
public:
    Multiplier(PlayerbotAI* botAI, std::string const name) : AiNamedObject(botAI, name) {}
    virtual ~Multiplier() {}

    virtual float GetValue([[maybe_unused]] Action* action) { return 1.0f; }

    /**
     * @brief Called once per tick with the whole candidate set, before any is popped
     *
     * GetValue() only sees the actions the engine actually tried, and
     * DoNextAction stops at the first one that executes: usually exactly one
     * per tick. A multiplier that needs to know what the alternatives WERE
     * must be told separately, which is what this is for. No effect by
     * default, and it must not modify anything.
     */
    virtual void ObserveQueue([[maybe_unused]] std::list<ActionBasket*> const& candidats) {}
};

#endif
