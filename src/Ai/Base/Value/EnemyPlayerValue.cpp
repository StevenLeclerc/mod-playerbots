/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EnemyPlayerValue.h"
#include "CombatManager.h"
#include "MercenaryRewards.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Vehicle.h"

namespace
{
// --- PvP mercenaire (CoA) ---------------------------------------------------
// Combien de mercenaires ont DEJA engage cette cible ? Chaque mercenaire decide
// seul : sans ce compte, trois d'entre eux a portee du meme joueur repondent
// trois fois oui, et c'est la mort assuree pour qui joue seul.
//
// Le compte se lit SUR LA CIBLE, dans ses propres references de combat, la
// meme source que EnemyPlayerValue::Calculate emploie deja. Il n'y a donc
// aucun conteneur global a tenir, et aucun verrou a prendre : c'est ce qui
// distingue ce garde-fou du motif qui a fait tomber le monde (P-050).
//
// Le plafond ne rompt jamais un combat en cours : un mercenaire deja engage
// retrouve sa cible par les references de combat (etape 1 de Calculate), qui
// ne passent pas par ce filtre. Il n'interdit que les ralliements.
bool MercenaryAttackSlotFree(Player* bot, Player* enemy)
{
    uint32 const cap = sPlayerbotAIConfig.wildPvpMaxAttackersPerTarget;
    if (!cap)
        return true;

    uint32 engaged = 0;
    for (auto const& [guid, combatRef] : enemy->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* other = combatRef->GetOther(enemy);
        if (!other || other == bot || !other->IsPlayer())
            continue;

        Player* attacker = other->ToPlayer();
        if (!GET_PLAYERBOT_AI(attacker))
            continue;

        if (!sPlayerbotAIConfig.IsMercenary(attacker->GetGUID().GetRawValue()))
            continue;

        if (++engaged >= cap)
            return false;
    }

    return true;
}
}

bool NearestEnemyPlayersValue::AcceptUnit(Unit* unit)
{
    // Apply parent's filtering first (includes level difference checks)
    if (!PossibleTargetsValue::AcceptUnit(unit))
        return false;

    bool inCannon = botAI->IsInVehicle(false, true);
    Player* enemy = dynamic_cast<Player*>(unit);

    // --- PvP mercenaire (CoA) ---------------------------------------------
    // Regle normale : faction opposee ET cible marquee PvP. Un bot mercenaire
    // s'en affranchit et s'en prend a toute cible FFA, y compris de sa propre
    // faction et y compris un autre bot : c'est ce qui fait vivre la guerre
    // sans le joueur, au lieu de la concentrer sur lui.
    // Le coeur reste seul juge de la legalite du coup ; on ne fait ici que
    // decider de la convoitise.
    bool mercenaire = enemy && sPlayerbotAIConfig.wildPvpEnabled &&
                      bot->IsFFAPvP() && enemy->IsFFAPvP() &&
                      sPlayerbotAIConfig.IsMercenary(bot->GetGUID().GetRawValue()) &&
                      (sPlayerbotAIConfig.wildPvpBotsFightBots ||
                       !GET_PLAYERBOT_AI(enemy));

    // Deux bornes, dans cet ordre : qui vient de mourir n'est pas reconvoite
    // tout de suite (sinon le mercenaire attend au cimetiere), et on ne se
    // rallie pas a une curee deja en cours.
    //
    // Elles se posent sur le BOT, pas sur la branche qui a laisse passer la
    // cible. Les rattacher a `mercenaire` seul laissait un trou : un
    // mercenaire qui attaque un joueur de faction opposee marque PvP passe par
    // la branche ordinaire, et echappait donc aux deux bornes. Un joueur seul
    // pouvait encore y etre submerge.
    if (enemy && sPlayerbotAIConfig.wildPvpEnabled &&
        sPlayerbotAIConfig.IsMercenary(bot->GetGUID().GetRawValue()) &&
        (MercenaryRewards::instance().IsUnderTruce(enemy->GetGUID().GetRawValue()) ||
         !MercenaryAttackSlotFree(bot, enemy)))
    {
        return false;
    }

    if (enemy && (mercenaire || (botAI->IsOpposing(enemy) && enemy->IsPvP())) &&
        !sPlayerbotAIConfig.IsPvpProhibited(enemy->GetZoneId(), enemy->GetAreaId()) &&
        !enemy->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NON_ATTACKABLE_2) &&
        ((inCannon || !enemy->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE))) &&
        /*!enemy->HasStealthAura() && !enemy->HasInvisibilityAura()*/ enemy->CanSeeOrDetect(bot) &&
        !(enemy->HasSpiritOfRedemptionAura()))
    {
        // If with master, only attack if master is PvP flagged.
        // Un mercenaire libre (sans maitre) n'est pas concerne ; un mercenaire
        // recrute dans un groupe reste tenu par cette regle, sinon il
        // declencherait des combats que son maitre n'a pas choisis.
        Player* master = botAI->GetMaster();
        if (master && !master->IsPvP() && !master->IsFFAPvP())
            return false;

        return true;
    }

    return false;
}

Unit* EnemyPlayerValue::Calculate()
{
    bool controllingCannon = false;
    bool controllingVehicle = false;
    if (Vehicle* vehicle = bot->GetVehicle())
    {
        VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
        if (!seat || !seat->CanControl())  // not in control of vehicle so cant attack anyone
            return nullptr;
        VehicleEntry const* vi = vehicle->GetVehicleInfo();
        if (vi && vi->m_flags & VEHICLE_FLAG_FIXED_POSITION)
            controllingCannon = true;
        else
            controllingVehicle = true;
    }

    // 1. Check units we are currently in PvP combat with.
    std::vector<Unit*> targets;
    Unit* pVictim = bot->GetVictim();
    for (auto const& [guid, combatRef] : bot->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* pTarget = combatRef->GetOther(bot);
        if (!pTarget || pTarget == pVictim || !pTarget->IsPlayer() || !pTarget->CanSeeOrDetect(bot) ||
            !bot->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL))
            continue;

        if ((bot->GetTeamId() == TEAM_HORDE && pTarget->HasAura(23333)) ||
            (bot->GetTeamId() == TEAM_ALLIANCE && pTarget->HasAura(23335)))
            return pTarget;

        targets.push_back(pTarget);
    }

    if (!targets.empty())
    {
        std::sort(targets.begin(), targets.end(),
                  [&](Unit const* pUnit1, Unit const* pUnit2)
                  { return bot->GetDistance(pUnit1) < bot->GetDistance(pUnit2); });

        return *targets.begin();
    }

    // 2. Find enemy player in range.

    GuidVector players = AI_VALUE(GuidVector, "nearest enemy players");
    float const maxAggroDistance = GetMaxAttackDistance();
    for (auto const& gTarget : players)
    {
        Unit* pUnit = botAI->GetUnit(gTarget);
        if (!pUnit)
            continue;

        Player* pTarget = dynamic_cast<Player*>(pUnit);
        if (!pTarget)
            continue;

        if (pTarget == pVictim)
            continue;

        if (bot->GetTeamId() == TEAM_HORDE)
        {
            if (pTarget->HasAura(23333))
                return pTarget;
        }
        else
        {
            if (pTarget->HasAura(23335))
                return pTarget;
        }

        // Aggro weak enemies from further away.
        // If controlling mobile vehicle only agro close enemies (otherwise will never reach objective)
        uint32 const aggroDistance = controllingVehicle                                               ? 5.0f
                                     : (controllingCannon || bot->GetHealth() > pTarget->GetHealth()) ? maxAggroDistance
                                                                                                      : 20.0f;
        if (!bot->IsWithinDist(pTarget, aggroDistance))
            continue;

        if (bot->IsWithinLOSInMap(pTarget) &&
            (controllingCannon || (fabs(bot->GetPositionZ() - pTarget->GetPositionZ()) < 30.0f)))
            return pTarget;
    }

    // 3. Check party attackers.

    if (Group* pGroup = bot->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->GetSource())
            {
                if (pMember == bot)
                    continue;

                if (ServerFacade::instance().GetDistance2d(bot, pMember) > 30.0f)
                    continue;

                if (Unit* pAttacker = pMember->getAttackerForHelper())
                    if (pAttacker->IsPlayer() && bot->IsWithinDist(pAttacker, maxAggroDistance * 2.0f) &&
                        bot->IsWithinLOSInMap(pAttacker) && pAttacker != pVictim && pAttacker->CanSeeOrDetect(bot))
                        return pAttacker;
            }
        }
    }

    return nullptr;
}

float EnemyPlayerValue::GetMaxAttackDistance()
{
    if (!bot->GetBattleground())
        return 60.0f;

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return 40.0f;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    if (bgType == BATTLEGROUND_IC)
    {
        if (botAI->IsInVehicle(false, true))
            return 120.0f;
    }

    return 40.0f;
}
