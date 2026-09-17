/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CustomStrategy.h"
#include "Playerbots.h"
#include <regex>
#include <stdexcept>

std::map<std::string, std::string> CustomStrategy::actionLinesCache;

NextAction toNextAction(std::string const action)
{
    // The relevance is what follows the LAST '!', and only when that is a
    // number. Splitting on every '!' assumed no action name contains one.
    //
    // Conquest of Azeroth has several that do: "Tavern Brawl!" (Barbarian),
    // "Zap!" and "My Greatest Invention!" (Tinker), "Cheers!" (Barbarian).
    // "cast melee::Tavern Brawl!!88" split into three tokens, which failed the
    // size checks below and threw the whole line away.
    //
    // For every line without a '!' in the name this behaves exactly as before.
    if (action.empty())
        throw std::invalid_argument("Invalid action");

    size_t const separator = action.rfind('!');
    if (separator != std::string::npos && separator + 1 < action.size())
    {
        std::string const relevance = action.substr(separator + 1);
        if (relevance.find_first_not_of("0123456789.+-") == std::string::npos)
        {
            std::string const name = action.substr(0, separator);
            if (name.empty())
                throw std::invalid_argument("Invalid action");

            return NextAction(name, atof(relevance.c_str()));
        }
    }

    return NextAction(action, ACTION_NORMAL);
}

std::vector<NextAction> toNextActionArray(const std::string actions)
{
    const std::vector<std::string> tokens = split(actions, ',');
    std::vector<NextAction> res = {};

    for (std::string const& token : tokens)
        res.push_back(toNextAction(token));

    return res;
}

TriggerNode* toTriggerNode(std::string const actionLine)
{
    std::vector<std::string> tokens = split(actionLine, '>');
    if (tokens.size() == 2)
        return new TriggerNode(tokens[0], toNextActionArray(tokens[1]));

    LOG_ERROR("playerbots", "Invalid action line {}", actionLine.c_str());
    return nullptr;
}

CustomStrategy::CustomStrategy(PlayerbotAI* botAI) : Strategy(botAI), Qualified() {}

void CustomStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    if (actionLines.empty())
    {
        if (actionLinesCache[qualifier].empty())
        {
            LoadActionLines((uint32)botAI->GetBot()->GetGUID().GetCounter());
            if (actionLines.empty())
                LoadActionLines(0);
        }
        else
        {
            std::vector<std::string> tokens = split(actionLinesCache[qualifier], '\n');
            std::regex tpl("\\(nullptr,\\s*'.+',\\s*'(.+)'\\)(,|;)");
            for (std::vector<std::string>::iterator i = tokens.begin(); i != tokens.end(); ++i)
            {
                std::string const line = *i;
                for (std::sregex_iterator j = std::sregex_iterator(line.begin(), line.end(), tpl);
                     j != std::sregex_iterator(); ++j)
                {
                    std::smatch match = *j;
                    std::string const actionLine = match[1].str();
                    if (!actionLine.empty())
                        actionLines.push_back(actionLine);
                }
            }
        }
    }

    for (std::vector<std::string>::iterator i = actionLines.begin(); i != actionLines.end(); ++i)
    {
        if (TriggerNode* tn = toTriggerNode(*i))
            triggers.push_back(tn);
    }
}

void CustomStrategy::LoadActionLines(uint32 owner)
{
    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_CUSTOM_STRATEGY_BY_OWNER_AND_NAME);
    stmt->SetData(0, owner);
    stmt->SetData(1, qualifier);
    PreparedQueryResult result = PlayerbotsDatabase.Query(stmt);
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            std::string const action = fields[1].Get<std::string>();
            actionLines.push_back(action);
        } while (result->NextRow());
    }
}

void CustomStrategy::Reset()
{
    actionLines.clear();
    actionLinesCache[qualifier].clear();
}
