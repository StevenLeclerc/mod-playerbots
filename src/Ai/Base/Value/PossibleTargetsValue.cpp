/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PossibleTargetsValue.h"
#include "AreaDefines.h"
#include "AttackersValue.h"
#include "CoaDirecteur.h"
#include "CellImpl.h"
#include "DBCStructure.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"
#include "Unit.h"

// Level difference thresholds for attack probability
constexpr int32 EXTREME_LEVEL_DIFF = 5;  // Don't attack if enemy is this much higher
constexpr int32 HIGH_LEVEL_DIFF = 4;     // 25% chance at +/- this difference
constexpr int32 MID_LEVEL_DIFF = 3;      // 50% chance at +/- this difference
constexpr int32 LOW_LEVEL_DIFF = 2;      // 75% chance at +/- this difference

// Time window for deterministic attack decisions
constexpr uint32 ATTACK_DECISION_TIME_WINDOW = 2 * MINUTE;

// 64 bit FNV-1a hash constants
constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

void PossibleTargetsValue::FindUnits(std::list<Unit*>& targets)
{
    Acore::AnyUnfriendlyUnitInObjectRangeCheck u_check(bot, bot, range);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, targets, u_check);
    Cell::VisitObjects(bot, searcher, range);
}

bool PossibleTargetsValue::AcceptUnit(Unit* unit)
{
    if (!AttackersValue::IsPossibleTarget(unit, bot, range))
        return false;

    // CoA -- Directeur, compagnons (27/09, revue M3) : le PACTE couvre aussi le
    // familier, le gardien et le charme d'un joueur pacte (le joueur lui-meme
    // est traite plus bas, dans la branche joueur). Sinon : le compagnon mord
    // le familier, le joueur riposte, et l'autodefense rend le combat legitime.
    // Riposte permise : un familier qui attaque le bot reste une cible. Sans
    // pacte : un chargement atomique.
    if (!unit->IsPlayer())
    {
        Player* maitre = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (maitre && CoaDirecteur::Pacte(bot->GetGUID().GetCounter(), maitre->GetGUID().GetCounter()) &&
            !(bot->IsInCombat() && bot->GetVictim() == unit) && bot->getAttackerForHelper() != unit &&
            !bot->InBattleground() && !bot->InArena())
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ECARTE_PACTE);
            return false;
        }
    }

    // Level-based PvP restrictions
    if (unit->IsPlayer())
    {
        // Self-defense - always allow fighting back
        if (bot->IsInCombat() && bot->GetVictim() == unit)
            return true;  // Already fighting

        Unit* botAttacker = bot->getAttackerForHelper();
        if (botAttacker)
        {
            if (botAttacker == unit)
                return true;  // Enemy attacking

            if (botAttacker->IsPet())
            {
                Unit* petOwner = botAttacker->GetOwner();
                if (petOwner && petOwner == unit)
                    return true;  // Enemy's pet attacking
            }
        }

        // CoA -- Directeur, lot D2a : verbe `treve` (SPEC-directeur-D2.md § 4.3).
        // Place APRES l'autodefense (la riposte reste permise, un combat deja
        // engage n'est pas interrompu) et AVANT les exemptions champ de
        // bataille, duel et capitales, qui rendent true : placee apres, la
        // treve ne mordrait pas en ville. Champ de bataille, arene et duel sont
        // donc testes ici.
        // SousTreve d'abord : sans treve, c'est un seul chargement atomique.
        if (CoaDirecteur::SousTreve(unit->GetGUID().GetCounter()) && !bot->InBattleground() &&
            !bot->InArena() && !(bot->duel && bot->duel->Opponent == unit))
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ECARTE_TREVE);
            return false;
        }

        // CoA -- Directeur, compagnons (27/09, option Directeur.Compagnons) :
        // PACTE. Un bot amene vers un joueur par `peupler` (toute presence :
        // etapes, finale, compagnon `C-`) ne convoite ni ce joueur ni les
        // autres bots pactes pour ce joueur (les deux portent souvent le FFA,
        // qui les rend hostiles hors groupe, Unit.cpp:7473). Meme
        // place et memes exceptions que la treve : apres l'autodefense (la
        // riposte reste permise), avant les exemptions qui rendent true.
        // Pacte d'abord teste sur son compteur : sans pacte, un chargement
        // atomique.
        if (CoaDirecteur::Pacte(bot->GetGUID().GetCounter(), unit->GetGUID().GetCounter()) &&
            !bot->InBattleground() && !bot->InArena() && !(bot->duel && bot->duel->Opponent == unit))
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ECARTE_PACTE);
            return false;
        }

        // CoA -- Directeur, lot D2c : verbe `suspendre_mercenaire` (SPEC § 5.2,
        // point 1). La garde porte sur le BOT, pas sur le candidat : un
        // mercenaire suspendu ne convoite AUCUN joueur. Placee dans le parent,
        // elle couvre « nearest enemy players » (donc la chasse et l'embuscade)
        // ET le grind du mode RPG, qui sinon proposerait des joueurs FFA.
        // Meme place que la treve, pour les memes raisons : apres
        // l'autodefense (qui a deja rendu true pour qui l'attaque), avant les
        // exemptions qui rendent true. Champ de bataille, arene et duel restent
        // permis : ce n'est pas de la convoitise.
        // MercenaireSuspendu d'abord : sans suspension, un chargement atomique.
        if (CoaDirecteur::MercenaireSuspendu(bot->GetGUID().GetCounter()) && !bot->InBattleground() &&
            !bot->InArena() && !(bot->duel && bot->duel->Opponent == unit))
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_ECARTE_SUSPENSION);
            return false;
        }

        // Skip restrictions in BG/Arena
        if (bot->InBattleground() || bot->InArena())
            return true;

        // Skip restrictions if in duel with this player
        if (bot->duel && bot->duel->Opponent == unit)
            return true;

        // Capital cities - no restrictions
        uint32 zoneId = bot->GetZoneId();
        bool inCapitalCity = (zoneId == AREA_STORMWIND_CITY ||
                            zoneId == AREA_IRONFORGE        ||
                            zoneId == AREA_DARNASSUS        ||
                            zoneId == AREA_THE_EXODAR       ||
                            zoneId == AREA_ORGRIMMAR        ||
                            zoneId == AREA_THUNDER_BLUFF    ||
                            zoneId == AREA_UNDERCITY        ||
                            zoneId == AREA_SILVERMOON_CITY);

        if (inCapitalCity)
            return true;

        // CoA -- Directeur, lot D4c (SPEC-directeur-D4.md § 6.3, point 6) : le
        // filtre probabiliste de niveau est saute pour un chasseur designe face
        // a SON prime (le service l'a choisi a [L - 2, L + 3], et le hachage
        // FNV-1a lui ferait sinon refuser son unique proie 25 a 75 % du temps).
        // Place APRES treve, pacte et suspension (qui rendent faux plus haut).
        // Sans chasseur : un chargement atomique.
        uint32 primeGuid = 0;
        if (CoaDirecteur::ProieImposee(bot->GetGUID().GetCounter(), &primeGuid) &&
            unit->GetGUID().GetCounter() == primeGuid)
            return true;

        // Level difference check
        int32 levelDifference = unit->GetLevel() - bot->GetLevel();
        int32 absLevelDifference = std::abs(levelDifference);

        // Extreme difference - do not attack
        if (levelDifference >= EXTREME_LEVEL_DIFF)
            return false;

        // Calculate attack chance based on level difference
        uint32 attackChance = 100; // Default 100%: Bot and target's levels are very close

        // There's a chance a bot might gank on an extremly low target
        if ((absLevelDifference < EXTREME_LEVEL_DIFF && absLevelDifference >= HIGH_LEVEL_DIFF) ||
            levelDifference <= -EXTREME_LEVEL_DIFF)
            attackChance = 25;

        else if (absLevelDifference < HIGH_LEVEL_DIFF && absLevelDifference >= MID_LEVEL_DIFF)
            attackChance = 50;

        else if (absLevelDifference < MID_LEVEL_DIFF && absLevelDifference >= LOW_LEVEL_DIFF)
            attackChance = 75;

        // If probability check needed, use deterministic hash-based decision
        if (attackChance < 100)
        {
            // Decisions remain stable for ATTACK_DECISION_TIME_WINDOW.
            time_t timeWindow = time(nullptr) / ATTACK_DECISION_TIME_WINDOW;

            // FNV-1a hash used to deterministically convert botGUID, targetGUID, and timeWindow
            // into a consistent percentage chance without needing to cache previous decisions.
            // See: http://www.isthe.com/chongo/tech/comp/fnv/
            uint64_t hash = FNV_OFFSET_BASIS;

            // Diffuse bot GUID, target GUID, and time window into the hash
            hash ^= bot->GetGUID().GetRawValue();
            hash *= FNV_PRIME;
            hash ^= unit->GetGUID().GetRawValue();
            hash *= FNV_PRIME;
            hash ^= static_cast<uint64_t>(timeWindow);
            hash *= FNV_PRIME;

            // Convert hash to 0-99 range and compare against attack chance percentage.
            // Ex: attackChance=75: hash 0-74 = attack (75%), hash 75-99 = don't attack (25%)
            return (hash % 100) < attackChance;
        }
    }

    return true;
}

void PossibleTriggersValue::FindUnits(std::list<Unit*>& targets)
{
    Acore::AnyUnfriendlyUnitInObjectRangeCheck u_check(bot, bot, range);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, targets, u_check);
    Cell::VisitObjects(bot, searcher, range);
}

bool PossibleTriggersValue::AcceptUnit(Unit* unit)
{
    if (!unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        return false;

    Unit::AuraEffectList const& aurasPeriodicTriggerSpell =
        unit->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    Unit::AuraEffectList const& aurasPeriodicTriggerWithValueSpell =
        unit->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE);
    for (Unit::AuraEffectList const& list : {aurasPeriodicTriggerSpell, aurasPeriodicTriggerWithValueSpell})
    {
        for (auto i = list.begin(); i != list.end(); ++i)
        {
            AuraEffect* aurEff = *i;
            SpellInfo const* spellInfo = aurEff->GetSpellInfo();
            if (!spellInfo)
                continue;
            SpellInfo const* triggerSpellInfo =
                sSpellMgr->GetSpellInfo(spellInfo->Effects[aurEff->GetEffIndex()].TriggerSpell);
            if (!triggerSpellInfo)
                continue;
            for (int j = 0; j < MAX_SPELL_EFFECTS; j++)
            {
                if (triggerSpellInfo->Effects[j].Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                    return true;
            }
        }
    }
    return false;
}
