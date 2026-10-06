/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAction.h"
#include "AreaDefines.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "CoaDirecteur.h"
#include "DBCStores.h"
#include "GossipDef.h"
#include "IVMapMgr.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotTextMgr.h"
#include "QuestDef.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "WaypointMovementGenerator.h"
#include "G3D/Vector2.h"
#include <cmath>
#include <cstdlib>

void TellRpgStatusAction::WhisperStatusChange(Player* owner, std::string const& statusName)
{
    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        RPG_STATUS_CHANGED_KEY, RPG_STATUS_CHANGED_DEFAULT,
        {{"%status", statusName}});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
}

bool TellRpgStatusAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    if (text.empty())
    {
        std::string out = botAI->rpgInfo.ToString();
        bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
        return true;
    }

    Player* master = botAI->GetMaster();
    bool isMaster = master && master->GetGUID() == owner->GetGUID();
    bool isGM = owner->GetSession() && owner->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    if (!isMaster && !isGM)
    {
        std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "rpg_debug_permission_error",
            "Only your master or a GM can change my rpg status.", {});
        bot->Whisper(msg, LANG_UNIVERSAL, owner);
        return false;
    }

    std::string name = text;
    uint32 questId = 0;
    static std::string const doQuestPrefix = "do quest ";
    size_t doQuestPos = text.find(doQuestPrefix);
    if (doQuestPos != std::string::npos)
    {
        name = "do quest";
        std::string idStr = text.substr(doQuestPos + doQuestPrefix.length());
        try
        {
            questId = static_cast<uint32>(std::stoul(idStr));
        }
        catch (std::exception const&)
        {
            questId = 0;
        }
    }

    NewRpgStatus status = NewRpgInfo::StatusFromString(name);
    NewRpgInfo& info = botAI->rpgInfo;

    if (status == RPG_IDLE)
    {
        info.ChangeToIdle();
        WhisperStatusChange(owner, "IDLE");
        return true;
    }
    else if (status == RPG_REST)
    {
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        WhisperStatusChange(owner, "REST");
        return true;
    }
    else if (status == RPG_WANDER_RANDOM)
    {
        info.ChangeToWanderRandom();
        WhisperStatusChange(owner, "WANDER_RANDOM");
        return true;
    }
    else if (status == RPG_WANDER_NPC)
    {
        info.ChangeToWanderNpc();
        WhisperStatusChange(owner, "WANDER_NPC");
        return true;
    }
    else if (status == RPG_GO_GRIND)
    {
        WorldPosition pos = SelectRandomGrindPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_grind_pos_error", "No grind position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoGrind(pos);
        WhisperStatusChange(owner, "GO_GRIND");
        return true;
    }
    else if (status == RPG_GO_CAMP)
    {
        WorldPosition pos = SelectRandomCampPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_camp_pos_error", "No camp position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoCamp(pos);
        WhisperStatusChange(owner, "GO_CAMP");
        return true;
    }
    else if (status == RPG_TRAVEL_FLIGHT)
    {
        uint32 flightMasterEntry = 0;
        WorldPosition flightMasterPos;
        std::vector<uint32> path;
        if (!SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_flight_path_error", "No flight path available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, std::move(path));
        WhisperStatusChange(owner, "TRAVEL_FLIGHT");
        return true;
    }
    else if (status == RPG_OUTDOOR_PVP)
    {
        info.ChangeToOutdoorPvp();
        WhisperStatusChange(owner, "OUTDOOR_PVP");
        return true;
    }
    else if (status == RPG_DO_QUEST)
    {
        if (!questId)
        {
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 qid = bot->GetQuestSlotQuestId(slot);
                if (!qid)
                    continue;
                std::vector<POIInfo> poi;
                if (GetQuestPOIPosAndObjectiveIdx(qid, poi, true))
                {
                    questId = qid;
                    break;
                }
            }
        }
        if (!questId)
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_quest_error", "No quest available; use 'do quest <id>'.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatus questStatus = bot->GetQuestStatus(questId);
        if (!quest || (questStatus != QUEST_STATUS_INCOMPLETE && questStatus != QUEST_STATUS_COMPLETE))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_invalid_quest_error", "Invalid quest %quest_id",
                {{"%quest_id", std::to_string(questId)}});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoQuest(questId, quest);
        WhisperStatusChange(owner, "DO_QUEST " + std::to_string(questId));
        return true;
    }

    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "rpg_unknown_status_error",
        "Unknown rpg status. Options: idle, rest, wander random, wander npc, "
        "go grind, go camp, do quest [<id>], travel flight, outdoor pvp.", {});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
    return false;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
        bot->Whisper("Start to do quest " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("Invalid quest " + text, LANG_UNIVERSAL, owner);
    return false;
}

static POIInfo const* PoiDeMission(CoaDirecteur::Affectation const& aff, std::vector<POIInfo> const& poiInfo);

// Missions (30/09) : la quete de la mission, sur le point d'objectif le plus
// proche du centre du disque (DoIncompleteQuest en tirerait un au hasard, peut-
// etre a 1 500 yd : la laisse le ramenerait, et l'aller-retour recommencerait).
// FAUX, sans rien changer, si la quete n'est plus incomplete (le fil monde la
// degradera en Chasse), si le bot l'a mise de cote (lowPriorityQuest : 5 min
// sans progres), ou si aucun point d'objectif restant n'est a portee de laisse :
// le bot erre alors dans son disque au lieu de relancer une quete sans fin
// (revue M1 : sans point, DoIncompleteQuest repassait a IDLE a chaque tick).
bool NewRpgStatusUpdateAction::DemarrerQueteDeMission(CoaDirecteur::Affectation const& aff)
{
    uint32 const questId = aff.missionParam;
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE ||
        botAI->lowPriorityQuest.count(questId))
        return false;
    std::vector<POIInfo> poiInfo;
    if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo) || poiInfo.empty())
        return false;
    POIInfo const* meilleur = PoiDeMission(aff, poiInfo);
    if (!meilleur)
        return false;
    float const dz = std::max(bot->GetMap()->GetHeight(meilleur->pos.x, meilleur->pos.y, MAX_HEIGHT),
                              bot->GetMap()->GetWaterLevel(meilleur->pos.x, meilleur->pos.y));
    if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
        return false;
    botAI->rpgInfo.ChangeToDoQuest(questId, quest);
    if (auto* data = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
    {
        data->pos = WorldPosition(bot->GetMapId(), meilleur->pos.x, meilleur->pos.y, dz);
        data->objectiveIdx = meilleur->objectiveIdx;
        data->lastReachPOI = 0;
    }
    return true;
}

// Missions : le point d'objectif le plus proche du centre de l'affectation, s'il
// est a portee de laisse (rayon + MISSION_LAISSE_MARGE_YD) ; sinon nul.
static POIInfo const* PoiDeMission(CoaDirecteur::Affectation const& aff, std::vector<POIInfo> const& poiInfo)
{
    POIInfo const* meilleur = nullptr;
    float meilleurD2 = 0.0f;
    for (POIInfo const& p : poiInfo)
    {
        float const ex = p.pos.x - aff.x;
        float const ey = p.pos.y - aff.y;
        float const d2 = ex * ex + ey * ey;
        if (!meilleur || d2 < meilleurD2)
        {
            meilleur = &p;
            meilleurD2 = d2;
        }
    }
    float const bout = aff.rayon + CoaDirecteur::MISSION_LAISSE_MARGE_YD;
    return meilleur && meilleurD2 <= bout * bout ? meilleur : nullptr;
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;

    // CoA -- Directeur, compagnons (27/09, option Directeur.Compagnons) :
    // presence `C-`. Le centre de l'affectation est LE JOUEUR (recale chaque
    // seconde par le fil monde, seul ecrivain de l'affectation) ; le bot se
    // tient dans la bande COMPAGNON_MIN_YD-COMPAGNON_MAX_YD. Place AVANT les
    // deux laisses : elles le meneraient a un point tire au hasard dans un
    // disque de 30 yd, jusque sous les pieds du joueur. En trajet comme en
    // sejour. Hors de la bande : GO_GRIND vers le point d'approche (22 yd, sur
    // la droite joueur -> bot), sauf s'il marche deja vers un point a au plus
    // COMPAGNON_CIBLE_YD + 3 du centre ACTUEL. Aucun point a sol sec : il ne
    // bouge pas (jamais vers le joueur lui-meme, revue M2) et reessaie au
    // tick suivant. Dans la bande en marchant : ARRET (StopMoving : changer
    // rpgInfo n'arrete pas un deplacement lance, revue M2), IDLE, tourne vers
    // le joueur. Hysterese : en marche il s'arrete a COMPAGNON_CIBLE_YD + 3,
    // a l'arret il ne repart qu'au-dela de COMPAGNON_MAX_YD. Il rend `true`
    // SEULEMENT quand il change quelque chose (revue M1) : sinon les actions
    // hors combat moins prioritaires (butin, manger, boire, grind) passent.
    // C'est cette action, proprietaire de rpgInfo, qui l'ecrit (P-134). Sans
    // affectation : un chargement atomique, puis le code d'origine.
    {
        CoaDirecteur::Affectation aff;
        if (CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) && aff.compagnon &&
            aff.carte == bot->GetMapId() && !bot->IsInFlight())
        {
            float const arret = CoaDirecteur::COMPAGNON_CIBLE_YD + 3.0f;
            float const dx = bot->GetPositionX() - aff.x;
            float const dy = bot->GetPositionY() - aff.y;
            float const d2 = dx * dx + dy * dy;
            auto const* allerA = std::get_if<NewRpgInfo::GoGrind>(&info.data);
            bool versLaBande = false;
            if (allerA && allerA->pos.GetMapId() == aff.carte)
            {
                float const gx = allerA->pos.GetPositionX() - aff.x;
                float const gy = allerA->pos.GetPositionY() - aff.y;
                versLaBande = gx * gx + gy * gy <= arret * arret;
            }
            float const seuil = versLaBande ? arret : CoaDirecteur::COMPAGNON_MAX_YD;
            if (d2 > seuil * seuil)
            {
                if (versLaBande)
                    return false;   // il y va : NewRpgGoGrindAction marche
                float x = aff.x, y = aff.y, z = aff.z;
                if (!CoaDirecteur::PointDApproche(bot->GetMap(), aff, bot->GetPositionX(), bot->GetPositionY(), x, y,
                                                  z))
                    return false;   // aucun sol pres du joueur (eau, pente, pont) : il attend
                info.ChangeToGoGrind(WorldPosition(aff.carte, x, y, z));
                LOG_DEBUG("playerbots", "coa directeur approche: guid={} id={} x={:.1f} y={:.1f} z={:.1f}",
                          bot->GetGUID().GetCounter(), aff.id, x, y, z);
                if (bot->getStandState() != UNIT_STAND_STATE_STAND)
                    bot->SetStandState(UNIT_STAND_STATE_STAND);
                CoaDirecteur::Compter(CoaDirecteur::COA_DIR_APPROCHES);
                return true;
            }
            if (allerA)
            {
                // Dans la bande (ou arrive a son point) : il s'arrete ici.
                bot->StopMoving();
                info.ChangeToIdle();
                bot->SetFacingTo(std::atan2(aff.y - bot->GetPositionY(), aff.x - bot->GetPositionX()));
                return true;
            }
            if (info.GetStatus() != RPG_IDLE)
            {
                info.ChangeToIdle();
                return true;
            }
            return false;
        }
    }

    // CoA -- Directeur, teleportation cachee (27/09, option Directeur.TeleportCache) :
    // le fil monde vient de teleporter ce bot dans le disque d'une etape `T-`.
    // Un statut RPG choisi AVANT le saut (GO_GRIND vers un point de farm, quete,
    // camp...) le ramenerait loin du point ; la laisse ne mord pas (le bot est
    // dans le disque). C'est cette action, proprietaire de rpgInfo, qui le remet
    // a IDLE (P-134) ; l'IDLE d'une etape ne tire qu'errance ou repos. Un statut
    // ecrit apres le saut (startT > sautMs : getMSTimeDiff reboucle et rend
    // plus de 2^31 ms) n'est pas touche. Sans affectation : un
    // chargement atomique.
    {
        CoaDirecteur::Affectation aff;
        if (CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) && aff.saut &&
            aff.phase == CoaDirecteur::CoaDirPhase::Trajet && info.GetStatus() != RPG_IDLE &&
            getMSTimeDiff(info.startT, aff.sautMs) < 0x80000000u &&
            CoaDirecteur::DansLeDisque(aff, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY()))
        {
            info.ChangeToIdle();
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_SAUTS_REMIS_IDLE);
            return true;
        }
    }

    // CoA -- Directeur, lot D2b (SPEC-directeur-D2.md § 6.3) : LA LAISSE. En
    // TRAJET, hors du disque et pas en vol, le bot va au disque et nulle part
    // ailleurs : aucun statut neuf n'est tire (le switch ci-dessous, seul
    // appelant de RandomChangeStatus, n'est pas atteint). C'est cette action,
    // proprietaire de rpgInfo, qui l'ecrit : pas de second ecrivain (P-134).
    // Dans le disque, ou en SEJOUR : rien, le parcours est normal.
    {
        CoaDirecteur::Affectation aff;
        if (CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) &&
            aff.phase == CoaDirecteur::CoaDirPhase::Trajet && aff.carte == bot->GetMapId() && !bot->IsInFlight() &&
            !CoaDirecteur::DansLeDisque(aff, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY()))
        {
            auto const* allerA = std::get_if<NewRpgInfo::GoGrind>(&info.data);
            bool const dejaVersLeDisque =
                allerA && CoaDirecteur::DansLeDisque(aff, allerA->pos.GetMapId(), allerA->pos.GetPositionX(),
                                                     allerA->pos.GetPositionY());
            if (!dejaVersLeDisque)
            {
                float x = aff.x, y = aff.y, z = aff.z;
                CoaDirecteur::TirageDisque const t = CoaDirecteur::TirerDansLeDisque(bot->GetMap(), aff, x, y, z);
                info.ChangeToGoGrind(WorldPosition(aff.carte, x, y, z));
                // Une ligne par tirage (la laisse ne retire que si le GO_GRIND
                // ne va pas deja au disque) : borne comme `laisses_rpg`.
                LOG_INFO("playerbots",
                         "coa directeur laisse: guid={} id={} x={:.1f} y={:.1f} z={:.1f} centre_z={:.1f} essais={}"
                         " refus_hauteur={} repli={}",
                         bot->GetGUID().GetCounter(), aff.id, x, y, z, aff.z, t.essais, t.refusesHauteur,
                         t.repli ? 1 : 0);
                // RPG_REST l'a assis (NewRpgBaseAction.cpp, RandomChangeStatus) :
                // assis, il ne marcherait pas.
                if (bot->getStandState() != UNIT_STAND_STATE_STAND)
                    bot->SetStandState(UNIT_STAND_STATE_STAND);
                CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LAISSES_RPG);
                return true;
            }
        }
    }

    // CoA -- Directeur, lot D3 (SPEC-directeur-D3.md § 4.2, decision n° 7) :
    // LAISSE DE SEJOUR d'un bot de PRESENCE (`peupler`). Arrive, il reprend son
    // parcours normal ; mais RPG_GO_CAMP (aubergiste a 50-2 500 yd) peut
    // l'emmener hors de la scene en une minute. Au-dela de PresenceLaisseYd du
    // centre (0 : pas de laisse), meme geste que la laisse de trajet, chez le
    // meme proprietaire de rpgInfo (P-134). Sans affectation : un chargement
    // atomique.
    // Missions (30/09, option Directeur.Missions) : un bot EN MISSION tient une
    // laisse centree sur sa mission, rayon + MISSION_LAISSE_MARGE_YD (et non plus
    // PresenceLaisseYd, 110 yd en service : il allait chercher son camp ou sa
    // quete lointaine, puis revenait) ; un suiveur de BANDE, BANDE_MAX_YD de son
    // chef (le centre, recale par le fil monde), et il le rejoint a
    // BANDE_CIBLE_YD. Option allumee, TOUTE laisse de sejour d'une presence tire
    // a plat (+-6 yd du centre : mesure du 30/09, points a z + 40 en plaine).
    {
        uint32 const laisse = sPlayerbotAIConfig.directeurPresenceLaisseYd;
        bool const missions = sPlayerbotAIConfig.directeurMissions;
        CoaDirecteur::Affectation aff;
        if ((laisse || missions) && CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) && aff.presence &&
            aff.phase == CoaDirecteur::CoaDirPhase::Sejour && aff.carte == bot->GetMapId() && !bot->IsInFlight())
        {
            bool const enMission = missions && aff.mission != CoaDirecteur::CoaDirMission::Aucune;
            bool const bande = enMission && aff.mission == CoaDirecteur::CoaDirMission::Bande;
            float const dx = bot->GetPositionX() - aff.x;
            float const dy = bot->GetPositionY() - aff.y;
            auto const* allerA = std::get_if<NewRpgInfo::GoGrind>(&info.data);
            // Deja en route vers le lieu : un GO_GRIND dans le disque (bande : a
            // BANDE_MAX_YD du chef ACTUEL, il a pu bouger depuis le tirage).
            bool dejaVersLeDisque = false;
            if (allerA && allerA->pos.GetMapId() == aff.carte)
            {
                float const gx = allerA->pos.GetPositionX() - aff.x;
                float const gy = allerA->pos.GetPositionY() - aff.y;
                float const r = bande ? CoaDirecteur::BANDE_MAX_YD : aff.rayon;
                dejaVersLeDisque = gx * gx + gy * gy <= r * r;
            }
            // Jamais plus courte que le rayon : un point tire dans un disque plus
            // large que la laisse la redeclencherait a chaque arrivee.
            float bout = std::max(static_cast<float>(laisse), aff.rayon);
            // Bande (revue M2) : BANDE_LAISSE_YD, au-dela de la cible (BANDE_CIBLE_YD)
            // + le seuil d'arrivee de GO_GRIND (10 yd) : pas de relance en boucle.
            if (enMission)
                bout = bande ? CoaDirecteur::BANDE_LAISSE_YD : aff.rayon + CoaDirecteur::MISSION_LAISSE_MARGE_YD;
            if ((laisse || enMission) && dx * dx + dy * dy > bout * bout && !dejaVersLeDisque)
            {
                float x = aff.x, y = aff.y, z = aff.z;
                CoaDirecteur::TirageDisque t;
                if (!bande || !CoaDirecteur::PointPres(bot->GetMap(), aff, bot->GetPositionX(), bot->GetPositionY(),
                                                       CoaDirecteur::BANDE_CIBLE_YD, x, y, z))
                    t = CoaDirecteur::TirerDansLeDisque(bot->GetMap(), aff, x, y, z, nullptr, 0.0f, missions);
                else
                    t.repli = false;
                info.ChangeToGoGrind(WorldPosition(aff.carte, x, y, z));
                if (bot->getStandState() != UNIT_STAND_STATE_STAND)
                    bot->SetStandState(UNIT_STAND_STATE_STAND);
                // Bande (revue M2) : rejoindre son chef n'est pas une laisse ; ni
                // la ligne `laisse:` ni `laisses_presence` ne la comptent.
                if (bande)
                {
                    LOG_DEBUG("playerbots", "coa directeur bande: guid={} id={} chef={} x={:.1f} y={:.1f}",
                              bot->GetGUID().GetCounter(), aff.id, aff.missionParam, x, y);
                    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_BANDES_REJOINTES);
                    return true;
                }
                LOG_INFO("playerbots",
                         "coa directeur laisse: guid={} id={} x={:.1f} y={:.1f} z={:.1f} centre_z={:.1f} essais={}"
                         " refus_hauteur={} repli={} sejour=1 mission={}",
                         bot->GetGUID().GetCounter(), aff.id, x, y, z, aff.z, t.essais, t.refusesHauteur,
                         t.repli ? 1 : 0, enMission ? static_cast<uint32>(aff.mission) : 0u);
                CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LAISSES_PRESENCE);
                return true;
            }
        }
    }

    // CoA -- Directeur, correctif du 26/09 : TRAJET LEVE. L'affectation qui
    // avait tire ce GO_GRIND est tombee (lever, remplace, echeance, abandon,
    // mort...) : le point ne vaut plus rien, et GO_GRIND n'a pas d'echeance --
    // le bot y marcherait sans fin, puis y sauterait (MoveFarTo, enlisement),
    // sous Orgrimmar le 26/09. Le fil monde a pose l'etat ; c'est ici, chez le
    // proprietaire de rpgInfo, qu'il est applique (P-134). Sans etat pose :
    // un chargement atomique, puis le code d'origine.
    if (auto const* allerA = std::get_if<NewRpgInfo::GoGrind>(&info.data))
    {
        if (CoaDirecteur::TrajetLeveVers(bot->GetGUID().GetCounter(), allerA->pos.GetMapId(),
                                         allerA->pos.GetPositionX(), allerA->pos.GetPositionY(), info.startT))
        {
            info.ChangeToIdle();
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_RPG_REMIS_IDLE);
            return true;
        }
    }

    NewRpgStatus status = info.GetStatus();
    switch (status)
    {
        case RPG_IDLE:
        {
            // CoA -- Directeur, lot D2b suite : ETAPE D'ITINERAIRE atteinte.
            // La laisse ci-dessus n'a pas mordu (le bot est dans le disque) et
            // l'etape ne passe jamais en SEJOUR : le bot attend l'etape
            // suivante SUR PLACE. Pas de camp, de grind lointain, de quete, de
            // vol ni de PvP exterieur, qui l'emmeneraient loin du disque ;
            // errance ou repos seulement. Sans affectation : un chargement
            // atomique, puis le tirage d'origine.
            CoaDirecteur::Affectation aff;
            if (CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) && aff.etape &&
                aff.phase == CoaDirecteur::CoaDirPhase::Trajet && aff.carte == bot->GetMapId())
                return RandomChangeStatus({RPG_WANDER_RANDOM, RPG_REST});
            // Missions (30/09) : EN SEJOUR, un bot en mission ne tire que ce qui le
            // garde au lieu : sa quete (le point d'objectif le plus proche du
            // centre), sinon errance dans le disque (NewRpgWanderRandomAction, a
            // plat) ou repos. Ni camp, ni grind lointain, ni vol, ni PvP exterieur :
            // c'etaient eux, les allers-retours (58 laisses en 20 min, 30/09).
            if (sPlayerbotAIConfig.directeurMissions && CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) &&
                aff.presence && aff.phase == CoaDirecteur::CoaDirPhase::Sejour &&
                aff.mission != CoaDirecteur::CoaDirMission::Aucune && aff.carte == bot->GetMapId())
            {
                if (aff.mission == CoaDirecteur::CoaDirMission::Quete && DemarrerQueteDeMission(aff))
                    return true;
                // Bots actifs (07/10/2026) : un inscrit a un evenement ne s'assied jamais (RPG_REST, tire ici, et
                // le repli de RandomChangeStatus quand aucun statut n'est permis) : il erre, et l'errance le mene
                // au joueur servi (NewRpgWanderRandomAction). « Ils m'ont regarde mourir sans bouger » (06/10).
                if (aff.mission == CoaDirecteur::CoaDirMission::Evenement && sPlayerbotAIConfig.directeurEvenementBots)
                {
                    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
                        bot->SetStandState(UNIT_STAND_STATE_STAND);
                    info.ChangeToWanderRandom();
                    return true;
                }
                return RandomChangeStatus({RPG_WANDER_RANDOM, RPG_REST});
            }
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});
        }

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderRandom();
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderNpc();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // Bots actifs (07/10/2026) : un inscrit a un evenement assis d'AVANT son inscription (ou de l'ancien
            // tirage) se leve tout de suite. Sans affectation : un chargement atomique.
            {
                CoaDirecteur::Affectation aff;
                if (sPlayerbotAIConfig.directeurMissions && sPlayerbotAIConfig.directeurEvenementBots &&
                    CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) && aff.presence &&
                    aff.phase == CoaDirecteur::CoaDirPhase::Sejour &&
                    aff.mission == CoaDirecteur::CoaDirMission::Evenement && aff.carte == bot->GetMapId())
                {
                    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
                        bot->SetStandState(UNIT_STAND_STATE_STAND);
                    info.ChangeToIdle();
                    return true;
                }
            }
            // REST -> IDLE
            if (info.HasStatusPersisted(statusRestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    // CoA -- Directeur, lot D2b (SPEC § 6.3) : en TRAJET, pas de quete en route.
    // Le bot va droit au disque ; il prendra et rendra ses quetes en SEJOUR.
    // Sans affectation : un chargement atomique. `quetes_trajet_evitees` compte
    // les RECHERCHES sautees (une par tick de GO_GRIND en trajet), pas des
    // quetes : le seul usage de la mesure est d'etre > 0 (§ 6.5).
    // Compagnons (27/09) : un compagnon qui rejoint son joueur ne fait pas de
    // detour par un donneur de quetes (non compte : ce n'est pas un trajet).
    CoaDirecteur::Affectation aff;
    bool const affecte = CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff);
    if (affecte && aff.phase == CoaDirecteur::CoaDirPhase::Trajet)
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_QUETES_TRAJET_EVITEES);
    else if (!(affecte && aff.compagnon) && SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    // CoA -- Directeur, lot D2b suite : ETAPE d'itineraire en TRAJET (une
    // etape atteinte, qui erre dans son disque en attendant la suivante), pas
    // de quete, comme NewRpgGoGrindAction. `aff.etape` n'est vrai que si
    // Directeur.Etapes = 1 a la pose : option a 0, une affectation ordinaire
    // garde la recherche de quetes du binaire D2. Sans affectation : un
    // chargement atomique, puis le code d'origine.
    CoaDirecteur::Affectation aff;
    bool const affecte = CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff);
    // Missions (30/09) : EN SEJOUR, un bot en mission erre DANS son disque, vers
    // un point a plat (+-6 yd du centre), pas au hasard autour de lui : les pas
    // de 20-50 yd de MoveRandomNear le menaient au flanc de la colline. Pas de
    // recherche de donneur de quete (il irait au camp). Aucun point : un petit
    // pas, comme d'origine.
    if (affecte && sPlayerbotAIConfig.directeurMissions && aff.presence &&
        aff.phase == CoaDirecteur::CoaDirPhase::Sejour && aff.mission != CoaDirecteur::CoaDirMission::Aucune &&
        aff.carte == bot->GetMapId())
    {
        if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
            return false;
        // Bots actifs (07/10/2026) : un inscrit a un evenement rejoint le joueur servi (a
        // EVENEMENT_RALLIEMENT_CIBLE_YD de lui, cote bot, sol sec a +-6 yd du sien) des qu'il en est a plus de
        // EVENEMENT_RALLIEMENT_YD et que le joueur est dans la laisse ; deja pres de lui, il reste a ses cotes
        // (l'errance dans le disque de 40 yd l'eloignerait, puis il reviendrait : va-et-vient). Joueur absent,
        // mort ou hors laisse, ou pas de point sec pres de lui (eau, pente) : l'errance d'origine.
        if (aff.mission == CoaDirecteur::CoaDirMission::Evenement)
        {
            float jx = 0.0f, jy = 0.0f, jz = 0.0f;
            bool const joueur = CoaDirecteur::JoueurDEvenement(bot, jx, jy, jz);
            if (joueur && bot->GetExactDist2d(jx, jy) <= CoaDirecteur::EVENEMENT_RALLIEMENT_YD)
                return false;   // a ses cotes : il attend (les assaillants de l'evenement le feront frapper)
            if (joueur)
            {
                CoaDirecteur::Affectation autourDuJoueur = aff;
                autourDuJoueur.x = jx;
                autourDuJoueur.y = jy;
                autourDuJoueur.z = jz;
                float px = jx, py = jy, pz = jz;
                if (CoaDirecteur::PointPres(bot->GetMap(), autourDuJoueur, bot->GetPositionX(), bot->GetPositionY(),
                                            CoaDirecteur::EVENEMENT_RALLIEMENT_CIBLE_YD, px, py, pz) &&
                    MoveTo(bot->GetMapId(), px, py, pz, false, false, false, true, MovementPriority::MOVEMENT_NORMAL))
                {
                    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PATROUILLES);
                    return true;
                }
            }
        }
        float x = aff.x, y = aff.y, z = aff.z;
        CoaDirecteur::Affectation disque = aff;
        if (aff.mission == CoaDirecteur::CoaDirMission::Bande)
            disque.rayon = CoaDirecteur::BANDE_MAX_YD;   // autour du chef
        CoaDirecteur::TirageDisque const t =
            CoaDirecteur::TirerDansLeDisque(bot->GetMap(), disque, x, y, z, nullptr, 0.0f, true);
        // Repli (aucun point a plat) : le centre lui-meme, sol connu a la pose,
        // plutot qu'un pas au hasard qui, en pente, le ferait monter (revue m2).
        if (bot->GetExactDist2d(x, y) > 5.0f &&
            MoveTo(bot->GetMapId(), x, y, z, false, false, false, true, MovementPriority::MOVEMENT_NORMAL))
        {
            CoaDirecteur::Compter(CoaDirecteur::COA_DIR_PATROUILLES);
            return true;
        }
        if (!t.repli)
            return false;   // deja au point tire : il attend le tick suivant
        return MoveRandomNear(10.0f);
    }
    if (affecte && aff.etape && aff.phase == CoaDirecteur::CoaDirPhase::Trajet)
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_QUETES_TRAJET_EVITEES);
    else if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // No npc can be found, switch to IDLE
        ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract();
        if (npcOrGo.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);
            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);
    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    info.ChangeToIdle();
    return true;
}

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    if (data.pos != WorldPosition())
    {
        /// @TODO: extract to a new function
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has completed
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        bool completed = true;
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] < quest->RequiredNpcOrGoCount[currentObjective])
                completed = false;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] <
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                completed = false;
        }
        // the current objective is completed, clear and find a new objective later
        if (completed)
        {
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
        }
    }
    if (data.pos == WorldPosition())
    {
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            // can't find a poi pos to go, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        uint32 rndIdx = urand(0, poiInfo.size() - 1);
        // CoA -- Directeur, missions (30/09, revue M1) : un bot en mission QUETE
        // sur CETTE quete, en sejour, prend le point le plus proche du centre de
        // sa mission, a portee de laisse ; aucun : IDLE (il errera dans son
        // disque). Sans affectation : un chargement atomique, puis le tirage.
        {
            CoaDirecteur::Affectation aff;
            if (sPlayerbotAIConfig.directeurMissions && CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) &&
                aff.presence && aff.phase == CoaDirecteur::CoaDirPhase::Sejour &&
                aff.mission == CoaDirecteur::CoaDirMission::Quete && aff.missionParam == questId &&
                aff.carte == bot->GetMapId())
            {
                POIInfo const* p = PoiDeMission(aff, poiInfo);
                if (!p)
                {
                    botAI->rpgInfo.ChangeToIdle();
                    return true;
                }
                rndIdx = static_cast<uint32>(p - poiInfo.data());
            }
        }
        G3D::Vector2 nearestPoi = poiInfo[rndIdx].pos;
        int32 objectiveIdx = poiInfo[rndIdx].objectiveIdx;

        float dx = nearestPoi.x, dy = nearestPoi.y;

        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = objectiveIdx;
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        // Long-range sampler couldn't land a candidate — nudge the
        // bot a short distance so the next tick retries from a
        // different position instead of sitting idle.
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective
    // kill mobs and looting quest should be done automatically by grind strategy

    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        bool hasProgression = false;
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has progression
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] != 0 && quest->RequiredNpcOrGoCount[currentObjective])
                hasProgression = true;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] != 0 &&
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                hasProgression = true;
        }
        if (!hasProgression)
        {
            // we has reach the poi for more than 5 mins but no progession
            // may not be able to complete this quest, marked as abandoned
            /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
            botAI->lowPriorityQuest.insert(questId);
            botAI->rpgStatistic.questAbandoned++;
            LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // clear and select another poi later
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        return true;
    }

    // At the POI: keep the bot actively placed but avoid large
    // random 20yd hops that look like pacing back and forth. A small
    // ~8yd wander reads as the bot looking around while grind/loot
    // strategies do their work.
    return MoveRandomNear(8.0f);
}

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    Quest const* quest = data.quest;

    // CoA -- Directeur, missions (30/09, revue 2, m2) : un bot EN MISSION, en
    // sejour de presence, ne part pas rendre sa quete au donneur (jusqu'a
    // 1 500 yd : une laisse de plus, puis l'aller-retour). Il reste au lieu
    // (le fil monde degrade la mission en Chasse) ; il la rendra a la fin de
    // sa presence, par son RPG ordinaire. Sans affectation : un chargement
    // atomique, puis le code d'origine.
    {
        CoaDirecteur::Affectation aff;
        if (sPlayerbotAIConfig.directeurMissions && CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) &&
            aff.presence && aff.phase == CoaDirecteur::CoaDirPhase::Sejour &&
            aff.mission != CoaDirecteur::CoaDirMission::Aucune)
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
    }

    if (data.objectiveIdx != -1)
    {
        // if quest is completed, back to poi with -1 idx to reward
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->rpgStatistic.questCompleted++;
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            // can't find a poi pos to reward, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }
        assert(poiInfo.size() > 0);
        // now we get the place to get rewarded
        float dx = poiInfo[0].pos.x, dy = poiInfo[0].pos.y;
        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
        return false;

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    // Now we are near the qoi of reward
    // the quest should be rewarded by SearchQuestGiverAndAcceptOrReward
    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        // e.g. Can not reward quest to gameobjects
        /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
        botAI->lowPriorityQuest.insert(questId);
        botAI->rpgStatistic.questAbandoned++;
        LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    return false;
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (bot->IsInFlight())
    {
        data.inFlight = true;
        ContinueCrossMapTaxi();
        return false;
    }

    if (bot->GetDistance(data.flightMasterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(data.flightMasterPos);

    Creature* flightMaster = bot->FindNearestCreature(data.flightMasterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
    {
        info.ChangeToIdle();
        return true;
    }
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
        return MoveFarTo(flightMaster);

    std::vector<uint32> nodes = data.path;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes.empty() ? 0 : nodes.front(), nodes.empty() ? 0 : nodes.back());
        info.ChangeToIdle();
        return true;
    }
    return true;
}

void NewRpgTravelFlightAction::ContinueCrossMapTaxi()
{
    if (bot->IsBeingTeleported())
        return;

    if (!bot->movespline->Finalized())
        return;

    MotionMaster* mm = bot->GetMotionMaster();
    if (!mm || mm->GetCurrentMovementGeneratorType() != FLIGHT_MOTION_TYPE)
        return;

    // Check if we are at our destination.
    uint32 nextDest = bot->m_taxi.GetTaxiDestination();
    if (!nextDest)
        return;

    // Confirm next node needs different map.
    TaxiNodesEntry const* nextNode = sTaxiNodesStore.LookupEntry(nextDest);
    if (!nextNode || nextNode->map_id == bot->GetMapId())
        return;

    FlightPathMovementGenerator* flight = dynamic_cast<FlightPathMovementGenerator*>(mm->top());
    if (!flight)
        return;

    LOG_DEBUG("playerbots", "[New RPG] {} continuing taxi across map boundary (next node {} on map {})",
              bot->GetName(), nextDest, nextNode->map_id);

    flight->SetCurrentNodeAfterTeleport();

    if (flight->HasArrived())
        return;

    TaxiPathNodeEntry const* node = flight->GetPath()[flight->GetCurrentNode()];
    flight->SkipCurrentNode();

    bot->TeleportTo(nextNode->map_id, node->x, node->y, node->z, bot->GetOrientation(), TELE_TO_NOT_LEAVE_TAXI);
}
