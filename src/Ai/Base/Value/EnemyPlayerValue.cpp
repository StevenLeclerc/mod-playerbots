/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EnemyPlayerValue.h"
#include "CombatManager.h"
#include "GameTime.h"
#include "MercenaryRewards.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Vehicle.h"

namespace
{
// --- PvP mercenaire (CoA) ---------------------------------------------------
// << Ce bot est-il un mercenaire ? >>, calcule UNE FOIS PAR BOT ET PAR TICK,
// et non une fois par candidat a portee.
//
// POURQUOI CE MEMO EXISTE. MercenaryRewards::EstMercenaire ne depend que du
// bot, mais AcceptUnit le reevaluait pour CHAQUE candidat :
// NearestUnitsValue::Calculate (NearestUnitsValue.cpp:16-20) appelle AcceptUnit
// en rafale sur toute la liste rendue par FindUnits. Dans une zone peuplee,
// cela multipliait par le nombre de candidats un appel a
// RandomPlayerbotMgr::IsRandomBot (RandomPlayerbotMgr.cpp:2206-2213), qui lit
// `currentBots` -- un std::unordered_set SANS AUCUN VERROU
// (RandomPlayerbotMgr.h:252) que le fil du monde mute en permanence : insert a
// RandomPlayerbotMgr.cpp:756 et :2277, erase a :1355, :1436 et :2748. Un
// `contains()` concurrent d'un rehash n'est pas une lecture perimee, c'est un
// comportement indefini -- la forme exacte de P-050.
//
// CE QUE CE MEMO CORRIGE, ET CE QU'IL NE CORRIGE PAS. Le defaut de fond est
// dans RandomPlayerbotMgr, qui n'appartient pas a ce lot : la table restera
// sans verrou apres ce correctif. Le memo divise l'exposition par le nombre de
// candidats evalues, ce qui est tout ce qu'on peut faire ici sans toucher un
// fichier voisin. Il ne rend PAS l'appel sur.
//
// LA CLE EST (GUID DU BOT, GameTime::GetGameTimeMS()). GameMSTime n'est ecrit
// qu'une fois par tick du monde, dans GameTime::UpdateGameTimers
// (src/server/game/Time/GameTime.cpp:62-68) : il est donc CONSTANT pendant tout
// un MapUpdate. Le memo vaut exactement << une fois par bot, par tick, par
// fil >>, et il se perime de lui-meme au tick suivant -- aucune invalidation a
// tenir quand un bot quitte le camp (deconnexion, `.bot reload`). Au pire, son
// camp est vu avec un tick de retard, ce qui ne se voit pas en jeu.
//
// thread_local ET NON static : trois fils de carte evaluent des bots differents
// en meme temps. Une cellule partagee redeviendrait un etat global a
// verrouiller, c'est-a-dire exactement ce qu'on cherche a ne pas ajouter sur ce
// chemin.
bool BotEstMercenaireMemo(Player* bot)
{
    if (!bot || !sPlayerbotAIConfig.wildPvpEnabled)
        return false;

    thread_local uint64 memoGuid = 0;
    thread_local uint64 memoTickMs = 0;
    thread_local bool memoValeur = false;

    uint64 const guid = bot->GetGUID().GetRawValue();
    // uint64 et non uint32 : tronquer GetGameTimeMS a 32 bits ferait revenir la
    // meme cle toutes les ~49,7 jours d'uptime, donc un faux succes de cache.
    // C'est improbable, mais gratuit a rendre impossible.
    uint64 const tickMs = static_cast<uint64>(GameTime::GetGameTimeMS().count());

    // Le GUID d'un joueur en jeu n'est jamais 0 : la cellule vierge (0, 0) ne
    // peut donc pas etre prise pour un resultat valide au premier appel du fil.
    if (guid == memoGuid && tickMs == memoTickMs)
        return memoValeur;

    memoGuid = guid;
    memoTickMs = tickMs;
    memoValeur = MercenaryRewards::EstMercenaire(bot);
    return memoValeur;
}

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
//
// CE QU'IL GARANTIT, ET CE QU'IL NE GARANTIT PAS. Il compte des engagements
// DEJA VISIBLES sur la cible ; il ne connait pas les intentions. Si plusieurs
// mercenaires evaluent la meme cible avant qu'aucun n'ait bouge, tous lisent le
// meme compte et tous engagent. Le plafond mord donc au ralliement, pas a la
// toute premiere meute -- le dire franchement vaut mieux que de laisser croire
// a une garantie qu'il n'a pas. La corriger vraiment demanderait une table de
// reservations de cible, donc un conteneur global et un verrou de plus sur le
// chemin le plus chaud du serveur : ce n'est pas un prix qu'on paie pour ca.
//
// On lit donc DEUX sources plutot qu'une, pour retrecir la fenetre autant que
// cela se peut sans nouvel etat : les references de combat PvP de la cible, qui
// ne naissent qu'au premier coup PORTE, et sa liste d'assaillants
// (Unit::getAttackers, Unit.h:902), peuplee des AttackStart -- donc des le
// degainage, avant tout degat. Un mercenaire qui a choisi la cible au tick
// precedent est ainsi vu par le suivant, ce que les seules references de combat
// manquaient. La deduplication se fait par GUID dans la table des references,
// qui est un unordered_map indexe par ObjectGuid (CombatManager.h:132).
//
// CE QUE CETTE FONCTION COUTE, ET POURQUOI ELLE N'EST PAS MEMORISEE. Les deux
// EstMercenaire ci-dessous portent sur les AUTRES joueurs, pas sur le bot : les
// passer par BotEstMercenaireMemo, qui n'a qu'une cellule, la ferait evincer a
// chaque tour de boucle et couterait plus qu'il ne rapporte. Ils heritent donc
// de la lecture sans verrou de `currentBots` decrite dans l'en-tete du memo.
// Ce qui borne la casse, et c'est mesure par lecture du code et non suppose :
// les deux boucles sortent des que `engaged` atteint `cap` (1 en service), et
// elles ne parcourent que les assaillants DEJA visibles sur la cible -- zero
// pour un joueur qui n'est pas en train de se battre, c'est-a-dire le cas
// commun. Le cout garanti par candidat, lui, etait le EstMercenaire(bot) de
// AcceptUnit : c'est celui-la que le memo supprime.
bool MercenaryAttackSlotFree(Player* bot, Player* enemy)
{
    uint32 const cap = sPlayerbotAIConfig.wildPvpMaxAttackersPerTarget;
    if (!cap)
        return true;

    auto const& pvpRefs = enemy->GetCombatManager().GetPvPCombatRefs();

    uint32 engaged = 0;
    for (auto const& [guid, combatRef] : pvpRefs)
    {
        Unit* other = combatRef->GetOther(enemy);
        if (!other || other == bot || !other->IsPlayer())
            continue;

        // EstMercenaire exige IsRandomBot EN PLUS du hachage de GUID, et c'est
        // la correction : GET_PLAYERBOT_AI seul etait vrai pour l'alt qu'un
        // joueur ajoute par `.bot add`. Avec MaxAttackersPerTarget = 1 (la
        // valeur en service), un tel alt saturait a lui seul l'unique creneau
        // et faisait renoncer les vrais mercenaires, alors qu'il n'appartient
        // pas au camp au sens ou MercenaryRewards::ComputeReward l'entend. Ce
        // fichier et celui-la definissaient le meme camp de deux facons.
        if (!MercenaryRewards::EstMercenaire(other->ToPlayer()))
            continue;

        if (++engaged >= cap)
            return false;
    }

    for (Unit* attacker : enemy->getAttackers())
    {
        if (!attacker || attacker == bot || !attacker->IsPlayer())
            continue;

        // Deja compte au-dessus : meme assaillant, vu par son autre source.
        if (pvpRefs.find(attacker->GetGUID()) != pvpRefs.end())
            continue;

        if (!MercenaryRewards::EstMercenaire(attacker->ToPlayer()))
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
    //
    // EstMercenaire et non IsMercenary : le camp exige aussi IsRandomBot. Sans
    // lui, un alt ou un selfbot qui se trouve dans une zone FFA du coeur (la
    // fosse de Gurubashi) et dont le GUID tombe dans le seau se mettrait a
    // convoiter sa propre faction, ce que son proprietaire n'a pas choisi.
    //
    // CALCULE UNE SEULE FOIS, ET MEME PAS A CHAQUE CANDIDAT. EstMercenaire
    // coute plus cher que l'ancien IsMercenary seul (IsRandomBot fait une
    // recherche dans le cache de personnages puis lit un conteneur global sans
    // verrou), et AcceptUnit est appele pour chaque candidat a portee. Le memo
    // ci-dessus le ramene a un appel par bot et par tick ; voir son en-tete
    // pour la raison, qui est un risque de P-050 et pas seulement un cout.
    // La condition `enemy` reste ici : elle porte sur le CANDIDAT, pas sur le
    // bot, et n'a donc rien a faire dans un memo indexe par le bot.
    bool const botEstMercenaire = enemy && BotEstMercenaireMemo(bot);

    bool mercenaire = botEstMercenaire &&
                      bot->IsFFAPvP() && enemy->IsFFAPvP() &&
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
    if (botEstMercenaire &&
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
