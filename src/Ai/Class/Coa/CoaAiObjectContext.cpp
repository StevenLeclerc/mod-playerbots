/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaLayaEtat.h"
#include "CoaLayaOracle.h"
#include "CoaAiObjectContext.h"
#include "CoaChasse.h"
#include "CoaArene1v1.h"
#include "CoaBancSoin.h"
#include "CoaRegistreCombat.h"

#include "Action.h"
#include "Battleground.h"
#include "CoaSpecialization.h"
#include "ChooseTargetActions.h"
#include "LootObjectStack.h"
#include "MovementActions.h"
#include "Multiplier.h"
#include "ObjectAccessor.h"
#include "Timer.h"
#include "CombatStrategy.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "NamedObjectContext.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "ServerFacade.h"
#include "SmartEnum.h"
#include "SpellMgr.h"
#include "Strategy.h"
#include "Trigger.h"
#include "mod-ascension-compat/src/AscensionSpecialization.h"
#include "mod-ascension-compat/src/AscensionGuardianStandardData.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <type_traits>
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
 * IA SOINS/SURVIE, LOT 1 (16/10/2026, coa-project/docs/DIAG-banc-soins-sorts-jamais-lances-20261016.md et
 * DIAG-banc-soins-rotation-20261016.md). Les seize bits de AbilityKind sont tous pris : ce que Classify sait de plus sur
 * un sort va dans ces marques, portees a cote de `kind` jusqu'aux actions (CoaAbility, CoaKnownAbility, Usable). Elles ne
 * sont posees que par le classement du lot 1, et KnownKit ne transmet celles du lot 1 que sous AiPlayerbot.CoaIa.Classement
 * (les autres servent aux lots 3 et 4, chacun sous son option).
 */
enum CoaMarque : uint16
{
    MARQUE_SOIN_SOI_SEUL   = 0x0001,  // sa partie soin ne vise que le lanceur : jamais offert pour soigner autrui
    MARQUE_PAS_SUR_SOI     = 0x0002,  // ne se lance jamais sur le lanceur (SPELL_ATTR1_EXCLUDE_CASTER, cible explicite ennemie)
    MARQUE_VOL_DE_VIE      = 0x0004,  // des degats qui soignent le lanceur (HEALTH_LEECH, PERIODIC_LEECH, drain)
    MARQUE_DEFENSIF_FAIBLE = 0x0008,  // KIND_DEFENSIVE ne tient qu'a une reduction de degats subis de moins de 10 %
    MARQUE_IMMOBILISE      = 0x0010,  // enracine son propre lanceur (aura MOD_ROOT sur soi)
    // I2 du 17/10 (CoaSurcharge::marques seulement, jamais par Classify) : rend de la mana au groupe (balise du Tinker).
    MARQUE_REGEN_MANA      = 0x0020,
    // Internes a Classify (relevees en descendant dans les sorts declenches), effacees avant la fin :
    MARQUE_I_SOIN_ALLIE    = 0x0100,  // un effet de soin vise un allie
    MARQUE_I_DEF_FORT      = 0x0200,  // KIND_DEFENSIVE pose par un effet qui n'est pas une reduction de moins de 10 %
    MARQUE_I_DRAIN         = 0x0400,  // HEALTH_LEECH ou PERIODIC_LEECH
    MARQUE_I_INTERNES      = 0x0700
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
    // Lot 1 (16/10/2026) : `kind` est le classement du lot 1 (regles nouvelles et surcharges), `kindAncien` celui d'avant,
    // rendu tel quel quand AiPlayerbot.CoaIa.Classement vaut 0. `marques` : voir CoaMarque.
    uint16 kindAncien = 0;
    uint16 marques = 0;
    // Revue du 16/10 : la surcharge de ce sort ne vaut que pour une specialisation de role Heal (CoaSurcharge::soigneur) ;
    // toute autre specialisation garde kindAncien.
    bool surchargeSoigneur = false;
};

struct ClassKit
{
    std::vector<CoaAbility> abilities;
    uint16 kinds = 0;        // every kind some ability of the class has
    uint16 kindsAncien = 0;  // la meme chose, classement d'avant le lot 1
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

// Lot 1 : la valeur d'un effet sans lanceur et sans tirage, comme SpellEffectInfo::CalcValue la rend au plus bas
// (SpellInfo.cpp : DieSides 0 -> BasePoints ; sinon BasePoints + 1 au minimum du tirage). Defiance 806228 : -81 + 1 = -80 ;
// Righteous Tempest 807274 et Zenith 712325 : -1 + 1 = 0.
int32 CoaValeurDeBase(SpellEffectInfo const& effect)
{
    return effect.BasePoints + (effect.DieSides ? 1 : 0);
}

// Lot 3 (DIAG survie, Righteous Tempest 807274 : un tourbillon de degats a « -1 % » de degats subis, lance 106 fois par
// « coa defensive » en 12 essais) : une reduction de degats subis de moins de 10 % ne fait pas un defensif. Les autres
// auras defensives (bouclier, parade, immunite...) ne sont pas mesurees ici.
bool CoaDefensifFaible(SpellEffectInfo const& effect)
{
    return effect.ApplyAuraName == SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN && CoaValeurDeBase(effect) > -10;
}

// Lot 1 (DIAG sorts, C8) : la cible explicite du sort est une unite ENNEMIE, et jamais un allie. Valkyr's Calling 520024
// (TargetA 6) etait lance par « coa defensive » sur le bot lui-meme : BAD_TARGETS puis banc de 20 s.
bool CoaCibleExpliciteEnnemie(SpellInfo const* info)
{
    uint32 const masque = info->GetExplicitTargetMask();
    return (masque & TARGET_FLAG_UNIT_ENEMY) &&
           !(masque & (TARGET_FLAG_UNIT_ALLY | TARGET_FLAG_UNIT_PARTY | TARGET_FLAG_UNIT_RAID));
}

/*
 * Lot 1 (DIAG rotation § 5.5, Babify 520499 du Chronomancer) : le sort SOIGNE SA CIBLE ENNEMIE. Babify pose sur l'ennemi un
 * etourdissement, une transformation et une aura 23 qui declenche 801260 « Babify Regen », HEAL_PCT sur TargetA 6 : range
 * dans la rotation d'attaque (KIND_HOSTILE | KIND_CONTROL), il rendait 183 000 points de vie a la creature du banc. Un tel
 * sort n'est jamais lance sur un ennemi par la rotation : Classify lui retire toute marque.
 *
 * Regle lue dans les donnees, sans id : un effet de soin (HEAL, HEAL_PCT, HEAL_MAX_HEALTH, aura PERIODIC_HEAL) dont la
 * cible est une unite ennemie (TARGET_UNIT_TARGET_ENEMY, zone ennemie en A ou en B) et aucune cible alliee, sur le sort
 * ou un sort qu'il declenche par TRIGGER_SPELL ou par aura PERIODIC_TRIGGER_SPELL (deux niveaux). Les boucliers poses
 * sur un ennemi n'y sont PAS : releve du 16/10 dans Spell.dbc, ce sont des marqueurs de debuff a 1 point (Rusty Shiv
 * 561315, Genesis 500501...), et les immunites (Imprison 801067) sont des controles voulus. Un soin a destination
 * ennemie mais sur zone alliee (Serpent's Fang 503936 : TargetA 53, TargetB 31) soigne les allies : non concerne. Une aura
 * DUMMY qui porte un TriggerSpell (Soul Shock 803989) n'est pas suivie : rien ne dit qu'elle le declenche.
 */
bool CoaSoigneLaCibleEnnemie(SpellInfo const* info, uint8 profondeur = 0)
{
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (!effect.IsEffect())
            continue;
        uint32 const targetA = effect.TargetA.GetTarget();
        uint32 const targetB = effect.TargetB.GetTarget();
        bool const aura = IsAuraEffect(effect);
        bool const soin = effect.Effect == SPELL_EFFECT_HEAL || effect.Effect == SPELL_EFFECT_HEAL_PCT ||
                          effect.Effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
                          (aura && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL);
        bool const ennemie = targetA == TARGET_UNIT_TARGET_ENEMY || IsEnemyAreaTarget(targetA) || IsEnemyAreaTarget(targetB);
        if (soin && ennemie && !IsAllyTarget(targetA) && !IsAllyTarget(targetB))
            return true;
        bool const suivi = effect.Effect == SPELL_EFFECT_TRIGGER_SPELL || effect.Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE ||
                           (aura && (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL ||
                                     effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE));
        if (suivi && effect.TriggerSpell && effect.TriggerSpell != info->Id && profondeur < 2)
            if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                if (CoaSoigneLaCibleEnnemie(declenche, profondeur + 1))
                    return true;
    }
    return false;
}

// What a spell does, looking two levels into the spells it triggers: CoA abilities often
// carry their heal, taunt or aura in a triggered spell.
//
// `lot1` (16/10/2026) : applique en plus les regles du lot 1 (AiPlayerbot.CoaIa.Classement) et pose les marques
// (CoaMarque). Faux, Classify rend exactement le classement d'avant : ClassAbilities le calcule aussi, dans kindAncien.
void Classify(SpellInfo const* info, CoaAbility& ability, uint8 depth = 0, bool lot1 = false)
{
    constexpr int32 LongAura = 5 * MINUTE * IN_MILLISECONDS;
    int32 const duration = info->GetMaxDuration();

    // UN SORT QUI NEUTRALISE SON PROPRE LANCEUR N'EST JAMAIS UN BUFF (02/10/2026).
    //
    // Moon Gaze 562723 (Bloodmage, niveau 16, 70 personnages) porte trois auras sur le
    // lanceur : MOD_SHAPESHIFT, DUMMY et MOD_STUN, pour 300 s, retirees seulement en
    // entrant en combat JcJ (AuraInterruptFlags 0x00800000). Sa description : « While
    // active, you are unable to move, attack, or cast spells or abilities. » C'est la
    // bascule vers la barre de Cursed Form, que les bots n'utilisent pas. L'effet DUMMY
    // suffisait pourtant a le classer KIND_BUFF ci-dessous (aura, sur soi, positif,
    // >= 5 min, pas un shapeshift) : « coa buff » le relancait toutes les 270 s, et le
    // bot restait etourdi et immobile, y compris quand une creature l'attaquait. Mesure
    // le 02/10 : 37 personnages avec l'aura 562723 lancee par eux-memes ; les trois sorts
    // les plus refuses de « coa attack failures » sont des sorts Bloodmage refuses
    // STUNNED (Sanguine Rupture x61 467, Scarlet Delirium x57 172, Taldaram's Torment
    // x23 129). Sur tous les sorts connus d'un personnage, Moon Gaze est le seul qui
    // reunisse une aura d'incapacite sur le lanceur et une duree >= 5 min : le filtre ne
    // touche rien d'autre. La racine n'est pas incluse : on lance encore enracine.
    bool selfIncapacitating = false;
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (!effect.IsEffect() || !IsAuraEffect(effect) || effect.TargetA.GetTarget() != TARGET_UNIT_CASTER)
            continue;

        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_MOD_STUN: case SPELL_AURA_MOD_PACIFY: case SPELL_AURA_MOD_SILENCE:
            case SPELL_AURA_MOD_PACIFY_SILENCE: case SPELL_AURA_MOD_FEAR: case SPELL_AURA_MOD_CONFUSE:
                selfIncapacitating = true;
                break;
            default:
                break;
        }
    }

    // UN BOT QUI FLOTTE (Nuit des morts du 09/10/2026, coa-project/docs/EVENEMENTS-groupe.md). Illidan's Guile
    // 806109 (Felsworn, 69 personnages) porte SPELL_AURA_HOVER (106) et SCHOOL_IMMUNITY (39, toutes ecoles) sur le
    // lanceur, 8 s (DurationIndex 31) : « immune to all physical attacks and spells [...] but during that time you
    // cannot attack, move or cast spells ». L'immunite le classait KIND_DEFENSIVE : « coa defensive » le lancait a
    // PV moyens ou bas, et le bot restait suspendu en l'air huit secondes sans frapper -- vu par le commanditaire
    // (« Folcal le bot flottait un peu dans les airs », Folcal Felsworn 40, 66 % de PV releves a 10:09:23 Paris).
    // Releve du 09/10 dans Spell.dbc : parmi les sorts connus d'un personnage, trois portent HOVER (806109, Venomwing
    // Form 520307, Aether Current 531512) ; seul 806109 etait un defensif. Un sort qui fait FLOTTER son lanceur n'est
    // plus un defensif de bot ; le joueur humain le garde, rien n'est retire du jeu.
    bool selfHover = false;
    for (SpellEffectInfo const& effect : info->Effects)
        if (effect.IsEffect() && IsAuraEffect(effect) && effect.TargetA.GetTarget() == TARGET_UNIT_CASTER &&
            effect.ApplyAuraName == SPELL_AURA_HOVER)
            selfHover = true;

    // Only the ability itself: a spell it triggers is not the stance the bot stands in.
    // Same rule for a stance as for a buff: one that incapacitates its caster would be
    // taken out of combat and kept, and the bot with it. No such spell is known by any
    // character today (releve du 02/10 : War Standards 801506, Power Cell 806624 et
    // les autres candidats du DBC ne figurent dans aucun livre de sorts).
    if (!depth && IsStance(info) && !selfIncapacitating)
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

        // Lot 1 (DIAG sorts, C1 : Voodoo Cauldron 804684, effet 27 PERSISTENT_AREA_AURA + aura PERIODIC_HEAL sur
        // TargetA 31) : une aura de zone persistante qui soigne periodiquement est un soin (de zone) periodique.
        // IsAuraEffect ne la compte pas : le soin n'etait pas vu, et le sort sortait de Classify avec kind == 0.
        bool const zonePersistante = lot1 && effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA;
        bool const periodicHeal = (aura || zonePersistante) && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL;
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
            // Lot 1 (DIAG rotation C3-bis, Devour Curse 800402) : la partie soin vise-t-elle un allie ?
            if (lot1 && ally)
                ability.marques |= MARQUE_I_SOIN_ALLIE;
        }

        // Lot 1 : des degats qui soignent le lanceur (vol de vie, lu par la survie du lot 3), et un sort qui enracine son
        // propre lanceur (Ignis Ultimatus 680369 : aura 26 sur soi pendant sa canalisation de 2 s).
        if (lot1 && (effect.Effect == SPELL_EFFECT_HEALTH_LEECH || (aura && effect.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH)))
            ability.marques |= MARQUE_I_DRAIN;
        if (lot1 && aura && self && effect.ApplyAuraName == SPELL_AURA_MOD_ROOT)
            ability.marques |= MARQUE_IMMOBILISE;

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

        if (aura && self && IsDefensiveAura(effect) && duration > 0 && duration < LongAura && !selfHover)
        {
            ability.kind |= KIND_DEFENSIVE;
            if (lot1 && !CoaDefensifFaible(effect))
                ability.marques |= MARQUE_I_DEF_FORT;
            // Lot 1 (DIAG rotation C5) : une aura de zone de groupe ou de raid centree sur le lanceur qui absorbe ou reduit
            // les degats protege tout le groupe : c'est aussi un soin de zone.
            if (lot1 && (effect.Effect == SPELL_EFFECT_APPLY_AREA_AURA_PARTY || effect.Effect == SPELL_EFFECT_APPLY_AREA_AURA_RAID) &&
                (effect.ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB || effect.ApplyAuraName == SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN))
                ability.kind |= KIND_GROUP_HEAL;
        }

        /*
         * Lot 1 (DIAG sorts C1, DIAG survie § 3 B ; DIAG rotation C5). Un bouclier ou une reduction de degats COURTE posee
         * sur un allie choisi ou sur une zone d'allies n'etait ni soin, ni buff (< 5 min), ni defensif (le test ci-dessus
         * exige TARGET_UNIT_CASTER) : kind == ALLY_CAST seul, et aucune action ne le lancait. Void Shield 578115, Blood Veil
         * 572279, Inferno Barrier 582764, Timeguard 804441 (allie choisi, TargetA 21), Forbidden Ritual 502273 (TargetA 56),
         * Spectral Warden 805716 (TargetB 30), Kiss of the Clouds 501446 (par son declenche 500292). Un allie choisi peut
         * etre le lanceur lui-meme (sauf SPELL_ATTR1_EXCLUDE_CASTER : MARQUE_PAS_SUR_SOI), et une zone d'allies centree sur
         * lui le couvre : KIND_DEFENSIVE, que « coa defensive » lance sur le bot. Le bouclier de ZONE est en plus un soin de
         * zone (KIND_GROUP_HEAL), que « coa group heal » pose quand plusieurs membres sont touches.
         */
        if (lot1 && aura && !self && !enemy && IsDefensiveAura(effect) && duration > 0 && duration < LongAura && !selfHover &&
            (IsAllyCastTarget(targetA) || IsAllyAreaTarget(targetA) || IsAllyAreaTarget(targetB)))
        {
            ability.kind |= KIND_DEFENSIVE;
            if (!CoaDefensifFaible(effect))
                ability.marques |= MARQUE_I_DEF_FORT;
            if ((IsAllyAreaTarget(targetA) || IsAllyAreaTarget(targetB)) &&
                (effect.ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB || effect.ApplyAuraName == SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN))
                ability.kind |= KIND_GROUP_HEAL;
        }

        // La furtivite se reconnait a son aura. Elle ressemble a une posture — aucune duree,
        // sur le lanceur seul — mais elle ne doit surtout pas etre portee en permanence :
        // Underwalk (800797) ralentit celui qui la porte, et neuf bots la gardaient en
        // continu, se trainant toute la journee pour rien. C'est une arme de chasse.
        if (aura && self && effect.ApplyAuraName == SPELL_AURA_MOD_STEALTH)
            ability.kind |= KIND_STEALTH;

        // Stances and forms (no duration) are left out: two of them would take turns forever.
        if (aura && (self || ally) && !heal && info->IsPositive() && duration >= LongAura &&
            effect.ApplyAuraName != SPELL_AURA_MOD_SHAPESHIFT && !selfIncapacitating)
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
            {
                // UN SOIN RESERVE AUX PORTEURS D'UNE MARQUE N'EST PAS UN SOIN DU BOT (revue du 11/10, c1). Transfer Life
                // 801530 (Necromancer) declenche, depuis un effet centre sur le lanceur (TARGET_SRC_CASTER), son soin
                // 801545 (HEAL_PCT sur TARGET_UNIT_TARGET_ALLY), mais 801545 exige sur sa cible l'aura Raise Marker 807177
                // (TargetAuraSpell, Spell.dbc) que seuls les serviteurs leves portent -- et le script du module ne garde
                // qu'eux (AscensionNecromancerAbilities.cpp:446-451). Classe soin, le sort etait offert au bot pour SE
                // soigner : 10 s de canalisation sur ses serviteurs. Regle lue dans les donnees, sans id : un sort
                // declenche par un effet TARGET_SRC_CASTER et qui exige une aura sur sa cible n'apporte aucune des marques
                // de soin. Releve du 11/10 (familles 18-40) : 2 capacites concernees, Transfer Life et Sacrifice Undead
                // 805027 (805031, aura exigee Necro Minion 805026) ; Guidance, Shadow Pennant, Darkfallen Lament et
                // Desecrate declenchent depuis une autre cible et gardent leur classement.
                bool const reserveAuxMarques = triggered->TargetAuraSpell && targetA == TARGET_SRC_CASTER;
                CoaAbility sous = ability;
                sous.kind = 0;
                sous.dispelMask = 0;
                sous.marques = 0;
                Classify(triggered, sous, depth + 1, lot1);
                // LOT 20 (13/10/2026, docs/ANALYSE-classes-faibles-arene-20261013.md § 1) : un soin porte par une aura
                // qui DECLENCHE periodiquement un soin est un soin periodique, comme une aura PERIODIC_HEAL. Vital Shield
                // 504849 (Bloodmage) : aura 23 (PERIODIC_TRIGGER_SPELL) -> 804309 (HEAL_PCT 2 % toutes les 5 s), 30 min.
                // Sans la marque, la survie d'arene ne testait pas son aura (test reserve a KIND_HOT) et le relancait a
                // chaque palier : 19 lancers en 54 s, 62 % des sorts du Bloodmage sur le banc du 12/10.
                if (aura && (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL ||
                             effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE) &&
                    (sous.kind & KIND_HEAL))
                    sous.kind |= KIND_HOT;
                if (reserveAuxMarques)
                {
                    sous.kind &= ~uint16(KIND_HEAL | KIND_SELF_HEAL | KIND_GROUP_HEAL | KIND_HOT | KIND_ALLY_CAST);
                    sous.marques &= ~uint16(MARQUE_I_SOIN_ALLIE);
                }
                ability.kind |= sous.kind;
                ability.dispelMask |= sous.dispelMask;
                ability.marques |= sous.marques;
            }
    }

    if (depth || !lot1)
        return;

    // Lot 1, fin du classement du sort lui-meme (pas d'un declenche).
    //
    // DIAG rotation C3-bis (Devour Curse 800402 du Cultist : dissipation sur un allie choisi, TargetA 21, et soin du
    // LANCEUR, TargetA 1) : KIND_ALLY_CAST venait de la dissipation, et CanHealOther l'offrait pour soigner le tank --
    // 206 soins sur 206 tombes sur le Cultist lui-meme, environ 30 % de sa mana de soin. Un soin dont aucun effet de soin
    // ne vise un allie n'est offert que pour le lanceur ; la dissipation garde KIND_ALLY_CAST.
    if ((ability.kind & KIND_HEAL) && (ability.kind & KIND_SELF_HEAL) && !(ability.kind & KIND_GROUP_HEAL) &&
        !(ability.marques & MARQUE_I_SOIN_ALLIE))
        ability.marques |= MARQUE_SOIN_SOI_SEUL;
    // DIAG sorts C8 : Temple Guardian 803796 (SPELL_ATTR1_EXCLUDE_CASTER, BAD_TARGETS x109 au banc sur soi) et Valkyr's
    // Calling 520024 (cible ennemie) etaient lances par « coa defensive » sur le bot.
    if (info->HasAttribute(SPELL_ATTR1_EXCLUDE_CASTER) || CoaCibleExpliciteEnnemie(info))
        ability.marques |= MARQUE_PAS_SUR_SOI;
    if ((ability.kind & KIND_DAMAGE) && ((ability.marques & MARQUE_I_DRAIN) || (ability.kind & KIND_SELF_HEAL)))
        ability.marques |= MARQUE_VOL_DE_VIE;
    if ((ability.kind & KIND_DEFENSIVE) && !(ability.marques & MARQUE_I_DEF_FORT))
        ability.marques |= MARQUE_DEFENSIF_FAIBLE;
    ability.marques &= ~uint16(MARQUE_I_INTERNES);

    // Babify et les sorts qui soignent leur cible ennemie : voir CoaSoigneLaCibleEnnemie.
    if (CoaSoigneLaCibleEnnemie(info))
    {
        ability.kind = 0;
        ability.dispelMask = 0;
        ability.marques = 0;
    }
}

/*
 * LOT 1 -- SURCHARGES DE CLASSEMENT (16/10/2026). Ce que Classify ne peut pas lire dans les effets : sorts reecrits par
 * mod-ascension-compat au chargement (l'effet fait par script ne se voit plus), sorts a double usage, auras dont le role
 * tient a un declencheur que Classify ne suit pas. Appliquees APRES Classify, au classement du lot 1 seulement (`kind`,
 * jamais `kindAncien`), a l'id cite et a tous les rangs du meme sort (meme premier rang). `force` : reduction de degats
 * equivalente en %, lue par le tri des defensifs du lot 3 quand l'effet n'est plus lisible (0 : calculee).
 * Sources : DIAG-banc-soins-sorts-jamais-lances-20261016.md (§ 2, § 3) et DIAG-banc-soins-survie-20261016.md (§ 3).
 */
struct CoaSurcharge
{
    uint32 spellId;
    uint16 ajoute;
    uint16 retire;
    uint8 force;
    // Revue du 16/10 : sort a double usage FORCE EN SOIN (C3). La surcharge ne vaut que pour une specialisation de role
    // Heal (GetCoaRole, CoaSpecStrategies.h) ; les autres gardent le classement d'avant (kindAncien), donc leur usage
    // offensif, et ne le lancent jamais sur elles-memes (sorts hostiles, exclus des soins sur soi).
    bool soigneur = false;
    // I2 du 17/10 : marques ajoutees au sort (CoaMarque), avec le classement du lot 1.
    uint16 marques = 0;
};

constexpr CoaSurcharge CoaSurcharges[] =
{
    // Defiance (Barbarian 1-3 ; C4, survie § 3 D). AscensionBarbarianCompletion.cpp:94 passe l'effet 1 (aura 87, -80 %) en
    // DUMMY ; la reduction est faite par le coeur, Unit::GetHealthBasedDamageTakenMultiplier (Unit.cpp:9437) : x(0,2 + 0,5 x
    // vie %), soit -80 % a 0 % de vie et -30 % a pleine vie. Classify n'y voyait plus rien : jamais tente (0 ligne au banc).
    // Force 65 : la reduction a 30 % de vie. Gratuit, 15 s, recharge 180 s, aucune incapacite du lanceur.
    { 806228, KIND_DEFENSIVE, 0, 65 },
    // Ignis Ultimatus (Pyromancer 37 ; C4). AscensionPyromancerContracts.cpp:109 vide l'effet 0 (PERIODIC_DUMMY, declencheur
    // efface) ; a la fin de la canalisation de 2 s, AscensionPyromancerAuras.cpp:122-129 lance 680370 (soin en cone allie
    // devant le lanceur, TargetA 59) et 680371 (degats en cone ennemi), d'apres les piles de Flamecasting consommees.
    // RISQUE GARDE : l'aura 26 (MOD_ROOT) ENRACINE le lanceur 2 s -- marque MARQUE_IMMOBILISE, que la survie (lot 3)
    // ecarte. Soin de zone seulement : ni HoT (ce n'est pas un soin periodique : le diagnostic proposait GROUP_HEAL|HOT, le
    // HoT l'aurait fait lancer a 85 % sur un seul membre), ni soin direct (KIND_HEAL le mettrait dans la liste de « coa heal »
    // pour un seul patient ; revue : retire). Seul « coa group heal » le lance. Exige Ember 807533 (CasterAuraSpell), 60 s.
    { 680369, KIND_GROUP_HEAL, 0, 0 },
    // Hand of Yogg-Saron (Cultist 40 ; C4). AscensionCultistContracts.cpp:226 vide l'effet 0 (PERIODIC_DUMMY) ; restent
    // -15 % de degats subis et +15 % de degats infliges sur un ALLIE, 15 s (le soin 704473 ne part qu'avec le passif
    // 524877, AscensionCultistAuras.cpp:221). SPELL_ATTR1_EXCLUDE_CASTER : jamais sur soi (MARQUE_PAS_SUR_SOI), donc
    // seulement utile a une action qui protege un allie -- aucune ne le fait aujourd'hui. La regle C1 lui donne deja
    // KIND_DEFENSIVE (aura 87 sur TargetA 21) ; l'entree le garantit et le documente.
    { 704476, KIND_DEFENSIVE | KIND_ALLY_CAST, 0, 15 },
    // Solar Invocation: Revelation (Sun Cleric 46-48, 98 ; C3). TargetA 25 : enracine un ennemi OU soigne un allie
    // (declenches 680702 et 680701). Range en attaque (Live : x879), jamais en soin. Soin d'un allie ; perte assumee de
    // l'enracinement. Recharge de categorie 767 partagee avec Solar Invocation: Ascension.
    // Revue : soigneurs seulement (`soigneur`) -- la categorie 767 est partagee avec Ascension, Conquest et Resplendence :
    // un Sun Cleric de degats perdait son enracinement, et un « coa soin soi » mettait ses Invocations offensives en
    // recharge 30 s.
    { 503651, KIND_HEAL | KIND_ALLY_CAST, KIND_HOSTILE | KIND_CONTROL | KIND_DAMAGE | KIND_AOE | KIND_INTERRUPT, 0, true },
    // Spirit Charge (Primalist 58 ; C3). Charge vers un allie, le soigne (TargetA 21) et frappe autour (TargetA 16) ;
    // SPELL_ATTR1_EXCLUDE_CASTER. « coa aoe » le visait sur l'ennemi : BAD_TARGETS (Live, code 12).
    // Revue : soigneurs seulement -- le Primalist de degats garde son attaque de zone.
    { 504570, KIND_HEAL | KIND_ALLY_CAST, KIND_HOSTILE | KIND_DAMAGE | KIND_AOE, 0, true },
    // Moonflow (Starcaller 43 ; C3). Soin de groupe et de raid (TargetA 56) ; le declenche 680803 (zone ennemie sous Lunar
    // Eclipse) le rangeait en attaque de zone : TARGET_NOT_PLAYER x54 a x281 par releve (SPELL_ATTR3_ONLY_ON_PLAYER).
    { 575038, KIND_HEAL | KIND_GROUP_HEAL, KIND_HOSTILE | KIND_AOE | KIND_DAMAGE | KIND_CONTROL, 0, true },
    // Kael's Command (Pyromancer 37 ; C3). Le Phenix s'endort (etourdissement 707060, d'ou KIND_CONTROL qui l'excluait de
    // tous les soins) et soigne en zone. Exige le Phenix (CasterAuraSpell 681128). HoT retire comme pour Ignis : recharge
    // 120 s, il ne doit pas partir a 85 % sur un seul membre.
    { 800816, KIND_HEAL | KIND_GROUP_HEAL, KIND_CONTROL | KIND_HOT, 0, true },
    // Fetid Ward (Necromancer 34, 36 ; C1b). Aura de 30 min (354 + 118) dont le soin declenche vise une zone : classee
    // HEAL|BUFF|GROUP_HEAL, refusee par « coa buff » (exclut HEAL) et par « coa defensive » (exclut GROUP_HEAL). Un buff.
    { 680388, KIND_BUFF, KIND_HEAL | KIND_GROUP_HEAL | KIND_SELF_HEAL | KIND_HOT | KIND_ALLY_CAST, 0 },
    // Carapace Regeneration (Venomancer 52). Aura permanente (-4 % de degats physiques, soin periodique reecrit) qui exige
    // Beetle Form 803183 : classee posture, « coa buff » ne la tentait que hors posture, donc hors Beetle
    // (CASTER_AURASTATE x132). Un buff non exclusif de la forme. Risque : elle draine de la rage.
    { 805931, KIND_BUFF, KIND_STANCE, 0 },
    // Aeon of Renewal (Chronomancer 31). Aura permanente sur soi qui modifie Epoch : STANCE et HEAL a la fois, que « coa
    // buff » excluait (filtre HEAL). Une posture seulement.
    { 806290, 0, KIND_HEAL | KIND_HOT | KIND_ALLY_CAST | KIND_GROUP_HEAL | KIND_SELF_HEAL, 0 },
    // Libram of Grace (Templar 22-24 ; C1). Soins recus +20 % et proc de soin, 20 s, recharge 180 s : kind 0 (proc non
    // suivi, aura 118 ni defensive ni buff long). Un defensif.
    { 801463, KIND_DEFENSIVE, 0, 20 },
    // Horn of Endurance (Ranger 28-30 ; C1). Aura de groupe 30 s dont le proc soigne (560809/560808), focus +3/s : ALLY_CAST
    // seul. Un defensif de recharge courte (60 s).
    { 806359, KIND_DEFENSIVE, 0, 15 },
    // Infinite Shield (Chronomancer 31, 33 ; C1). Sur un allie (TargetA 21), 10 charges de proc qui soignent (520458) ;
    // SPELL_ATTR5_LIMIT_N. Le proc n'est pas suivi : ALLY_CAST seul. Un defensif (lance sur soi par « coa defensive »).
    { 520457, KIND_DEFENSIVE | KIND_ALLY_CAST, 0, 20 },
    // Zenith (Runemaster ; revue du 16/10). Trois auras sur soi, 6 s, recharge 45 s : 107 (+100 % de chance de declencher
    // les Weapon Engravings, description « your runic tattoos flare up »), 87 a -1 + 1 = 0 %, et 138. Son seul classement
    // venait de l'aura 87 (KIND_DEFENSIVE), que la survie du lot 3 retire (MARQUE_DEFENSIF_FAIBLE) : plus aucune action ne
    // le lancait. C'est un temps fort OFFENSIF : KIND_DAMAGE le met dans la rotation d'attaque (IsAttack), donc en combat,
    // contre la cible courante (sort sur soi : la cible n'y joue aucun role), une fois par recharge.
    { 712325, KIND_DAMAGE, KIND_DEFENSIVE, 0 },
    // I1 du 17/10 (Tinker 51 ; coa-project/docs/DIAG-equilibrage-classes-20261017.md § 2). Deux soins de zone que Classify
    // ne voyait pas : effet 0 DUMMY, effet 1 SUMMON (28) d'un PNJ qui soigne par script, les deux sur TargetA 87
    // (TARGET_DEST_DEST : pose au sol, a la position de la cible -- PlayerbotAI::CastSpell la prend sur l'unite visee).
    // Spell.dbc lu le 17/10 : My Greatest Invention! 560744 (« healing party and raid members in a $560746a1 yd radius »,
    // recharge 180 s, 22 % de mana de base) ; Build: Restorative Beacon 801801 (« heals raid and party members in the
    // area », 3 charges, 25 %). kind 0 : 0 ligne au journal du banc. Ni HoT (une recharge de 180 s ne doit pas partir a
    // 85 % sur un seul membre, meme raison qu'Ignis 680369) ; KIND_HEAL pour 560744 seulement, comme le propose le
    // diagnostic : il entre aussi dans « coa heal », au dernier rang (zone, CoaHealAction::Rang).
    { 560744, KIND_HEAL | KIND_GROUP_HEAL, 0, 0 },
    { 801801, KIND_GROUP_HEAL, 0, 0 },
    // I2 du 17/10 (Tinker 51) : Build: Replenishment Beacon 560750 (« restoring [...] $560819s1% base mana every $560751T1
    // sec to allies », 3 charges, 5 % de mana de base ; DUMMY + SUMMON, kind 0) ; limite par la mana au volet A (0-2 % a la
    // fin de chaque essai A1/A2). Marque MARQUE_REGEN_MANA, lue par « coa regen mana » (CoaRegenManaTrigger).
    { 560750, 0, 0, 0, false, MARQUE_REGEN_MANA },
};

/*
 * I5 du 17/10 (coa-project/docs/DIAG-equilibrage-classes-20261017.md, Ranger 28 et 29) : surcharges qui ne valent que pour
 * le bot qui connait un passif. Survival Potion 802839 (recharge 60 s) ne fait que dissiper (aura 23 -> 704341, deux
 * DISPEL sur soi : KIND_DISPEL) ; le passif Survival Kit 504329 la fait soigner (spell_proc 504329 : famille 27, masque
 * 0x20000000 = celui de 802839, phase de lancer -> 504328, aura 20 OBS_MOD_HEALTH, 10 % de la vie max toutes les 3 s, 9 s).
 * Classify ne suit pas un proc : jamais lancee au banc (0 ligne), lancee en monde ouvert comme dissipation seulement. Avec
 * le passif : soin sur soi seulement (MARQUE_SOIN_SOI_SEUL), periodique ; KIND_DISPEL garde. Sans lui, rien ne change.
 * Appliquees par KnownKit au classement du lot 1 (AiPlayerbot.CoaIa.Classement), a l'id cite et aux autres rangs.
 */
struct CoaSurchargeSiPassif
{
    uint32 spellId;
    uint32 passif;
    uint16 ajoute;
    uint16 marques;
};

constexpr CoaSurchargeSiPassif CoaSurchargesSiPassif[] =
{
    { 802839, 504329, KIND_HEAL | KIND_SELF_HEAL | KIND_HOT, MARQUE_SOIN_SOI_SEUL },
};

CoaSurcharge const* CoaSurchargeDe(uint32 spellId)
{
    for (CoaSurcharge const& surcharge : CoaSurcharges)
        if (surcharge.spellId == spellId)
            return &surcharge;
    return nullptr;
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
            {
                // Lot 1 : le classement d'avant (kindAncien, AiPlayerbot.CoaIa.Classement = 0) et celui du lot 1 (kind,
                // marques), calcules une fois, au premier bot, et choisis a chaque construction de kit (KnownKit).
                CoaAbility ancien = ability;
                Classify(info, ancien);
                Classify(info, ability, 0, true);
                ability.kindAncien = ancien.kind;
                ability.dispelMask = ancien.dispelMask | ability.dispelMask;
            }

            kit.abilities.push_back(ability);
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

        // Lot 1 : surcharges (CoaSurcharges), a l'id cite et aux autres rangs du meme sort, puis l'union des marques de
        // chaque classe pour ClassHas, dans les deux classements.
        uint32 surcharges = 0;
        for (auto& [classId, kit] : byClass)
        {
            std::unordered_map<uint32, CoaSurcharge const*> parPremierRang;
            for (CoaAbility const& ability : kit.abilities)
                if (CoaSurcharge const* surcharge = CoaSurchargeDe(ability.spellId))
                    if (ability.firstSpellId)  // revue : un premier rang nul n'est pas un sort, il relierait tout
                        parPremierRang[ability.firstSpellId] = surcharge;
            for (CoaAbility& ability : kit.abilities)
            {
                CoaSurcharge const* surcharge = CoaSurchargeDe(ability.spellId);
                if (!surcharge && ability.firstSpellId)
                {
                    auto const itr = parPremierRang.find(ability.firstSpellId);
                    surcharge = itr != parPremierRang.end() ? itr->second : nullptr;
                }
                if (surcharge)
                {
                    ability.kind = uint16((ability.kind | surcharge->ajoute) & ~surcharge->retire);
                    ability.marques |= surcharge->marques;
                    ability.surchargeSoigneur = surcharge->soigneur;
                    if (surcharge->ajoute & KIND_DEFENSIVE)
                        ability.marques &= ~uint16(MARQUE_DEFENSIF_FAIBLE);
                    ++surcharges;
                }
                kit.kinds |= ability.kind;
                kit.kindsAncien |= ability.kindAncien;
                // I5 : ce qu'une surcharge sous passif PEUT ajouter compte pour ClassHas (test bon marche avant les sorts).
                for (CoaSurchargeSiPassif const& c : CoaSurchargesSiPassif)
                    if (ability.spellId == c.spellId || (ability.firstSpellId && ability.firstSpellId == c.spellId))
                        kit.kinds |= c.ajoute;
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
        LOG_INFO("playerbots", "coa ia soins: classement du lot 1 calcule, {} entrees de kit surchargees", surcharges);
        return byClass;
    }();

    return abilities;
}

// Whether the bot's class has any ability of that kind: a cheap test before looking at spells.
bool ClassHas(Player* bot, uint16 kind)
{
    auto const& all = ClassAbilities();
    auto const found = all.find(bot->getClass());
    return found != all.end() &&
           ((sPlayerbotAIConfig.coaIaClassement ? found->second.kinds : found->second.kindsAncien) & kind);
}

struct Usable
{
    SpellInfo const* info;
    uint16 kind;
    uint32 dispelMask;
    uint16 marques = 0;  // lot 1 : voir CoaMarque
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

    // Revue du 16/10 : role lu une fois par construction du kit (CoaSurcharge::soigneur).
    bool const soigneur = GetCoaRole(bot) == CoaRole::Heal;
    for (CoaAbility const& ability : found->second.abilities)
    {
        if (ability.requiredLevel > level || !bot->HasSpell(ability.spellId))
            continue;

        // Class grants include passives (e.g. 552011 Resilient Constitution).
        SpellInfo const* info = sSpellMgr->GetSpellInfo(ability.spellId);
        if (!info || info->IsPassive())
            continue;

        // Lot 1 : le classement choisi par AiPlayerbot.CoaIa.Classement ; a 0, ni les marques du lot 1 (soin d'autrui, cible
        // soi) ni le reste du nouveau classement. Les options sont relues a chaque construction (CoaKitLifeSeconds au plus).
        bool const lot1 = sPlayerbotAIConfig.coaIaClassement;
        // Revue du 16/10 : un sort force en soin ne l'est que pour un soigneur (CoaSurcharge::soigneur).
        uint16 kind = lot1 && !(ability.surchargeSoigneur && !soigneur) ? ability.kind : ability.kindAncien;
        uint16 marques = ability.marques;
        if (!lot1)
            marques &= ~uint16(MARQUE_SOIN_SOI_SEUL | MARQUE_PAS_SUR_SOI);
        // Lot 3 (AiPlayerbot.CoaIa.Survie) : une reduction de degats subis de moins de 10 % n'est pas un defensif
        // (Righteous Tempest 807274, Zenith 712325 : « -1 % », soit 0). Le sort garde ses autres marques (degats...).
        if (sPlayerbotAIConfig.coaIaSurvie && (marques & MARQUE_DEFENSIF_FAIBLE))
            kind &= ~uint16(KIND_DEFENSIVE);
        // I5 du 17/10 : surcharge sous passif (CoaSurchargesSiPassif), au classement du lot 1 seulement.
        if (lot1)
            for (CoaSurchargeSiPassif const& c : CoaSurchargesSiPassif)
                if ((ability.spellId == c.spellId || (ability.firstSpellId && ability.firstSpellId == c.spellId)) &&
                    bot->HasSpell(c.passif))
                {
                    kind |= c.ajoute;
                    marques |= c.marques;
                }
        coaContext->knownKit.push_back({ info, kind, ability.dispelMask, ability.firstSpellId, marques });
    }

    return coaContext->knownKit;
}

// Active abilities the bot has reached and actually knows, for which `wanted(kind)` is true.
// Le filtre recoit `kind`, ou `kind` et `marques` (lot 1) s'il prend deux arguments.
template <typename Filter>
bool CoaVeut(Filter& wanted, uint16 kind, uint16 marques)
{
    if constexpr (std::is_invocable_v<Filter&, uint16, uint16>)
        return wanted(kind, marques);
    else
        return wanted(kind);
}

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
        if (!CoaVeut(wanted, ability.kind, ability.marques))
            continue;

        auto const at = std::find(firsts.begin(), firsts.end(), ability.firstSpellId);
        if (at == firsts.end())
        {
            firsts.push_back(ability.firstSpellId);
            usable.push_back({ ability.info, ability.kind, ability.dispelMask, ability.marques });
        }
        else
            usable[size_t(at - firsts.begin())] = { ability.info, ability.kind, ability.dispelMask, ability.marques };
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
// Lot 1 (DIAG rotation C3-bis) : sauf si l'allie choisi ne recoit pas le soin (MARQUE_SOIN_SOI_SEUL, Devour Curse 800402 :
// la dissipation vise l'allie, le soin le lanceur). `marques` a 0 : le test d'avant.
bool CanHealOther(uint16 kind, uint16 marques = 0)
{
    return !(kind & KIND_SELF_HEAL) || (kind & KIND_GROUP_HEAL) ||
           ((kind & KIND_ALLY_CAST) && !(marques & MARQUE_SOIN_SOI_SEUL));
}

SpellInfo const* EffectiveSpell(Player* bot, SpellInfo const* info);

/*
 * Refus CERTAINS du coeur, lus sur le lanceur seul, pour tous les bots CoA (banc d'arene du 10/10/2026 22:55-23:00
 * Paris : ONLY_SHAPESHIFT x5, EQUIPPED_ITEM_CLASS x3, NO_POWER x20). StrictCheck les rendait deja tous : aucun de ces
 * sorts ne partait. Mais ONLY_SHAPESHIFT et EQUIPPED_ITEM_CLASS sont des echecs « durables » (IsLastingFailure), mis au
 * banc 20 s -- un sort de forme restait au banc 20 s apres la prise de la forme -- et NO_POWER au banc 5 s : le Ranger
 * 2617 a sec a 20:59:20, 20:59:27 et 20:59:40 UTC perdait ses quatre tirs d'un coup, 5 s durant, alors que sa
 * ressource revenait plus tot. Ecartes d'avance, SANS banc ni echec consigne (comme CasterAuraSpell), ils repartent
 * des que la forme, l'arme ou la ressource est la.
 *
 * Chaque test est une RECOPIE du coeur, jamais plus stricte :
 *   - forme : Spell::CheckCast (Spell.cpp:5802-5819), branche `strict` sans TRIGGERED_IGNORE_SHAPESHIFT, celle
 *     qu'emprunte CastSpell (TRIGGERED_NONE) : aucune aura SPELL_AURA_MOD_IGNORE_SHAPESHIFT qui touche le sort, et
 *     SpellInfo::CheckShapeshift(forme) != OK ;
 *   - arme : Spell::CheckCast -> CheckItems (Spell.cpp:6109-6118, 7406-7411), sort non passif, sans objet lanceur ni
 *     objet cible (CastFirst vise une unite) : Player::HasItemFitToSpellRequirements faux ;
 *   - ressource : le pre-controle de StrictCheck, tel quel (CoaPeutPayer, que StrictCheck appelle desormais).
 * Spell::CheckCast ne rend OK nulle part avant ces tests (lu, 5730-6120) : un sort qu'ils ecartent etait refuse.
 */
bool CoaPeutPayer(Player* bot, SpellInfo const* info)
{
    if (info->PowerType < MAX_POWERS && info->PowerType != POWER_HEALTH)
    {
        int32 const cost = info->CalcPowerCost(bot, info->GetSchoolMask());
        if (cost > 0 && bot->GetPower(Powers(info->PowerType)) < cost)
            return false;
    }
    return true;
}

bool CoaRefuseParFormeOuArme(Player* bot, SpellInfo const* info)
{
    bool verifierForme = true;
    for (AuraEffect const* effet : bot->GetAuraEffectsByType(SPELL_AURA_MOD_IGNORE_SHAPESHIFT))
        if (effet->IsAffectedOnSpell(info))
        {
            verifierForme = false;
            break;
        }
    if (verifierForme && info->CheckShapeshift(bot->GetShapeshiftForm()) != SPELL_CAST_OK)
        return true;
    return !info->IsPassive() && !bot->HasItemFitToSpellRequirements(info);
}

/*
 * Etat ou aura du LANCEUR exige ou interdit : RECOPIE de Spell::CheckCast (Spell.cpp:5846-5859), branche sans
 * TRIGGERED_IGNORE_CASTER_AURASTATE, celle qu'emprunte PlayerbotAI::CastSpell (TRIGGERED_NONE). Memes appels que le
 * coeur : HasAuraState(etat, sort, lanceur) -- qui lit aussi SPELL_AURA_ABILITY_IGNORE_AURASTATE -- et
 * GetSpellIdForDifficulty. Le coeur rend SPELL_FAILED_CASTER_AURASTATE, que IsLastingFailure met au banc 20 s.
 * Banc d'arene du 11/10 00:30-00:47 UTC (tour 7), CASTER_AURASTATE x9 : Smash 501029 (CasterAuraState 17) x3,
 * Sanguine Rupture 802456 x2, Bloodmoon Blast 501610, Taldaram's Torment 802570 (ExcludeCasterAuraSpell 524861),
 * Sanguine Mend 504081, Scarlet Delirium 803685 (ExcludeCasterAuraSpell 802877). Avant ce jour, seul CasterAuraSpell
 * etait lu (10/10 soir).
 */
bool CoaRefuseParAuraDuLanceur(Player* bot, SpellInfo const* info)
{
    if (info->CasterAuraState && !bot->HasAuraState(AuraStateType(info->CasterAuraState), info, bot))
        return true;
    if (info->CasterAuraStateNot && bot->HasAuraState(AuraStateType(info->CasterAuraStateNot), info, bot))
        return true;
    if (info->CasterAuraSpell && !bot->HasAura(sSpellMgr->GetSpellIdForDifficulty(info->CasterAuraSpell, bot)))
        return true;
    return info->ExcludeCasterAuraSpell &&
           bot->HasAura(sSpellMgr->GetSpellIdForDifficulty(info->ExcludeCasterAuraSpell, bot));
}

/*
 * Familier exige et absent : RECOPIE de Spell::CheckCast (Spell.cpp:6045-6058). Le premier effet visant
 * TARGET_UNIT_PET decide ; sans familier gardien (GetGuardianPet) ni unite possedee (GetCharm), le coeur rend
 * SPELL_FAILED_NO_PET a un sort lance par CastSpell (TRIGGERED_NONE, sans aura declencheuse). Lu sur le lanceur seul.
 * Ecarte sans banc : le sort repart des que le familier est la.
 */
bool CoaExigeFamilierAbsent(Player* bot, SpellInfo const* info)
{
    for (SpellEffectInfo const& effect : info->Effects)
        if (effect.TargetA.GetTarget() == TARGET_UNIT_PET)
            return !bot->GetGuardianPet() && !bot->GetCharm();
    return false;
}

/*
 * Refus CERTAINS du coeur lus sur la cible, pour tous les bots CoA (banc d'arene du 11/10/2026 00:36-00:56 Paris :
 * TARGET_AURASTATE x13 -- Slaughter 500430 et Electrocute 501423 sous 35 % de vie (TargetAuraState 13), Ice Rune 524930
 * sur cible gelee (4), Lunar Lance 502335 et Umbral Blade 681546 sous l'aura 804378 (TargetAuraSpell) ; NOTHING_TO_DISPEL
 * x3, Stormbreaker 705669 ; BAD_TARGETS x4, Manaburn 805248 sur une cible sans mana). TARGET_AURASTATE mettait le sort au
 * banc 20 s (IsLastingFailure) : une execution ratee a 40 % de vie restait ecartee pendant toute la fenetre sous 35 %.
 * NOTHING_TO_DISPEL n'avait aucun banc : retente a chaque tour.
 *
 * Chaque test est une RECOPIE du coeur, jamais plus stricte :
 *   - seulement si le coeur garde la cible unite (Spell::InitExplicitTargets, Spell.cpp:735-744 : masque explicite
 *     d'unite) -- sinon il ne la juge pas, et nous non plus ;
 *   - etat d'aura : SpellInfo::CheckTarget (SpellInfo.cpp:2007-2020), appele par Spell::CheckCast (Spell.cpp:5965)
 *     sur la cible unite, memes appels (HasAuraState avec sort et lanceur, GetSpellIdForDifficulty) et meme exception
 *     (lanceur vehicule, cible maitre du lanceur) ;
 *   - brulure ou drain de ressource : Spell::CheckCast (Spell.cpp:6289-6297), lanceur joueur, cible autre que lui ;
 *   - dissipation : Spell::CheckCast (Spell.cpp:6157-6200), tous les effets sont des DISPEL ni de zone ni a
 *     SPELL_ATTR1_INITIATE_COMBAT, et GetDispellableAuraList vide. Le refus de sanctuaire n'est PAS recopie (moins strict).
 * Aucun de ces blocs n'est precede d'un « return SPELL_CAST_OK » dans CheckCast pour un sort non declenche (lu,
 * 5730-6300 ; seul TRIGGERED_IGNORE_EFFECTS en sort, Spell.cpp:6134) : un sort qu'ils ecartent etait refuse.
 * Les refus que rend un SCRIPT (Testament of Faith 501602, Necromancer 500443) restent a StrictCheck.
 * Fil de l'IA du bot ; la cible est sur la meme carte (P-137).
 */
bool CoaRefusCibleCertain(Player* bot, SpellInfo const* info, Unit* target)
{
    if (!target || !target->IsInWorld() || target->GetMap() != bot->GetMap() || info->IsPassive() ||
        !(info->GetExplicitTargetMask() & TARGET_FLAG_UNIT_MASK))
        return false;
    // Relecture : un sort a point au sol ou a source est lance par PlayerbotAI::CastSpell SANS cible unite
    // (PlayerbotAI.cpp:3961-3970) ; InitExplicitTargets la reprend alors dans la selection, ou prend le lanceur si la
    // selection ne convient pas -- la cible jugee peut ne pas etre `target`. Laisse au coeur.
    if (info->Targets & (TARGET_FLAG_DEST_LOCATION | TARGET_FLAG_SOURCE_LOCATION))
        return false;

    if (!bot->IsVehicle() && bot->GetCharmerOrOwner() != target)
    {
        if (info->TargetAuraState && !target->HasAuraState(AuraStateType(info->TargetAuraState), info, bot))
            return true;
        if (info->TargetAuraStateNot && target->HasAuraState(AuraStateType(info->TargetAuraStateNot), info, bot))
            return true;
    }
    if (info->TargetAuraSpell && !target->HasAura(sSpellMgr->GetSpellIdForDifficulty(info->TargetAuraSpell, bot)))
        return true;
    if (info->ExcludeTargetAuraSpell &&
        target->HasAura(sSpellMgr->GetSpellIdForDifficulty(info->ExcludeTargetAuraSpell, bot)))
        return true;

    bool zoneOuCombat = false;
    bool autreEffet = false;
    uint32 masque = 0;
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if ((effect.Effect == SPELL_EFFECT_POWER_BURN || effect.Effect == SPELL_EFFECT_POWER_DRAIN) && target != bot &&
            !target->HasActivePowerType(Powers(effect.MiscValue)))
            return true;
        if (effect.Effect == SPELL_EFFECT_DISPEL)
        {
            if (effect.IsTargetingArea() || info->HasAttribute(SPELL_ATTR1_INITIATE_COMBAT))
                zoneOuCombat = true;
            else
                masque |= SpellInfo::GetDispelMask(DispelType(effect.MiscValue));
        }
        else if (effect.IsEffect())
            autreEffet = true;
    }
    // Le coeur sort de sa boucle au premier effet de zone ou non-DISPEL : le masque qu'il a lu jusque-la ne compte
    // que si aucun des deux n'existe, ce qui revient au meme test sur la boucle complete.
    if (!zoneOuCombat && !autreEffet && masque)
    {
        DispelChargesList liste;
        target->GetDispellableAuraList(bot, masque, liste, info);
        if (liste.empty())
            return true;
    }
    return false;
}

/*
 * LOT 20 (13/10/2026) -- SOIN OU BOUCLIER DEJA POSE, pour tous les bots CoA.
 *
 * Banc d'arene du 12/10 (docs/ANALYSE-classes-faibles-arene-20261013.md § 1) : Vital Shield 504849 relance jusqu'a 19 fois
 * en 54 s alors que son aura dure 30 min. Deux causes : Classify ne le marquait pas KIND_HOT (son soin passe par une aura
 * 23 qui declenche 804309 ; corrige dans Classify), et le controle « aura deja posee » de la survie ne valait QUE pour
 * KIND_HOT. La regle est desormais generale, sans id : un sort de soin ou de defense dont TOUT l'effet passe par une aura
 * (aucun soin direct, voir CoaSoinDirect) n'est pas relance tant que cette aura -- la sienne, ou celle d'un sort qu'il
 * declenche sans aura -- est posee PAR LE BOT sur la cible et dure encore plus d'une seconde (ou est permanente). Une
 * aura d'un AUTRE lanceur ne bloque rien (elle se cumule par lanceur). Un sort qui soigne aussi directement (soin + HoT,
 * Fleshcraft 801952 : HEAL_PCT + aura de hate) reste lancable : le relancer soigne.
 */

// Le sort soigne DIRECTEMENT (HEAL, HEAL_PCT, HEAL_MAX_HEALTH), lui-meme ou par un sort qu'il declenche sans aura
// (TRIGGER_SPELL, TRIGGER_SPELL_WITH_VALUE, declenchement differe Ascension 183), sur deux niveaux. Un soin porte par une
// aura (PERIODIC_HEAL, declencheur periodique, aura factice qui soigne au prochain coup comme Dark Infusion 570249) n'en
// est pas un.
bool CoaSoinDirect(SpellInfo const* info, uint8 profondeur = 0)
{
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (!effect.IsEffect())
            continue;
        if (effect.Effect == SPELL_EFFECT_HEAL || effect.Effect == SPELL_EFFECT_HEAL_PCT ||
            effect.Effect == SPELL_EFFECT_HEAL_MAX_HEALTH)
            return true;
        bool const declencheSansAura = effect.Effect == SPELL_EFFECT_TRIGGER_SPELL ||
                                       effect.Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE ||
                                       effect.Effect == SPELL_EFFECT_ASCENSION_TRIGGER_SPELL_DELAYED;
        if (declencheSansAura && effect.TriggerSpell && effect.TriggerSpell != info->Id && profondeur < 2)
            if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                if (CoaSoinDirect(declenche, profondeur + 1))
                    return true;
    }
    return false;
}

// Marge sous laquelle une aura posee se renouvelle : relancer a moins d'une seconde de la fin ne gaspille presque rien.
constexpr int32 CoaAuraRenouvelerMs = 1000;

// L'aura du sort, ou celle d'un sort qu'il declenche sans aura (un niveau), posee par `lanceur` sur `cible` et encore
// la pour plus de CoaAuraRenouvelerMs (ou permanente).
bool CoaAuraPoseePar(Unit* cible, ObjectGuid lanceur, SpellInfo const* info)
{
    auto posee = [cible, lanceur](uint32 id)
    {
        Aura const* aura = cible->GetAura(id, lanceur);
        if (!aura)
            return false;
        // Revue du lot 20 (13/10) : une aura qui s'EMPILE par relance (StackAmount > 1) et n'est pas au maximum reste a
        // poser -- Carapace Regeneration 805931 (« stacking 3 times », aura permanente, 3 charges) restait sinon a une
        // pile pour toujours. Sauf si ses piles fondent aux coups (ProcFlags : Abyssal Ward 804670, 5 piles qui baissent
        // a chaque attaque recue) : la relancer ne ferait que racheter ce que les coups ont pris. Meme lecture que la
        // regle 3 de CoaAreneRefusTactique (aEmpiler).
        uint32 const pileMax = aura->GetSpellInfo()->StackAmount;
        if (pileMax > 1 && aura->GetStackAmount() < pileMax && !aura->GetSpellInfo()->ProcFlags)
            return false;
        return aura->GetDuration() < 0 || aura->GetDuration() > CoaAuraRenouvelerMs;
    };
    if (posee(info->Id))
        return true;
    for (SpellEffectInfo const& effect : info->Effects)
        if ((effect.Effect == SPELL_EFFECT_TRIGGER_SPELL || effect.Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE) &&
            effect.TriggerSpell && effect.TriggerSpell != info->Id && posee(effect.TriggerSpell))
            return true;
    return false;
}

// Soin ou bouclier (KIND_HEAL ou KIND_DEFENSIVE, ni hostile ni degats) sans soin direct, dont l'aura est deja posee par
// le bot sur `cible`. Juge sur `info`, le sort EFFECTIF (EffectiveSpell), celui que PlayerbotAI::CastSpell lance : un
// remplacement qui pose une AUTRE aura n'est pas ecarte parce que l'aura du sort de base est la (meme regle que
// CoaAreneRefusTactique, revue du 11/10). `spell.kind` : les marques du sort de la liste.
/*
 * IA soins/survie, lot 4 (DIAG rotation C2, AiPlayerbot.CoaIa.AuraSurLanceur) : toutes les auras du sort se posent sur
 * le LANCEUR. Ingredient: Jungle Shrooms 801660 (Witch Doctor) : aura 23 sur soi (TargetA 1), permanente, qui declenche le
 * soin de zone 802703. « Deja pose » le cherchait sur le PATIENT, ne le trouvait jamais, et le sort etait relance en boucle
 * (1 855 lancers au volet A, 5 % de la mana de base chacun ; 131 s sans soin direct sur un membre a 45 %).
 */
bool CoaAuraSurLanceur(SpellInfo const* info)
{
    bool aura = false;
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (!effect.IsEffect() || !IsAuraEffect(effect))
            continue;
        if (effect.TargetA.GetTarget() != TARGET_UNIT_CASTER)
            return false;
        aura = true;
    }
    return aura;
}

bool CoaSoinDejaPose(Player* bot, Usable const& spell, SpellInfo const* info, Unit* cible)
{
    if (!cible || !cible->IsInWorld() || !(spell.kind & (KIND_HEAL | KIND_DEFENSIVE)) ||
        (spell.kind & (KIND_HOSTILE | KIND_DAMAGE)))
        return false;
    if (CoaSoinDirect(info))
        return false;
    // Lot 4 : une aura qui ne se pose que sur le lanceur se cherche sur lui, quel que soit le patient.
    if (sPlayerbotAIConfig.coaIaAuraSurLanceur && CoaAuraSurLanceur(info))
        return CoaAuraPoseePar(bot, bot->GetGUID(), info);
    return CoaAuraPoseePar(cible, bot->GetGUID(), info);
}

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
        // Same caster state and aura test as CastFirst (10/10 soir, 11/10) : a spell CastFirst now skips must not count
        // as ready.
        if (CoaRefuseParAuraDuLanceur(bot, info))
            continue;
        // Same form, weapon and resource tests as CastFirst (banc d'arene du 10/10) : skipped there, not ready here.
        if (CoaRefuseParFormeOuArme(bot, info) || !CoaPeutPayer(bot, info))
            continue;
        // Same pet test as CastFirst (banc d'arene du 11/10).
        if (CoaExigeFamilierAbsent(bot, info))
            continue;
        // IA soins/survie, lot 4 (AiPlayerbot.CoaIa.UtiliteAlignee) : sous la recharge globale, CastFirst ne lance rien
        // (il rend nullptr au premier sort qui l'a) : un sort qui la subit n'est pas pret. Le moteur passe a l'action
        // suivante, comme apres un echec (Engine::DoNextAction) : rien ne change en jeu, sinon les actions vides tracees.
        if (sPlayerbotAIConfig.coaIaUtiliteAlignee && bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
            continue;
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
    if (!CoaPeutPayer(bot, info))
        return SPELL_FAILED_NO_POWER;

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
// La portee MAXIMALE est filtree depuis le 2026-10-02 (quatrieme passe, en bas). On
// ecrivait ici qu'un sort trop LOIN « se corrige en s'approchant » et qu'il fallait le
// laisser pour donner au moteur la raison d'avancer : c'etait faux. Le seul mouvement de
// portee d'un bot CoA est « enemy out of spell » -> « reach spell », qui le place a
// AiPlayerbot.SpellDistance (28.5 yd) sans rien lire de cette liste ; un sort a 20 yd
// n'y entre donc jamais, et il etait refuse OUT_OF_RANGE a chaque tick.
//
// Les passes suivantes (deuxieme a quatrieme) ajoutent d'autres causes « ne peut pas
// reussir ici » ; STUNNED et NO_POWER restent hors de ce filtre, pour la raison dite
// plus haut.
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
        [bot](Usable const& spell)
        {
            SpellInfo const* info = spell.info;

            // Spell.cpp : HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) && !HasStealthAura().
            // ORIENTATION RETIREE D'ICI le 10/10/2026 (banc d'arene) : ecarter le sort laissait le bot
            // dos a sa cible pour toujours -- seul PlayerbotAI::CastSpell le tourne, et il n'etait plus
            // appele. CastFirst tourne desormais le bot avant de juger le sort (CoaPreControleCible).
            return info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) && !bot->HasStealthAura();
        }),
        spells.end());

    // Troisieme passe, ajoutee le 2026-10-02 : le TYPE de la cible, qui ne change pas
    // d'un tick a l'autre. Releve « coa attack failures » du 02/10 20:58 UTC :
    //
    //   TARGET_IS_PLAYER   x19 721  Imprison (801067), « Imprison a Demon » :
    //                               TargetCreatureType = 4 (demons seuls), tente
    //                               sur des joueurs
    //   TARGET_NOT_PLAYER  x21 034  Moonflow (575030), SPELL_ATTR3_ONLY_ON_PLAYER,
    //                               tente sur des creatures
    //
    // Ni l'un ni l'autre n'est mis au banc (IsLastingFailure ne les connait pas) : le
    // sort etait reverifie a chaque tick, Spell construit et CheckCast complet, pour un
    // refus connu d'avance. Un Imprison tente sur un non-demon qui n'est pas un joueur
    // rendait BAD_TARGETS et mettait le sort au banc 20 s, y compris contre le demon
    // suivant.
    //
    // Recopie de SpellInfo::CheckTarget (SpellInfo.cpp), que Spell::CheckCast appelle
    // sans condition sur la cible explicite : les trois tests ne dependent que du
    // sort et de la cible. CheckTargetCreatureType est appele tel quel, avec son cas
    // particulier (Curse of Doom) et le masque de type dynamique (une forme de demon
    // rend Imprison valable sur un joueur).
    spells.erase(std::remove_if(spells.begin(), spells.end(),
        [bot, target](Usable const& spell)
        {
            // Le sort que CheckCast verra : le remplacant s'il y en a un (voir
            // EffectiveSpell et StrictCheck). Un remplacant peut viser autre chose que
            // le sort de base ; juger la base l'ecarterait a tort.
            SpellInfo const* info = EffectiveSpell(bot, spell.info);

            if (target->IsPlayer())
            {
                if (info->HasAttribute(SPELL_ATTR5_NOT_ON_PLAYER))
                    return true;
            }
            else
            {
                if (info->HasAttribute(SPELL_ATTR3_ONLY_ON_PLAYER))
                    return true;
                if (info->HasAttribute(SPELL_ATTR5_NOT_ON_PLAYER_CONTROLLED_NPC) && target->IsControlledByPlayer())
                    return true;
            }

            return !info->CheckTargetCreatureType(target);
        }),
        spells.end());

    // Quatrieme passe, ajoutee le 2026-10-02 : la portee MAXIMALE. Refus OUT_OF_RANGE
    // releves le 02/10 (port 8888 et CoaBots.log) : Hypovolemic Shock 572305, Leyfeed
    // 520269 et Resonance Rune 803679 portent a 20 yd, Starsunder 802470 a 5 yd, Glacial
    // Rune 807842 a 30 yd (au sol). Un bot a distance se tient a SpellDistance = 28.5 yd :
    // il les tentait a chaque tick, Spell construit et CheckCast complet, pour un refus
    // certain. Aucun changement de jeu : StrictCheck les refusait deja tous.
    //
    // Le test ne doit JAMAIS etre plus strict que Spell::CheckRange (Spell.cpp), sans quoi
    // il retirerait un sort que le coeur accepte. D'ou, a chaque endroit ou le coeur a
    // deux branches, la plus large :
    //   - portee : celle de Unit::GetSpellMaxRangeForTarget (colonne amie ou hostile,
    //     bonus de portee des techniques de melee) plus SPELLMOD_RANGE, comme le coeur ;
    //     et au moins SpellInfo::GetMaxRange avec le bot comme porteur de modificateurs,
    //     qui ajoute le cas Starcaller 801975 (50 yd) ;
    //   - sort a distance : la tolerance non stricte du coeur (10 %, 3 yd au plus) est
    //     ajoutee, alors que StrictCheck verifie en strict ;
    //   - sort de melee : la branche « leeway » du coeur (max - MIN_MELEE_REACH), la plus
    //     large des deux, IsWithinMeleeRange ajoutant lui-meme le leeway s'il y en a.
    //
    // Ne sont PAS concernes : un sort sans portee maximale (0 : melee nue ou soi), la portee
    // ID 1 (le coeur ne verifie rien), et un sort qui ne prend ni cible unite ni point au sol
    // (il vise le lanceur ou sa zone, la cible d'attaque n'y joue aucun role).
    spells.erase(std::remove_if(spells.begin(), spells.end(),
        [bot, target](Usable const& spell)
        {
            // Soi-meme, et le vehicule qui porte le bot : le coeur ne teste pas la portee
            // contre lui (targetIsVehicleBase, Spell::CheckRange).
            if (target == bot || bot->GetVehicleBase() == target)
                return false;

            // Le sort que StrictCheck verra, remplacant compris (voir la troisieme passe).
            SpellInfo const* info = EffectiveSpell(bot, spell.info);
            if (!info->RangeEntry || info->RangeEntry->ID == 1)
                return false;
            if (!(info->GetExplicitTargetMask() & (TARGET_FLAG_UNIT_MASK | TARGET_FLAG_DEST_LOCATION)))
                return false;

            float maxRange = bot->GetSpellMaxRangeForTarget(target, info);
            bot->ApplySpellMod(info->Id, SPELLMOD_RANGE, maxRange);
            bool const friendly = info->RangeEntry->RangeMax[1] != info->RangeEntry->RangeMax[0] &&
                !bot->IsHostileTo(target);
            maxRange = std::max(maxRange, info->GetMaxRange(friendly, bot));
            if (maxRange <= 0.0f)
                return false;

            if (info->RangeEntry->Flags == SPELL_RANGE_MELEE)
                return !bot->IsWithinMeleeRange(target, std::max(maxRange - MIN_MELEE_REACH, 0.0f));

            return !bot->IsWithinCombatRange(target, maxRange + std::min(3.0f, maxRange * 0.1f));
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
// Banc d'arene du 11/10 00:36-00:56 (Paris) : SPELL_FAILED_NO_PET x34, tous Transfer Life 801530 (Necromancer 537), que
// le SCRIPT du module refuse sans serviteur releve (AscensionNecromancerAbilities.cpp:158) -- ni test de DBC a recopier,
// ni banc : le sort etait retente a chaque tour (jusqu'a 6 fois par seconde). Banc court, comme NO_POWER : le sort
// repart au plus 5 s apres l'arrivee d'un serviteur.
constexpr time_t NoPetBenchSeconds = 5;

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

// CoaArene IA, lot L0 (CoaAreneIa.inc, inclus plus bas) : sous controle en arene ou en duel, ne pas tenter ce que
// le coeur refusera a cause de l'etat du lanceur. Faux sans AiPlayerbot.CoaArene.IaCommune.
bool CoaAreneFiltreEtatActif(PlayerbotAI* botAI, Player* bot);
bool CoaAreneBloqueParEtat(Player* bot, SpellInfo const* info);
// Trace des gladiateurs d'arene 1v1 (AiPlayerbot.Arene1v1.Trace, CoaAreneIa.inc) : une ligne par tentative de sort.
// `motif` : refus du moteur (nullptr : `resultat`, le SpellCastResult du coeur, fait foi).
bool CoaAreneTraceActive(PlayerbotAI* botAI, Player* bot);
void CoaAreneTracerSort(PlayerbotAI* botAI, Player* bot, SpellInfo const* info, Unit* target, SpellCastResult resultat,
                        char const* motif);
// CoaArene : le bot suit un deplacement VOULU de l'IA d'arene (recul, retour, cachette L2, cacheur du banc). Faux pour
// tout bot hors de ces etats, donc pour tout bot hors arene et hors duel. Defini dans CoaAreneIa.inc.
bool CoaAreneMouvementVoulu(PlayerbotAI* botAI);
// CoaArene, choix tactiques (11/10, arene 1v1 id=89 : 11 lancers, 0 degat) : sort de zone sans l'adversaire dans la
// zone, revelation sans adversaire furtif, debuff de zone deja pose. Arene 1v1 et duel seulement ; voir CoaAreneIa.inc.
enum CoaAreneRefusTactiqueType : uint8
{
    COA_TACTIQUE_AUCUN = 0,
    COA_TACTIQUE_HORS_ZONE,     // adversaire hors du rayon de toutes les zones du sort
    COA_TACTIQUE_HORS_CONE,     // dans le rayon d'un cone, mais pas devant : un demi-tour peut suffire
    COA_TACTIQUE_REVELATION,    // sort de revelation, adversaire ni furtif ni invisible
    COA_TACTIQUE_DEJA_POSE,     // debuff de zone deja sur l'adversaire, ni a renouveler ni a empiler
    COA_TACTIQUE_VERROU,        // le sort enferme le lanceur (aura 263), adversaire hors de portee de ses coups
    COA_TACTIQUE_SURVIE_CHARGE, // charge ou saut vers l'adversaire, bot au palier de survie danger ou critique
    COA_TACTIQUE_SURVIE_INCANTATION // attaque de 2 s ou plus au palier danger ou critique, un soin pret
};
Unit* CoaAreneTactiqueAdversaire(PlayerbotAI* botAI, Player* bot);
uint8 CoaAreneRefusTactique(Player* bot, SpellInfo const* info, Unit* adv, Unit* cible);
void CoaAreneNoterRefusTactique(PlayerbotAI* botAI, Player* bot, SpellInfo const* info, Unit* adv, uint8 refus, bool trace);
CoaAreneEtat& CoaAreneEtatDe(PlayerbotAI* botAI);
bool CoaAreneSortDeRapprochement(SpellInfo const* info);
bool CoaAreneSurvieRefuseIncantation(PlayerbotAI* botAI, Player* bot, SpellInfo const* info, uint16 kind);
// Survie d'arene (11/10) : sort ecarte faute de ressource (CoaPeutPayer) par un gladiateur trace -- une ligne
// « refus_ressource » par sort et par 5 s, et le compte du bilan. Defini dans CoaAreneIa.inc.
void CoaAreneNoterRefusRessource(PlayerbotAI* botAI, Player* bot, SpellInfo const* info, Unit* target);
// Lot 20 : soin ou bouclier deja pose ecarte (CoaSoinDejaPose) en arene 1v1 ou en duel -- une ligne « deja_pose » par
// sort et par 5 s, et le compte du bilan. Defini dans CoaAreneIa.inc.
void CoaAreneNoterDejaPose(PlayerbotAI* botAI, Player* bot, SpellInfo const* info, Unit* target, bool trace);

/*
 * Pre-controle de la cible, pour tous les bots CoA (banc d'arene du 10/10/2026 23:31-23:37 Paris : OUT_OF_RANGE 61,
 * LINE_OF_SIGHT 37, UNIT_NOT_INFRONT 17). Ces refus venaient de StrictCheck : ils ne coutaient pas de temps de
 * recharge global, mais un Spell construit et un CheckCast complet par sort et par tour pour un refus certain -- et,
 * pour l'orientation, le bot ne se tournait JAMAIS : StrictCheck refusait avant PlayerbotAI::CastSpell, seul a tourner
 * le bot (PlayerbotAI.cpp, ServerFacade::SetFacingTo), et la deuxieme passe de DropUncastableHere ecartait le sort.
 *
 * Rend le refus que le coeur rendrait, ou SPELL_CAST_OK. RECOPIE de ce que Spell::prepare verifie (CheckCast(true),
 * donc CheckRange(strict = true) : Spell.cpp:3559, 6123, 7162-7241), jamais plus strict :
 *   - seulement pour un sort que PlayerbotAI::CastSpell lance sur l'UNITE cible (ni point au sol, ni source, ni objet,
 *     ni crochetage/depecage) : les autres ont une autre geometrie dans le coeur, on les laisse a StrictCheck ;
 *   - ligne de vue : CheckCast (Spell.cpp:5979-6006), lanceur joueur, ni IGNORE_LINE_OF_SIGHT ni ALWAYS_AOE_LINE_OF_SIGHT,
 *     modeles M2 ignores, toutes verifications ; calculee UNE fois par appel (`vue`, -1 = pas encore) ;
 *   - portee maximale : branche melee (leeway, sinon 2 x MIN_MELEE_REACH) ou IsWithinCombatRange, SPELLMOD_RANGE compris,
 *     SANS la tolerance de 10 % du mode non strict (le coeur ne l'accorde qu'apres l'incantation) ;
 *   - orientation : FacingCasterFlags INFRONT, HasInArc(pi), IsWithinBoundaryRadius ;
 *   - portee minimale : les deux branches de CheckRange (RANGED + portee de melee, sinon IsWithinCombatRange).
 * Soi-meme, le vehicule porteur, une cible hors du monde ou d'une autre carte, un sort passif ou sans portee lisible
 * (RangeEntry nul ou ID 1) : SPELL_CAST_OK, le coeur jugera.
 */
SpellCastResult CoaPreControleCible(Player* bot, SpellInfo const* info, Unit* target, int8& vue)
{
    if (!target || target == bot || bot->GetVehicleBase() == target || !target->IsInWorld() ||
        target->GetMap() != bot->GetMap() || info->HasAttribute(SPELL_ATTR0_PASSIVE))
        return SPELL_CAST_OK;
    if (info->Targets & (TARGET_FLAG_ITEM | TARGET_FLAG_GAMEOBJECT_ITEM))
        return SPELL_CAST_OK;
    if (info->Effects[0].Effect == SPELL_EFFECT_OPEN_LOCK || info->Effects[0].Effect == SPELL_EFFECT_SKINNING)
        return SPELL_CAST_OK;
    /*
     * Lot 18 (11/10, match humain id=43 : Overload Mechanical 707365 « mecaniques seulement », Call of the Maelstrom 654545
     * « elementaires seulement » ; plus tot Mass Grave 803741, Ghoulify 800706) : SPELL_FAILED_TARGET_IS_PLAYER rendu par
     * StrictCheck a chaque tour. RECOPIE de SpellInfo::CheckTarget (SpellInfo.cpp:1951-1983) : sur un joueur,
     * SPELL_ATTR5_NOT_ON_PLAYER, puis CheckTargetCreatureType (TargetCreatureType contre le masque de type de la cible, cas
     * Warlock/categorie 1179 compris).
     *   - sort a cible unite (branche `else` de PlayerbotAI::CastSpell, PlayerbotAI.cpp:3973) : CheckCast l'appelle sur la
     *     cible (Spell.cpp:5965) -- meme verdict, ou au pire un autre refus plus tot dans CheckTarget ;
     *   - sort au sol ou a source (SetDst, aucune cible unite) : le coeur ne le refuserait pas, mais la selection des
     *     cibles de zone passe chaque unite par le meme CheckTarget (implicit, Spell.cpp:1965 et 9199) -- le joueur vise
     *     ne serait jamais touche. Limite : un familier du bon type pres de lui aurait pu l'etre (cas non rencontre).
     *   Dans les DEUX cas, aucun changement de jeu : StrictCheck (appele juste apres par CastFirst) pose la cible unite
     *   sur le Spell quel que soit le sort (SetUnitTarget, sans InitExplicitTargets), et Spell::CheckCast appelle
     *   CheckTarget sur elle sans condition (Spell.cpp:5963-5967) -- il rendait deja TARGET_IS_PLAYER, hors banc. Ce test
     *   n'ecarte rien de ce que StrictCheck accepte ; il evite le Spell construit, le demi-tour inutile et l'echec
     *   consigne (RecordFailure : TARGET_IS_PLAYER ne figure plus dans les compteurs d'echec de CoaBots.log).
     *   Cible non joueur (PNJ, familier) : non jugee ici, StrictCheck seul.
     */
    if (target->IsPlayer() && (info->HasAttribute(SPELL_ATTR5_NOT_ON_PLAYER) || !info->CheckTargetCreatureType(target)))
        return SPELL_FAILED_TARGET_IS_PLAYER;
    /*
     * Sort au sol (champ Targets de Spell.dbc = TARGET_FLAG_DEST_LOCATION) : PlayerbotAI::CastSpell le lance sur la
     * POSITION de la cible (targets.SetDst(*target), PlayerbotAI.cpp:3965), et Spell::CheckRange (strict, Spell.cpp:7251-
     * 7254) refuse OUT_OF_RANGE si cette position est a plus de max_range + GetLeewayBonusRadius() (+ taille du lanceur,
     * IsWithinDist3d) du lanceur. Tour 8 du banc (11/10 01:22-01:42 UTC) : Tranquil Circle 801205 (portee 20, refus a
     * 24,7 yd) x14, Standard of Might 706805 (portee 25, refus a 28,5-29,9 yd) x7, Resonance Rune 803679 (portee 20) x4,
     * tous rendus par StrictCheck a chaque tour, sans banc.
     * RECOPIE de la branche destination seule, jamais plus stricte :
     *   - seulement si le coeur garde la destination : InitExplicitTargets (Spell.cpp:773-787) la retire quand le masque
     *     explicite ne demande pas TARGET_FLAG_DEST_LOCATION ;
     *   - portee maximale : le coeur sans cible unite prend GetMaxRange(true) (amie, Unit.cpp:12552), StrictCheck prend
     *     celle de la cible ; on garde la PLUS GRANDE des deux, SPELLMOD_RANGE compris (fonction croissante) ;
     *   - ni portee minimale, ni vue, ni la branche unite que StrictCheck ajoute (il pose aussi la cible unite) : laisses
     *     a StrictCheck, comme avant. Pas de trajectoire (HasTraj) : m_speed n'est pose que par un paquet client.
     */
    if (info->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        if (!(info->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION) || !info->RangeEntry ||
            info->RangeEntry->ID == 1)
            return SPELL_CAST_OK;
        float portee = std::max(bot->GetSpellMaxRangeForTarget(target, info),
                                bot->GetSpellMaxRangeForTarget(nullptr, info));
        if (Player* modOwner = bot->GetSpellModOwner())
            modOwner->ApplySpellMod(info->Id, SPELLMOD_RANGE, portee);
        if (!bot->IsWithinDist3d(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
                                 portee + bot->GetLeewayBonusRadius()))
            return SPELL_FAILED_OUT_OF_RANGE;
        return SPELL_CAST_OK;
    }
    if (info->Targets & TARGET_FLAG_SOURCE_LOCATION)
        return SPELL_CAST_OK;
    bool const ignoreVue = info->HasAttribute(SPELL_ATTR2_IGNORE_LINE_OF_SIGHT) ||
                           info->HasAttribute(SPELL_ATTR5_ALWAYS_AOE_LINE_OF_SIGHT);
    // Revue m1 : sans cible unitaire EXPLICITE (zone autour du lanceur, soin de groupe), le coeur retire la cible
    // (Spell::InitExplicitTargets) et ne teste ni vue ni portee sur elle : ne pas etre plus strict.
    // SAUF la ligne de vue, que StrictCheck (appele juste apres) teste quand meme : il pose la cible unite sur le Spell
    // SANS passer par InitExplicitTargets, et Spell::CheckCast (Spell.cpp:5977-6006) juge alors la vue sur elle. Tour 4
    // du banc (10/10 22:05-22:10 UTC, Starcaller 1422) : 22 refus LINE_OF_SIGHT, tous sur des sorts sans cible unite
    // explicite (cibles implicites lues dans Spell.dbc : Eclipse of Fury 570218 = 22 SRC_CASTER, Celestial Cleave
    // 802435 = 104 cone, Vigil of the Moon 802797 = 87 DEST_DEST, Stellar Drift 800501 et Starshatter 801135 = 1
    // CASTER). Les ecarter ici rend le MEME verdict que StrictCheck, sans construire de Spell : aucun changement de jeu.
    // Tour 8 du banc (11/10 01:22-01:42 UTC) : meme raisonnement pour la PORTEE. Hoarfrost 572733 (Runemaster, cibles
    // implicites 104 cone, portee 8, champ Targets 0) : x57 OUT_OF_RANGE de StrictCheck a 24-110 yd, a chaque tour. Le
    // coeur, lui, lancerait le cone dans le vide (cible unite retiree, aucun test de portee) ; StrictCheck le refuse par
    // la branche unite de CheckRange, recopiee plus bas. La portee est donc jugee ici comme StrictCheck la juge (rien de
    // ce qu'il accepte n'est ecarte) ; l'orientation non : StrictCheck la refuse, mais ici elle ferait TOURNER le bot.
    bool const uniteExplicite = info->GetExplicitTargetMask() & TARGET_FLAG_UNIT_MASK;

    if (!ignoreVue)
    {
        if (vue < 0)
            vue = bot->IsWithinLOSInMap(target, VMAP::ModelIgnoreFlags::M2, LINEOFSIGHT_ALL_CHECKS) ? 1 : 0;
        if (!vue)
            return SPELL_FAILED_LINE_OF_SIGHT;
    }

    if (!info->RangeEntry || info->RangeEntry->ID == 1)
        return SPELL_CAST_OK;
    uint32 const rangeType = info->RangeEntry->Flags;
    float maxRange = bot->GetSpellMaxRangeForTarget(target, info);
    float const minRange = bot->GetSpellMinRangeForTarget(target, info);
    if (Player* modOwner = bot->GetSpellModOwner())
        modOwner->ApplySpellMod(info->Id, SPELLMOD_RANGE, maxRange);

    if (rangeType == SPELL_RANGE_MELEE)
    {
        float const reelle = maxRange - (bot->GetLeewayBonusRange(target) > 0.0f ? MIN_MELEE_REACH : 2 * MIN_MELEE_REACH);
        if (!bot->IsWithinMeleeRange(target, std::max(reelle, 0.0f)))
            return SPELL_FAILED_OUT_OF_RANGE;
    }
    else if (!bot->IsWithinCombatRange(target, maxRange))
        return SPELL_FAILED_OUT_OF_RANGE;

    if (uniteExplicite && (info->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT) &&
        !bot->HasInArc(static_cast<float>(M_PI), target) && !bot->IsWithinBoundaryRadius(target))
        return SPELL_FAILED_UNIT_NOT_INFRONT;

    if (rangeType == SPELL_RANGE_RANGED && !bot->IgnoresSpellMinRange(info))
    {
        if (bot->IsWithinRange(target, minRange + bot->GetMeleeRange(target)))
            return SPELL_FAILED_TOO_CLOSE;
    }
    else if (minRange > 0.0f && bot->IsWithinCombatRange(target, minRange))
        return SPELL_FAILED_TOO_CLOSE;

    return SPELL_CAST_OK;
}

/*
 * Le coeur refusera ce sort parce que le bot bouge : condition RECOPIEE de Spell::prepare (Spell.cpp:3592-3604), lanceur
 * joueur, sort non declenche (CastSpell : TRIGGERED_NONE) -- canalisation ou temps d'incantation, drapeau d'interruption
 * par le mouvement, ni grace de Manastorm ni sort mobile du coeur (Unit::CanCastSpellWhileMoving), et pour une
 * canalisation sans temps d'incantation, pas SPELL_ATTR5_ALLOW_ACTION_DURING_CHANNEL (IsActionAllowedChannel).
 * Banc du 11/10 00:30-00:47 UTC (tour 7) : la canalisation sans temps d'incantation manquait -- Testament of Fortitude
 * 500678 (SPELL_ATTR1_IS_CHANNELED, InterruptFlags MOVEMENT) partait au coeur en courant, refus_castspell x4 et banc.
 * Le temps d'incantation est lu sans Spell (le coeur passe le sien a CalcCastTime), comme avant ce correctif.
 */
bool CoaRefuseEnMouvement(Player* bot, SpellInfo const* info)
{
    if (!bot->isMoving() || !(info->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT))
        return false;
    uint32 const incantationMs = info->CalcCastTime(bot);
    if (!incantationMs && !info->IsChanneled())
        return false;
    if (bot->HasManastormMovementGrace() || bot->CanCastSpellWhileMoving(info))
        return false;
    return incantationMs || !info->IsActionAllowedChannel();
}

// Casts the first ability of the list that passes the strict check on the target. Returns it,
// or nullptr when none went off. With a usage kind, why each ability failed is counted.
SpellInfo const* CastFirst(PlayerbotAI* botAI, Player* bot, std::vector<Usable> const& spells, Unit* target,
                           uint8 usage = 255)
{
    time_t const now = time(nullptr);
    auto& benched = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->benchedSpells;
    // CoaArene L0 : un seul test par appel, faux hors engagement ou option eteinte.
    bool const filtreEtat = !spells.empty() && CoaAreneFiltreEtatActif(botAI, bot);
    // Trace d'arene 1v1 : un test par appel ; faux (InArena) pour tout bot hors arene.
    bool const trace = !spells.empty() && CoaAreneTraceActive(botAI, bot);
    // CoA (10/10/2026 nuit, arene 1v1 id=1, gladiateur 2543) : sous une aura SPELL_AURA_ALLOW_ONLY_ABILITY (263 ; ici
    // Boulder Dash 500692, dont chaque coup 500693 la repose pendant les 10 s de la ruee ; aussi Bladestorm, Killing
    // Spree...), le joueur porte PLAYER_ALLOW_ONLY_ABILITY et Spell::CheckCast (Spell.cpp:5754) refuse TOUT sort lance
    // sans drapeau de declenchement par SPELL_FAILED_SPELL_IN_PROGRESS, sauf les sorts « au prochain coup »
    // (SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE) et les passifs. Le bot les tentait tous a chaque tick (36 refus en 9 s,
    // trace du 10/10 19:59:04-19:59:12), sans banc (ce refus n'est pas durable), et l'action d'attaque occupait le
    // tour au lieu de laisser le bot avancer. Meme test que le coeur : rien de ce qu'il accepterait n'est ecarte.
    bool const seulementCapacites = !spells.empty() && bot->HasPlayerFlag(PLAYER_ALLOW_ONLY_ABILITY);
    // Pre-controle de la cible (CoaPreControleCible) : ligne de vue calculee une fois par appel, et un seul demi-tour.
    int8 vueCible = -1;
    bool tourne = false;
    // Choix tactiques d'arene 1v1 et de duel : l'adversaire, ou nullptr (tout autre bot : deux tests de champ).
    Unit* const advTactique = !spells.empty() ? CoaAreneTactiqueAdversaire(botAI, bot) : nullptr;

    for (Usable const& spell : spells)
    {
        // What the bot will really cast: a proc or a talent may have replaced this spell (see
        // EffectiveSpell). Cooldowns, cast time, the bench and the failure log all go by the
        // replacement; only the cast itself keeps the id the rotation lists, because that is the
        // id the lists hold and the one PlayerbotAI::CastSpell has to be given for the proc to be
        // consumed.
        SpellInfo const* const info = EffectiveSpell(bot, spell.info);

        // CoaArene L0 : refuse d'avance par l'etat du lanceur (etourdi, en fuite, silence...), voir
        // CoaAreneBloqueParEtat. Ni banc ni echec consigne : le sort n'est pas en cause.
        if (filtreEtat && CoaAreneBloqueParEtat(bot, info))
            continue;

        // Voir `seulementCapacites` : refuse d'avance, ni banc ni echec consigne.
        if (seulementCapacites && !info->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE) &&
            !info->HasAttribute(SPELL_ATTR0_PASSIVE))
            continue;

        // A spell on cooldown would only fail with SPELL_FAILED_NOT_READY.
        if (bot->HasSpellCooldown(info->Id))
            continue;

        // CoA (10/10/2026 soir, arene 1v1) : un sort qui exige une aura du lanceur absente (CasterAuraSpell : les
        // consommateurs d'Advantage du Ranger -- Skullpiercer, Serrated Shot, Assault, Uncover Weakness -- ou les
        // techniques d'une forme) serait refuse SPELL_FAILED_CASTER_AURASTATE, que IsLastingFailure met au banc
        // 20 s : le Ranger perdait son burst 20 s chaque fois que le tourniquet le tentait a 0 charge. Meme test
        // que Spell::CheckCast (Spell.cpp:5856), sans banc : le sort repart des que l'aura est la.
        // Banc du 11/10 (tour 7) : aussi CasterAuraState, CasterAuraStateNot et ExcludeCasterAuraSpell, le reste du meme
        // bloc du coeur (CoaRefuseParAuraDuLanceur).
        if (CoaRefuseParAuraDuLanceur(bot, info))
            continue;

        // Banc d'arene du 10/10 (CoaRefuseParFormeOuArme, CoaPeutPayer) : forme absente, arme requise absente,
        // ressource insuffisante -- refus certains du coeur, ecartes sans banc ni echec consigne : le sort repart des
        // que la forme, l'arme ou la ressource est la (avant : banc de 20 s, 20 s et 5 s).
        if (CoaRefuseParFormeOuArme(bot, info))
            continue;
        if (!CoaPeutPayer(bot, info))
        {
            // Survie d'arene (11/10, Sun Cleric 478, match id=93) : les refus faute de mana etaient muets -- 18 s sans un
            // sort, adversaire en vue et a portee. Trace seulement (gladiateur d'arene 1v1) ; le sort reste ecarte.
            if (trace)
                CoaAreneNoterRefusRessource(botAI, bot, info, target);
            continue;
        }

        // Banc d'arene du 11/10 : familier exige et absent (CoaExigeFamilierAbsent), refus certains lus sur la cible --
        // etat d'aura, ressource a bruler, rien a dissiper (CoaRefusCibleCertain). Recopies du coeur, ecartes sans banc
        // ni echec consigne : le sort repart des que le familier, l'etat ou l'aura est la.
        if (CoaExigeFamilierAbsent(bot, info) || CoaRefusCibleCertain(bot, info, target))
            continue;

        // Lot 20 (13/10) : soin ou bouclier dont l'aura est deja posee par le bot sur la cible (CoaSoinDejaPose), pour
        // tous les bots CoA. Ecarte sans banc ni echec consigne : il repart des que l'aura tombe. Trace en arene 1v1 et en
        // duel seulement (advTactique).
        if (CoaSoinDejaPose(bot, spell, info, target))
        {
            if (advTactique)
                CoaAreneNoterDejaPose(botAI, bot, info, target, trace);
            continue;
        }

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

        // Choix tactiques (CoaAreneRefusTactique) : le coeur accepterait ce sort, mais il ne toucherait pas l'adversaire.
        // Ecarte sans banc ni echec consigne. Hors du cone mais dans son rayon : le bot se tourne (memes gardes que le
        // demi-tour du pre-controle ci-dessous, un seul par appel) et le sort est rejuge.
        if (advTactique)
        {
            uint8 refus = CoaAreneRefusTactique(bot, info, advTactique, target);
            // 11/10 (match humain id=14, Gardien 448) : a pv quasi nul, Battle Rush 501546 l'a jete sur l'adversaire au
            // lieu de rester sous son etendard. Aux paliers danger et critique, plus aucune charge ni saut VERS lui.
            if (refus == COA_TACTIQUE_AUCUN && CoaAreneEtatDe(botAI).survieNiveau >= 2 && CoaAreneSortDeRapprochement(info))
                refus = COA_TACTIQUE_SURVIE_CHARGE;
            if (refus == COA_TACTIQUE_AUCUN && CoaAreneSurvieRefuseIncantation(botAI, bot, info, spell.kind))
                refus = COA_TACTIQUE_SURVIE_INCANTATION;
            if (refus == COA_TACTIQUE_HORS_CONE && !tourne && bot->IsAlive() &&
                !bot->HasUnitState(UNIT_STATE_LOST_CONTROL) && !CoaAreneMouvementVoulu(botAI))
            {
                ServerFacade::instance().SetFacingTo(bot, advTactique);
                tourne = true;
                refus = CoaAreneRefusTactique(bot, info, advTactique, target);
            }
            if (refus != COA_TACTIQUE_AUCUN)
            {
                CoaAreneNoterRefusTactique(botAI, bot, info, advTactique, refus, trace);
                continue;
            }
        }

        // Pre-controle (CoaPreControleCible) : hors de portee, sans ligne de vue ou trop pres, le sort est ecarte d'avance,
        // sans banc ni echec consigne (StrictCheck ne mettait aucun de ces trois refus au banc). Pas face a la cible :
        // le bot se TOURNE (comme PlayerbotAI::CastSpell le ferait), une fois par appel, sauf s'il suit un deplacement
        // voulu de l'IA d'arene (recul, retour, cachette : se retourner le figerait) ou s'il a perdu le controle
        // (etourdi, en fuite, en charge) ; sinon, le sort est ecarte.
        SpellCastResult pre = CoaPreControleCible(bot, info, target, vueCible);
        if (pre == SPELL_FAILED_UNIT_NOT_INFRONT && !tourne && bot->IsAlive() &&
            !bot->HasUnitState(UNIT_STATE_LOST_CONTROL) && !CoaAreneMouvementVoulu(botAI))
        {
            ServerFacade::instance().SetFacingTo(bot, target);
            tourne = true;
            pre = CoaPreControleCible(bot, info, target, vueCible);
        }
        if (pre != SPELL_CAST_OK)
            continue;

        SpellCastResult const check = StrictCheck(bot, info, target);
        if (check == SPELL_CAST_OK)
        {
            // PlayerbotAI::CastSpell refuses a spell with a cast time while the bot moves: stop
            // now and cast it on a later tick, once standing still.
            // CoA (banc d'arene du 10/10, `en_mouvement` x15) : sauf pendant un deplacement VOULU de l'IA d'arene
            // (recul, retour, cachette, cacheur en route) -- l'arreter defaisait ce deplacement (le retour se
            // declarait fini des que le bot ne bougeait plus) ; on passe alors au sort suivant, un instantane s'il y en a.
            // CoA (banc d'arene du 11/10) : seulement si le mouvement l'interrompt. Un sort a incantation SANS
            // SPELL_INTERRUPT_FLAG_MOVEMENT (Makeshift Dynamite 504590 du Tinker, « Usable while moving » ; Throw Weapon
            // 806878, Felbreak 800203) part en courant chez le coeur (Spell::prepare, Spell.cpp:3594) : l'arreter etait
            // plus strict que lui. PlayerbotAI::CastSpell ne l'empeche pas : son test (PlayerbotAI.cpp:4012) lit
            // Spell::GetCastTime() AVANT prepare, donc 0 (Spell.cpp:671) -- c'est prepare qui refuse, sur ce drapeau.
            // Condition RECOPIEE de prepare : drapeau de mouvement, ni grace de Manastorm, ni sort mobile du coeur
            // (Unit::CanCastSpellWhileMoving : sorts Tinker, Starcaller, Witch Doctor... et aura 313).
            // Banc du 11/10 (tour 7) : et la canalisation sans temps d'incantation (CoaRefuseEnMouvement).
            if (CoaRefuseEnMouvement(bot, info))
            {
                bool const voulu = CoaAreneMouvementVoulu(botAI);
                // Jamais pendant une charge, un saut ou sous controle (UNIT_STATE_LOST_CONTROL) : arreter couperait la
                // trajectoire imposee (meme garde que le demi-tour plus haut). Le sort est ecarte quand meme.
                if (!voulu && !bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
                    bot->StopMoving();
                if (usage != 255)
                    RecordFailure(usage, info->Id, FAILURE_MOVING);
                if (trace)
                    CoaAreneTracerSort(botAI, bot, info, target, check, voulu ? "en_mouvement_voulu" : "en_mouvement");
                continue;
            }

            if (PreemptsAttackCast(usage))
                DropAttackCast(bot);

            bool const sitting = !bot->IsStandState();
            bool const casting = bot->IsNonMeleeSpellCast(false, true, true);
            if (botAI->CastSpell(spell.info->Id, target))
            {
                if (trace)
                    CoaAreneTracerSort(botAI, bot, info, target, SPELL_CAST_OK, nullptr);
                return spell.info;
            }
            if (trace)
                CoaAreneTracerSort(botAI, bot, info, target, check,
                                   sitting ? "assis" : casting ? "incantation_en_cours" : "refus_castspell");

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
        // Familier ou serviteur absent, refus rendu par un script (voir NoPetBenchSeconds).
        else if (check == SPELL_FAILED_NO_PET)
            benched[info->Id] = now + NoPetBenchSeconds;

        if (usage != 255)
            RecordFailure(usage, info->Id, uint16(check));
        if (trace)
            CoaAreneTracerSort(botAI, bot, info, target, check, nullptr);
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
    USAGE_SOIN_SOI,  // IA soins/survie, lot 3 : « coa soin soi » (soin sur soi des tanks et des degats)
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
    "poste peuple",
    "soin soi"
};

bool PreemptsAttackCast(uint8 usage)
{
    switch (usage)
    {
        case USAGE_HEAL: case USAGE_GROUP_HEAL: case USAGE_HOT:
        case USAGE_TAUNT: case USAGE_DEFENSIVE: case USAGE_DISPEL: case USAGE_INTERRUPT:
        case USAGE_SOIN_SOI:
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

// Banc des soins (S0, CoaBancSoin.cpp) : le dernier refus consigne par CE fil, relu par la trace `coa soin action:`
// d'une action de soin (BancSoinTracer). Deux ecritures par refus, sans verrou ; remis a zero au debut de chaque action
// de soin. Rien d'autre ne le lit.
struct BancSoinEchec
{
    uint32 sort = 0;
    uint16 code = 0;
};
thread_local BancSoinEchec t_bancSoinEchec;

void RecordFailure(uint8 kind, uint32 spellId, uint16 reason)
{
    t_bancSoinEchec.sort = spellId;
    t_bancSoinEchec.code = reason;
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
                            USAGE_STEALTH, USAGE_SOIN_SOI })
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

// Banc des soins (S0) : une ligne `coa soin action:` pour le passage d'une action de soin (heal, hot, group_heal,
// defensive) d'un membre d'essai en cours -- CoaBancSoin::TracerAction borne le debit et sort sur un chargement
// atomique hors essai. `cast` : le sort lance (resultat=lance) ; sinon `vide` (aucun sort candidat : rien, code
// FAILURE_NOTHING) ; sinon le dernier refus consigne par RecordFailure pendant l'action (refus), ou rien.
void BancSoinTracer(Player* bot, char const* action, SpellInfo const* cast, bool vide)
{
    if (!bot || !CoaBancSoin::MembreEngage(bot->GetGUID().GetCounter()))
        return;
    if (cast)
        CoaBancSoin::TracerAction(bot, action, "lance", cast->Id, 0);
    else if (vide)
        CoaBancSoin::TracerAction(bot, action, "rien", 0, FAILURE_NOTHING);
    else if (t_bancSoinEchec.sort || t_bancSoinEchec.code)
        CoaBancSoin::TracerAction(bot, action, "refus", t_bancSoinEchec.sort, t_bancSoinEchec.code);
    else
        CoaBancSoin::TracerAction(bot, action, "rien", 0, 0);
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

    // CoA, lot S des evenements (08/10/2026, coa-project/docs/EVENEMENTS-lot-S.md) : le joueur servi par un bot
    // INSCRIT a son evenement, hors du groupe, recoit aussi buffs et dissipations (CoaBuffAction, CoaDispelAction,
    // CoaDispelTrigger), a la meme portee. Option Directeur.EvenementSoins eteinte : nul, rien ne change.
    if (Player* servi = CoaDirecteur::JoueurASoigner(bot))
        if (std::find(members.begin(), members.end(), servi) == members.end() && bot->IsWithinDistInMap(servi, 40.0f))
            members.push_back(servi);

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

// CoaArene IA, lots L0 et L1 (10/10/2026) : etat d'engagement, multiplicateur, recul, rupture, ouverture,
// burst. Inerte sans AiPlayerbot.CoaArene.*. Voir l'en-tete du fichier inclus.
#include "CoaAreneIa.inc"

/*
 * IA SOINS/SURVIE, LOT 3 (16/10/2026, coa-project/docs/DIAG-banc-soins-survie-20261016.md, priorites 2, 5 et 6) : LA
 * SURVIE DE TOUS LES ROLES, sous AiPlayerbot.CoaIa.Survie. Avant : un bot de degats ou un tank n'avait qu'un point
 * d'entree, « coa defensive » sous 45 % (65 % pour un tank), qui lancait le premier defensif pret dans l'ordre du niveau
 * requis, puis un soin MONO-cible sur soi ; aucun soin sur soi hors de ce repli, aucune reaction a une perte rapide, le vol
 * de vie jamais avance, la potion seulement sous 25 %. Mesure au banc (volet B) : premier defensif a 43 % de vie en
 * mediane (degats, B1).
 *
 * Paliers, recopies de la survie d'arene (CoaAreneIa.inc, IaSurvie : pression 70 / danger 50 / critique 30, pente de 15
 * points en 2 a 4 s, recharge legere <= 60 s) sans l'arene :
 *   - defensif LEGER (recharge <= 60 s) sous mediumHealth (65 %) ou sur une perte rapide (>= 15 points en 4 s au plus) ;
 *   - defensif MOYEN (60 a 120 s) sous lowHealth (45 %), le filet d'avant ;
 *   - GROS defensif (recharge >= 120 s) sous 30 % ;
 *   - candidats tries par force (reduction ou absorption equivalente), puis par niveau requis ;
 *   - soin sur soi sous 65 % (« coa soin soi », tanks et degats ; le soigneur se soigne par « coa heal »), soins de zone
 *     qui incluent le lanceur admis en dernier recours ; jamais un sort qui enracine son lanceur (MARQUE_IMMOBILISE) ;
 *   - vol de vie en tete de la rotation d'attaque sous 65 % ;
 *   - potion des 45 % quand aucun defensif n'est pret (AiPlayerbot.CoaIa.SurviePotion).
 * En arene 1v1 et en duel, la survie d'arene (IaSurvie) garde la main : rien de ce lot n'y joue (CoaIaSurvieActive),
 * comme « coa defensive » que le multiplicateur d'arene y coupe deja.
 */
constexpr float CoaIaCritiquePv = 30.0f;
constexpr float CoaIaPerteRapidePv = 15.0f;
constexpr uint32 CoaIaReleveMs = 250;
constexpr uint32 CoaIaPerteFenetreMs = 4000;
constexpr uint32 CoaIaRechargeLegereMs = 60 * IN_MILLISECONDS;
constexpr uint32 CoaIaRechargeLourdeMs = 120 * IN_MILLISECONDS;
// Force d'un defensif dont l'effet ne se lit pas (script, proc) : au milieu, derriere les forts mesures.
constexpr float CoaIaForceInconnue = 20.0f;

// Revue du lot 5 : vie gardee apres le paiement d'un sort paye en vie (PowerType POWER_HEALTH ; Bloodmage : formes
// maudites, Eternal Resolve 801962 22 %, Wicked Howl 804207 17 % de la vie de base). Spell::CheckPower ne refuse que
// vie <= cout ; la survie (lots 3 et 5) ne lance pas un sort qui laisserait le bot sous ce plancher.
constexpr float CoaIaResteMinPv = 15.0f;

/*
 * I4 du 17/10 (coa-project/docs/DIAG-equilibrage-classes-20261017.md, Felsworn Slayer 8 : Immolation Aura 572617 coute 50 PV
 * par seconde, Spell.dbc champ 44 ManaPerSecond, PowerType -2 ; perte sous bouclier pleinement absorbant 0,8-1,6 %/s contre
 * 0,07-0,13 %/s pour l'Infernal qui ne la connait pas). Le coeur preleve ce cout tant que l'aura tient (Aura::Update, cout
 * par seconde sur le lanceur proprietaire, aura retiree seulement quand la vie ne suffit plus). CoaIaPayeEnVieTropCher ne
 * lisait que le cout du lancer (50, aussi). Sorts concernes parmi ceux qu'un personnage connait (releve du 17/10) : les
 * 7 rangs d'Immolation Aura (800207, 501270-501272, 572615-572617), Final Embrace 524865 (Bloodmage, 30 + 1 par niveau),
 * Transfer Life 801530 (Necromancer, 5 + 1 par niveau). Sous AiPlayerbot.CoaIa.Survie.
 */
bool CoaIaPayeEnVieParSeconde(SpellInfo const* info)
{
    return info->PowerType == POWER_HEALTH && (info->ManaPerSecond > 0 || info->ManaPerSecondPerLevel > 0);
}

// Sort paye en vie PAR SECONDE, et le bot sous mediumHealth (65 %) : ne pas le lancer (survie, « coa attack », « coa aoe »).
bool CoaIaVieParSecondeRefusee(Player* bot, SpellInfo const* info)
{
    return sPlayerbotAIConfig.coaIaSurvie && CoaIaPayeEnVieParSeconde(info) &&
           bot->GetHealthPct() < float(sPlayerbotAIConfig.mediumHealth);
}

// Sous lowHealth (45 %) ou sur une perte rapide : retire du bot les auras qu'il s'est posees et qui lui coutent de la vie
// chaque seconde (CoaIaPayeEnVieParSeconde). Appele par « coa survie » (CoaSurvieTrigger), a chaque passage. Une aura posee
// sur un AUTRE (Transfer Life 801530 sur les serviteurs) n'est pas appartenue au bot : non touchee.
void CoaIaRetirerAurasPayeesEnVie(Player* bot, bool perteRapide)
{
    if (!sPlayerbotAIConfig.coaIaSurvie || !bot->IsAlive() ||
        (bot->GetHealthPct() >= float(sPlayerbotAIConfig.lowHealth) && !perteRapide))
        return;
    ObjectGuid const lanceur = bot->GetGUID();
    bool trouve = false;
    for (auto const& [id, aura] : bot->GetOwnedAuras())
        if (aura && aura->GetCasterGUID() == lanceur && CoaIaPayeEnVieParSeconde(aura->GetSpellInfo()))
        {
            trouve = true;
            break;
        }
    if (trouve)
        bot->RemoveOwnedAuras([lanceur](Aura const* aura)
            { return aura->GetCasterGUID() == lanceur && CoaIaPayeEnVieParSeconde(aura->GetSpellInfo()); });
}

bool CoaIaPayeEnVieTropCher(Player* bot, SpellInfo const* info)
{
    if (info->PowerType != POWER_HEALTH)
        return false;
    if (CoaIaVieParSecondeRefusee(bot, info))
        return true;
    float const reste = float(bot->GetHealth()) - float(std::max(0, info->CalcPowerCost(bot, info->GetSchoolMask())));
    return reste * 100.0f / std::max(1.0f, float(bot->GetMaxHealth())) < CoaIaResteMinPv;
}

bool CoaIaSurvieActive(Player* bot)
{
    return sPlayerbotAIConfig.coaIaSurvie && bot->IsAlive() && !(CoaAreneSurvieArmee() && CoaAreneUnContreUn(bot));
}

// Plus forte baisse de vie (en points de %) sur les 4 dernieres secondes, et met a jour le releve (CoaSurvieReleve).
float CoaIaPerteRecente(PlayerbotAI* botAI, Player* bot)
{
    CoaAiObjectContext::CoaSurvieReleve& r = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->survieReleve;
    if (!bot->IsAlive())
    {
        r = CoaAiObjectContext::CoaSurvieReleve();
        return 0.0f;
    }
    uint32 const maintenant = getMSTime();
    float const pv = bot->GetHealthPct();
    uint8 const dernier = uint8((r.tete + 15) % 16);
    if (!r.n || getMSTimeDiff(r.ms[dernier], maintenant) >= CoaIaReleveMs)
    {
        r.ms[r.tete] = maintenant;
        r.pv[r.tete] = pv;
        r.tete = uint8((r.tete + 1) % 16);
        if (r.n < 16)
            ++r.n;
    }
    float pire = 0.0f;
    for (uint8 i = 0; i < r.n; ++i)
    {
        uint8 const idx = uint8((r.tete + 15 - i) % 16);
        if (getMSTimeDiff(r.ms[idx], maintenant) > CoaIaPerteFenetreMs)
            break;
        pire = std::max(pire, r.pv[idx] - pv);
    }
    return pire;
}

uint32 CoaIaRechargeMs(SpellInfo const* info)
{
    return std::max(info->RecoveryTime, info->CategoryRecoveryTime);
}

// Le palier de vie admet-il ce defensif (sort effectif) ?
bool CoaIaDefensifAdmis(SpellInfo const* info, float pv, bool perteRapide)
{
    uint32 const recharge = CoaIaRechargeMs(info);
    if (pv < CoaIaCritiquePv)
        return true;
    if (recharge >= CoaIaRechargeLourdeMs)
        return false;
    if (pv < float(sPlayerbotAIConfig.lowHealth))
        return true;
    if (recharge > CoaIaRechargeLegereMs)
        return false;
    return pv < float(sPlayerbotAIConfig.mediumHealth) || perteRapide;
}

// Force d'un defensif : reduction de degats equivalente, en %. Surcharge du lot 1 d'abord (effet fait par script), sinon
// lue dans les effets (valeurs de base, sans tirage), un niveau de sort declenche ; 0 si rien ne se lit.
float CoaIaForceDefensive(Player* bot, SpellInfo const* info, uint8 profondeur = 0)
{
    if (!profondeur)
        if (CoaSurcharge const* surcharge = CoaSurchargeDe(info->Id))
            if (surcharge->force)
                return float(surcharge->force);
    float const vieMax = std::max(1.0f, float(bot->GetMaxHealth()));
    float force = 0.0f;
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (!effect.IsEffect())
            continue;
        if (IsAuraEffect(effect))
        {
            float const valeur = float(CoaValeurDeBase(effect));
            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN: force = std::max(force, -valeur); break;
                case SPELL_AURA_SCHOOL_IMMUNITY: force = std::max(force, 100.0f); break;
                case SPELL_AURA_SCHOOL_ABSORB: force = std::max(force, std::min(100.0f, valeur * 100.0f / vieMax)); break;
                case SPELL_AURA_MOD_INCREASE_HEALTH_PERCENT: force = std::max(force, valeur); break;
                case SPELL_AURA_MOD_INCREASE_HEALTH: force = std::max(force, std::min(100.0f, valeur * 100.0f / vieMax)); break;
                case SPELL_AURA_MOD_PARRY_PERCENT: case SPELL_AURA_MOD_DODGE_PERCENT: case SPELL_AURA_MOD_BLOCK_PERCENT:
                    force = std::max(force, valeur / 2.0f);
                    break;
                default:
                    break;
            }
        }
        else if ((effect.Effect == SPELL_EFFECT_TRIGGER_SPELL || effect.Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE) &&
                 effect.TriggerSpell && effect.TriggerSpell != info->Id && !profondeur)
            if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                force = std::max(force, CoaIaForceDefensive(bot, declenche, 1));
    }
    return force;
}

/*
 * Revue du 16/10 : un sort a cible unique (SpellInfo::IsSingleTarget, SPELL_ATTR5_LIMIT_N : relance, il quitte son porteur)
 * que le bot tient deja sur un AUTRE membre vivant de son groupe ou raid, sur sa carte. Infinite Shield 520457 (Chronomancer,
 * bouclier a 10 charges de proc) : « coa defensive » sous 65 % l'otait au tank pour le poser sur le Chronomancer. La survie
 * ne le pose pas sur le bot, sauf au palier critique (CoaIaCritiquePv). Meme lecture que CoaIaHotTenuAilleurs (lot 4), sans
 * seuil de duree : un bouclier pose ailleurs y reste tant qu'il tient.
 */
bool CoaIaUniqueTenuAilleurs(Player* bot, SpellInfo const* info, uint32 listeId)
{
    if (!info->IsSingleTarget())
        return false;
    for (Aura const* aura : bot->GetSingleCastAuras())
    {
        if (!aura || (aura->GetId() != info->Id && aura->GetId() != listeId))
            continue;
        WorldObject* owner = aura->GetOwner();
        if (!owner || owner == bot || owner->GetMap() != bot->GetMap())
            continue;
        Unit const* porteur = owner->ToUnit();
        if (porteur && porteur->IsAlive() && bot->IsInRaidWith(porteur))
            return true;
    }
    return false;
}

// Retire les sorts a cible unique tenus sur un autre membre (CoaIaUniqueTenuAilleurs), hors palier critique.
void CoaIaRetirerUniquesTenus(Player* bot, std::vector<Usable>& spells)
{
    if (bot->GetHealthPct() < CoaIaCritiquePv)
        return;
    spells.erase(std::remove_if(spells.begin(), spells.end(), [bot](Usable const& spell)
        { return CoaIaUniqueTenuAilleurs(bot, EffectiveSpell(bot, spell.info), spell.info->Id); }), spells.end());
}

/*
 * I6 du 17/10 (coa-project/docs/DIAG-equilibrage-classes-20261017.md, Necromancer 34-36) : Sacrifice Undead 807941, soin
 * « for each Undead minion destroyed », lance par le repli de soin de « coa defensive » sans aucun serviteur (14 lancers a
 * vide sur 16, 9/9 en Rime) : soin nul et 30 s de recharge perdues. Le module ne le refuse pas (seuls 802121 et 801530
 * rendent NO_PET). Meme regle que la survie d'arene (CoaAreneSurvieSorts) : un sort « par serviteur »
 * (CoaAreneSortParServiteur, rangs compris) est retire tant que le bot n'a aucune unite controlee vivante issue d'une
 * invocation (CoaAreneAServiteur). Listes de la survie du lot 3 seulement (AiPlayerbot.CoaIa.Survie).
 */
void CoaIaRetirerParServiteurSansServiteur(Player* bot, std::vector<Usable>& spells)
{
    if (spells.empty() || CoaAreneAServiteur(bot))
        return;
    spells.erase(std::remove_if(spells.begin(), spells.end(), [bot](Usable const& spell)
        { return CoaAreneSortParServiteur(spell.info->Id) || CoaAreneSortParServiteur(EffectiveSpell(bot, spell.info)->Id); }),
        spells.end());
}

// Les defensifs que le palier admet, du plus fort au plus faible (ordre du niveau requis a force egale). `trier` faux :
// la liste seule (isUseful, declencheurs).
std::vector<Usable> CoaIaDefensifs(PlayerbotAI* botAI, Player* bot, float pv, bool perteRapide, bool trier)
{
    std::vector<Usable> defensifs = KnownAbilities(botAI, bot, [](uint16 kind, uint16 marques)
        { return (kind & KIND_DEFENSIVE) && !(marques & MARQUE_PAS_SUR_SOI); });
    defensifs.erase(std::remove_if(defensifs.begin(), defensifs.end(), [bot, pv, perteRapide](Usable const& spell)
        { return !CoaIaDefensifAdmis(EffectiveSpell(bot, spell.info), pv, perteRapide) ||
                 CoaIaPayeEnVieTropCher(bot, EffectiveSpell(bot, spell.info)); }), defensifs.end());
    CoaIaRetirerUniquesTenus(bot, defensifs);
    CoaIaRetirerParServiteurSansServiteur(bot, defensifs);
    if (trier && defensifs.size() > 1)
    {
        // Relevee une fois par sort, puis triee : jamais de calcul dans le comparateur.
        std::vector<std::pair<float, Usable>> classes;
        classes.reserve(defensifs.size());
        for (Usable const& spell : defensifs)
        {
            float force = CoaIaForceDefensive(bot, EffectiveSpell(bot, spell.info));
            classes.emplace_back(force > 0.0f ? force : CoaIaForceInconnue, spell);
        }
        std::stable_sort(classes.begin(), classes.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        for (size_t i = 0; i < classes.size(); ++i)
            defensifs[i] = classes[i].second;
    }
    return defensifs;
}

// Un soin de zone lance sur le bot le couvre-t-il ? Non s'il soigne en cone devant le lanceur (TARGET_UNIT_CONE_ALLY).
bool CoaIaZoneInclutLanceur(SpellInfo const* info)
{
    for (SpellEffectInfo const& effect : info->Effects)
        if (effect.IsEffect() &&
            (effect.TargetA.GetTarget() == TARGET_UNIT_CONE_ALLY || effect.TargetB.GetTarget() == TARGET_UNIT_CONE_ALLY))
            return false;
    return true;
}

/*
 * IA SOINS/SURVIE, LOT 4 (16/10/2026, coa-project/docs/DIAG-banc-soins-rotation-20261016.md, points 1, 3, 4, 5, 6).
 */

// C4 (AiPlayerbot.CoaIa.ReserveSoigneur) : un soigneur en groupe n'attaque avec des sorts payes en mana que si tout le
// groupe proche (40 yd) est au-dessus de almostFullHealth (85 %) ET sa mana au-dessus de CoaIa.ReserveSoigneurMana (80 %).
// Avant : la reserve n'entrait en jeu que sous 35 % de mana (CoaHealerManaReserve) ; Tinker A1, essai 423 : mana de 89 a
// 32 % en 32 s sans un soin, puis le tank sous 65 % avec moins de 35 % de mana pour le soigner. Hors groupe (bot qui
// quete seul), rien ne change : « party member to heal » ne rend que lui-meme.
bool CoaIaSoigneurRetient(PlayerbotAI* botAI, Player* bot)
{
    if (!sPlayerbotAIConfig.coaIaReserveSoigneur || bot->getPowerType() != POWER_MANA || GetCoaRole(bot) != CoaRole::Heal)
        return false;
    Group* group = bot->GetGroup();
    if (!group)
        return false;
    bool besoin = bot->GetPowerPct(POWER_MANA) < float(sPlayerbotAIConfig.coaIaReserveSoigneurMana);
    for (GroupReference* ref = group->GetFirstMember(); ref && !besoin; ref = ref->next())
    {
        Player* member = ref->GetSource();
        // Meme lieu d'abord (P-137 : rien d'un autre fil), puis la vie.
        if (member && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
            bot->IsWithinDistInMap(member, 40.0f) && member->IsAlive() &&
            member->GetHealthPct() < float(sPlayerbotAIConfig.almostFullHealth))
            besoin = true;
    }
    if (!besoin)
        return false;
    // Comme SavingManaForHeals : un bot qui ne connait aucun soin ne retient rien.
    return !KnownAbilities(botAI, bot, [](uint16 kind)
        { return (kind & (KIND_HEAL | KIND_HOT)) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); }).empty();
}

// C2-bis (AiPlayerbot.CoaIa.HotUnique) : HoT a cible unique (SpellInfo::IsSingleTarget, SPELL_ATTR5_LIMIT_N) que le bot
// tient deja sur une AUTRE unite avec plus de 30 % de sa duree : ne pas le deplacer, sauf urgence (cible sous lowHealth,
// porteur au-dessus de mediumHealth). Nanobot Reconstruction 502561 (Tinker) : 483 lancers au volet A, deplace de membre
// en membre a chaque recharge globale, 17 % de mana de base chacun -- la mana du Tinker fondait en 46 s.
bool CoaIaHotTenuAilleurs(Player* bot, SpellInfo const* info, uint32 listeId, Unit* cible)
{
    if (!info->IsSingleTarget())
        return false;
    for (Aura const* aura : bot->GetSingleCastAuras())
    {
        if (!aura || (aura->GetId() != info->Id && aura->GetId() != listeId))
            continue;
        WorldObject* owner = aura->GetOwner();
        if (!owner || owner == cible || owner->GetMap() != bot->GetMap())
            continue;
        int32 const max = aura->GetMaxDuration();
        int32 const reste = aura->GetDuration();
        if (max > 0 && reste >= 0 && int64(reste) * 10 <= int64(max) * 3)
            continue;
        Unit const* porteur = owner->ToUnit();
        if (!porteur || !porteur->IsAlive())
            continue;
        if (cible->GetHealthPct() < float(sPlayerbotAIConfig.lowHealth) &&
            porteur->GetHealthPct() > float(sPlayerbotAIConfig.mediumHealth))
            continue;
        return true;
    }
    return false;
}

// C3 : vie que le sort rendra a `cible`, lue dans ses effets (valeurs de base sans tirage, bonus de soin du lanceur
// Unit::SpellHealingBonusDone), HoT sur toute sa duree, un niveau de declenche par TRIGGER_SPELL ou par aura
// PERIODIC_TRIGGER_SPELL. 0 ou 1 point : soin fait par script, illisible.
float CoaIaSoinEstime(Player* bot, SpellInfo const* info, Unit* cible, uint8 profondeur = 0)
{
    float total = 0.0f;
    float const vieMax = float(cible->GetMaxHealth());
    int32 const duree = info->GetMaxDuration();
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = info->Effects[i];
        if (!effect.IsEffect())
            continue;
        uint32 const valeur = uint32(std::max(0, CoaValeurDeBase(effect)));
        int32 const ticks = (duree > 0 && effect.Amplitude > 0) ? duree / effect.Amplitude : 1;
        if (effect.Effect == SPELL_EFFECT_HEAL)
            total += float(bot->SpellHealingBonusDone(cible, info, valeur, HEAL, i));
        else if (effect.Effect == SPELL_EFFECT_HEAL_PCT)
            total += vieMax * float(valeur) / 100.0f;
        else if (effect.Effect == SPELL_EFFECT_HEAL_MAX_HEALTH)
            total += vieMax;
        else if ((effect.Effect == SPELL_EFFECT_TRIGGER_SPELL || effect.Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE) &&
                 effect.TriggerSpell && effect.TriggerSpell != info->Id && !profondeur)
        {
            if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                total += CoaIaSoinEstime(bot, declenche, cible, 1);
        }
        // I3 du 17/10 (Cultist 40, Dark Infusion 570249 et ses rangs 500721, 570248, 570250-570252) : aura DUMMY dont le
        // declencheur est un soin a valeur vide (570263 : HEAL, base 0 + 1) que le module lance avec la valeur de l'aura
        // (AscensionCultistEvents.cpp, Copy(player, recipient, 570263, amount) : SPELLVALUE_BASE_POINT0 = montant de
        // l'effet 0). Estimee a un soin du declenche portant la valeur de l'aura (bonus du declenche, comme au lancer
        // reel) ; avant : 0, donc « illisible », et le tri C3 lui laissait sa place (36 % de la mana de soin du Heretic
        // pour 5 % du soin au volet A1). Un seul soin par lancer : l'expansion aux allies voisins n'est pas comptee.
        else if (IsAuraEffect(effect) && effect.ApplyAuraName == SPELL_AURA_DUMMY && effect.TriggerSpell &&
                 effect.TriggerSpell != info->Id && !profondeur && valeur > 1)
        {
            if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                for (uint8 j = 0; j < MAX_SPELL_EFFECTS; ++j)
                    if (declenche->Effects[j].Effect == SPELL_EFFECT_HEAL && CoaValeurDeBase(declenche->Effects[j]) <= 1)
                    {
                        total += float(bot->SpellHealingBonusDone(cible, declenche, valeur, HEAL, j));
                        break;
                    }
        }
        else if (IsAuraEffect(effect) && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL)
            total += float(bot->SpellHealingBonusDone(cible, info, valeur, DOT, i)) * float(ticks);
        else if (IsAuraEffect(effect) && effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL && effect.TriggerSpell &&
                 effect.TriggerSpell != info->Id && !profondeur)
            if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                total += CoaIaSoinEstime(bot, declenche, cible, 1) * float(ticks);
    }
    return total;
}

/*
 * C3 (AiPlayerbot.CoaIa.TriSoins) : soins tries selon le deficit de la cible et le rendement en mana. Cultist (40), mesure :
 * Devour Curse et Dark Infusion lances 205 et 134 fois au-dessus de 35 % de mana, Eldritch Mending (le meilleur soin) 6
 * fois -- l'ordre etait celui du niveau requis, et CheapestFirst ne jouait que sous la reserve.
 *   - d'abord les soins dont l'estimation tient dans 1,2 x le deficit, du meilleur rendement (vie par point de ressource,
 *     un sort gratuit en tete) au moins bon ; puis ceux qui deborderaient, du plus petit au plus grand ;
 *   - un soin illisible (estimation <= 1, script) garde sa place dans la liste ; les autres se trient entre eux, et
 *     seulement dans leur groupe (`rang`, l'ordre direct / HoT / zone de « coa heal ») ;
 *   - cible sous criticalHealth : le plus rapide (temps d'incantation) d'abord, a l'interieur de chaque groupe.
 * Les estimations sont relevees une fois par sort avant le tri (jamais dans le comparateur : CalcValue tire au sort).
 * Limite : le cout est celui du DBC (CalcPowerCost) ; un cout fait par script (Touch of Moonlight 574328 du Starcaller,
 * 10 % de la mana maximale, AscensionStarcallerData.h) n'est pas vu.
 */
template <typename Rang>
void CoaIaTrierSoins(Player* bot, Unit* cible, std::vector<Usable>& spells, Rang rang)
{
    size_t const n = spells.size();
    if (n < 2 || !cible)
        return;
    struct Cle
    {
        float estime;
        float rendement;
        int32 incantation;
        int rang;
    };
    std::vector<Cle> cles(n);
    for (size_t i = 0; i < n; ++i)
    {
        SpellInfo const* info = EffectiveSpell(bot, spells[i].info);
        float const estime = CoaIaSoinEstime(bot, info, cible);
        int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
        cles[i] = { estime, estime / float(std::max(cout, 1)), info->CalcCastTime(bot), rang(spells[i].kind) };
    }
    float const deficit = std::max(1.0f, float(cible->GetMaxHealth()) - float(cible->GetHealth()));
    bool const critique = cible->GetHealthPct() < float(sPlayerbotAIConfig.criticalHealth);

    std::vector<Usable> const avant = spells;
    std::vector<size_t> ordre(n);
    for (size_t i = 0; i < n; ++i)
        ordre[i] = i;
    // Groupe par groupe (`rang` contigu : la liste est deja triee par groupe), seuls les soins lisibles bougent, et ils
    // reprennent les places des soins lisibles.
    size_t debut = 0;
    while (debut < n)
    {
        size_t fin = debut;
        while (fin < n && cles[fin].rang == cles[debut].rang)
            ++fin;
        std::vector<size_t> places;
        for (size_t i = debut; i < fin; ++i)
            if (cles[i].estime > 1.0f)
                places.push_back(i);
        std::vector<size_t> lisibles = places;
        std::stable_sort(lisibles.begin(), lisibles.end(), [&cles, deficit](size_t a, size_t b)
        {
            bool const tientA = cles[a].estime <= 1.2f * deficit;
            bool const tientB = cles[b].estime <= 1.2f * deficit;
            if (tientA != tientB)
                return tientA;
            if (tientA)
                return cles[a].rendement > cles[b].rendement;
            return cles[a].estime < cles[b].estime;
        });
        for (size_t k = 0; k < places.size(); ++k)
            ordre[places[k]] = lisibles[k];
        if (critique)
            std::stable_sort(ordre.begin() + long(debut), ordre.begin() + long(fin),
                [&cles](size_t a, size_t b) { return cles[a].incantation < cles[b].incantation; });
        debut = fin;
    }
    for (size_t i = 0; i < n; ++i)
        spells[i] = avant[ordre[i]];
}

// Les soins sur soi : mono-cible d'abord (ordre d'avant : le moins cher d'abord sous la reserve de mana), puis, en dernier
// recours, les soins de zone qui couvrent le lanceur. Jamais un sort qui exclut le lanceur, vise un ennemi, ou enracine.
std::vector<Usable> CoaIaSoinsSurSoi(PlayerbotAI* botAI, Player* bot)
{
    std::vector<Usable> soins = KnownAbilities(botAI, bot, [](uint16 kind, uint16 marques)
        { return (kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE)) &&
                 !(marques & (MARQUE_PAS_SUR_SOI | MARQUE_IMMOBILISE)); });
    // Lot 4 (TriSoins) : par deficit et rendement (CoaIaTrierSoins), a la place du moins cher d'abord sous la reserve.
    if (sPlayerbotAIConfig.coaIaTriSoins)
        CoaIaTrierSoins(bot, bot, soins, [](uint16) { return 0; });
    else if (SavingManaForHeals(botAI, bot))
        CheapestFirst(bot, soins);
    for (Usable const& spell : KnownAbilities(botAI, bot, [](uint16 kind, uint16 marques)
             { return (kind & KIND_GROUP_HEAL) && !(kind & (KIND_CONTROL | KIND_HOSTILE)) &&
                      !(marques & (MARQUE_PAS_SUR_SOI | MARQUE_IMMOBILISE)); }))
        if (CoaIaZoneInclutLanceur(EffectiveSpell(bot, spell.info)))
            soins.push_back(spell);
    // Revue du 16/10 : un HoT ou un soin a cible unique tenu sur un autre membre n'est pas retire pour le bot.
    CoaIaRetirerUniquesTenus(bot, soins);
    // I6 du 17/10 : ni Sacrifice Undead ni autre sort « par serviteur » sans serviteur.
    CoaIaRetirerParServiteurSansServiteur(bot, soins);
    // Revue du lot 5 : plancher de vie apres paiement (CoaIaPayeEnVieTropCher).
    soins.erase(std::remove_if(soins.begin(), soins.end(), [bot](Usable const& spell)
        { return CoaIaPayeEnVieTropCher(bot, EffectiveSpell(bot, spell.info)); }), soins.end());
    return soins;
}

// Un sort de la liste passerait les filtres de CastFirst qui ne dependent que du lanceur (memes tests que
// HasReadyAbility), et n'est pas deja pose par le bot sur `cible` (CoaSoinDejaPose).
// `gcd` faux (revue) : la recharge globale ne compte pas -- pour la potion, qui ne doit pas partir parce qu'un defensif
// attend la fin d'une recharge globale de 1,5 s.
bool CoaIaUnPret(PlayerbotAI* botAI, Player* bot, std::vector<Usable> const& spells, Unit* cible, bool gcd = true)
{
    auto const& benched = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->benchedSpells;
    time_t const now = time(nullptr);
    for (Usable const& spell : spells)
    {
        SpellInfo const* const info = EffectiveSpell(bot, spell.info);
        if (CoaRefuseParAuraDuLanceur(bot, info) || CoaRefuseParFormeOuArme(bot, info) || !CoaPeutPayer(bot, info) ||
            CoaExigeFamilierAbsent(bot, info))
            continue;
        if (bot->HasSpellCooldown(info->Id) || IsBenched(benched, spell.info->Id, now) || IsBenched(benched, info->Id, now))
            continue;
        if (cible && CoaSoinDejaPose(bot, spell, info, cible))
            continue;
        // Lot 4 (AiPlayerbot.CoaIa.UtiliteAlignee) : aussi la recharge globale et les refus certains lus sur la cible,
        // que CastFirst applique (HasReadyAbility). Ni portee ni ligne de vue : trop cheres a chaque passage.
        if (sPlayerbotAIConfig.coaIaUtiliteAlignee &&
            ((gcd && bot->GetGlobalCooldownMgr().HasGlobalCooldown(info)) || (cible && CoaRefusCibleCertain(bot, info, cible))))
            continue;
        return true;
    }
    return false;
}

/*
 * Revue C1 (16/10, AiPlayerbot.CoaIa.SoigneurResteEnCombat) : depuis C1, un soigneur garde une cible vivante et vue que
 * « dps target » propose encore, meme controlee (CoaInvalidTargetTrigger) ; avant, « drop target » a 99 l'effacait
 * toujours. Ses attaques (« coa attack », « coa aoe ») ne frappent donc pas une cible porteuse d'un controle que les
 * degats brisent : HasBreakableByDamageCrowdControlAura du coeur (Unit.cpp : confusion/metamorphose, peur,
 * etourdissement, racines, transformation, a AuraInterruptFlags TAKE_DAMAGE 0x2 ; le canal du bot lui-meme exclu),
 * plus toute aura MOD_FEAR : Fear 5782 et Psychic Scream 8122 n'ont PAS 0x2 (Spell.dbc champ 32 = 0x480000, temoins
 * Polymorph 118 = 0x80002, Gouge 1776 = 0x2), ils cassent par proc et le coeur ne les compte pas. Option a 0 ou bot
 * qui n'est pas soigneur (GetCoaRole, Dps pour les classes 1 a 11) : rien ne change.
 */
bool CoaSoigneurEpargneControle(Player* bot, Unit* target)
{
    if (!sPlayerbotAIConfig.coaIaSoigneurResteEnCombat || !bot || !target)
        return false;
    if (!target->HasBreakableByDamageCrowdControlAura(bot) && !target->HasAuraType(SPELL_AURA_MOD_FEAR))
        return false;
    return GetCoaRole(bot) == CoaRole::Heal;
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
        if (CoaSoigneurEpargneControle(bot, target))
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
        // IA soins/survie, lot 4 (C4) : un soigneur en groupe retient aussi sa mana tant que le groupe n'est pas plein ou
        // que sa mana est sous CoaIa.ReserveSoigneurMana (CoaIaSoigneurRetient).
        bool const saveMana = SavingManaForHeals(botAI, bot) || CoaIaSoigneurRetient(botAI, bot);

        std::vector<Usable> usable = KnownAbilities(botAI, bot, [tank](uint16 kind) { return IsAttack(kind, tank); });
        // CoaArene, survie (11/10, arene 1v1 et duel, IaSurvie) : les etendards et autres sorts au sol de soutien ne sont
        // plus lances vers l'adversaire -- « coa arene soutien » les pose sur le bot. Voir CoaAreneRetirerSoutiens.
        CoaAreneRetirerSoutiens(botAI, bot, usable);
        if (usable.empty())
            return false;

        if (saveMana)
            DropManaSpells(bot, usable);
        // I4 du 17/10 : sous mediumHealth, aucun sort paye en vie par seconde (Immolation Aura du Slayer).
        if (CoaIaSurvieActive(bot))
            usable.erase(std::remove_if(usable.begin(), usable.end(), [this](Usable const& spell)
                { return CoaIaVieParSecondeRefusee(bot, EffectiveSpell(bot, spell.info)); }), usable.end());
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

        // CoaArene L1 (Ranger, arene ou duel) : les consommateurs de pile gardes pour une fenetre, mis devant
        // quand elle est ouverte. Sans effet hors engagement ou option eteinte. Voir CoaAreneOrdonnerBurst.
        // Revue m2 : le burst retire ou reordonne des sorts ; l'index du tourniquet se relit donc sur l'ordre
        // d'AVANT lui. Copie prise seulement si une option CoaArene est armee : rien ne change sinon.
        std::vector<Usable> const tourniquet = CoaAreneArmee() ? usable : std::vector<Usable>();
        CoaAreneBurst const burst = CoaAreneOrdonnerBurst(botAI, bot, target, usable);
        if (usable.empty())
            return RecordUsage(USAGE_ATTACK, nullptr);
        // CoaArene (11/10, arene 1v1 et duel, IaCommune) : l'adversaire vient d'etre controle par le bot, ses plus gros
        // sorts de degats passent devant. Ordre seulement, rien de retire ; jamais pour le Ranger de L1 (burst ci-dessus).
        // Avant le rapprochement : une melee loin de sa cible controlee charge d'abord. Voir CoaAreneOrdonnerFenetre.
        CoaAreneFenetre const fenetre = CoaAreneOrdonnerFenetre(botAI, bot, target, usable, burst.fenetre);
        // CoaArene (melee, duel ou arene 1v1) : adversaire a plus de 10 yd, charge et saut devant. Apres la copie du
        // tourniquet, comme le burst : l'index se relit sur l'ordre d'avant. Voir CoaAreneOrdonnerRapprochement.
        CoaAreneOrdonnerRapprochement(botAI, bot, target, usable);

        // IA soins/survie, lot 3 : sous mediumHealth, le vol de vie passe devant (ordre seulement, rien de retire).
        // L'index du tourniquet se relit sur l'ordre d'avant, comme pour le burst.
        std::vector<Usable> avantVol;
        if (bot->GetHealthPct() < float(sPlayerbotAIConfig.mediumHealth) && CoaIaSurvieActive(bot) &&
            std::any_of(usable.begin(), usable.end(), [](Usable const& spell) { return (spell.marques & MARQUE_VOL_DE_VIE) != 0; }))
        {
            if (tourniquet.empty())
                avantVol = usable;
            std::stable_partition(usable.begin(), usable.end(),
                [](Usable const& spell) { return (spell.marques & MARQUE_VOL_DE_VIE) != 0; });
        }

        // The cast goes through CastFirst rather than through a second loop of its own. The one
        // written here judged every refusal by PlayerbotAI::CastSpell to be the spell's fault and
        // benched it for a minute after three of them - while CastSpell also refuses for reasons
        // that pass in a tick and have nothing to do with the spell: a cast time while the bot is
        // moving, a bot still sitting, a cast already in progress. A ranged bot, which moves for
        // the whole fight, lost its main spell for a minute in under a second, and the count never
        // decayed, so three refusals hours apart did the same. CastFirst handles the three cases,
        // benches for 8 seconds and says why in the failure log.
        // CoaArene, survie (11/10, arene 1v1 et duel, IaSurvie) : sous 25 % de mana, le moins cher d'abord et une reserve
        // pour un sort de survie. Apres tous les ordres ci-dessus : a sec, le cout passe devant. Voir CoaAreneReserveMana.
        CoaAreneReserveMana(botAI, bot, usable);
        if (usable.empty())
            return RecordUsage(USAGE_ATTACK, nullptr);

        SpellInfo const* const cast = CastFirst(botAI, bot, usable, target, USAGE_ATTACK);
        if (cast)
        {
            std::vector<Usable> const& ordre = !tourniquet.empty() ? tourniquet : !avantVol.empty() ? avantVol : usable;
            auto const itr = std::find_if(ordre.begin(), ordre.end(),
                [cast](Usable const& spell) { return spell.info == cast; });
            if (itr != ordre.end())
                next = start + size_t(std::distance(ordre.begin(), itr)) + 1;
            CoaAreneNoterBurst(botAI, bot, target, burst, cast);
            CoaAreneNoterFenetre(botAI, bot, target, fenetre, cast);
        }

        return RecordUsage(USAGE_ATTACK, cast);
    }

    bool isUseful() override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        return target && target->IsAlive() && !CoaSoigneurEpargneControle(bot, target);
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
        if (CoaSoigneurEpargneControle(bot, target))
            return false;

        std::vector<Usable> spells = KnownAbilities(botAI, bot, [](uint16 kind)
            { return (kind & KIND_AOE) && (kind & (KIND_DAMAGE | KIND_HOSTILE)); });
        if (SavingManaForHeals(botAI, bot) || CoaIaSoigneurRetient(botAI, bot))
            DropManaSpells(bot, spells);
        // I4 du 17/10 : comme dans « coa attack », aucun sort paye en vie par seconde sous mediumHealth.
        if (CoaIaSurvieActive(bot))
            spells.erase(std::remove_if(spells.begin(), spells.end(), [this](Usable const& spell)
                { return CoaIaVieParSecondeRefusee(bot, EffectiveSpell(bot, spell.info)); }), spells.end());
        // CoaArene, survie (11/10) : comme dans « coa attack », ni etendard ni soutien au sol vise sur l'adversaire.
        CoaAreneRetirerSoutiens(botAI, bot, spells);
        CoaAreneReserveMana(botAI, bot, spells);

        return RecordUsage(USAGE_AOE, CastFirst(botAI, bot, spells, target, USAGE_AOE));
    }

    bool isUseful() override
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        return target && target->IsAlive() && !CoaSoigneurEpargneControle(bot, target) && ClassHas(bot, KIND_AOE) &&
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

        std::vector<Usable> spells = Candidats(target);

        // IA soins/survie, lot 4 (C3, AiPlayerbot.CoaIa.TriSoins) : soin direct trie par deficit et rendement, dans
        // chaque groupe direct / HoT / zone ; le moins cher d'abord sous la reserve ne s'y applique plus. Zone et HoT
        // gardent l'ordre d'avant.
        if (sPlayerbotAIConfig.coaIaTriSoins && mode == Mode::Direct)
            CoaIaTrierSoins(bot, target, spells, Rang);
        else if (SavingManaForHeals(botAI, bot))
            CheapestFirst(bot, spells);

        UsageKind const usage = mode == Mode::Group ? USAGE_GROUP_HEAL : mode == Mode::OverTime ? USAGE_HOT : USAGE_HEAL;
        if (spells.empty())
            RecordFailure(usage, 0, FAILURE_NOTHING);

        // Banc des soins (S0) : remis a zero ICI, apres le FAILURE_NOTHING ci-dessus (le cas `vide` le dit deja).
        t_bancSoinEchec = BancSoinEchec();
        SpellInfo const* cast = RecordUsage(usage, CastFirst(botAI, bot, spells, target, usage));
        BancSoinTracer(bot, mode == Mode::Group ? "group_heal" : mode == Mode::OverTime ? "hot" : "heal", cast,
                       spells.empty());
        return cast;
    }

    bool isUseful() override
    {
        Unit* target = AI_VALUE(Unit*, "party member to heal");
        uint16 const wanted = mode == Mode::Group ? KIND_GROUP_HEAL : mode == Mode::OverTime ? KIND_HOT : KIND_HEAL;
        // IA soins/survie, lot 4 (AiPlayerbot.CoaIa.UtiliteAlignee) : les memes candidats qu'Execute (Candidats : HoT deja
        // pose, cible unique tenue ailleurs, soin d'autrui, hors groupe), puis les memes refus que CastFirst (CoaIaUnPret :
        // recharge globale, deja pose, refus certains sur la cible). Avant, l'action se disait utile et ne trouvait rien :
        // 2 388 « hot rien code=1001 » du Chronomancer au banc.
        if (sPlayerbotAIConfig.coaIaUtiliteAlignee)
            return target && target->IsAlive() && target->GetHealthPct() < sPlayerbotAIConfig.almostFullHealth &&
                   ClassHas(bot, wanted) && CoaIaUnPret(botAI, bot, Candidats(target), target);
        return IsUsefulAvant(target, wanted);
    }

private:
    // Direct / HoT / zone : l'ordre des groupes de « coa heal ».
    static int Rang(uint16 kind) { return ((kind & KIND_GROUP_HEAL) ? 2 : 0) + ((kind & KIND_HOT) ? 1 : 0); }

    // Les soins candidats pour `target`, avant tout tri de rendement : ce que Execute lance, et ce que isUseful juge
    // (lot 4). Options du lot 4 a 0 : la liste d'avant, a l'identique.
    std::vector<Usable> Candidats(Unit* target)
    {
        std::vector<Usable> spells;
        ObjectGuid const caster = bot->GetGUID();
        switch (mode)
        {
            case Mode::Group:
                spells = KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_GROUP_HEAL) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); });
                break;
            case Mode::OverTime:
            {
                spells = KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_HOT) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); });
                // Lot 4 (C2, AuraSurLanceur) : une aura qui ne se pose que sur le lanceur se cherche sur lui.
                bool const surLanceur = sPlayerbotAIConfig.coaIaAuraSurLanceur;
                Player* const lanceur = bot;
                spells.erase(std::remove_if(spells.begin(), spells.end(),
                    [target, caster, surLanceur, lanceur](Usable const& spell)
                    {
                        Unit* const porteur = surLanceur && CoaAuraSurLanceur(spell.info) ? lanceur : target;
                        return porteur->HasAura(spell.info->Id, caster);
                    }),
                    spells.end());
                break;
            }
            default:
                // Single target heals first, direct ones before those over time; area heals last.
                spells = KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_HEAL) && !(kind & (KIND_CONTROL | KIND_HOSTILE)); });
                std::stable_sort(spells.begin(), spells.end(), [](Usable const& a, Usable const& b)
                {
                    return Rang(a.kind) < Rang(b.kind);
                });
                break;
        }

        // Lot 4 (C2-bis, HotUnique) : un HoT a cible unique tenu ailleurs n'est pas deplace, sauf urgence.
        if (sPlayerbotAIConfig.coaIaHotUnique)
        {
            Player* const lanceur = bot;
            spells.erase(std::remove_if(spells.begin(), spells.end(), [target, lanceur](Usable const& spell)
                {
                    return (spell.kind & KIND_HOT) &&
                           CoaIaHotTenuAilleurs(lanceur, EffectiveSpell(lanceur, spell.info), spell.info->Id, target);
                }), spells.end());
        }

        // Since a heal aimed at the caster alone is classified as a heal (KIND_SELF_HEAL), it has
        // to be kept out of the list when the wounded member is somebody else: the strict check
        // would turn it down for bad targets and bench it for 20 seconds, blaming a spell that was
        // simply asked the impossible.
        if (target != bot)
            spells.erase(std::remove_if(spells.begin(), spells.end(),
                [](Usable const& spell) { return !CanHealOther(spell.kind, spell.marques); }), spells.end());
        // Lot 1 (DIAG sorts C8) : sur le bot lui-meme, jamais un sort qui exclut son lanceur ou vise un ennemi. Marque
        // posee seulement sous AiPlayerbot.CoaIa.Classement (KnownKit) : sinon rien n'est retire.
        else
            spells.erase(std::remove_if(spells.begin(), spells.end(),
                [](Usable const& spell) { return (spell.marques & MARQUE_PAS_SUR_SOI) != 0; }), spells.end());

        // CoA, lot S des evenements (08/10/2026) : la cible peut etre le joueur SERVI par un bot inscrit
        // (CoaDirecteur::JoueurASoigner), hors du groupe du bot. Un soin de zone y est refuse (explicite PARTY/RAID :
        // SPELL_FAILED_BAD_TARGETS, SpellInfo::CheckExplicitTarget) ou soigne le groupe du bot au lieu du joueur (zone
        // PARTY autour du lanceur) : il serait mis au banc pour tout le monde. Hors groupe, soins de zone retires.
        if (HorsGroupe(bot, target))
            spells.erase(std::remove_if(spells.begin(), spells.end(),
                [](Usable const& spell) { return (spell.kind & KIND_GROUP_HEAL) != 0; }), spells.end());

        return spells;
    }

    // isUseful d'avant le lot 4 (UtiliteAlignee = 0), inchange.
    bool IsUsefulAvant(Unit* target, uint16 wanted)
    {
        // Same rule as Execute: a caster-only heal makes this action useful for the bot itself,
        // never for another member, or the action would run every tick and cast nothing.
        bool const onSelf = target == bot;
        // Lot S : meme regle qu'Execute, sinon l'action se dirait utile a chaque tick pour un joueur servi hors groupe
        // et ne lancerait rien (FAILURE_NOTHING).
        bool const horsGroupe = target && HorsGroupe(bot, target);
        return target && target->IsAlive() && target->GetHealthPct() < sPlayerbotAIConfig.almostFullHealth &&
               ClassHas(bot, wanted) &&
               HasReadyAbility(botAI, bot,[wanted, onSelf, horsGroupe](uint16 kind, uint16 marques)
                   { return (kind & wanted) && !(kind & (KIND_CONTROL | KIND_HOSTILE)) &&
                            (onSelf ? !(marques & MARQUE_PAS_SUR_SOI) : CanHealOther(kind, marques)) &&
                            !(horsGroupe && (kind & KIND_GROUP_HEAL)); });
    }

    Mode mode;

    // Lot S : une cible autre que le bot et hors de son groupe ou raid (le joueur servi d'un evenement).
    static bool HorsGroupe(Player* bot, Unit* target) { return target != bot && !bot->IsInRaidWith(target); }
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
//
// IA soins/survie, lot 3, revue : « coa defensive critique » est la meme action, armee a ACTION_EMERGENCY sous 30 % de vie
// (« coa survie critique ») pour les GROS defensifs ; elle ne lance que des defensifs, jamais le repli de soin, sans quoi
// un soigneur a 29 % se soignerait a la priorite 90, devant les soins de son groupe (« coa heal », 30 a 34). Hors Survie,
// elle n'est jamais armee (declencheur inerte) et ne lancerait rien (isUseful faux).
class CoaDefensiveAction : public Action
{
public:
    CoaDefensiveAction(PlayerbotAI* botAI, std::string const name = "coa defensive", bool critique = false)
        : Action(botAI, name), critique(critique) {}

    bool Execute(Event /*event*/) override
    {
        t_bancSoinEchec = BancSoinEchec();   // banc des soins (S0) : refus de CETTE action seulement
        bool vide = true;                    // aucun sort candidat, ni defensif ni soin sur soi
        // IA soins/survie, lot 3 : paliers de vie, tri par force, soins de zone en dernier recours (CoaIaDefensifs).
        if (critique && !CoaIaSurvieActive(bot))
            return false;
        if (CoaIaSurvieActive(bot))
        {
            float const pv = bot->GetHealthPct();
            bool const perte = CoaIaPerteRecente(botAI, bot) >= CoaIaPerteRapidePv;
            std::vector<Usable> const defensifs = CoaIaDefensifs(botAI, bot, pv, perte, true);
            vide = defensifs.empty();
            SpellInfo const* cast = CastFirst(botAI, bot, defensifs, bot, USAGE_DEFENSIVE);
            if (!cast && !critique && pv < float(sPlayerbotAIConfig.mediumHealth))
            {
                std::vector<Usable> const soins = CoaIaSoinsSurSoi(botAI, bot);
                vide = vide && soins.empty();
                cast = CastFirst(botAI, bot, soins, bot, USAGE_DEFENSIVE);
            }
            RecordUsage(USAGE_DEFENSIVE, cast);
            BancSoinTracer(bot, "defensive", cast, vide);
            return cast;
        }
        // Lot 1 (DIAG sorts C8) : ni defensif ni soin qui exclut son lanceur ou vise un ennemi, puisque la cible est le bot.
        std::vector<Usable> const defensifs = KnownAbilities(botAI, bot, [](uint16 kind, uint16 marques)
            { return (kind & KIND_DEFENSIVE) && !(marques & MARQUE_PAS_SUR_SOI); });
        vide = defensifs.empty();
        SpellInfo const* cast = CastFirst(botAI, bot, defensifs, bot, USAGE_DEFENSIVE);
        if (!cast)
        {
            std::vector<Usable> heals = KnownAbilities(botAI, bot, [](uint16 kind, uint16 marques)
                { return (kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE)) &&
                         !(marques & MARQUE_PAS_SUR_SOI); });
            if (SavingManaForHeals(botAI, bot))
                CheapestFirst(bot, heals);

            vide = vide && heals.empty();
            cast = CastFirst(botAI, bot, heals, bot, USAGE_DEFENSIVE);
        }

        RecordUsage(USAGE_DEFENSIVE, cast);
        BancSoinTracer(bot, "defensive", cast, vide);
        return cast;
    }

    bool isUseful() override
    {
        if (CoaIaSurvieActive(bot))
        {
            if (!ClassHas(bot, KIND_DEFENSIVE | KIND_HEAL | KIND_GROUP_HEAL))
                return false;
            float const pv = bot->GetHealthPct();
            bool const perte = CoaIaPerteRecente(botAI, bot) >= CoaIaPerteRapidePv;
            return CoaIaUnPret(botAI, bot, CoaIaDefensifs(botAI, bot, pv, perte, false), bot) ||
                   (!critique && pv < float(sPlayerbotAIConfig.mediumHealth) &&
                    CoaIaUnPret(botAI, bot, CoaIaSoinsSurSoi(botAI, bot), bot));
        }
        if (critique)
            return false;
        return bot->IsAlive() && ClassHas(bot, KIND_DEFENSIVE | KIND_HEAL) && HasReadyAbility(botAI, bot,[](uint16 kind, uint16 marques)
            { return !(marques & MARQUE_PAS_SUR_SOI) &&
                     ((kind & KIND_DEFENSIVE) || ((kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE)))); });
    }

private:
    bool critique;
};

// IA soins/survie, lot 3 : soin sur soi sous mediumHealth (65 %) pour les tanks et les degats, qui n'ont pas « coa heal ».
// Le soigneur se soigne par « coa heal » (le bot est candidat de « party member to heal ») : exclu ici.
class CoaSoinSoiAction : public Action
{
public:
    CoaSoinSoiAction(PlayerbotAI* botAI) : Action(botAI, "coa soin soi") {}

    bool Execute(Event /*event*/) override
    {
        t_bancSoinEchec = BancSoinEchec();
        std::vector<Usable> const soins = CoaIaSoinsSurSoi(botAI, bot);
        SpellInfo const* cast = RecordUsage(USAGE_SOIN_SOI, CastFirst(botAI, bot, soins, bot, USAGE_SOIN_SOI));
        BancSoinTracer(bot, "soin_soi", cast, soins.empty());
        return cast;
    }

    bool isUseful() override
    {
        return CoaIaSurvieActive(bot) && GetCoaRole(bot) != CoaRole::Heal &&
               bot->GetHealthPct() < float(sPlayerbotAIConfig.mediumHealth) && ClassHas(bot, KIND_HEAL | KIND_GROUP_HEAL) &&
               CoaIaUnPret(botAI, bot, CoaIaSoinsSurSoi(botAI, bot), bot);
    }
};

/*
 * I2 du 17/10 (coa-project/docs/DIAG-equilibrage-classes-20261017.md, Tinker 51 : mana a 0-2 % a la fin de chaque essai
 * A1/A2, balise de mana jamais posee). Un soigneur CoA en combat, sous CoaRegenManaPct de mana, pose un sort marque
 * MARQUE_REGEN_MANA (Build: Replenishment Beacon 560750, surcharge du lot 1) sur lui-meme : la balise se pose au sol a sa
 * position et rend de la mana aux allies autour. Sous AiPlayerbot.CoaIa.ReserveSoigneur (la reserve de mana du soigneur,
 * lot 4) ET AiPlayerbot.CoaIa.Classement (la marque vient d'une surcharge du lot 1) : pas d'option propre, le seuil de
 * 60 % est celui du diagnostic. Hors arene 1v1 et duel (CoaIaDecisionActive) comme les autres decisions de combat.
 * Une balise posee n'est pas relancee avant la fin de sa duree (Spell.dbc, DurationIndex) : le sort a 3 charges, et
 * rien d'autre ne dirait qu'une balise est deja au sol (c'est une invocation, pas une aura).
 */
constexpr float CoaRegenManaPct = 60.0f;

bool CoaIaDecisionActive(Player* bot);  // lot 5, plus bas

bool CoaRegenManaArmee(Player* bot)
{
    return sPlayerbotAIConfig.coaIaReserveSoigneur && sPlayerbotAIConfig.coaIaClassement && bot->getPowerType() == POWER_MANA &&
           bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < CoaRegenManaPct && GetCoaRole(bot) == CoaRole::Heal &&
           CoaIaDecisionActive(bot);
}

std::vector<Usable> CoaRegenManaSorts(PlayerbotAI* botAI, Player* bot)
{
    return KnownAbilities(botAI, bot, [](uint16, uint16 marques) { return (marques & MARQUE_REGEN_MANA) != 0; });
}

class CoaRegenManaTrigger : public Trigger
{
public:
    CoaRegenManaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa regen mana") {}

    bool IsActive() override
    {
        return CoaRegenManaArmee(bot) && CoaIaUnPret(botAI, bot, CoaRegenManaSorts(botAI, bot), bot);
    }
};

class CoaRegenManaAction : public Action
{
public:
    CoaRegenManaAction(PlayerbotAI* botAI) : Action(botAI, "coa regen mana") {}

    bool Execute(Event /*event*/) override
    {
        std::vector<Usable> const sorts = CoaRegenManaSorts(botAI, bot);
        t_bancSoinEchec = BancSoinEchec();
        SpellInfo const* cast = CastFirst(botAI, bot, sorts, bot);
        BancSoinTracer(bot, "regen_mana", cast, sorts.empty());
        if (cast)
        {
            int32 const duree = cast->GetMaxDuration();
            prochainMs = getMSTime() + uint32(duree > 0 ? duree : 15 * IN_MILLISECONDS);
            if (!prochainMs)
                prochainMs = 1;
        }
        return cast != nullptr;
    }

    // Le delai est teste ici, comme le repli de CoaStealthAction : une action inutile laisse le tour aux autres.
    bool isUseful() override
    {
        return (!prochainMs || getMSTimeDiff(prochainMs, getMSTime()) < 0x80000000u) && CoaRegenManaArmee(bot);
    }

private:
    uint32 prochainMs = 0;  // getMSTime ; 0 : aucune balise posee par cette instance (une instance d'action par bot)
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

/*
 * IA SOINS/SURVIE, LOT 2 (16/10/2026, DIAG-banc-soins-survie-20261016.md priorite 1 ; DIAG-banc-soins-sorts-jamais-
 * lances-20261016.md C5) : LA POSTURE SELON LE ROLE. CoaBuffAction prenait la premiere posture du kit trie par niveau,
 * quel que soit le role : les Guardian Vanguard (specialisation 21, tank) restaient en Assault Formation 803417 (niveau 1)
 * et ne pouvaient lancer aucun des quatre sorts qui exigent Tower Formation 800317 (CasterAuraSpell) -- Raise Shield,
 * Turn The Blade, Brace, et Knight's Calling, leur seul soin sur soi --, ecartes en silence par CastFirst.
 *
 * Regle generique, sans id : pour une specialisation TANK (GetCoaRole), la posture connue qui est l'aura de lanceur exigee
 * (CasterAuraSpell) du plus grand nombre de sorts DEFENSIFS connus du kit. Egalite : le plus petit id (choix fixe, pas de
 * va-et-vient). Aucun defensif ne depend d'une posture : 0, la regle d'avant s'applique. Hors combat seulement (comme
 * « coa buff »), une fois par StanceRecastSeconds au plus (garde `recent` de CoaBuffAction).
 *
 * Postures ECARTEES de la regle (decisions de jeu du 16/10, tranchees par le lot 5 : CoaMauditPlanifier, CoaVenoSoinVoulu) :
 *   - Cursed Form 524861 et 525031 (Bloodmage, Eternal 99 tank) : entrer en forme maudite pour ses defensifs ;
 *   - Spider Form 800841 et Beetle Form 803183 (Venomancer, Fortitude 52 tank) : formes, voir C6 du diagnostic.
 */
constexpr uint32 CoaPosturesHorsRegle[] = { 524861, 525031, 800841, 803183 };

uint32 CoaPostureDeRole(PlayerbotAI* botAI, Player* bot, std::vector<Usable> const& postures)
{
    if (!sPlayerbotAIConfig.coaIaPostureRole || GetCoaRole(bot) != CoaRole::Tank)
        return 0;
    std::vector<Usable> const defensifs =
        KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_DEFENSIVE) != 0; });
    if (defensifs.empty())
        return 0;
    uint32 meilleure = 0;
    size_t meilleurCompte = 0;
    for (Usable const& posture : postures)
    {
        if (!(posture.kind & KIND_STANCE))
            continue;
        uint32 const id = posture.info->Id;
        if (std::find(std::begin(CoaPosturesHorsRegle), std::end(CoaPosturesHorsRegle), id) != std::end(CoaPosturesHorsRegle))
            continue;
        size_t const compte = size_t(std::count_if(defensifs.begin(), defensifs.end(),
            [id](Usable const& spell) { return spell.info->CasterAuraSpell == id; }));
        if (compte > meilleurCompte || (compte && compte == meilleurCompte && id < meilleure))
        {
            meilleure = id;
            meilleurCompte = compte;
        }
    }
    return meilleure;
}

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
        // P-167 : un sort lance rompt la furtivite (Elude 801345 porte
        // AURA_INTERRUPT_FLAG_CAST). Le chasseur d'une prime en approche furtive
        // ne se rebuffe pas. Les autres bots ne passent pas ce test (aura d'abord).
        // Reserve de chasseurs (02/10) : ChasseurDe et non ProieImposee -- un
        // chasseur qui tient a distance un prime AFK reste furtif aussi.
        if (bot->HasAuraType(SPELL_AURA_MOD_STEALTH) &&
            CoaDirecteur::ChasseurDe(bot->GetGUID().GetCounter()))
            return RecordUsage(USAGE_BUFF, nullptr);

        // I7 du 17/10 : un serviteur leve hors combat (CoaIaServiteurVoulu).
        if (SpellInfo const* serviteur = LeverUnServiteur())
            return RecordUsage(USAGE_BUFF, serviteur);

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
        //
        // One exception, and only one: a Venomancer (CLASS_PROPHET, 29) in Stalking (53).
        // Facemelter, Rotfang and Widowmaker - its Spenders - require Spider Form 800841
        // (AscensionVenomancerAbilities.cpp, OnSpellCheckCast), and the rule above kept a bot
        // that had first taken Beetle Form 803183 in Beetle for ever. For that spec, Spider
        // Form is the only stance it takes, and it counts as "in a stance" only while in
        // Spider Form: from Beetle it shifts once, then HasAura(800841) holds and nothing is
        // recast. Every other class and spec keeps the rule unchanged.
        constexpr uint32 SpiderForm = 800841;
        constexpr uint32 StalkingSpecialization = 53;
        bool const wantsSpider = bot->getClass() == CLASS_PROPHET &&
            GetAscensionActiveSpecialization(bot) == StalkingSpecialization &&
            std::any_of(spells.begin(), spells.end(), [](Usable const& spell)
                { return (spell.kind & KIND_STANCE) && spell.info->Id == SpiderForm; });
        // Lot 2 : la posture de son role pour un tank (CoaPostureDeRole) ; il en change s'il en porte une autre.
        uint32 const voulue = wantsSpider ? 0 : CoaPostureDeRole(botAI, bot, spells);
        bool const inStance = wantsSpider ? bot->HasAura(SpiderForm) : voulue ? bot->HasAura(voulue) :
            std::any_of(spells.begin(), spells.end(), [this](Usable const& spell)
                { return (spell.kind & KIND_STANCE) && bot->HasAura(spell.info->Id); });
        // Beetle Form 803183 is a true shapeshift: 800841 carries SPELL_ATTR0_NOT_SHAPESHIFTED, so
        // CheckCast refuses Spider Form with SPELL_FAILED_NOT_SHAPESHIFT while the bot is still a
        // beetle. A player's client cancels the form first; do the same for the bot.
        constexpr uint32 BeetleForm = 803183;
        if (wantsSpider && !inStance && bot->HasAura(BeetleForm))
            bot->RemoveAurasDueToSpell(BeetleForm);

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
                if (wantsSpider && (spell.kind & KIND_STANCE) && spell.info->Id != SpiderForm)
                    continue;
                if (voulue && (spell.kind & KIND_STANCE) && spell.info->Id != voulue)
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

    bool isUseful() override
    {
        // Revue du lot 5 : hors combat, la sortie de forme du Venomancer (CoaVenoEtat) est oubliee, ecart anti-boucle
        // compris : la forme que « coa buff » remet ici n'est pas une reprise, et le combat suivant repart a neuf.
        if (!bot->IsInCombat())
            static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->venoForme = CoaAiObjectContext::CoaVenoForme();
        return ClassHas(bot, KIND_BUFF | KIND_STANCE) || CoaIaServiteurVoulu(bot);
    }

private:
    /*
     * I7 du 17/10 (coa-project/docs/DIAG-equilibrage-classes-20261017.md, Necromancer 34-36) : hors arene, aucun
     * Necromancer ne levait de serviteur -- l'invocation n'existait que dans l'IA d'arene (CoaAreneFamilierTrigger, cadres
     * ARENE et DUEL). Sacrifice Undead, Transfer Life et Vampiric Aura restaient sans objet. Hors combat (« coa buff »),
     * un bot a Life Force (CoaAreneKitLifeForce : l'aura de capacite 805011 du module) SANS serviteur vivant
     * (CoaAreneAFamilier) leve UN serviteur : le moins cher de ses invocations permanentes (CoaAreneEstFamilier), comme la
     * preparation d'arene. Plafond : un seul, puisqu'il n'est tente que sans serviteur. Jamais en arene (InArena, la
     * preparation a sa propre regle) ni en duel. Un essai toutes les CoaIaServiteurReessaiSecondes au plus, reussi ou non.
     * Sous AiPlayerbot.CoaIa.Survie : pas d'option propre (le diagnostic en proposait une, eteinte par defaut).
     */
    static constexpr time_t CoaIaServiteurReessaiSecondes = 30;

    bool CoaIaServiteurVoulu(Player* lanceur) const
    {
        return sPlayerbotAIConfig.coaIaSurvie && lanceur->IsAlive() && !lanceur->IsInCombat() && !lanceur->InArena() &&
               !lanceur->duel && CoaAreneKitLifeForce(lanceur) && !CoaAreneAFamilier(lanceur) &&
               time(nullptr) >= prochainServiteur;
    }

    SpellInfo const* LeverUnServiteur()
    {
        if (!CoaIaServiteurVoulu(bot))
            return nullptr;
        prochainServiteur = time(nullptr) + CoaIaServiteurReessaiSecondes;
        std::vector<Usable> familiers = KnownAbilities(botAI, bot, [](uint16) { return true; });
        Player* const lanceur = bot;
        familiers.erase(std::remove_if(familiers.begin(), familiers.end(), [lanceur](Usable const& s)
            { return !CoaAreneEstFamilier(EffectiveSpell(lanceur, s.info)); }), familiers.end());
        if (familiers.empty())
            return nullptr;
        std::stable_sort(familiers.begin(), familiers.end(), [lanceur](Usable const& a, Usable const& b)
            {
                SpellInfo const* ia = EffectiveSpell(lanceur, a.info);
                SpellInfo const* ib = EffectiveSpell(lanceur, b.info);
                return ia->CalcPowerCost(lanceur, ia->GetSchoolMask()) < ib->CalcPowerCost(lanceur, ib->GetSchoolMask());
            });
        return CastFirst(botAI, bot, familiers, bot);
    }

    std::map<std::pair<ObjectGuid, uint32>, time_t> recent;
    time_t prochainServiteur = 0;  // I7 : prochain essai de levee (une instance d'action par bot)
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

        // P-167 : la furtivite du chasseur d'une prime appartient a sa traque
        // (CoaChasse.h, TraquerLaPrime : des 100 yd, et rompue a la fin de la
        // traque). Un second ecrivain la poserait hors traque, ou personne ne le
        // menerait au contact (EnemyPlayerValue.cpp, approcheFurtive). Memes
        // interrupteurs que cette valeur. Sans chasseur : un chargement atomique.
        // Reserve de chasseurs (02/10) : ChasseurDe (prime AFK compris) : un
        // chasseur dont le prime est AFK n'embusque personne d'autre.
        if (sPlayerbotAIConfig.wildPvpChasse && sPlayerbotAIConfig.wildPvpTraque &&
            CoaDirecteur::ChasseurDe(bot->GetGUID().GetCounter()))
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

// IA soins/survie, lot 3 : vie sous mediumHealth, ou perte rapide (CoaIaPerteRapidePv points en 4 s au plus). Arme
// « coa defensive » (palier leger) et « coa soin soi ». Met a jour le releve de vie a chaque passage.
class CoaSurvieTrigger : public Trigger
{
public:
    CoaSurvieTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa survie") {}

    bool IsActive() override
    {
        if (!CoaIaSurvieActive(bot))
            return false;
        float const perte = CoaIaPerteRecente(botAI, bot);
        // I4 du 17/10 : une aura payee en vie par seconde ne se garde pas sous lowHealth ni sur une perte rapide.
        CoaIaRetirerAurasPayeesEnVie(bot, perte >= CoaIaPerteRapidePv);
        return bot->GetHealthPct() < float(sPlayerbotAIConfig.mediumHealth) || perte >= CoaIaPerteRapidePv;
    }
};

// IA soins/survie, lot 3 : vie sous 30 %, le palier des gros defensifs (recharge >= 120 s).
class CoaSurvieCritiqueTrigger : public Trigger
{
public:
    CoaSurvieCritiqueTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa survie critique") {}

    bool IsActive() override { return CoaIaSurvieActive(bot) && bot->GetHealthPct() < CoaIaCritiquePv; }
};

// IA soins/survie, lot 3 (AiPlayerbot.CoaIa.SurviePotion) : vie sous lowHealth (45 %) et aucun defensif pret -> potion de
// soin (« healing potion », action amont : isPossible exige l'objet). Seulement si le bot porte la strategie « potions »
// (absente en champ de bataille, AiFactory) : la meme que celle du repli amont sous criticalHealth (25 %).
class CoaPotionTrigger : public Trigger
{
public:
    CoaPotionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa potion") {}

    bool IsActive() override
    {
        if (!sPlayerbotAIConfig.coaIaSurviePotion || !bot->IsAlive() || !bot->IsInCombat() ||
            (CoaAreneSurvieArmee() && CoaAreneUnContreUn(bot)))
            return false;
        float const pv = bot->GetHealthPct();
        if (pv >= float(sPlayerbotAIConfig.lowHealth) || !botAI->HasStrategy("potions", BOT_STATE_COMBAT))
            return false;
        std::vector<Usable> const defensifs = CoaIaSurvieActive(bot)
            ? CoaIaDefensifs(botAI, bot, pv, CoaIaPerteRecente(botAI, bot) >= CoaIaPerteRapidePv, false)
            : KnownAbilities(botAI, bot, [](uint16 kind, uint16 marques)
                  { return (kind & KIND_DEFENSIVE) && !(marques & MARQUE_PAS_SUR_SOI); });
        return !CoaIaUnPret(botAI, bot, defensifs, bot, false);
    }
};

/*
 * IA SOINS/SURVIE, LOT 5 (16/10/2026) : DEUX DECISIONS DE JEU DU COMMANDITAIRE (DIAG-banc-soins-sorts-jamais-lances-
 * 20261016.md, C5 et C6). Les deux jouent en combat, bot vivant et non monte, hors arene 1v1 et duel (la survie d'arene
 * IaSurvie garde la main, comme pour le lot 3). Rien ne touche un joueur humain : ces declencheurs ne vivent que dans
 * les strategies de combat des bots CoA.
 */
bool CoaIaDecisionActive(Player* bot)
{
    return bot->IsAlive() && bot->IsInCombat() && !bot->IsMounted() && !(CoaAreneSurvieArmee() && CoaAreneUnContreUn(bot));
}

// Le sort passerait les filtres de CastFirst qui ne dependent que du lanceur (ceux de CoaIaUnPret), sauf, a la demande,
// l'aura exigee du lanceur (`sansAura` : elle viendra de la forme qu'on va prendre) ou la forme (`sansForme` : on va la
// quitter). Le cout en vie (POWER_HEALTH, que CoaPeutPayer ne lit pas) laisse au moins CoaIaResteMinPv (revue du lot 5).
bool CoaIaPretSauf(PlayerbotAI* botAI, Player* bot, Usable const& spell, Unit* cible, bool sansAura, bool sansForme, bool gcd)
{
    auto const& benched = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->benchedSpells;
    time_t const now = time(nullptr);
    SpellInfo const* const info = EffectiveSpell(bot, spell.info);
    if (!sansAura && CoaRefuseParAuraDuLanceur(bot, info))
        return false;
    if (sansForme ? (!info->IsPassive() && !bot->HasItemFitToSpellRequirements(info)) : CoaRefuseParFormeOuArme(bot, info))
        return false;
    if (!CoaPeutPayer(bot, info) || CoaExigeFamilierAbsent(bot, info))
        return false;
    if (CoaIaPayeEnVieTropCher(bot, info))
        return false;
    if (bot->HasSpellCooldown(info->Id) || IsBenched(benched, spell.info->Id, now) || IsBenched(benched, info->Id, now))
        return false;
    if (cible && CoaSoinDejaPose(bot, spell, info, cible))
        return false;
    return !gcd || !bot->GetGlobalCooldownMgr().HasGlobalCooldown(info);
}

/*
 * 1. BLOODMAGE (classe 20), AiPlayerbot.CoaIa.FormeMaudite. Decision : « sous 45 % de vie en combat, il entre en forme
 * maudite, puis utilise ses defensifs de forme » -- Endure the Curse 681190 et Eternal Resolve 801962 (CasterAuraSpell
 * 524861), Wicked Howl 804207 (CasterAuraSpell 525031). Avant : ecartes en silence par CastFirst hors forme (C5).
 *
 * Etabli le 16/10 : aucun sort du DBC ne pose 524861 ni 525031. C'est le module qui les pose sur la forme reelle
 * (SyncCursedFormRequirement, mod-ascension-compat/src/AscensionBloodmageTalents.cpp:102) : les deux pour Blood Curse
 * 562720, Accursed Form 562572 (spe 27) et Eternal Curse 800157 (spe 99) ; 525031 seul pour Sanguine Essence 680692
 * (spe 25) et Transgression 801076 (spe 26), qui n'enferment pas la forme mortelle (LocksMortalForm). La table
 * ci-dessous recopie cette liste (CursedForms) : un ajout cote module doit y etre reporte.
 *
 * Entrees SANS etourdissement : Blood Curse 562720 et ses remplacants (aura 36 forme 57, 30 s, recharge a la sortie,
 * payes en vie : 40 % et 25 % de la vie de base), Eternal Curse 800157 (forme sans duree). Moon Gaze 562723 (forme 57 +
 * MOD_STUN 300 s, « Cannot be used in combat ») n'est PAS dans la table : jamais lance ici (voir Classify, 02/10/2026).
 *
 * Regle : un defensif de forme a la fois (aucun deja actif) ; dans la forme, le defensif pret ; hors forme, la forme qui
 * pose le marqueur d'un defensif pret par ailleurs (recharge, ressource). Jamais une seconde forme par-dessus une
 * premiere (marqueur deja tenu). Prix connu (arene 1v1 du 11/10, CoaAreneSortForme) : la forme enferme les sorts mortels
 * (524861 est l'ExcludeCasterAuraSpell de 47 d'entre eux), la rotation n'a plus que les sorts de forme pendant 30 s.
 */
constexpr uint32 CoaMauditMarqueur = 524861;      // SPELL_CURSED_FORM_REQUIREMENT_2 du module
constexpr uint32 CoaMauditMarqueurBase = 525031;  // SPELL_CURSED_FORM_REQUIREMENT du module
constexpr uint8 COA_MAUDIT_VERROU = 1;            // la forme pose 524861
constexpr uint8 COA_MAUDIT_BASE = 2;              // la forme pose 525031

uint8 CoaMauditMarqueursDeForme(uint32 forme)
{
    switch (forme)
    {
        case 562720: case 562572: case 800157:
            return COA_MAUDIT_VERROU | COA_MAUDIT_BASE;
        case 680692: case 801076:
            return COA_MAUDIT_BASE;
        default:
            return 0;
    }
}

uint8 CoaMauditMarqueurExige(SpellInfo const* info)
{
    return info->CasterAuraSpell == CoaMauditMarqueur ? COA_MAUDIT_VERROU :
           info->CasterAuraSpell == CoaMauditMarqueurBase ? COA_MAUDIT_BASE : 0;
}

// Les sorts a lancer maintenant : `defensifs` (dans la forme) ou, a defaut, `formes`. Faux : rien a faire. Le calcul ne
// lit le kit que pour un Bloodmage sous lowHealth en combat (deux comparaisons pour tout autre bot).
bool CoaMauditPlanifier(PlayerbotAI* botAI, Player* bot, std::vector<Usable>* defensifsPrets, std::vector<Usable>* formesPretes)
{
    if (!sPlayerbotAIConfig.coaIaFormeMaudite || bot->getClass() != CLASS_SON_OF_ARUGAL ||
        bot->GetHealthPct() >= float(sPlayerbotAIConfig.lowHealth) || !CoaIaDecisionActive(bot))
        return false;
    std::vector<Usable> defensifs = KnownAbilities(botAI, bot, [](uint16 kind, uint16 marques)
        { return (kind & KIND_DEFENSIVE) && !(marques & MARQUE_PAS_SUR_SOI); });
    defensifs.erase(std::remove_if(defensifs.begin(), defensifs.end(), [bot](Usable const& spell)
        { return !CoaMauditMarqueurExige(EffectiveSpell(bot, spell.info)); }), defensifs.end());
    if (defensifs.empty())
        return false;
    for (Usable const& spell : defensifs)
        if (CoaAreneAuraDeSoi(bot, spell.info, EffectiveSpell(bot, spell.info)))
            return false;
    bool const verrou = bot->HasAura(CoaMauditMarqueur);
    bool const base = bot->HasAura(CoaMauditMarqueurBase);
    std::vector<Usable> prets;
    uint8 manque = 0;
    for (Usable const& spell : defensifs)
    {
        uint8 const exige = CoaMauditMarqueurExige(EffectiveSpell(bot, spell.info));
        bool const tenu = exige == COA_MAUDIT_VERROU ? verrou : base;
        if (tenu)
        {
            if (CoaIaPretSauf(botAI, bot, spell, bot, false, false, true))
                prets.push_back(spell);
        }
        else if (CoaIaPretSauf(botAI, bot, spell, bot, true, false, false))
            manque |= exige;
    }
    if (!prets.empty())
    {
        if (defensifsPrets)
            *defensifsPrets = std::move(prets);
        return true;
    }
    // Deja dans une forme maudite (un marqueur tenu) : jamais une seconde forme par-dessus.
    if (!manque || verrou || base)
        return false;
    // La forme se paie en vie (Blood Curse 40 %, Accursed Form 25 % de la vie de base) : CoaIaPretSauf garde le plancher
    // CoaIaResteMinPv.
    // Revue du lot 5 : seulement une forme a DUREE BORNEE (Blood Curse 562720, Accursed Form 562572 : 30 s ; Transgression
    // 801076 : 15 s). Eternal Curse 800157 et Sanguine Essence 680692 n'ont pas de duree : prises ici, elles resteraient
    // (Eternal Curse enferme la forme mortelle, rien ne la retire). La spe 25 (Blood Curse remplace par Sanguine Essence)
    // n'entre donc plus en forme pour Wicked Howl ; la spe 99 entre par Blood Curse, qu'elle garde. Deja en forme, rien
    // ne change : le defensif pret part.
    std::vector<Usable> formes = KnownAbilities(botAI, bot, [](uint16) { return true; });
    formes.erase(std::remove_if(formes.begin(), formes.end(), [&](Usable const& spell)
        { SpellInfo const* const info = EffectiveSpell(bot, spell.info);
          return !(CoaMauditMarqueursDeForme(info->Id) & manque) || info->GetMaxDuration() <= 0 ||
                 !CoaIaPretSauf(botAI, bot, spell, bot, false, false, true); }), formes.end());
    if (formes.empty())
        return false;
    if (formesPretes)
        *formesPretes = std::move(formes);
    return true;
}

class CoaFormeMauditeAction : public Action
{
public:
    CoaFormeMauditeAction(PlayerbotAI* botAI) : Action(botAI, "coa forme maudite") {}

    bool Execute(Event /*event*/) override
    {
        std::vector<Usable> defensifs;
        std::vector<Usable> formes;
        if (!CoaMauditPlanifier(botAI, bot, &defensifs, &formes))
            return false;
        if (!defensifs.empty())
            return RecordUsage(USAGE_DEFENSIVE, CastFirst(botAI, bot, defensifs, bot, USAGE_DEFENSIVE));
        return CastFirst(botAI, bot, formes, bot);
    }

    bool isUseful() override { return CoaMauditPlanifier(botAI, bot, nullptr, nullptr); }
};

class CoaFormeMauditeTrigger : public Trigger
{
public:
    CoaFormeMauditeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa forme maudite") {}

    bool IsActive() override { return CoaMauditPlanifier(botAI, bot, nullptr, nullptr); }
};

/*
 * 2. VENOMANCER (classe 29 ; specialisations 52 Fortitude tank, 53 Stalking, 54 Rot), AiPlayerbot.CoaIa.VenomancerSoin.
 * Decision, mot pour mot : « il se heal des 35% ». Emerald Veil 801152 et Shadra's Prayer 502947 portent
 * SPELL_ATTR0_NOT_SHAPESHIFTED, et Beetle Form 803183 / Spider Form 800841 (flags1 0x2d8, sans SHAPESHIFT_FLAG_STANCE) les
 * interdisent : ecartes en silence par CoaRefuseParFormeOuArme (C6).
 *
 * Sous 35 % en combat, dans l'une des deux formes, avec un tel soin pret hors forme : le bot QUITTE la forme et soigne
 * sur soi dans le meme passage (le retrait de l'aura remet la forme a zero tout de suite). Il la REPREND des qu'il remonte
 * a 40 % (5 points d'hysteresis), des que plus aucun de ces soins n'est pret, ou au bout de 5 s hors forme pour un tank
 * (8 s sinon) : le tank n'a pas ses 64 sorts de Beetle Form pendant ce temps. Contre la boucle sortir-entrer : apres une
 * reprise, pas de nouvelle sortie avant 15 s pour un tank (8 s sinon). Une reprise impossible (mana : Beetle Form coute
 * 30 % de la mana de base) est abandonnee au bout de 30 s ; « coa buff » remet la forme hors combat comme avant.
 */
constexpr uint32 CoaVenoBeetle = 803183;
constexpr uint32 CoaVenoSpider = 800841;
constexpr float CoaVenoSoinPv = 35.0f;
constexpr float CoaVenoRemonteePv = 40.0f;
constexpr uint32 CoaVenoHorsFormeTankMs = 5 * IN_MILLISECONDS;
constexpr uint32 CoaVenoHorsFormeMs = 8 * IN_MILLISECONDS;
constexpr uint32 CoaVenoEcartTankMs = 15 * IN_MILLISECONDS;
constexpr uint32 CoaVenoEcartMs = 8 * IN_MILLISECONDS;
constexpr uint32 CoaVenoAbandonMs = 30 * IN_MILLISECONDS;

uint32 CoaVenoFormeTenue(Player* bot)
{
    ObjectGuid const moi = bot->GetGUID();
    return bot->HasAura(CoaVenoBeetle, moi) ? CoaVenoBeetle : bot->HasAura(CoaVenoSpider, moi) ? CoaVenoSpider : 0;
}

bool CoaVenoTank(Player* bot) { return GetCoaRole(bot) == CoaRole::Tank; }

// L'etat de la sortie de forme, remis a zero quand le bot est mort, a repris sa forme (par ce lot ou par « coa buff »), a
// pris l'autre forme, ou quand la reprise est abandonnee.
CoaAiObjectContext::CoaVenoForme& CoaVenoEtat(PlayerbotAI* botAI, Player* bot)
{
    CoaAiObjectContext::CoaVenoForme& e = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->venoForme;
    if (!e.forme)
        return e;
    // Relecture : une forme reprise par un AUTRE chemin (« coa buff », ou CoaChassePrendreSaForme qui la remet toutes les
    // 5 s pendant une traque, en combat compris) compte comme une reprise : l'ecart anti-boucle s'applique aussi.
    // Revue du lot 5 : seulement en combat et peu apres la sortie (borne hors forme + la cadence de 5 s de la traque) ; une
    // forme retrouvee plus tard (combat fini sans passage de « coa buff ») efface l'etat sans armer l'ecart.
    if (bot->IsAlive() && CoaVenoFormeTenue(bot))
    {
        bool const recente = bot->IsInCombat() && getMSTimeDiff(e.sortieMs, getMSTime()) <
            (CoaVenoTank(bot) ? CoaVenoHorsFormeTankMs : CoaVenoHorsFormeMs) + 5 * IN_MILLISECONDS;
        e.forme = 0;
        if (recente)
            e.repriseMs = std::max<uint32>(getMSTime(), 1);
    }
    else if (!bot->IsAlive() || getMSTimeDiff(e.sortieMs, getMSTime()) >= CoaVenoAbandonMs)
        e.forme = 0;
    return e;
}

// Les soins sur soi que la forme interdit (sort effectif SPELL_ATTR0_NOT_SHAPESHIFTED), dans l'ordre du lot 3.
std::vector<Usable> CoaVenoSoins(PlayerbotAI* botAI, Player* bot)
{
    std::vector<Usable> soins = CoaIaSoinsSurSoi(botAI, bot);
    soins.erase(std::remove_if(soins.begin(), soins.end(), [bot](Usable const& spell)
        { return !EffectiveSpell(bot, spell.info)->HasAttribute(SPELL_ATTR0_NOT_SHAPESHIFTED); }), soins.end());
    return soins;
}

bool CoaVenoSoinPret(PlayerbotAI* botAI, Player* bot, bool sansForme, bool gcd)
{
    for (Usable const& spell : CoaVenoSoins(botAI, bot))
        if (CoaIaPretSauf(botAI, bot, spell, bot, false, sansForme, gcd))
            return true;
    return false;
}

// Sortir de forme (ou continuer hors forme) pour se soigner.
bool CoaVenoSoinVoulu(PlayerbotAI* botAI, Player* bot)
{
    if (!sPlayerbotAIConfig.coaIaVenomancerSoin || bot->getClass() != CLASS_PROPHET ||
        bot->GetHealthPct() >= CoaVenoSoinPv || !CoaIaDecisionActive(bot))
        return false;
    CoaAiObjectContext::CoaVenoForme const& e = CoaVenoEtat(botAI, bot);
    uint32 const maintenant = getMSTime();
    bool const tank = CoaVenoTank(bot);
    if (CoaVenoFormeTenue(bot))
    {
        if (e.repriseMs && getMSTimeDiff(e.repriseMs, maintenant) < (tank ? CoaVenoEcartTankMs : CoaVenoEcartMs))
            return false;
        return CoaVenoSoinPret(botAI, bot, true, true);
    }
    // Hors forme sans l'avoir quittee ici : le lot 3 (« coa soin soi ») soigne deja, rien a decider.
    if (!e.forme || getMSTimeDiff(e.sortieMs, maintenant) >= (tank ? CoaVenoHorsFormeTankMs : CoaVenoHorsFormeMs))
        return false;
    return CoaVenoSoinPret(botAI, bot, false, true);
}

// La forme quittee par ce lot, a reprendre maintenant ; nullptr sinon. Ne depend pas de l'option : une sortie en cours se
// termine meme si l'option vient d'etre eteinte.
SpellInfo const* CoaVenoRepriseVoulue(PlayerbotAI* botAI, Player* bot, std::vector<Usable>* forme)
{
    if (bot->getClass() != CLASS_PROPHET || !bot->IsAlive() || !bot->IsInCombat() || bot->IsMounted())
        return nullptr;
    CoaAiObjectContext::CoaVenoForme const& e = CoaVenoEtat(botAI, bot);
    if (!e.forme)
        return nullptr;
    bool const remonte = bot->GetHealthPct() >= CoaVenoRemonteePv;
    bool const temps = getMSTimeDiff(e.sortieMs, getMSTime()) >= (CoaVenoTank(bot) ? CoaVenoHorsFormeTankMs : CoaVenoHorsFormeMs);
    if (!remonte && !temps && CoaVenoSoinPret(botAI, bot, false, false))
        return nullptr;
    uint32 const voulue = e.forme;
    for (Usable const& spell : KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_STANCE) != 0; }))
        if (spell.info->Id == voulue && CoaIaPretSauf(botAI, bot, spell, nullptr, false, false, true))
        {
            if (forme)
                forme->assign(1, spell);
            return spell.info;
        }
    return nullptr;
}

class CoaVenoSoinAction : public Action
{
public:
    CoaVenoSoinAction(PlayerbotAI* botAI) : Action(botAI, "coa veno soin") {}

    bool Execute(Event /*event*/) override
    {
        if (!CoaVenoSoinVoulu(botAI, bot))
            return false;
        bool quittee = false;
        if (uint32 const tenue = CoaVenoFormeTenue(bot))
        {
            CoaAiObjectContext::CoaVenoForme& e = CoaVenoEtat(botAI, bot);
            e.forme = tenue;
            e.sortieMs = getMSTime();
            bot->RemoveAurasDueToSpell(tenue, bot->GetGUID());
            quittee = true;
        }
        t_bancSoinEchec = BancSoinEchec();
        std::vector<Usable> const soins = CoaVenoSoins(botAI, bot);
        SpellInfo const* cast = RecordUsage(USAGE_SOIN_SOI, CastFirst(botAI, bot, soins, bot, USAGE_SOIN_SOI));
        BancSoinTracer(bot, "soin_soi", cast, soins.empty());
        return cast || quittee;
    }

    bool isUseful() override { return CoaVenoSoinVoulu(botAI, bot); }
};

class CoaVenoFormeAction : public Action
{
public:
    CoaVenoFormeAction(PlayerbotAI* botAI) : Action(botAI, "coa veno forme") {}

    bool Execute(Event /*event*/) override
    {
        std::vector<Usable> forme;
        if (!CoaVenoRepriseVoulue(botAI, bot, &forme))
            return false;
        if (!CastFirst(botAI, bot, forme, bot))
            return false;
        CoaAiObjectContext::CoaVenoForme& e = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->venoForme;
        e.forme = 0;
        e.repriseMs = std::max<uint32>(getMSTime(), 1);
        return true;
    }

    bool isUseful() override { return CoaVenoRepriseVoulue(botAI, bot, nullptr) != nullptr; }
};

class CoaVenoSoinTrigger : public Trigger
{
public:
    CoaVenoSoinTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa veno soin") {}

    bool IsActive() override { return CoaVenoSoinVoulu(botAI, bot); }
};

class CoaVenoFormeTrigger : public Trigger
{
public:
    CoaVenoFormeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa veno forme") {}

    bool IsActive() override { return CoaVenoRepriseVoulue(botAI, bot, nullptr) != nullptr; }
};

/*
 * C1 du 16/10 (coa-project/docs/DIAG-banc-soins-inertie-pyro-sunc-20261016.md, option
 * AiPlayerbot.CoaIa.SoigneurResteEnCombat) : un soigneur ne quitte plus son moteur de combat parce que sa
 * cible ennemie est devenue invalide.
 *
 * Avant : « invalid target » -> « drop target » (99, CombatStrategy.cpp) -> ChangeEngine(BOT_STATE_NON_COMBAT)
 * (ChooseTargetActions.cpp, DropTargetAction::Execute). Le moteur hors combat n'a aucun soin, le coeur tient
 * le bot en combat, et le seul retour (« dps assist », DpsAssistStrategy.cpp) exige une cible VUE depuis sa
 * place : 40 a 130 s d'inertie a mana pleine, groupe mort.
 *
 * Vrai seulement pour un soigneur CoA (role Heal : GetCoaRole rend Dps pour les classes 1 a 11), vivant,
 * que le coeur tient en combat, et dont le combat vit encore : lui-meme blesse ou attaque, ou un membre
 * vivant du groupe, sur la meme carte et a 100 yd au plus, en combat ou blesse. Des que c'est faux (coeur
 * hors combat, ou plus personne en combat ni blesse), le chemin d'origine reprend et rend le bot au moteur
 * hors combat : pas de soigneur coince en combat.
 *
 * Cout : appele seulement quand « invalid target » est deja vrai ; au pire une boucle sur le groupe.
 */
bool CoaSoigneurResteEnCombat(Player* bot)
{
    if (!sPlayerbotAIConfig.coaIaSoigneurResteEnCombat || !bot || !bot->IsAlive() || !bot->IsInCombat())
        return false;
    if (GetCoaRole(bot) != CoaRole::Heal)
        return false;
    if (bot->GetHealthPct() < 100.0f || !bot->getAttackers().empty())
        return true;
    Group* group = bot->GetGroup();
    if (!group)
        return false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        // Meme carte d'abord (P-137 : rien d'un autre fil), puis la distance, puis l'etat.
        if (!member || member == bot || !member->IsInWorld() || member->GetMapId() != bot->GetMapId() ||
            !member->IsAlive() || !bot->IsWithinDistInMap(member, 100.0f))
            continue;
        if (member->IsInCombat() || member->GetHealthPct() < 100.0f)
            return true;
    }
    return false;
}

/*
 * Remplace « invalid target » dans « coa heal » (CoaHealStrategy::InitTriggers). Hors du cas ci-dessus, meme
 * verdict que l'amont (InvalidTargetTrigger, GenericTriggers.cpp). Dans le cas ci-dessus, ne part que sur une
 * cible NON NULLE : InvalidTargetValue rend vrai pour une cible nulle (InvalidTargetValue.cpp, `return
 * !target;`), et l'effacement repasserait alors a chaque tour, a 99, devant tous les soins (Engine.cpp
 * s'arrete a la premiere action executee).
 */
class CoaInvalidTargetTrigger : public Trigger
{
public:
    CoaInvalidTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa invalid target") {}

    bool IsActive() override
    {
        if (!AI_VALUE2(bool, "invalid target", "current target"))
            return false;
        if (!CoaSoigneurResteEnCombat(bot))
            return true;
        Unit* target = AI_VALUE(Unit*, "current target");
        if (!target)
            return false;
        // Relecture : une cible vivante et vue que « dps target » propose encore (effrayee, isolee : ni
        // AttackersValue ni AttackAction ne l'ecartent) serait reprise au tour suivant par « dps assist » (50), puis
        // effacee de nouveau a 99 -- un tour sur deux, sans un soin entre les deux. On la garde : les soins ne
        // dependent pas de la cible ennemie. Une cible morte, cachee (LOS lue ici) ou en esquive reste effacee.
        if (target->IsAlive() && target == AI_VALUE(Unit*, "dps target") && bot->IsWithinLOSInMap(target))
            return false;
        return true;
    }
};

/*
 * Remplace « drop target » derriere « coa invalid target ». Dans le cas de CoaSoigneurResteEnCombat : le meme
 * effacement que DropTargetAction::Execute (butin d'une cible morte, cible courante, selection, AttackStop),
 * SANS ChangeEngine. Sinon : DropTargetAction::Execute, tel quel.
 */
class CoaDropTargetAction : public Action
{
public:
    CoaDropTargetAction(PlayerbotAI* botAI) : Action(botAI, "coa drop target"), amont(botAI) {}

    bool Execute(Event event) override
    {
        if (!CoaSoigneurResteEnCombat(bot))
            return amont.Execute(event);

        Unit* target = AI_VALUE(Unit*, "current target");
        if (target && target->isDead())
        {
            ObjectGuid guid = target->GetGUID();
            if (guid)
                context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);
        }
        context->GetValue<Unit*>("current target")->Set(nullptr);
        bot->SetTarget(ObjectGuid::Empty);
        bot->SetSelection(ObjectGuid());
        bot->AttackStop();
        return true;
    }

private:
    DropTargetAction amont;
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

        // CoA (2026-10-02) : « coa ranged » derive de CombatStrategy et non de
        // RangedCombatStrategy ; elle n'avait donc aucun recul, et un bot a distance colle au
        // contact n'en repartait jamais (« reach spell » rapproche, ne recule pas). Meme
        // declencheur, meme action et meme priorite que RangedCombatStrategy.cpp (amont) :
        // « enemy too close for spell » -> « flee », ACTION_MOVE + 4. Ce declencheur ne
        // part que si la cible ne frappe PAS ce bot, ou si elle est gelee ou enracinee
        // (RangeTriggers.cpp) : sans effet en un contre un, il joue en groupe, PvE compris
        // (le monstre tenu par le tank). Pose ici, dans la strategie existante, et non par
        // un addStrategy (P-130). « coa heal » passe aussi ranged = true mais n'est pas
        // « coa ranged » : il n'est pas touche.
        if (ranged && getName() == "coa ranged")
            triggers.push_back(new TriggerNode("enemy too close for spell", { NextAction("flee", ACTION_MOVE + 4) }));

        triggers.push_back(new TriggerNode("coa enemy casting", { NextAction("coa interrupt", InterruptPriority()) }));
        triggers.push_back(new TriggerNode("coa dispel", { NextAction("coa dispel", DispelPriority()) }));
        triggers.push_back(new TriggerNode("medium aoe", { NextAction("coa aoe", ACTION_HIGH + 2) }));
        triggers.push_back(new TriggerNode("low health", { NextAction("coa defensive", ACTION_HIGH + 8) }));
        // IA soins/survie, lot 3 : declencheurs inertes sous AiPlayerbot.CoaIa.Survie = 0 (et SurviePotion = 0), qui
        // rendent alors le comportement d'avant.
        triggers.push_back(new TriggerNode("coa survie", { NextAction("coa defensive", ACTION_HIGH + 8),
                                                           NextAction("coa soin soi", ACTION_HIGH + 7) }));
        triggers.push_back(new TriggerNode("coa survie critique", { NextAction("coa defensive critique", ACTION_EMERGENCY) }));
        triggers.push_back(new TriggerNode("coa potion", { NextAction("healing potion", ACTION_HIGH + 7.5f) }));
        // IA soins/survie, lot 5 : decisions de jeu du 16/10 (Bloodmage, Venomancer). Inertes pour toute autre classe et
        // sous AiPlayerbot.CoaIa.FormeMaudite = 0 / VenomancerSoin = 0. Au-dessus de « coa defensive » (ACTION_HIGH + 8),
        // sous « coa defensive critique » (ACTION_EMERGENCY) ; la reprise de forme passe devant la sortie.
        triggers.push_back(new TriggerNode("coa forme maudite", { NextAction("coa forme maudite", ACTION_HIGH + 8.5f) }));
        triggers.push_back(new TriggerNode("coa veno soin", { NextAction("coa veno soin", ACTION_HIGH + 8.5f) }));
        triggers.push_back(new TriggerNode("coa veno forme", { NextAction("coa veno forme", ACTION_HIGH + 9) }));
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
        size_t const debut = triggers.size();
        CoaCombatStrategy::InitTriggers(triggers);

        // C1 du 16/10 (CoaSoigneurResteEnCombat) : « invalid target » -> « drop target » (CombatStrategy) devient
        // « coa invalid target » -> « coa drop target », a la meme pertinence. Seuls les noeuds poses par cet
        // appel sont touches : le vecteur est celui du moteur entier (Engine::Init). Le choix entre l'ancien et
        // le nouveau comportement se fait a chaque tour, sur l'option : a 0, le declencheur rend le verdict de
        // l'amont et l'action appelle DropTargetAction::Execute.
        for (size_t i = debut; i < triggers.size(); ++i)
        {
            if (triggers[i] && triggers[i]->getName() == "invalid target")
            {
                delete triggers[i];
                triggers[i] = new TriggerNode("coa invalid target", { NextAction("coa drop target", 99) });
            }
        }

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
        // I2 du 17/10 (CoaRegenManaTrigger) : 25, sous les soins (30+), « coa defensive » (28), l'interruption (27) et la
        // dissipation (26) du soigneur ; devant « coa aoe » (22) et la rotation (10). Inerte hors soigneur a balise.
        triggers.push_back(new TriggerNode("coa regen mana", { NextAction("coa regen mana", ACTION_MEDIUM_HEAL + 5) }));
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
        // Reserve de chasseurs (02/10, revue M2) : strategie de COMBAT du chasseur
        // d'une prime (CoaChasse.h). Meme fabrique sans freres : sa pose ne retire
        // rien du moteur de combat.
        creators["coa chasseur prime"] = &CoaChasseStrategyFactoryInternal::coa_chasseur_prime;
    }

private:
    static Strategy* coa_chasse(PlayerbotAI* botAI) { return new CoaChasseStrategy(botAI); }
    static Strategy* coa_chasseur_prime(PlayerbotAI* botAI) { return new CoaChasseurPrimeStrategy(botAI); }
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
        creators["coa soin soi"] = &CoaActionFactoryInternal::coa_soin_soi;
        creators["coa defensive critique"] = &CoaActionFactoryInternal::coa_defensive_critique;
        creators["coa forme maudite"] = &CoaActionFactoryInternal::coa_forme_maudite;
        creators["coa veno soin"] = &CoaActionFactoryInternal::coa_veno_soin;
        creators["coa veno forme"] = &CoaActionFactoryInternal::coa_veno_forme;
        creators["coa drop target"] = &CoaActionFactoryInternal::coa_drop_target;
        creators["coa regen mana"] = &CoaActionFactoryInternal::coa_regen_mana;
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
    static Action* coa_soin_soi(PlayerbotAI* botAI) { return new CoaSoinSoiAction(botAI); }
    static Action* coa_defensive_critique(PlayerbotAI* botAI)
    {
        return new CoaDefensiveAction(botAI, "coa defensive critique", true);
    }
    static Action* coa_forme_maudite(PlayerbotAI* botAI) { return new CoaFormeMauditeAction(botAI); }
    static Action* coa_veno_soin(PlayerbotAI* botAI) { return new CoaVenoSoinAction(botAI); }
    static Action* coa_veno_forme(PlayerbotAI* botAI) { return new CoaVenoFormeAction(botAI); }
    static Action* coa_drop_target(PlayerbotAI* botAI) { return new CoaDropTargetAction(botAI); }
    static Action* coa_regen_mana(PlayerbotAI* botAI) { return new CoaRegenManaAction(botAI); }
};

class CoaTriggerFactoryInternal : public NamedObjectContext<Trigger>
{
public:
    CoaTriggerFactoryInternal()
    {
        creators["coa dispel"] = &CoaTriggerFactoryInternal::coa_dispel;
        creators["coa enemy casting"] = &CoaTriggerFactoryInternal::coa_enemy_casting;
        creators["coa ambush"] = &CoaTriggerFactoryInternal::coa_ambush;
        creators["coa survie"] = &CoaTriggerFactoryInternal::coa_survie;
        creators["coa survie critique"] = &CoaTriggerFactoryInternal::coa_survie_critique;
        creators["coa potion"] = &CoaTriggerFactoryInternal::coa_potion;
        creators["coa forme maudite"] = &CoaTriggerFactoryInternal::coa_forme_maudite;
        creators["coa veno soin"] = &CoaTriggerFactoryInternal::coa_veno_soin;
        creators["coa veno forme"] = &CoaTriggerFactoryInternal::coa_veno_forme;
        creators["coa invalid target"] = &CoaTriggerFactoryInternal::coa_invalid_target;
        creators["coa regen mana"] = &CoaTriggerFactoryInternal::coa_regen_mana;
    }

private:
    static Trigger* coa_dispel(PlayerbotAI* botAI) { return new CoaDispelTrigger(botAI); }
    static Trigger* coa_enemy_casting(PlayerbotAI* botAI) { return new CoaEnemyCastingTrigger(botAI); }
    static Trigger* coa_ambush(PlayerbotAI* botAI) { return new CoaAmbushTrigger(botAI); }
    static Trigger* coa_survie(PlayerbotAI* botAI) { return new CoaSurvieTrigger(botAI); }
    static Trigger* coa_survie_critique(PlayerbotAI* botAI) { return new CoaSurvieCritiqueTrigger(botAI); }
    static Trigger* coa_potion(PlayerbotAI* botAI) { return new CoaPotionTrigger(botAI); }
    static Trigger* coa_forme_maudite(PlayerbotAI* botAI) { return new CoaFormeMauditeTrigger(botAI); }
    static Trigger* coa_veno_soin(PlayerbotAI* botAI) { return new CoaVenoSoinTrigger(botAI); }
    static Trigger* coa_veno_forme(PlayerbotAI* botAI) { return new CoaVenoFormeTrigger(botAI); }
    static Trigger* coa_invalid_target(PlayerbotAI* botAI) { return new CoaInvalidTargetTrigger(botAI); }
    static Trigger* coa_regen_mana(PlayerbotAI* botAI) { return new CoaRegenManaTrigger(botAI); }
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
 * P-167 -- les deux ponts de l'approche furtive d'une prime (declares et
 * expliques dans Ai/Coa/CoaChasse.h). Ils ne sont appeles que par la traque
 * d'un chasseur designe (CoaChasseAction::TraquerLaPrime) : aucun autre bot ne
 * passe ici.
 */

// Vitesse d'approche prudente d'un bot furtif (la course vaut 7 yd/s ; Underwalk
// 800797 ralentit de 21 % : CONCEPTION-mercenaire-chasseur.md § 2.3). Sert a ne
// pas lancer a 100 yd une furtivite breve qui tomberait en route.
constexpr float ApprocheFurtiveYdParS = 5.0f;

bool CoaChasseEntrerEnFurtivite(PlayerbotAI* botAI, Player* bot, float distance)
{
    if (!botAI || !bot || !ClassHas(bot, KIND_STEALTH))
        return false;
    if (bot->IsInCombat() || bot->IsMounted() || bot->IsInFlight() || !bot->IsAlive() ||
        bot->HasAuraType(SPELL_AURA_MOD_STEALTH))
        return false;

    // Le meme filtre que CoaStealthAction : une furtivite, jamais un sort hostile.
    std::vector<Usable> const spells = KnownAbilities(botAI, bot, [](uint16 kind)
        { return (kind & KIND_STEALTH) && !(kind & (KIND_HOSTILE | KIND_DAMAGE)); });

    for (Usable const& spell : spells)
    {
        int32 const duree = spell.info->GetMaxDuration();
        if (duree > 0 && float(duree) / float(IN_MILLISECONDS) * ApprocheFurtiveYdParS < distance)
            continue;
        // Embuscade (02/10) : jamais une furtivite CANALISEE -- Twisted Sight 532229
        // du Felsworn (AttributesEx 0x00022004 : SPELL_ATTR1_IS_CHANNELED, lu dans
        // Spell.dbc) lie la vue a un oeil invoque : le bot resterait plante a
        // canaliser au lieu d'approcher. Revue m6 : seulement sous
        // Directeur.Embuscade = 1 -- a 0, la traque P-167 a l'identique.
        if (sPlayerbotAIConfig.directeurEmbuscade && spell.info->IsChanneled())
            continue;

        SpellCastResult const check = StrictCheck(bot, spell.info, bot);
        if (check != SPELL_CAST_OK)
        {
            RecordFailure(USAGE_STEALTH, spell.info->Id, uint16(check));
            continue;
        }

        if (botAI->CastSpell(spell.info->Id, bot))
        {
            RecordUsage(USAGE_STEALTH, spell.info);
            return true;
        }

        RecordFailure(USAGE_STEALTH, spell.info->Id, FAILURE_REFUSED);
    }

    RecordUsage(USAGE_STEALTH, nullptr);
    return false;
}

bool CoaChassePrendreSaForme(PlayerbotAI* botAI, Player* bot)
{
    if (!botAI || !bot || !ClassHas(bot, KIND_STANCE))
        return false;
    if (bot->IsInCombat() || bot->IsMounted() || bot->IsInFlight() || !bot->IsAlive() ||
        bot->HasAuraType(SPELL_AURA_MOD_STEALTH))
        return false;

    // Le filtre des postures de CoaBuffAction, postures seules.
    std::vector<Usable> const postures = KnownAbilities(botAI, bot, [](uint16 kind)
        { return (kind & KIND_STANCE) && !(kind & KIND_STEALTH) &&
                 !(kind & (KIND_HOSTILE | KIND_DAMAGE | KIND_TAUNT | KIND_HEAL | KIND_CONTROL)); });
    if (postures.empty())
        return false;

    // La regle de d723518 (CoaBuffAction) : un Venomancer Stalking ne prend que
    // Spider Form, apres avoir quitte Beetle Form. Toute autre classe : une
    // posture seulement s'il n'en porte aucune.
    constexpr uint32 SpiderForm = 800841;
    constexpr uint32 StalkingSpecialization = 53;
    constexpr uint32 BeetleForm = 803183;
    bool const wantsSpider = bot->getClass() == CLASS_PROPHET &&
        GetAscensionActiveSpecialization(bot) == StalkingSpecialization &&
        std::any_of(postures.begin(), postures.end(), [](Usable const& spell) { return spell.info->Id == SpiderForm; });
    // Lot 2 : la posture de son role pour un tank, comme CoaBuffAction (CoaPostureDeRole).
    uint32 const voulue = wantsSpider ? 0 : CoaPostureDeRole(botAI, bot, postures);
    bool const inStance = wantsSpider ? bot->HasAura(SpiderForm) : voulue ? bot->HasAura(voulue) :
        std::any_of(postures.begin(), postures.end(), [bot](Usable const& spell) { return bot->HasAura(spell.info->Id); });
    if (inStance)
        return false;
    if (wantsSpider && bot->HasAura(BeetleForm))
        bot->RemoveAurasDueToSpell(BeetleForm);

    for (Usable const& spell : postures)
    {
        if (wantsSpider && spell.info->Id != SpiderForm)
            continue;
        if (voulue && spell.info->Id != voulue)
            continue;
        SpellCastResult const check = StrictCheck(bot, spell.info, bot);
        if (check != SPELL_CAST_OK)
        {
            RecordFailure(USAGE_BUFF, spell.info->Id, uint16(check));
            continue;
        }
        if (botAI->CastSpell(spell.info->Id, bot))
        {
            RecordUsage(USAGE_BUFF, spell.info);
            return true;
        }
        RecordFailure(USAGE_BUFF, spell.info->Id, FAILURE_REFUSED);
    }
    return false;
}

uint32 CoaChasseOuvrirEnFurtif(PlayerbotAI* botAI, Player* bot, Unit* cible, bool& connue)
{
    connue = false;
    if (!botAI || !bot || !cible || !bot->HasAuraType(SPELL_AURA_MOD_STEALTH))
        return 0;

    bool const tank = GetCoaRole(bot) == CoaRole::Tank;
    std::vector<Usable> ouvertures = KnownAbilities(botAI, bot, [tank](uint16 kind) { return IsAttack(kind, tank); });
    // « Seulement furtif », et jamais un controle (revue m1) : un Venomancer
    // ouvrirait par Arachnophobia 804970, une peur, et la proie fuirait.
    ouvertures.erase(std::remove_if(ouvertures.begin(), ouvertures.end(), [](Usable const& spell)
        { return !spell.info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) || (spell.kind & KIND_CONTROL); }),
        ouvertures.end());
    if (ouvertures.empty())
        return 0;
    connue = true;

    // Face a la proie AVANT le filtre d'orientation de DropUncastableHere (Flank
    // porte FacingCasterFlags = 1, lu dans Spell.dbc) ; arrete, pour que la
    // marche en cours ne remette pas l'orientation de son trajet.
    if (bot->isMoving())
        bot->StopMoving();
    ServerFacade::instance().SetFacingTo(bot, cible);

    DropUncastableHere(bot, cible, ouvertures);
    if (ouvertures.empty())
        return 0;

    SpellInfo const* const cast = CastFirst(botAI, bot, ouvertures, cible, USAGE_ATTACK);
    RecordUsage(USAGE_ATTACK, cast);
    return cast ? cast->Id : 0;
}

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
    // CoaArene IA : contexte sans freres lui aussi (P-130), sa pose ne retire rien.
    strategyContexts.Add(new CoaAreneStrategyFactoryInternal());
}

void CoaAiObjectContext::BuildSharedActionContexts(SharedNamedObjectContextList<Action>& actionContexts)
{
    AiObjectContext::BuildSharedActionContexts(actionContexts);
    actionContexts.Add(new CoaActionFactoryInternal());
    actionContexts.Add(new CoaAreneActionFactoryInternal());
}

void CoaAiObjectContext::BuildSharedTriggerContexts(SharedNamedObjectContextList<Trigger>& triggerContexts)
{
    AiObjectContext::BuildSharedTriggerContexts(triggerContexts);
    triggerContexts.Add(new CoaTriggerFactoryInternal());
    triggerContexts.Add(new CoaAreneTriggerFactoryInternal());
}

void CoaAiObjectContext::BuildSharedValueContexts(SharedNamedObjectContextList<UntypedValue>& valueContexts)
{
    AiObjectContext::BuildSharedValueContexts(valueContexts);
}
