/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CheckMountStateAction.h"
#include "AreaDefines.h"
#include "CoaDirecteur.h"
#include "BattleGroundTactics.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "DBCStores.h"
#include "Event.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SpellAuraEffects.h"
#include "Timer.h"
#include "CoaAiObjectContext.h"
#include <mutex>
#include "mod-ascension-compat/src/AscensionSpecialization.h"

static constexpr uint32 SPELL_COLD_WEATHER_FLYING = 54197;
static constexpr float PARACHUTE_LAND_THRESHOLD = 15.0f;
// CoA -- embuscade des chasseurs de prime (02/10) : la borne de
// CoaChasseAction::MontureInterditeYd (Ai/Coa/CoaChasse.h).
static constexpr float COA_MONTURE_INTERDITE_YD = 300.0f;

// CoA -- embuscade (02/10). Le bot est-il le chasseur d'une prime (option
// Directeur.Embuscade, prime non AFK) a moins de COA_MONTURE_INTERDITE_YD de son
// prime ? Le prime n'est lu que sur la carte du bot (botAI->GetUnit : fil du bot).
// Mesure de l'arc 5 (02/10, 14:55 Paris) : les chasseurs arrivaient « en monture a
// fond » -- cette action, a 1,0, les remontait a chaque tick ou la chasse rendait
// faux (aucune cible courante : shouldMount). Sans chasseur : un chargement atomique.
static bool CoaChasseurPresDeSonPrime(PlayerbotAI* botAI, Player* bot)
{
    CoaDirecteur::EtatEmbuscade emb;
    if (!CoaDirecteur::Embuscade(bot->GetGUID().GetCounter(), &emb))
        return false;
    Unit* prime = botAI->GetUnit(ObjectGuid::Create<HighGuid::Player>(emb.joueur));
    return prime && prime->IsInWorld() && prime->GetMapId() == bot->GetMapId() &&
           bot->GetExactDist2d(prime) < COA_MONTURE_INTERDITE_YD;
}

/*
 * CoA -- montures des bots (26/10/2026, coa-project/docs/DIAG-bots-montures-20261026.md). Ces gardes ne jouent que
 * pour les classes CoA (IsAscensionCustomClassId) : le comportement des classes vanille reste celui de l'amont.
 *
 * FORME DE CLASSE. Cinq sorts du kit CoA posent SPELL_AURA_MOUNTED en effet 0 (Spell.dbc lu le 26/10) : Divine Charge
 * 527272 (15 s), Huntress Saber 704772 et Moonchaser Kodo 707752 (30 s), Build: Mechsuit 803451 (18 h, pose par
 * 801384), Hellbound Charge 807248 (3 s). Tous ont une DUREE FINIE ; les vraies montures (5 506 sorts « aura 78 » de
 * famille vanille) sont a duree infinie, comme Celestial Steed 75614 et Boots of Speed 800284/801315 (CoA, infinies,
 * gardees). Sans filtre, CollectMountData les range parmi les montures : Divine Charge (vitesse 99) aurait ete tiree au
 * sort comme monture de voyage, en brulant son temps de recharge. Et Execute descendait (HandleCancelMountAura) le bot
 * de ces formes des qu'une cible courante etait proche ou que son maitre etait a pied -- un Tinker perdait son Mechsuit.
 * Les 11 sorts vanille « aura 78 » a duree finie sont des montures de quete ou d'evenement (Rental Racing Ram...).
 */
//
// Revue du 26/10 (M2) : deux autres sorts de classe posent l'aura 78 en effet 1, avec une duree INFINIE, et le filtre par
// duree ne les voyait pas : Xorothian Warsteed 804703 et Battlestrider 807383 (Knight of Xoroth, famille 23 ; sans eux
// pas de Hellfire Trample ni de Dreadrider's Stomp). Critere complet, verifie sur Spell.dbc le 26/10 : aura de monture,
// ET (duree finie, OU famille CoA (> SPELLFAMILY_PET) sans l'aura de monture en effet 0). Il retient exactement ces sept
// sorts parmi les sorts CoA ; Celestial Steed 75614 et Boots of Speed 800284/801315 (aura en effet 0, infinies) restent
// des montures. CollectMountData ne range que les sorts a aura de monture en effet 0 : 804703/807383 n'y entrent pas.
static bool CoaSortFormeDeClasse(SpellInfo const* info)
{
    if (!info || !info->HasAura(SPELL_AURA_MOUNTED))
        return false;
    if (info->GetMaxDuration() > 0)
        return true;
    return info->SpellFamilyName > SPELLFAMILY_PET && info->Effects[0].ApplyAuraName != SPELL_AURA_MOUNTED;
}

// Monte par une forme de classe ? Vaut pour le bot comme pour son maitre (revue M3).
static bool CoaMonteEnFormeDeClasse(Unit const* unit)
{
    if (!unit || !unit->IsMounted())
        return false;
    for (AuraEffect const* eff : unit->GetAuraEffectsByType(SPELL_AURA_MOUNTED))
        if (CoaSortFormeDeClasse(eff->GetSpellInfo()))
            return true;
    return false;
}

// Le maitre est-il monte au sens de cette action ? Une forme de classe n'est pas une monture a suivre (revue M3 : un
// maitre Templar en Divine Charge ou Tinker en Mechsuit faisait monter puis descendre ses compagnons). Classes CoA
// seulement : pour un bot vanille, master->IsMounted() comme chez l'amont.
static bool CoaMaitreMonte(Player* bot, Player* master)
{
    if (!master->IsMounted())
        return false;
    return !IsAscensionCustomClassId(bot->getClass()) || !CoaMonteEnFormeDeClasse(master);
}

// Revue m1 : CastSpell rend vrai au DEBUT de l'incantation. Si le bot n'est pas monte passe ce delai (1,5 s
// d'incantation, plus la marge), la montee a echoue (interrompue, refusee par le coeur) : le repit s'arme.
static constexpr uint32 COA_MONTURE_VERIF_MS = 3 * IN_MILLISECONDS;

// Revue B1 : la table des montures preferees (statique, partagee par tous les bots) est remplie UNE fois, sous
// std::call_once, puis seulement lue par find(). Avant : un `mountCache[guid]` par appel (insertion dans un
// unordered_map statique, sans verrou) depuis les fils de carte (MapUpdate.Threads) -- forme de P-050.
static std::once_flag sCoaMonturesPrefereesChargees;

// REPIT APRES UN ECHEC. TryRandomMountFiltered arrete le bot (StopMoving) AVANT CanCastSpell -- il le faut : CanCastSpell
// refuse tout sort a incantation tant que le bot bouge (PlayerbotAI.cpp, « Casting time and bot is moving »). Un bot
// eligible dont tous les essais echouent etait donc arrete net a chaque seconde (« timer »), et refaisait le parcours de
// tout son grimoire (CollectMountData). Apres une montee ratee, plus d'essai pendant COA_MONTURE_REPIT_MS : au plus un
// arret par bot et par repit.
static constexpr uint32 COA_MONTURE_REPIT_MS = 30 * IN_MILLISECONDS;

// Define the static map / init bool for caching bot preferred mount data globally
std::unordered_map<uint32, PreferredMountCache> CheckMountStateAction::mountCache;
bool CheckMountStateAction::preferredMountTableChecked = false;

MountData CollectMountData(Player const* bot)
{
    MountData data;
    for (auto& entry : bot->GetSpellMap())
    {
        uint32 spellId = entry.first;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->Effects[0].ApplyAuraName != SPELL_AURA_MOUNTED)
            continue;

        if (entry.second->State == PLAYERSPELL_REMOVED || !entry.second->Active || spellInfo->IsPassive())
            continue;

        int32 effect1 = spellInfo->Effects[1].BasePoints;
        int32 effect2 = spellInfo->Effects[2].BasePoints;

        int32 speed = std::max(effect1, effect2);

        // Update max speed if appropriate.
        if (speed > data.maxSpeed)
            data.maxSpeed = speed;  // In BG, clamp max speed to 99 later; here we just store the maximum found.

        // Determine index: flight if either effect has flight aura or specific mount ID.
        uint32 index = (spellInfo->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                        spellInfo->Effects[2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                        // Winged Steed of the Ebon Blade
                        // This mount is meant to autoscale from a 150% flyer
                        // up to a 280% as you train your flying skill up.
                        // This incorrectly gets categorised as a ground mount, force this to flyer only.
                        // TODO: Add other scaling mounts here if they have the same issue, or adjust above
                        // checks so that they are all correctly detected.
                        spellInfo->Id == 54729) ? 1 : 0;
        data.allSpells[index][speed].push_back(spellId);
    }
    return data;
}

bool CheckMountStateAction::Execute(Event /*event*/)
{
    // Forced flight dismount:
    // Bots get stale flight movement flags after a forced dismount (e.g: Dalaran) because the post landing dismount cleanup
    // needs MSG_MOVE_FALL_LAND (a client opcode) and client movement packets. The stale flags cause the bot to be stuck with
    // the parachute, or even keep the bot hovering indefinitely and block MMAP routing.
    // Note: Without MSG_MOVE_FALL_LAND, HandleFall doesn't trigger, meaning bots don't get fall damage in forced dismounts anyway,
    // so the parachute usage here is more of an immersion feature.
    if (bot->HasFeatherFallAura())
    {
        float floorZ = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (floorZ != INVALID_HEIGHT && floorZ != VMAP_INVALID_HEIGHT_VALUE &&
            bot->GetPositionZ() - floorZ <= PARACHUTE_LAND_THRESHOLD)
            bot->RemoveAurasByType(SPELL_AURA_FEATHER_FALL);
    }
    ClearStaleFlightFlags();

    // CoA -- embuscade (02/10) : a pied pres de son prime, et jamais remonte.
    if (CoaChasseurPresDeSonPrime(botAI, bot))
    {
        if (bot->IsMounted() && !bot->IsFlying())
        {
            Dismount();
            return true;
        }
        return false;
    }

    // Determine if there are no attackers
    bool noAttackers = !AI_VALUE2(bool, "combat", "self target") || !AI_VALUE(uint8, "attacker count");
    bool enemy = AI_VALUE(Unit*, "enemy player target");
    bool dps = AI_VALUE(Unit*, "dps target");
    bool shouldDismount = false;
    bool shouldMount = false;

    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    if (currentTarget)
    {
        float dismountDistance = CalculateDismountDistance();
        float mountDistance = CalculateMountDistance();
        float combatReach = bot->GetCombatReach() + currentTarget->GetCombatReach();
        float distanceToTarget = bot->GetExactDist(currentTarget);

        shouldDismount = (distanceToTarget <= dismountDistance + combatReach);
        shouldMount = (distanceToTarget > mountDistance + combatReach);
    }
    else
    {
        shouldMount = true;
    }

    // If should dismount, or master (if any) is no longer in travel form, yet bot still is, remove the shapeshifts
    if (shouldDismount ||
        (masterInShapeshiftForm != FORM_TRAVEL && botInShapeshiftForm == FORM_TRAVEL) ||
        (masterInShapeshiftForm != FORM_FLIGHT && botInShapeshiftForm == FORM_FLIGHT && master && !master->IsMounted()) ||
        (masterInShapeshiftForm != FORM_FLIGHT_EPIC && botInShapeshiftForm == FORM_FLIGHT_EPIC && master && !master->IsMounted()))
        botAI->RemoveShapeshift();

    if (shouldDismount && bot->IsMounted())
    {
        Dismount();
        return true;
    }

    bool inBattleground = bot->InBattleground();
    bool const noRealMaster = (!master || master == bot);

    // If there is a master and bot not in BG, follow master's mount state regardless of group leader
    if (!noRealMaster && !inBattleground)
    {
        if (ShouldFollowMasterMountState(master, noAttackers, shouldMount))
            return Mount();

        else if (ShouldDismountForMaster(master) && bot->IsMounted())
        {
            Dismount();
            return true;
        }

        return false;
    }

    // No real master (random bot or self-bot) OR bot in BG
    if ((noRealMaster || inBattleground) && !bot->IsMounted() &&
        noAttackers && shouldMount && !bot->IsInCombat())
        return Mount();

    if (!bot->IsFlying() && shouldDismount && bot->IsMounted() &&
        (enemy || dps || (!noAttackers && bot->IsInCombat())))
    {
        Dismount();
        return true;
    }

    return false;
}

bool CheckMountStateAction::isUseful()
{
    // Not useful when:
    if (botAI->IsInVehicle() || bot->isDead() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT) ||
        !bot->IsOutdoors() || bot->InArena())
        return false;

    master = GetMaster();

    // CoA (26/10) : jamais une valeur d'un appel precedent, ni indeterminee (sans maitre, rien ne les ecrivait).
    botInShapeshiftForm = FORM_NONE;
    masterInShapeshiftForm = FORM_NONE;

    // Get shapeshift states, only applicable when there's a master
    if (master)
    {
        botInShapeshiftForm = bot->GetShapeshiftForm();
        masterInShapeshiftForm = master->GetShapeshiftForm();
    }

    bool const coa = IsAscensionCustomClassId(bot->getClass());

    // CoA (26/10, revue M2) : une forme de classe a aura de monture (Mechsuit, Huntress Saber, Divine Charge,
    // Xorothian Warsteed...) n'est JAMAIS retiree par cette action, ni en combat ni hors combat :
    // Dismount -> HandleCancelMountAuraOpcode retire toutes les auras 78. Et rien ne monte par-dessus.
    if (coa && CoaMonteEnFormeDeClasse(bot))
        return false;

    // Not useful when in combat and not currently mounted / travel formed
    if ((bot->IsInCombat() || botAI->GetState() == BOT_STATE_COMBAT) &&
        !bot->IsMounted() && botInShapeshiftForm != FORM_TRAVEL && botInShapeshiftForm != FORM_FLIGHT && botInShapeshiftForm != FORM_FLIGHT_EPIC)
        return false;

    // Not useful when level lower than minimum required
    // (CoA revue m5 : remonte avant GetMapWaterOrGroundLevel, plus cher ; meme resultat, toutes ces gardes rendent faux)
    if (bot->GetLevel() < sPlayerbotAIConfig.useGroundMountAtMinLevel)
        return false;

    // CoA (26/10) -- gardes de montee des classes CoA, les moins cheres d'abord (revue m5). Un bot deja monte n'est pas
    // concerne : la descente reste celle de l'amont.
    if (coa && !bot->IsMounted())
    {
        uint32 const maintenant = getMSTime();
        // Revue m1 : montee lancee (CastSpell vrai au debut de l'incantation) -- verifiee COA_MONTURE_VERIF_MS plus tard.
        if (coaMonteeLancee)
        {
            if (getMSTimeDiff(coaMonteeLanceeMs, maintenant) < COA_MONTURE_VERIF_MS)
                return false;
            coaMonteeLancee = false;
            coaEchecMonture = true;   // toujours a pied : la montee a echoue
            coaEchecMontureMs = maintenant;
        }
        // Repit apres une montee ratee.
        if (coaEchecMonture && getMSTimeDiff(coaEchecMontureMs, maintenant) < COA_MONTURE_REPIT_MS)
            return false;
        // Revue m3 : assis (repas, boisson) -- StopMoving et l'incantation le releveraient et couperaient la regeneration.
        if (bot->IsSitState())
            return false;
        // Furtif : l'incantation romprait la furtivite.
        if (bot->HasAuraType(SPELL_AURA_MOD_STEALTH))
            return false;
        // Revue M1 : un embusqueur (mercenaire d'une classe a furtivite, WildPvp.StealthAmbush) reste a pied. Monte,
        // CoaAmbushTrigger et CoaStealthAction le refusent, et il descendait au contact sans ouverture. Sauf le
        // compagnon d'un humain : il suit l'etat monte de son maitre (ShouldFollowMasterMountState).
        if (!botAI->HasGameClientMaster() && CoaEmbusqueur(bot))
            return false;
        // Joueur ennemi retenu comme cible (EnemyPlayerValue, au plus un calcul par seconde) : pas d'arret de 1,5 s
        // devant lui ; « attack enemy player » (55) a la main. Garde P-035 : jamais AI_VALUE sur un nom non verifie.
        if (Value<Unit*>* ennemi = context->GetValue<Unit*>("enemy player target"))
            if (ennemi->Get())
                return false;
    }
    else if (coa)
        coaMonteeLancee = false;   // monte : la montee lancee a abouti

    // In addition to checking IsOutdoors, also check whether bot is clipping below floor slightly because that will
    // cause bot to falsly indicate they are outdoors. This fixes bug where bot tries to mount indoors (which seems
    // to mostly be an issue in tunnels of WSG and AV)
    float posZ = bot->GetPositionZ();
    float groundLevel = bot->GetMapWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(), posZ);
    if (!bot->IsMounted() && !bot->HasWaterWalkAura() && posZ < groundLevel)
        return false;

    // Not useful when bot does not have mount strat and is not currently mounted
    if (!GET_PLAYERBOT_AI(bot)->HasStrategy("mount", BOT_STATE_NON_COMBAT) && !bot->IsMounted())
        return false;

    // Allow mounting while transformed only if the form allows it
    if (bot->HasAuraType(SPELL_AURA_TRANSFORM) && bot->IsInDisallowedMountForm())
        return false;

    // BG Logic
    if (bot->InBattleground())
    {
        // Do not use when carrying BG Flags
        if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) || bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
            return false;

        // Only mount if BG starts in less than 30 sec
        if (Battleground* bg = bot->GetBattleground())
            if (bg->GetStatus() == STATUS_WAIT_JOIN && bg->GetStartDelayTime() > BG_START_DELAY_30S)
                return false;
    }

    return true;
}

bool CheckMountStateAction::Mount()
{
    bool const monte = MountInterne();
    // CoA (26/10) : repit apres un echec (voir COA_MONTURE_REPIT_MS). Un lancement reussi n'est qu'un debut
    // d'incantation : il est verifie par isUseful COA_MONTURE_VERIF_MS plus tard (revue m1).
    if (IsAscensionCustomClassId(bot->getClass()))
    {
        coaEchecMonture = !monte;
        coaMonteeLancee = monte && !bot->IsMounted();
        if (monte)
            coaMonteeLanceeMs = getMSTime();
        else
            coaEchecMontureMs = getMSTime();
    }
    return monte;
}

bool CheckMountStateAction::MountInterne()
{
    // Remove current Shapeshift if need be
    if (botInShapeshiftForm != FORM_TRAVEL &&
        botInShapeshiftForm != FORM_FLIGHT &&
        botInShapeshiftForm != FORM_FLIGHT_EPIC)
    {
        botAI->RemoveShapeshift();
        botAI->RemoveAura("tree of life");
    }

    if (TryPreferredMount(master))
        return true;

    // Get bot mount data
    MountData mountData = CollectMountData(bot);
    int32 masterMountType = GetMountType(master);
    int32 masterSpeed = CalculateMasterMountSpeed(master);

    // Try shapeshift
    if (TryForms(master, masterMountType, masterSpeed))
        return true;

    // Try random mount
    auto spellsIt = mountData.allSpells.find(masterMountType);
    if (spellsIt != mountData.allSpells.end())
    {
        auto& spells = spellsIt->second;
        if (TryRandomMountFiltered(spells, masterSpeed))
            return true;
    }

    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "mount");
    if (!items.empty())
        return UseItemAuto(*items.begin());

    return false;
}

void CheckMountStateAction::Dismount()
{
    if (bot->isMoving())
        bot->StopMoving();

    WorldPacket emptyPacket;
    bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);

    ClearStaleFlightFlags();
}

void CheckMountStateAction::ClearStaleFlightFlags()
{
    if (bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura())
        return;

    if (bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY))
    {
        bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_CAN_FLY);
        if (!bot->IsRooted())
            bot->SendMovementFlagUpdate();
    }
}

void CheckMountStateAction::CompleteDismount(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return;

    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const startZ = bot->GetPositionZ();

    float groundZ = startZ;
    bot->UpdateAllowedPositionZ(x, y, groundZ);

    bot->GetMotionMaster()->MoveFall();
    MovementInfo fallInfo = bot->m_movementInfo;
    // Need to set the start of the fall, otherwise the fall may start from too high of a Z and kill the bot.
    bot->SetFallInformation(0, startZ);
    fallInfo.pos.Relocate(x, y, groundZ);
    bot->HandleFall(fallInfo);
    bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
}

bool CheckMountStateAction::TryForms(Player* master, int32 masterMountType, int32 masterSpeed) const
{
    if (!master)
        return false;

    // If both master and bot are in matching forms or master is mounted with corresponding speed, nothing to do
    else if
        ((masterInShapeshiftForm == FORM_TRAVEL && botInShapeshiftForm == FORM_TRAVEL) ||
        ((masterInShapeshiftForm == FORM_FLIGHT || (masterMountType == 1 && masterSpeed == 149)) && botInShapeshiftForm == FORM_FLIGHT) ||
        ((masterInShapeshiftForm == FORM_FLIGHT_EPIC || (masterMountType == 1 && masterSpeed >= 279)) && botInShapeshiftForm == FORM_FLIGHT_EPIC))
        return true;

    // Check if master is in Travel Form and bot can do the same
    if (botAI->CanCastSpell(SPELL_TRAVEL_FORM, bot, true) &&
        masterInShapeshiftForm == FORM_TRAVEL && botInShapeshiftForm != FORM_TRAVEL)
    {
        botAI->CastSpell(SPELL_TRAVEL_FORM, bot);
        return true;
    }

    // Check if master is in Flight Form or has a flying mount and bot can flight form
    if (botAI->CanCastSpell(SPELL_FLIGHT_FORM, bot, true) &&
        ((masterInShapeshiftForm == FORM_FLIGHT && botInShapeshiftForm != FORM_FLIGHT) ||
        (masterMountType == 1 && masterSpeed == 149)))
    {
        botAI->CastSpell(SPELL_FLIGHT_FORM, bot);

        // Compensate speedbuff
        bot->SetSpeed(MOVE_RUN, 2.5, true);
        return true;
    }

    // Check if master is in Swift Flight Form or has an epic flying mount and bot can swift flight form
    if (botAI->CanCastSpell(SPELL_SWIFT_FLIGHT_FORM, bot, true) &&
        ((masterInShapeshiftForm == FORM_FLIGHT_EPIC && botInShapeshiftForm != FORM_FLIGHT_EPIC) ||
        (masterMountType == 1 && masterSpeed >= 279)))
    {
        botAI->CastSpell(SPELL_SWIFT_FLIGHT_FORM, bot);

        // Compensate speedbuff
        bot->SetSpeed(MOVE_RUN, 3.8, true);
        return true;
    }

    return false;
}

bool CheckMountStateAction::TryPreferredMount(Player* master) const
{
    uint32 botGUID = bot->GetGUID().GetRawValue();

    // Build cache (only once) -- CoA revue B1 : sous std::call_once, seul ecrivain de mountCache et du drapeau.
    std::call_once(sCoaMonturesPrefereesChargees, []
    {
        // Verify preferred mounts table existance in the database
        QueryResult checkTable = PlayerbotsDatabase.Query(
            "SELECT EXISTS(SELECT * FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'playerbots_preferred_mounts')");

        preferredMountTableChecked = true;
        if (checkTable && checkTable->Fetch()[0].Get<uint32>() == 1)
        {
            // Cache all mounts of both types globally, for all entries
            QueryResult result = PlayerbotsDatabase.Query("SELECT guid, spellid, type FROM playerbots_preferred_mounts");

            if (result)
            {
                uint32 totalResults = 0;
                do
                {
                    Field* row = result->Fetch();
                    uint32 guid = row[0].Get<uint32>();
                    uint32 spellId = row[1].Get<uint32>();
                    uint32 mountType = row[2].Get<uint32>();

                    if (mountType == 0)
                        mountCache[guid].groundMounts.push_back(spellId);

                    else if (mountType == 1)
                        mountCache[guid].flightMounts.push_back(spellId);

                    totalResults++;
                } while (result->NextRow());
                LOG_INFO("playerbots", "Preferred mounts initialized | Total records: {}", totalResults);
            }
        }
        else // If the SQL table is missing, log an error
            LOG_DEBUG("playerbots", "Preferred mounts SQL table playerbots_preferred_mounts does not exist!");
    });

    // Lecture seule apres le chargement : find(), jamais operator[] (qui insererait).
    auto const entree = mountCache.find(botGUID);
    if (entree == mountCache.end())
        return false;
    PreferredMountCache const& preferees = entree->second;

    // Pick a random preferred mount from the selection, if available
    uint32 chosenMountId = 0;

    if (GetMountType(master) == 0 && !preferees.groundMounts.empty())
    {
        uint32 index = urand(0, preferees.groundMounts.size() - 1);
        chosenMountId = preferees.groundMounts[index];
    }

    else if (GetMountType(master) == 1 && !preferees.flightMounts.empty())
    {
        uint32 index = urand(0, preferees.flightMounts.size() - 1);
        chosenMountId = preferees.flightMounts[index];
    }

    // No suitable preferred mount found
    if (chosenMountId == 0)
        return false;

    // Check if spell exists
    if (!sSpellMgr->GetSpellInfo(chosenMountId))
    {
        LOG_ERROR("playerbots", "Preferred mount failed: Invalid spell {} | Bot Guid: {}", chosenMountId, botGUID);
        return false;
    }

    // Required here as otherwise bots won't mount in BG's due to them constant moving
    if (bot->isMoving())
        bot->StopMoving();

    // Check if spell can be cast - for now allow all, even if the bot does not have the actual mount
    //if (botAI->CanCastSpell(mountId, botAI->GetBot()))
    //{
    botAI->CastSpell(chosenMountId, botAI->GetBot());
    return true;
    //}

    LOG_DEBUG("playerbots", "Preferred mount failed! | Bot Guid: {}", botGUID);
    return false;
}

bool CheckMountStateAction::TryRandomMountFiltered(std::map<int32, std::vector<uint32>> const& spells, int32 masterSpeed) const
{
    for (auto it = spells.rbegin(); it != spells.rend(); ++it)
    {
        int32 currentSpeed = it->first;

        if ((masterSpeed > 59 && currentSpeed < 99) || (masterSpeed > 149 && currentSpeed < 279))
            continue;

        // Pick a random mount from the candidate group.
        auto const& idsBruts = it->second;
        // CoA (26/10) : un bot CoA ne tire jamais une forme de classe (duree finie) comme monture de voyage.
        std::vector<uint32> idsCoa;
        bool const coa = IsAscensionCustomClassId(bot->getClass());
        if (coa)
            for (uint32 id : idsBruts)
                if (!CoaSortFormeDeClasse(sSpellMgr->GetSpellInfo(id)))
                    idsCoa.push_back(id);
        auto const& ids = coa ? idsCoa : idsBruts;
        if (!ids.empty())
        {
            // Required here as otherwise bots won't mount in BGs due to them constant moving
            if (bot->isMoving())
                bot->StopMoving();

            uint32 index = urand(0, ids.size() - 1);

            if (botAI->CanCastSpell(ids[index], bot))
            {
                // CoA (26/10) : le resultat de CastSpell compte. Ignore, un lancement refuse rendait « monte » et
                // aucun repit ne s'armait. Vanille : inchange.
                if (botAI->CastSpell(ids[index], bot) || !coa)
                    return true;
            }
        }
    }
    return false;
}

float CheckMountStateAction::CalculateDismountDistance() const
{
    // Warrior bots should dismount far enough to charge (because it's important for generating some initial rage),
    // a real player would be riding toward enemy mashing the charge key but the bots won't cast charge while mounted.
    bool isMelee = PlayerbotAI::IsMelee(bot);
    float dismountDistance = isMelee ? sPlayerbotAIConfig.meleeDistance + 2.0f : sPlayerbotAIConfig.spellDistance + 2.0f;
    return bot->getClass() == CLASS_WARRIOR ? std::max(18.0f, dismountDistance) : dismountDistance;
}

float CheckMountStateAction::CalculateMountDistance() const
{
    // Mount distance should be >= 21 regardless of class, because when travelling a distance < 21 it takes longer
    // to cast mount-spell than the time saved from the speed increase. At a distance of 21 both approaches take 3
    // seconds:
    // 21 / 7  =  21 / 14 + 1.5  =  3   (7 = dismounted speed  14 = epic-mount speed  1.5 = mount-spell cast time)
    bool isMelee = PlayerbotAI::IsMelee(bot);
    float baseDistance = isMelee ? sPlayerbotAIConfig.meleeDistance + 10.0f : sPlayerbotAIConfig.spellDistance + 10.0f;
    return std::max(21.0f, baseDistance);
}

bool CheckMountStateAction::ShouldFollowMasterMountState(Player* master, bool noAttackers, bool shouldMount) const
{
    // CoA revue M3 : une forme de classe du maitre n'est pas une monture a suivre (CoaMaitreMonte).
    bool isMasterMounted = CoaMaitreMonte(bot, master) || (masterInShapeshiftForm == FORM_FLIGHT ||
                                                    masterInShapeshiftForm == FORM_FLIGHT_EPIC ||
                                                    masterInShapeshiftForm == FORM_TRAVEL);
    bool isBotMountedOrForm = bot->IsMounted() || botInShapeshiftForm == FORM_FLIGHT ||
                              botInShapeshiftForm == FORM_FLIGHT_EPIC || botInShapeshiftForm == FORM_TRAVEL;
    return isMasterMounted && !isBotMountedOrForm && noAttackers &&
        shouldMount && !bot->IsInCombat() && botAI->GetState() != BOT_STATE_COMBAT;
}

bool CheckMountStateAction::ShouldDismountForMaster(Player* master) const
{
    // CoA revue M3 : un maitre en forme de classe est a pied pour cette action (CoaMaitreMonte).
    bool isMasterMounted = CoaMaitreMonte(bot, master) || (masterInShapeshiftForm == FORM_FLIGHT ||
                                                   masterInShapeshiftForm == FORM_FLIGHT_EPIC ||
                                                   masterInShapeshiftForm == FORM_TRAVEL);
    return !isMasterMounted && bot->IsMounted();
}

static bool BotCanUseFlyingMount(Player const* bot)
{
    if (bot->GetPureSkillValue(SKILL_RIDING) < 225)
        return false;

    AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId());
    if (!area || !area->IsFlyable())
        return false;
    if (area->flags & AREA_FLAG_NO_FLY_ZONE)
        return false;

    uint32 const vmap = GetVirtualMapForMapAndZone(bot->GetMapId(), bot->GetZoneId());
    if (vmap == MAP_NORTHREND && !bot->HasSpell(SPELL_COLD_WEATHER_FLYING))
        return false;

    return true;
}

int32 CheckMountStateAction::CalculateMasterMountSpeed(Player* master) const
{
    // Check riding skill and level requirements
    int32 ridingSkill = bot->GetPureSkillValue(SKILL_RIDING);
    int32 botLevel = bot->GetLevel();

    if (ridingSkill <= 75 && botLevel < static_cast<int32>(sPlayerbotAIConfig.useFastGroundMountAtMinLevel))
        return 59;

    // check if bot has master and if master is self
    bool const noRealMaster = (!master || master == bot);

    if (!noRealMaster && !bot->InBattleground())
    {
        auto auraEffects = master->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
        if (!auraEffects.empty())
        {
            SpellInfo const* masterSpell = auraEffects.front()->GetSpellInfo();
            int32 effect1 = masterSpell->Effects[1].BasePoints;
            int32 effect2 = masterSpell->Effects[2].BasePoints;
            return std::max(effect1, effect2);
        }
        else if (masterInShapeshiftForm == FORM_FLIGHT_EPIC)
            return 279;
        else if (masterInShapeshiftForm == FORM_FLIGHT)
            return 149;
        return 59;  // walk pace
    }

    // No real master OR battleground: pick speed by skill tier.
    if (!bot->InBattleground() && BotCanUseFlyingMount(bot))
        return (ridingSkill >= 300) ? 279 : 149;

    int32 maxGround = (ridingSkill >= 150) ? 99 : 59;
    if (bot->InBattleground() && maxGround > 99)
        maxGround = 99;
    return maxGround;
}

uint32 CheckMountStateAction::GetMountType(Player* master) const
{
    bool const noRealMaster = (!master || master == bot);

    if (noRealMaster)
        return (!bot->InBattleground() && BotCanUseFlyingMount(bot)) ? 1 : 0;

    auto auraEffects = master->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
    if (!auraEffects.empty())
    {
        SpellInfo const* masterSpell = auraEffects.front()->GetSpellInfo();
        return (masterSpell->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                masterSpell->Effects[2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) ? 1 : 0;
    }
    else if (masterInShapeshiftForm == FORM_FLIGHT || masterInShapeshiftForm == FORM_FLIGHT_EPIC)
        return 1;

    return 0;
}
