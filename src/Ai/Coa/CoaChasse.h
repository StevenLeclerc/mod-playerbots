/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COACHASSE_H
#define _PLAYERBOT_COACHASSE_H

#include "AttackersValue.h"
#include "CoaPostes.h"
#include "Creature.h"
#include "GridTerrainData.h"
#include "Map.h"
#include "MercenaryRewards.h"
#include "NewRpgBaseAction.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Strategy.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "Unit.h"

#include <algorithm>
#include <utility>
#include <vector>

/*
 * LA CHASSE DU MERCENAIRE -- lot 2 de docs/CONCEPTION-mercenaire-chasseur.md.
 *
 * CE QUE CE FICHIER REPARE. Un mercenaire n'a ni « grind » ni « new rpg »
 * (AiFactory.cpp:714-723) : son seul organe de deplacement autonome est
 * « move random » (1,5), et « move random » ne se deplace pas. MovementActions.cpp:2728
 * calcule bien une distance de 15 a 35 yd, mais :2737-2738 tirent urand(0, distance)
 * SEPAREMENT sur x et sur y, et :2736 tire l'angle dans [0, pi) : ce n'est pas une
 * marche aleatoire, c'est une derive vers +Y a pas quasi nul. Mesure du dossier :
 * trois mercenaires retrouves a 0,01 yd de leur position plusieurs heures plus tard.
 *
 * POURQUOI UN FICHIER SANS .cpp. Tout tient dans cet en-tete, inclus par le seul
 * CoaAiObjectContext.cpp qui enregistre l'action. Deux raisons, dans cet ordre :
 *   - un .cpp NEUF n'est pas dans /opt/coa/build-main/compile_commands.json tant
 *     que cmake n'a pas ete rejoue : outils/verifier-syntaxe.py ne saurait pas le
 *     verifier, et ce mandat interdit de compiler. Inclus ici, le code est
 *     verifie par la vraie ligne de compilation de CoaAiObjectContext.cpp ;
 *   - les sources du module sont ramassees par un file(GLOB) au moment de la
 *     CONFIGURATION (src/cmake/macros/AutoCollect.cmake:28, appele depuis
 *     modules/CMakeLists.txt:205). Un .cpp ajoute sans reconfigurer n'est pas
 *     compile, et l'erreur n'apparait qu'a l'edition de liens.
 *
 * LA MACHINE A ETATS (section 3.3 du dossier), six etats :
 *
 *   REPLI  le bot est en lieu sans PvP -- il ne peut ni frapper ni etre frappe.
 *          Il sort, vers le point chassable LE PLUS PROCHE.
 *   CHOIX  il tire un poste de guet parmi les hubs de son palier, filtres par
 *          CoaLieuSansPvp ; a defaut une cellule de grind, filtree de meme.
 *   ROUTE  MoveFarTo vers CE poste, la MEME destination a chaque tick.
 *   GUET   il tient le poste et attend que les proies viennent a lui.
 *   TRAQUE (lot 3) une proie est dans « nearest enemy players » (75 yd) mais
 *          hors de portee d'engagement : il se rapproche jusqu'a 60 yd, ou
 *          jusqu'a 20 yd s'il a moins de PV qu'elle.
 *   RETOUR (lot 3) la traque s'est terminee -- en engagement ou en echec : il
 *          rentre au poste, sauf si ses PV sont sous le seuil de decrochage,
 *          auquel cas il en tire un autre.
 *
 * ENGAGEMENT n'est pas un etat EXECUTABLE : c'est le trou entre deux passages de
 * cette action. Des que « enemy player target » est garni, isUseful rend false
 * et « attack enemy player » (55) prend le tick ; la chasse ne reprend qu'a la
 * sortie du combat, et elle reprend en RETOUR.
 *
 * La riposte PvE ne fait pas partie de cette machine ; elle est portee par
 * « dps assist » / « tank assist » (50,0), qui gagnent le tick d'eux-memes. Ce
 * que le lot 3 lui ajoute est son CONTRAIRE : CoaDecrochageAction (50,5), qui
 * rompt un combat PvE perdu ou interdit. Voir son en-tete, tout en bas.
 *
 * LES TROIS INVARIANTS, et ou ils sont tenus :
 *   1. l'action rend FALSE des qu'elle n'a rien a faire, et TOUJOURS en combat
 *      -- CoaChasseAction::isUseful et le premier test de Execute. TRAQUE ne
 *      fait pas exception : elle rend false des qu'une proie est a portee, pour
 *      que « attack enemy player » (55) et « coa stealth » (56) prennent le tick ;
 *   2. la selection de proie emploie le meme predicat que l'attaque --
 *      CoaChasseAction::ChoisirProie. Les deux moities sont DEJA dans la valeur
 *      « nearest enemy players » : NearestEnemyPlayersValue::AcceptUnit commence
 *      par « if (!PossibleTargetsValue::AcceptUnit(unit)) return false; »
 *      (EnemyPlayerValue.cpp:169-172, commentaire amont « Apply parent's
 *      filtering first (includes level difference checks) »), et
 *      PossibleTargetsValue::AcceptUnit EST AttackersValue::IsPossibleTarget
 *      suivi du filtre de niveau FNV-1a (PossibleTargetsValue.cpp:41-137).
 *
 *      RECTIFICATION. Une version precedente de ce bloc affirmait le contraire
 *      -- que NearestEnemyPlayersValue « redefinit » AcceptUnit et que le filtre
 *      de niveau ne s'y applique donc pas -- et faisait, sur cette foi, lire la
 *      valeur « possible targets » pour intersecter. C'etait faux deux fois :
 *      le filtre y est deja, et la portee de « possible targets » est
 *      sightDistance = 100 yd (PossibleTargetsValue.h:19) contre grindDistance
 *      = 75 yd pour « nearest enemy players » (EnemyPlayerValue.h:20), donc
 *      l'intersection ne pouvait rien retirer. Elle coutait, elle, un balayage
 *      de grille de 100 yd plus un IsWithinLOSInMap par unite hostile a chaque
 *      lecture (checkInterval = 1, NearestUnitsValue.h:20-22 : CalculatedValue
 *      recalcule a chaque Get). Elle est SUPPRIMEE ;
 *   3. toute destination passe par CoaLieuSansPvp -- CoaChasseAction::EstChassable,
 *      applique au tirage ET au repli SelectRandomGrindPos. Le repli par
 *      teleportation de MoveFarTo (NewRpgBaseAction.cpp:104-121) vise cette meme
 *      destination deja validee, il n'en invente pas d'autre. TRAQUE n'invente
 *      aucune destination, mais elle en RETIENT une : la position de la proie.
 *      Elle est donc testee par CoaLieuSansPvp comme les autres, dans
 *      ChoisirProie et a chaque pas de Traquer().
 *
 *      RECTIFICATION, ici aussi. Ce bloc affirmait qu'« une proie n'est dans
 *      nearest enemy players que si le bot peut la frapper la ou elle est ».
 *      C'est faux : le seul test de lieu de NearestEnemyPlayersValue::AcceptUnit
 *      est « !IsPvpProhibited(enemy->GetZoneId(), enemy->GetAreaId()) »
 *      (EnemyPlayerValue.cpp:223), c'est-a-dire les LISTES DE CONF, et
 *      AttackersValue::IsPossibleTarget emploie exactement le meme predicat
 *      (AttackersValue.cpp:175-177). Aucun des deux ne lit le drapeau
 *      sanctuaire du DBC, alors que le coeur, lui, refuse le coup dessus :
 *      Unit::_IsValidAttackTarget rend false des que « target->IsInSanctuary()
 *      || IsInSanctuary() » (Unit.cpp:11341). Le DBC marque 228 aires 0x800
 *      quand la conf en liste 21 : sans ce test, la traque conduisait le bot
 *      jusqu'a des proies qu'aucun coup ne pouvait toucher.
 */

/*
 * Les deux compteurs de ce lot, definis dans CoaAiObjectContext.cpp a cote des
 * autres compteurs UsageKind. Ils ne peuvent pas etre appeles directement d'ici :
 * l'enum USAGE_* et RecordUsage vivent dans l'espace de noms ANONYME de ce
 * fichier-la (CoaAiObjectContext.cpp:38, :953-963, :1072), ouvert bien apres
 * l'inclusion de cet en-tete. Ces deux fonctions sont le pont, et rien d'autre.
 */
void CoaChasseComptePostePris();
void CoaChasseComptePosteAtteint();
// Lot 4 : monte quand le poste retenu vient du CATALOGUE, en plus de « poste
// pris ». Sans lui, rien ne distingue dans CoaBots.log une chasse qui tire dans
// la table d'une chasse repliee sur GetTravelHubs.
void CoaChasseComptePosteCatalogue();
// Et le compteur de la CLASSE du poste tire -- « poste auberge », « poste
// volerie », « poste cimetiere », « poste route » -- qui monte en meme temps que
// « poste catalogue ». L'agrege ne suffisait pas : il montait pareil pour une
// auberge (enrichissement mesure x18) et pour un carrefour (x1), donc une baisse
// de la metrique de preuve du lot -- la part des rencontres a moins de N yd d'un
// poste -- ne pouvait pas etre imputee. `classe` est un CoaSourcePoste
// (Ai/Coa/CoaPostes.h) ; une valeur hors bornes est ignoree.
void CoaChasseComptePosteClasse(uint8 classe);
// LOT 5. << poste peuple >> monte quand le poste finalement retenu se trouve
// dans une cellule ou l'index avait vu au moins une proie attaquable. Lu contre
// << poste pris >>, il donne la part des postes tires qui menaient quelque part
// -- la seule preuve que l'index sert a autre chose qu'a exister.
void CoaChasseComptePostePeuple();
void CoaChasseCompteTraque();
void CoaChasseCompteRetour();
void CoaChasseCompteDecrochage();

/*
 * LE SEUIL DE PV, UN SEUL CORPS POUR LES DEUX ACTIONS.
 *
 * Il sert a TROIS endroits, et c'est ce qui rend ses bords si couteux a se
 * tromper : l'entree en TRAQUE (etat GUET), la transition RETOUR -> CHOIX, et
 * la rupture de combat PvE. La premiere ecriture le dupliquait dans les deux
 * classes « pour ne pas faire heriter deux actions sans rapport d'une base
 * commune » ; le corps a grossi de deux bords, et deux corps jumeaux qui
 * grossissent divergent. Une fonction libre coute la meme chose et ne peut plus
 * diverger.
 *
 * DEUX BORDS, ET LES DEUX FAISAIENT MENTIR LA CONF.
 *
 *   A 0. PlayerbotAIConfig.h:388-394 promet « a 0, ni l'un ni l'autre ne se
 *   declenche sur les PV ». L'ecriture entiere (PV * 100 / PVmax) <= 0 rendait
 *   pourtant VRAI pour tout bot sous 1 % de ses PV max, la division tronquant
 *   vers le bas. La bande est etroite, mais c'est une option documentee qui
 *   ment, et le prix a payer pour ne plus mentir est un test.
 *
 * CE QUE CE REGLAGE ETEINT EN PLUS DU DECROCHAGE, et qui n'etait ecrit nulle
 * part. Il ne garde pas que la rupture de combat : il garde aussi l'ENTREE en
 * traque (etat GUET) et la transition RETOUR -> CHOIX. Monte haut, il eteint
 * donc la traque sans un mot dans les journaux -- a 100, valeur vers laquelle
 * PlayerbotAIConfig.cpp:750-751 ramene toute saisie superieure, plus aucun bot
 * ne traque, tout RETOUR relache son poste, et le compteur « traque » reste a
 * zero. Un exploitant qui pousse DecrochagePct pour eprouver le decrochage
 * mesure alors une traque morte et l'attribue au filtre de niveau ou aux
 * creneaux d'attaquant.
 *
 * ON NE BORNE PAS A 99 A LA LECTURE, et c'est un choix. Ce garde-la avait ete
 * ecrit puis retire : il fait disparaitre le seul point ou le reglage est
 * absurde (« PV <= 100 % », vrai meme a PV pleins) sans rien changer a 99, ou
 * la traque ne part deja plus que d'un bot a PV pleins. Il aurait donne
 * l'apparence d'une borne sans la realite, et fait dire au code autre chose que
 * la conf. La bonne reparation est ecrite, pas silencieuse : elle est ici, et
 * elle est rappelee aux deux sites de garde (etat GUET, etat RETOUR).
 * RESTE A FAIRE, hors des fichiers de ce mandat : la meme phrase dans le bloc
 * AiPlayerbot.WildPvp.DecrochagePct de playerbots.conf.dist et dans le
 * commentaire de PlayerbotAIConfig.h:388-394, qui ne parlent que du decrochage.
 *
 * SANS TRONCATURE. On compare PV * 100 a PVmax * pct en uint64 plutot que de
 * diviser : la division entiere decalait le seuil d'un point vers le bas pour
 * tout bot dont les PV max ne sont pas un multiple de 100.
 */
inline bool CoaPvSousLeSeuil(Player* bot)
{
    uint32 const pct = sPlayerbotAIConfig.wildPvpDecrochagePct;
    if (!pct)
        return false;

    uint32 const maxPv = bot->GetMaxHealth();
    if (!maxPv)
        return false;

    return uint64(bot->GetHealth()) * 100 <= uint64(maxPv) * uint64(pct);
}

enum class CoaEtatChasse : uint8
{
    Repli,
    Choix,
    Route,
    Guet,
    Traque,
    Retour
};

class CoaChasseAction : public NewRpgBaseAction
{
public:
    // Herite de NewRpgBaseAction, dont le constructeur est PUBLIC et ne demande
    // rien de plus (NewRpgBaseAction.h:30-33). C'est ce qui donne acces a
    // MoveFarTo, ForceToWait et SelectRandomGrindPos sans toucher a l'amont.
    CoaChasseAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "coa chasse") {}

    // 2,0 : voir la bande de pertinence, section 3.2 du dossier. Le moteur
    // n'execute QU'UNE action par tick (Engine.cpp:230-237, break au premier
    // succes). A 2,0 la chasse n'affame que « move random » (1,5), qu'elle
    // remplace, et « follow » (1,0), inutile sans maitre ; l'engagement (55),
    // la furtivite (56), la riposte (50), le butin, la nourriture et les buffs
    // gardent la priorite.
    static constexpr float Pertinence = 2.0f;

    // ROUTE -> GUET. 20 yd : le modele est NewRpgAction.cpp, qui accepte a 10 yd
    // pour un PNJ ponctuel ; un poste de guet est une zone, pas un point.
    static constexpr float RayonPoste = 20.0f;
    // Au-dela, le guetteur se recentre. Plus large que RayonPoste pour qu'un bot
    // pose a 19,9 yd ne fasse pas l'aller-retour a chaque tick.
    static constexpr float RayonGuet = 30.0f;

    // Un poste plus proche que ceci n'apprend rien : le bot y est deja.
    // Meme valeur que SelectRandomCampPos (NewRpgBaseAction.cpp:1044).
    static constexpr float DistanceMin = 50.0f;
    // Plafond de voyage. SelectRandomCampPos accepte 2500 yd ; on s'arrete a
    // 1500 pour que la route tienne dans DureeRouteMaxMs meme a pied et meme si
    // le chemin contourne un relief.
    static constexpr float DistanceMax = 1500.0f;

    // Duree d'un guet, tiree entre ces bornes pour que 250 mercenaires ne
    // changent pas de poste tous en meme temps. Modele : statusWanderNpcDuration
    // (NewRpgAction.h:65), 5 min.
    static constexpr uint32 DureeGuetMinMs = 120 * 1000;
    static constexpr uint32 DureeGuetMaxMs = 300 * 1000;

    // Garde-fou de route. MoveFarTo porte deja son propre repli anti-enlisement
    // (5 essais et 90 s, NewRpgBaseAction.cpp:104-121) ; celui-ci ne couvre que
    // le cas ou MoveFarTo rend false sans jamais armer ce compteur.
    static constexpr uint32 DureeRouteMaxMs = 600 * 1000;

    // Immobilisation du guetteur, meme forme que la patrouille de
    // NewRpgOutdoorPvP.cpp:126-133.
    static constexpr uint32 AttenteMinMs = 3000;
    static constexpr uint32 AttenteMaxMs = 6000;

    // Delai avant de rejouer un tirage qui n'a rien donne. Sans lui, un bot
    // enferme dans un sanctuaire sans aucun hub chassable a portee relancerait
    // GetTravelHubs a chaque tick. Meme raison que CoaStealthAction::RepliSecondes
    // (CoaAiObjectContext.cpp:1765-1770).
    static constexpr time_t RepliChoixSecondes = 10;

    /*
     * LES DEUX PORTEES DE LA TRAQUE, LUES ET NON DEDUITES.
     *
     * EnemyPlayerValue::Calculate (EnemyPlayerValue.cpp:315-318) :
     *
     *     uint32 const aggroDistance = controllingVehicle ? 5.0f
     *         : (controllingCannon || bot->GetHealth() > pTarget->GetHealth())
     *             ? maxAggroDistance : 20.0f;
     *     if (!bot->IsWithinDist(pTarget, aggroDistance)) continue;
     *
     * et maxAggroDistance = GetMaxAttackDistance() (:288), qui rend 60,0 hors
     * champ de bataille et seulement hors champ de bataille (:351-353 :
     * « if (!bot->GetBattleground()) return 60.0f; », puis 40 ou 120 en BG).
     * isUseful ecarte deja InBattleground : les deux branches BG ne peuvent pas
     * se presenter ici.
     *
     * LE TEST PORTE SUR LES PV ABSOLUS, pas sur un pourcentage : c'est
     * GetHealth() contre GetHealth(). Un bot de niveau 20 a plein PV « a moins
     * de PV » qu'un niveau 60 blesse, et devra donc s'approcher a 20 yd. C'est
     * le comportement du coeur du module, on le reproduit tel quel plutot que
     * de l'ameliorer en silence : la traque doit s'arreter la ou l'engagement
     * commence, ni avant ni apres.
     */
    static constexpr float PorteeEngagementForte = 60.0f;
    static constexpr float PorteeEngagementFaible = 20.0f;

    // La detection, elle, porte a grindDistance (75 yd par defaut,
    // playerbots.conf:415) : c'est la portee de « nearest enemy players »
    // (NearestEnemyPlayersValue, EnemyPlayerValue.h:20). On relit le reglage
    // plutot que d'ecrire 75, et on s'accorde une marge pour ne pas lacher une
    // proie qui vient de faire un pas de trop.
    static float PorteeDetection() { return sPlayerbotAIConfig.grindDistance * 1.2f; }

    // Une traque qui n'aboutit pas ne dure pas eternellement : la proie peut
    // courir plus vite, monter, ou simplement etre inatteignable (|dz| >= 30 yd
    // fait refuser l'engagement, EnemyPlayerValue.cpp:320-322).
    static constexpr uint32 DureeTraqueMaxMs = 45 * 1000;

    // Delai minimal entre deux recherches de proie. CE N'EST PAS UN CONFORT :
    // « nearest enemy players » est un PossibleTargetsValue construit avec
    // checkInterval = 1 (NearestUnitsValue.h:20-22, aucun appelant ne le
    // renseigne pour ce nom), et CalculatedValue::Get recalcule alors A CHAQUE
    // APPEL sans jamais armer lastCheckTime (Value.h:71-85). Chaque lecture est
    // donc un balayage de grille de 75 yd. Le coeur en fait deja un par seconde
    // (EnemyPlayerValue est, lui, en cache 1 s, EnemyPlayerValue.h:33) ; ce
    // delai borne ce que la chasse y ajoute.
    static constexpr uint32 DelaiScanProieMs = 2000;

    // Et le delai, bien plus long, apres un tirage qui n'a RIEN rendu. Voir
    // RetenirUnPoste : un mercenaire enferme dans une zone entierement
    // sanctuaire est dans ce cas a chaque passage, et le tirage n'est pas bon
    // marche. Il reste borne et non infini : la zone du bot change des que le
    // gestionnaire le redeploie (toutes les 3600 a 18000 s).
    static constexpr time_t RepliEchecSecondes = 120;

    /*
     * LOT 4. Nombre maximal de postes du catalogue testes par EstChassable en un
     * tirage.
     *
     * POURQUOI UNE BORNE ICI ALORS QUE LE TIRAGE DES HUBS N'EN A PAS. La liste
     * des hubs d'un palier est courte, et le tirage la parcourt au plus une
     * fois. Le catalogue, lui, peut rendre plusieurs centaines de postes a
     * portee sur une meme carte (mesure a la generation : jusqu'a 653 postes
     * ouverts au meme niveau). Ce n'est pas le nombre qui coute, c'est
     * EstChassable : Map::GetZoneAndAreaId peut faire creer la grille de terrain
     * du point, donc lire maps/{map}{x}{y}.map sur disque, mutex _gridLock tenu,
     * depuis le fil de carte (Map.cpp:1102-1115, :183-186,
     * Grids/MapGridManager.cpp:5-30).
     *
     * CE QUE LA BORNE COUTE, dit franchement : un tirage peut echouer alors
     * qu'un poste chassable existait plus loin dans la liste. C'est sans
     * consequence, parce que l'echec n'est pas une impasse -- TirerPoste
     * enchaine sur les hubs puis sur les cellules de grind, et le tirage se
     * rejoue de toute facon au prochain passage.
     *
     * HUIT, et pas trois ni cinquante : le catalogue a deja ete filtre hors
     * ligne par le meme predicat, donc un refus ici est un DESACCORD entre la
     * grille 2D de l'outil et le VMAP du serveur, pas le cas courant. Huit
     * laisse passer une poignee de desaccords sans jamais transformer un tirage
     * en balayage.
     *
     * UN SECOND MOTIF DE REFUS EXISTE DEPUIS LA RELECTURE : le plancher
     * DistanceMin, deplace APRES PoserSurLeSol parce qu'il portait sur un Z non
     * verifie. Il ne change pas la borne : la separation des postes est garantie
     * a la generation, et par camp (> 50 yd), donc au plus une poignee de postes
     * peuvent tenir dans le disque de DistanceMin autour du bot -- ils coutent
     * un tirage chacun, une fois, le sans-remise les retirant de la liste.
     */
    static constexpr uint32 MaxTestsCatalogue = 8;
    // La meme borne pour le REPLI, plus large : la liste y est triee par
    // distance, donc chaque refus coute le meilleur candidat restant, et un
    // REPLI qui echoue laisse le bot inerte. Voir le corps de
    // TirerPosteCatalogue.
    static constexpr uint32 MaxTestsCatalogueRepli = 32;

    /*
     * LOT 5. LE FACTEUR DE PRESENCE, borne a 1 + PlafondProies.
     *
     * L'index des proies (CoaIndexProies, Bot/MercenaryRewards.h) rend le nombre
     * de proies ATTAQUABLES PAR CE BOT recensees, il y a moins de 30 s, dans la
     * cellule de 250 yd du poste candidat. On en fait un multiplicateur du poids
     * de tirage du poste, DANS SA CLASSE (CoaNormaliserParClasse le prend avant
     * de normaliser, ce qui laisse la part des classes intacte).
     *
     * POURQUOI 1 + min(proies, 4), ET PAS proies. Trois raisons, dans cet ordre :
     *
     *   - LE PLANCHER A 1. Un poste desert doit rester tirable. L'index est un
     *     instantane de 30 s d'une population qui se deplace : le dossier mesure
     *     25 proies sur un poste a 08h45 et 18 au meme metre une heure plus tard
     *     (section 5, << la persistance dans le temps >>). Un poste vide au
     *     dernier balayage n'est pas un poste vide.
     *
     *   - LE PLAFOND A 4. Sans lui, la cellule la plus peuplee du dernier
     *     balayage ecraserait toutes les autres et ferait converger les
     *     mercenaires du palier au meme endroit -- exactement la curee que
     *     MaxAttackersPerTarget = 1 ne sait PAS empecher a l'ouverture
     *     (EnemyPlayerValue.cpp:115-119, son propre en-tete le dit). Le rapport
     *     5:1 entre un poste plein et un poste vide est du meme ordre que le
     *     rapport 3,4:1 entre un cimetiere et un carrefour : il pese, il
     *     n'ecrase pas.
     *
     *   - LE TOUT RESTE SOUS CoaFacteurPresenceMax = 16 (Ai/Coa/CoaPostes.h),
     *     qui borne la majoration de debordement de la normalisation.
     */
    static constexpr uint32 PlafondProies = 4;

    /*
     * L'INTERRUPTEUR EST LU A CHAQUE APPEL, comme wildPvpChasse : le changer a
     * chaud prend effet sans reconnexion des bots.
     *
     * IL EXIGE LES DEUX OPTIONS, ET PAS SEULEMENT LA SIENNE. L'index est
     * REMPLI depuis MercenaryRewards::RecomputeMedian, que
     * MercenaryRewardsWorldScript::OnUpdate n'appelle plus du tout quand
     * wildPvpEnabled est faux (sortie en une instruction). Couper
     * WildPvp.Enabled en laissant WildPvp.IndexProies a 1 -- ce que rien
     * n'invite a eviter, la conf presentant le second comme un sous-reglage de
     * la chasse -- laissait donc le dernier tampon publie en place pour une
     * duree ILLIMITEE, et la remise en service ponderait le tirage, jusqu'a 5:1,
     * par une carte vieille de plusieurs heures ; les postes ainsi tires etant
     * retenus pour RepliChoixSecondes puis parcourus pendant des minutes,
     * l'effet survivait largement aux 30 s qui separent du balayage suivant.
     * Exiger les deux rend ce regime impossible par construction, au lieu de le
     * rattraper par un vidage de plus.
     *
     * CE QUE << ETEINT >> GARANTIT, ET CE QU'IL NE GARANTIT PAS. Eteint, tous
     * les facteurs valent 1 :
     *   - le tirage du CATALOGUE est alors bit pour bit celui du lot 4. Chaine
     *     de la preuve : `facteurs` reste vide, CoaNormaliserParClasse recoit
     *     nullptr, facteurDe rend 1, et la formule du poids effectif est
     *     identique ;
     *   - le tirage des HUBS, lui, a la meme LOI mais pas la meme SUITE. Le lot
     *     5 y a remplace le Fisher-Yates paresseux du lot 2 par un tirage
     *     pondere sans remise ; a poids tous egaux ce dernier construit bien une
     *     permutation uniforme, mais il ne consomme pas le generateur de la meme
     *     facon. Le bloc des hubs, plus bas dans ce fichier, le dit deja ; c'est
     *     l'affirmation globale qui etait fausse, et c'est celle qu'on lisait en
     *     premier.
     * La consequence pratique compte, parce que le catalogue n'est PAS applique
     * en base aujourd'hui (Updates.EnableDatabases = 0, TirerPosteCatalogue rend
     * WorldPosition() d'emblee) : le chemin NOMINAL est celui des hubs, donc
     * celui qui ne rejoue pas la suite du lot 4. Une campagne a graine figee qui
     * compare lot 4 et lot 5 interrupteur eteint doit comparer des LOIS, pas des
     * suites de postes.
     */
    static bool IndexProiesActif()
    {
        return sPlayerbotAIConfig.wildPvpEnabled && sPlayerbotAIConfig.wildPvpIndexProies;
    }

    /*
     * COMBIEN DE PROIES AUTOUR DE CE POINT, selon l'index.
     *
     * CE QUE CET APPEL COUTE, lu et non suppose : au plus 4 x 8 = 32 sequences
     * de sondage dans un tableau de 4096 cases -- quatre cellules, celles du
     * bloc 2x2 le plus proche du point (voir TailleCellule dans
     * Bot/MercenaryRewards.h), fois huit paliers -- chacune s'arretant a la
     * premiere case vide ou a la cle trouvee, soit quelques chargements
     * atomiques relaxed et un melangeur splitmix64 par cle. Aucun verrou, aucune
     * allocation, aucun acces a la base ni au terrain. C'est un ordre de
     * grandeur moins cher que EstChassable, qui peut faire lire une tuile .map
     * sur disque sous le mutex de grille -- et c'est pourquoi le facteur est
     * calcule dans la PREMIERE passe, celle qui ne touche pas au terrain, pour
     * TOUS les candidats, sans changer le nombre de tests de lieu du lot 4.
     *
     * PIEGE 11 EN FILIGRANE : la carte est passee explicitement et entre dans la
     * cle. Un point d'une autre carte ne peut donc pas emprunter le compte d'une
     * cellule de la carte courante.
     */
    /*
     * CE QU'ON COMPTE : LA POPULATION EXOGENE, ET RIEN D'AUTRE.
     *
     * L'index recense toute proie, donc aussi les mercenaires -- qui sont des
     * cibles legitimes les uns pour les autres (BotsFightBots = 1,
     * EnemyPlayerValue.cpp:202-204). Mais les employer comme POIDS DE TIRAGE
     * serait remettre la sortie de ce lot dans son entree : le tirage envoie k
     * mercenaires dans une cellule, le balayage suivant les y recense, la
     * cellule garde son facteur, et ainsi de suite -- un attracteur
     * auto-entretenu, avec en prime le chasseur qui se compte lui-meme et se
     * voit pousse a redessiner un poste la ou il se tient deja.
     *
     * On retranche donc la part mercenaire. La demonstration complete, avec ce
     * que ce choix coute, est a << L'INDEX NE SE MANGE PAS LUI-MEME >>
     * (Bot/MercenaryRewards.h). Consequence directe ici : le bot ne peut plus
     * peser sur son propre tirage, sans qu'aucun test d'identite soit ecrit.
     *
     * SOUSTRACTION SATURANTE. La part ne peut pas depasser le total -- elle
     * n'est incrementee que lorsque le total l'est -- mais les deux saturent a
     * des niveaux differents dans la case, et un uint32 qui passerait sous zero
     * rendrait ~4 milliards, donc le facteur maximal partout.
     */
    uint32 ProiesAutour(uint32 carte, float x, float y) const
    {
        uint32 mercenaires = 0;
        uint32 const total =
            CoaIndexProies::instance().Proies(carte, x, y, bot->GetLevel(), &mercenaires);

        return total - std::min<uint32>(total, mercenaires);
    }

    uint32 ProiesAutour(WorldLocation const& lieu) const
    {
        if (!IndexProiesActif())
            return 0;

        return ProiesAutour(lieu.GetMapId(), lieu.GetPositionX(), lieu.GetPositionY());
    }

    // Le facteur de tirage qui en decoule. Toujours >= 1 : voir l'en-tete de
    // PlafondProies.
    //
    // L'INTERRUPTEUR EST UN PARAMETRE, ET CE N'EST PAS UN DETAIL DE STYLE. Il
    // etait relu a CHAQUE appel ; sur le chemin des hubs, ou le facteur est
    // calcule par candidat dans une boucle, un rechargement de conf au milieu de
    // cette boucle laissait les premiers hubs avec un facteur tire de l'index et
    // les suivants a 1. Le tirage pondere sans remise qui suit se faisait alors
    // sur une demi-carte, biaisee vers les hubs que GetTravelHubs rend en tete
    // -- toujours les memes. Le prendre en parametre force l'appelant a le lire
    // UNE FOIS par tirage, comme TirerPosteCatalogue le fait deja pour le
    // catalogue, et rend la fenetre inecrivable plutot que fermee a la main.
    uint32 FacteurPresence(bool indexActif, uint32 carte, float x, float y) const
    {
        if (!indexActif)
            return 1;

        return 1 + std::min<uint32>(ProiesAutour(carte, x, y), PlafondProies);
    }

    /*
     * CE QUE COUTE UN TEST DE LIEU, et comment ce cout est borne.
     *
     * EstChassable resout le terrain du point candidat par Map::GetZoneAndAreaId,
     * et cette resolution n'est PAS en lecture seule : Map::GetAreaId appelle
     * GetGridTerrainData, qui appelle EnsureGridCreated (Map.cpp:1102-1115,
     * :183-186), qui appelle MapGridManager::CreateGrid -- lequel prend le mutex
     * _gridLock et execute GridTerrainLoader::LoadTerrain(), c'est-a-dire la
     * lecture du fichier maps/{map}{x}{y}.map (Grids/MapGridManager.cpp:5-30).
     * Un candidat dans une grille encore inexistante coute donc une lecture
     * disque sur le fil de mise a jour de la carte, mutex tenu, et cette grille
     * n'est plus jamais liberee avant l'arret de la carte (Map::UnloadGrid n'est
     * appele que depuis Map::UnloadAll).
     *
     * DEUX BORNES, ET PAS DE PLAFOND ARBITRAIRE. Un plafond sur le nombre de
     * candidats testes a ete essaye puis retire : il fait manquer un hub
     * chassable qui existe, et le bot se met alors en attente longue pour rien.
     * Les deux bornes retenues sont exactes :
     *   - SORTIE AU PREMIER CHASSABLE. Le tirage ne construit plus la liste
     *     complete des hubs chassables : il parcourt les candidats dans un ordre
     *     tire au hasard et s'arrete au premier qui passe. Un tirage qui
     *     REUSSIT ne coute donc que le nombre de candidats inertes rencontres
     *     avant lui, jamais le total.
     *   - RepliEchecSecondes. Un tirage qui ECHOUE, lui, a bien parcouru tous
     *     les candidats -- mais il ne se rejoue alors que toutes les 120 s, et
     *     non toutes les 10 s. C'est exactement le cas du mercenaire enferme.
     *
     * La premiere passe (carte + distance) reste arithmetique et ne touche pas
     * au terrain : elle ecarte l'essentiel du catalogue avant tout test de lieu.
     */

    bool isUseful() override
    {
        // L'interrupteur est teste EN PREMIER : eteint, cette action coute un
        // booleen et le moteur passe a la suivante du panier. C'est ce qui
        // permet la montee en charge par paliers demandee par le dossier
        // (section 5, « le cout CPU de MoveFarTo a 250 mercenaires »).
        if (!sPlayerbotAIConfig.wildPvpChasse)
            return false;

        // INVARIANT 1. Teste ICI et non dans Execute : une action jugee inutile
        // est ecartee par le moteur (Engine.cpp:250-254), qui passe alors a
        // l'action suivante. La tester dans Execute la laisserait gagner le tour
        // pour ne rien faire. Meme raison que CoaStealthAction::isUseful
        // (CoaAiObjectContext.cpp:1811-1817).
        //
        // IsInCombat est indispensable : le moteur NON-COMBAT tourne encore
        // pendant le combat (PlayerbotAI.cpp:1719-1730), et c'est par lui que
        // « dps assist » (50) riposte. Une chasse qui bougerait la tirerait le
        // bot hors de sa riposte.
        //
        // IsInFlight : MoveFarTo rend true SANS RIEN FAIRE quand le bot est sur
        // un taxi (NewRpgBaseAction.cpp:49-51). Le tick serait brule pour rien.
        if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->InBattleground())
            return false;

        // Les strategies sont posees a l'initialisation de l'IA et jamais
        // reevaluees (AiFactory.cpp:709-713). Le camp, lui, se relit a chaque
        // appel (PlayerbotAIConfig.cpp:1033). Porter la condition ICI est ce qui
        // la fait suivre un changement de MercenaryPercent, et ce qui empeche un
        // bot recrute par un joueur de partir chasser au lieu de le suivre.
        // IsMercenary commence par verifier wildPvpEnabled (PlayerbotAIConfig.cpp:1035).
        if (botAI->HasGameClientMaster())
            return false;

        // Sous WildPvp.MinLevel le drapeau FFA n'est jamais pose
        // (PlayerbotAI.cpp:423-429) : le bot n'a aucune proie possible, et
        // voyager ne ferait que le deplacer.
        if (bot->GetLevel() < sPlayerbotAIConfig.wildPvpMinLevel)
            return false;

        if (!sPlayerbotAIConfig.IsMercenary(bot->GetGUID().GetRawValue()))
            return false;

        // INVARIANT 1 encore, et INVARIANT 2 par omission : une proie est deja
        // designee, « attack enemy player » (55) et « coa stealth » (56) doivent
        // prendre le tick. Ce lot ne choisit AUCUNE proie de lui-meme -- c'est
        // le lot 3 (etat TRAQUE) qui le fera, avec le predicat de l'attaque.
        //
        // P-035 : le nom enregistre est « enemy player target ». AI_VALUE vaut
        // context->GetValue<type>(name)->Get() : sur un nom inconnu GetValue rend
        // nullptr et la fleche dereference zero. D'ou le garde ci-dessous. La
        // valeur est mise en cache 1 s (EnemyPlayerValue.h:33), la lire ici ne
        // recalcule donc rien que le declencheur « enemy player near » ne
        // recalculerait deja.
        //
        // MAIS ON NE CEDE LE TICK QUE SI L'ENGAGEMENT EST POSSIBLE ICI. Le seul
        // test de lieu de NearestEnemyPlayersValue::AcceptUnit porte sur la
        // zone/aire de l'ENNEMI et n'emploie que IsPvpProhibited, donc les
        // listes de conf -- pas CoaLieuSansPvp (EnemyPlayerValue.cpp, la
        // condition « !sPlayerbotAIConfig.IsPvpProhibited(enemy->GetZoneId(),
        // enemy->GetAreaId()) »). Dans une aire sanctuaire absente des listes
        // (Coldridge Valley 132, Shadowglen 188, Northshire Valley 9... : le DBC
        // marque 228 aires 0x800 quand la conf en liste 21), un ennemi voisin
        // remplit donc « enemy player target » a chaque tick. « attack enemy
        // player » (55) prend alors le tick, echoue (AttackAction.cpp coupe a
        // l'instant de frapper), et PERSONNE ne sort le bot du sanctuaire :
        // c'est exactement l'inertie que ce lot doit guerir, et le test
        // d'inertie d'Execute est en aval de ce verrou, donc jamais atteint.
        //
        // En lieu inerte la chasse prime donc l'engagement, puisque
        // l'engagement y est impossible. Le predicat n'est evalue que quand une
        // proie existe : deux acces indexes dans sAreaTableStore, et seulement
        // alors (PlayerbotAIConfig.cpp, corps de CoaLieuSansPvp).
        Value<Unit*>* convoitee = context->GetValue<Unit*>("enemy player target");
        if (convoitee && convoitee->Get() &&
            !sPlayerbotAIConfig.CoaLieuSansPvp(bot->GetZoneId(), bot->GetAreaId()))
            return false;

        return true;
    }

    bool Execute(Event /*event*/) override
    {
        // INVARIANT 1, redit ici : isUseful et Execute ne sont pas appeles au
        // meme instant du tick, et le combat peut s'ouvrir entre les deux.
        if (bot->IsInCombat())
            return false;

        // On relit le lieu depuis le DBC a chaque tick, et non pvpInfo.IsInNoPvPArea,
        // qui est faux dans les 31 sous-aires de capitale recensees (P-126).
        // GetZoneId()/GetAreaId() sont les valeurs rafraichies paresseusement par
        // UpdatePositionData, pas celles du minuteur de zone.
        bool const inerte = sPlayerbotAIConfig.CoaLieuSansPvp(bot->GetZoneId(), bot->GetAreaId());

        if (inerte)
        {
            if (etat != CoaEtatChasse::Repli)
            {
                etat = CoaEtatChasse::Repli;
                poste = WorldPosition();
                // La proie aussi : en lieu inerte le bot ne peut pas la frapper
                // (CoaLieuSansPvp vient de rendre vrai), et la garder ferait
                // repartir une traque des la sortie, vers un guid qui sera hors
                // de portee depuis longtemps.
                proie = ObjectGuid::Empty;
            }

            // LE POSTE DE REPLI SE PERIME LUI AUSSI. Cette branche etait la
            // seule des quatre a ne rien reverifier : la remise a vide est
            // gardee par la TRANSITION vers Repli ci-dessus, donc un poste
            // devenu invalide alors qu'on est DEJA en Repli n'etait jamais
            // reexamine. Deux facons de le perimer, et le gestionnaire les
            // provoque toutes deux : RandomPlayerbotMgr::RandomTeleport deplace
            // le bot tous les 3600 a 18000 s, souvent vers une autre carte et
            // souvent vers un autre lieu inerte (mesure du dossier : 53 % des
            // mercenaires libres sont en aire sanctuaire).
            //
            // PIEGE 11, le meme qu'en ROUTE et en GUET : les comparaisons de
            // distance de MoveFarTo passent par bot->GetDistance(dest), qui
            // ignore le mapId, et MovementAction::MoveTo n'emploie PAS son
            // parametre mapId (MovementActions.cpp, corps de MoveTo : il
            // deplace en x,y,z sur la carte courante). Un poste d'une autre
            // carte fait donc marcher le bot vers le meme X/Y sur la sienne,
            // sans jamais approcher, jusqu'a ce que le repli d'enlisement le
            // teleporte a l'autre bout du monde.
            if (poste != WorldPosition() && poste.GetMapId() != bot->GetMapId())
                poste = WorldPosition();

            // Et une borne de temps, la meme qu'en ROUTE : un repli qui
            // n'aboutit pas relache sa destination et en tire une autre, au
            // lieu de s'y accrocher a vie. debutRoute est arme par
            // PasserEnRoute juste apres le tirage ci-dessous.
            if (poste != WorldPosition() && GetMSTimeDiffToNow(debutRoute) >= DureeRouteMaxMs)
                poste = WorldPosition();

            if (poste == WorldPosition())
            {
                if (!RetenirUnPoste(true))
                    return false;

                // PasserEnRoute arme debutRoute ; l'etat qu'elle pose est
                // ecrase par le Repli du tick suivant si le bot est encore en
                // lieu inerte, et c'est voulu : s'il en est sorti entre-temps,
                // la sortie de Repli plus bas le remet en Route avec ce meme
                // poste, deja valide par EstChassable.
                PasserEnRoute();
                etat = CoaEtatChasse::Repli;
            }

            // PIEGE 8 : MoveFarTo n'est pas un « aller a », c'est un « avancer
            // d'un pas vers ». Elle doit etre rappelee avec la MEME destination
            // a chaque tick, sans quoi SetMoveFarTo (NewRpgBaseAction.cpp:52-56)
            // reinitialise le compteur d'enlisement et le repli ne part jamais.
            return MoveFarTo(poste);
        }

        // Sorti du lieu inerte. Le poste choisi pour en sortir reste valable.
        if (etat == CoaEtatChasse::Repli)
        {
            if (poste == WorldPosition())
                etat = CoaEtatChasse::Choix;
            else
                PasserEnRoute();
        }

        switch (etat)
        {
            case CoaEtatChasse::Choix:
            {
                if (!RetenirUnPoste(false))
                    return false;

                PasserEnRoute();
                return MoveFarTo(poste);
            }

            case CoaEtatChasse::Route:
            {
                // PIEGE 11 : les comparaisons de distance de MoveFarTo ignorent
                // le mapId (Object.cpp). Un poste reste sur une autre carte
                // ferait marcher le bot vers le meme X/Y sur la sienne.
                if (!PosteEncoreValable())
                {
                    Oublier();
                    return false;
                }

                if (bot->GetExactDist(poste) <= RayonPoste)
                {
                    etat = CoaEtatChasse::Guet;
                    debutGuet = getMSTime();
                    dureeGuetMs = urand(DureeGuetMinMs, DureeGuetMaxMs);
                    CoaChasseComptePosteAtteint();
                    return ForceToWait(urand(AttenteMinMs, AttenteMaxMs));
                }

                if (GetMSTimeDiffToNow(debutRoute) >= DureeRouteMaxMs)
                {
                    Oublier();
                    return false;
                }

                return MoveFarTo(poste);
            }

            case CoaEtatChasse::Guet:
            {
                if (!PosteEncoreValable())
                {
                    Oublier();
                    return false;
                }

                if (GetMSTimeDiffToNow(debutGuet) >= dureeGuetMs)
                {
                    Oublier();
                    return false;
                }

                // GUET -> TRAQUE. La recherche est AVANT le garde d'attente
                // ci-dessous, et c'est voulu : ForceToWait immobilise le bot 3 a
                // 6 s, et un guetteur qui ne regarderait qu'entre deux attentes
                // laisserait passer une proie qui traverse. Elle est bornee par
                // son propre delai (DelaiScanProieMs), pas par celui de
                // l'attente, parce que ce qu'elle coute n'a rien a voir : voir
                // l'en-tete de DelaiScanProieMs.
                // LE SEUIL DE DECROCHAGE GARDE AUSSI L'ENTREE EN TRAQUE, et pas
                // seulement la sortie de combat. Un mercenaire a 20 % de PV qui
                // part chercher une proie a 70 yd lui offre un mort ; en restant
                // au poste il laisse « food » (4,1) et « drink » (4,2) primer la
                // chasse (2,0) et se remet d'aplomb. Une traque DEJA ENGAGEE,
                // elle, n'est pas abandonnee pour autant : le bot y est, la
                // proie est proche, fuir a decouvert serait pire -- et le
                // decrochage PvE, lui, ne juge que le combat contre les
                // creatures.
                //
                // CE GARDE EST LE TROISIEME USAGE DE DecrochagePct -- et le seul
                // que ni la conf ni PlayerbotAIConfig.h n'annoncaient avant le
                // 2026-09-24. Il faut le
                // savoir pour lire une mesure : monte haut, le reglage eteint
                // la traque ici, silencieusement, et le compteur « traque »
                // reste a zero. A 100 -- le plafond de PlayerbotAIConfig.cpp:750
                // -- plus aucune traque ne part, PV pleins compris. Voir
                // l'en-tete de CoaPvSousLeSeuil.
                if (sPlayerbotAIConfig.wildPvpTraque && !PvSousLeSeuil() &&
                    GetMSTimeDiffToNow(dernierScanProie) >= DelaiScanProieMs)
                {
                    dernierScanProie = getMSTime();

                    ObjectGuid const tiree = ChoisirProie();
                    if (!tiree.IsEmpty())
                    {
                        proie = tiree;
                        etat = CoaEtatChasse::Traque;
                        debutTraque = getMSTime();
                        CoaChasseCompteTraque();
                        return Traquer();
                    }
                }

                // LE GARDE QUI MANQUAIT, et sans lequel cet etat viole
                // l'invariant 1. Le modele cite en tete de cet etat,
                // NewRpgOutdoorPvpAction::PatrolCapturePoint, commence par
                // exactement cette ligne (NewRpgOutdoorPvP.cpp:121-123) : c'est
                // elle qui fait rendre FALSE pendant l'attente.
                //
                // Sans elle, ForceToWait -- qui rend true INCONDITIONNELLEMENT
                // (NewRpgBaseAction.cpp, corps de ForceToWait : un Set sur
                // « last movement » puis return true) -- reposait un verrou de
                // 3 a 6 s a CHAQUE tick. Deux degats, et le second est le pire :
                //   (a) l'action gagnait le tick pendant 2 a 5 minutes sans rien
                //       faire, ce que l'invariant 1 interdit en toutes lettres ;
                //   (b) le verrou MOVEMENT_NORMAL n'expirait jamais, et
                //       IsWaitingForLastMove est relu par TOUTES les actions de
                //       deplacement, y compris de pertinence bien SUPERIEURE a
                //       2,0 -- « move to loot » (7,0) par exemple, qui passe par
                //       MoveNear puis MoveTo, lequel rend false sur ce meme
                //       verrou (MovementActions.cpp, corps de MoveTo). Le
                //       guetteur ne ramassait plus rien de tout son guet.
                //
                // Le bot reste immobile -- le verrou court toujours -- mais le
                // tick redescend, et ce qui est au-dessus de 2,0 retrouve son
                // tour. « move random » (1,5) est bloque par le meme verrou :
                // le guet n'en derive donc pas pour autant.
                if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
                    return false;

                float const distance = bot->GetExactDist(poste);

                // Loin du poste : un combat l'en a tire, ou le decor l'a pousse.
                // Au-dela de pathFinderDis (70 yd, NewRpgBaseAction.h:68) c'est
                // une route, pas un recentrage : on repasse par MoveFarTo, qui
                // sait contourner un relief. PasserEnRoute rearme le compteur
                // d'enlisement -- ce retour au poste est un voyage neuf vers une
                // destination inchangee, le cas meme que MoveFarTo ne detecte
                // pas (NewRpgBaseAction.cpp:52-56). Voir RearmerEnlisement.
                if (distance > pathFinderDis)
                {
                    PasserEnRoute();
                    return MoveFarTo(poste);
                }

                // Recentrage court : MoveTo et NON MoveFarTo. Rappeler MoveFarTo
                // sur une destination deja atteinte fait grimper son compteur
                // d'enlisement (NewRpgBaseAction.cpp:98-104), qui finit par
                // teleporter le bot a quelques metres de lui-meme.
                if (distance > RayonGuet)
                    return MoveTo(poste.GetMapId(), poste.GetPositionX(), poste.GetPositionY(),
                                  poste.GetPositionZ(), false, false, false, true);

                // Poste tenu. ForceToWait pose un delai dans « last movement »
                // (NewRpgBaseAction.cpp:285-291), que tout MovementAction relit
                // par IsWaitingForLastMove : sans lui, « move random » (1,5)
                // reprendrait le tick suivant et ferait deriver le guetteur.
                //
                // On n'emploie PAS MoveRandomNear pour patrouiller : son
                // parametre `center` n'est pas nomme dans la definition
                // (NewRpgBaseAction.cpp:240, contre la declaration .h:39), donc
                // ignore -- la patrouille tournerait autour du BOT et deriverait
                // hors du poste (piege 12).
                return ForceToWait(urand(AttenteMinMs, AttenteMaxMs));
            }

            case CoaEtatChasse::Traque:
                return Traquer();

            case CoaEtatChasse::Retour:
            {
                /*
                 * LE SEUIL DE DECROCHAGE, COTE CHASSE (section 3.3 du dossier,
                 * transition « RETOUR -> CHOIX : PV sous le seuil »).
                 *
                 * Rentrer au poste a 20 % de PV, c'est y remourir au premier
                 * passant. On relache le poste : « food » (4,1) et « drink »
                 * (4,2) -- que ce lot rend a la strategie, recopiees de
                 * GrindingStrategy.cpp:12-15 -- priment alors la chasse (2,0) et
                 * le bot se restaure avant de repartir. Le nouveau tirage est de
                 * toute facon retenu RepliChoixSecondes.
                 *
                 * MEME SEUIL QUE LE DECROCHAGE PvE, a dessein : un seul reglage,
                 * une seule notion de « combat perdu ». C'est le TROISIEME usage
                 * de DecrochagePct, et le plus discret : monte haut, il fait
                 * relacher son poste a tout retour, ce qui defait le guet sans
                 * qu'aucun journal ne le dise. Voir l'en-tete de CoaPvSousLeSeuil.
                 */
                if (PvSousLeSeuil())
                {
                    Oublier();
                    return false;
                }

                if (!PosteEncoreValable())
                {
                    Oublier();
                    return false;
                }

                if (bot->GetExactDist(poste) <= RayonPoste)
                {
                    /*
                     * ON NE REARME NI debutGuet NI dureeGuetMs, et c'est le
                     * garde-fou contre un enfermement que la premiere ecriture
                     * de cet etat portait.
                     *
                     * Le cycle GUET -> TRAQUE -> RETOUR -> GUET peut tourner
                     * sans jamais produire d'engagement : il suffit que la proie
                     * soit a plus de 30 yd de denivele, cas ou
                     * EnemyPlayerValue.cpp:320-322 refuse de la designer alors
                     * qu'elle reste, elle, dans « nearest enemy players ». En
                     * rendant une duree de guet neuve a chaque passage, le bot
                     * restait colle a ce poste sterile indefiniment.
                     *
                     * La fenetre du guet court donc depuis l'arrivee INITIALE au
                     * poste (ROUTE -> GUET) et jusqu'a son terme, traques
                     * comprises. Si elle est deja echue, le prochain passage en
                     * GUET rend la main a CHOIX des sa premiere ligne -- ce qui
                     * est exactement la transition « duree de guet ecoulee » du
                     * dossier.
                     */
                    etat = CoaEtatChasse::Guet;
                    CoaChasseCompteRetour();
                    return ForceToWait(urand(AttenteMinMs, AttenteMaxMs));
                }

                // La meme borne de temps qu'en ROUTE, et pour la meme raison :
                // debutRoute est rearme par PasserEnRetour a l'entree de cet
                // etat, donc ce plafond compte bien le retour et non le voyage
                // aller qui l'a precede.
                if (GetMSTimeDiffToNow(debutRoute) >= DureeRouteMaxMs)
                {
                    Oublier();
                    return false;
                }

                // PIEGE 8, ici aussi : MEME destination a chaque tick, et donc
                // aucun rearmement DANS l'etat. Il a lieu une seule fois, a
                // l'entree, dans PasserEnRetour -- sans quoi ce retour heritait
                // du compteur d'enlisement laisse par le voyage aller et
                // finissait en teleportation. Voir RearmerEnlisement.
                return MoveFarTo(poste);
            }

            case CoaEtatChasse::Repli:
            default:
                // Inatteignable : le cas inerte est traite plus haut et rend.
                return false;
        }
    }

private:
    // Tout l'etat vit ICI, dans l'instance de l'action. Il y a une instance par
    // bot : NamedObjectContextList::GetContextObject (NamedObjectContext.h:204-213)
    // est un membre de l'AiObjectContext du bot et met en cache l'objet cree.
    // C'est le patron de CoaStealthAction::prochainEssai (CoaAiObjectContext.cpp:1820).
    //
    // ON NE RANGE AUCUN ETAT DE CHASSE DANS botAI->rpgInfo (section 3.1 du
    // dossier) : MoveFarTo y ecrit deja (NewRpgBaseAction.cpp:52-56, :98-110),
    // et un second ecrivain de la MEME information ferait diverger les deux.
    //
    // NUANCE, ajoutee par la relecture du lot 3 : RearmerEnlisement, elle,
    // ecrit bien dans rpgInfo -- par SetMoveFarTo, et pour les quatre seuls
    // champs d'enlisement. Ce n'est pas un second ecrivain, c'est le MEME
    // etat remis a zero a l'ouverture d'un voyage neuf, ce que MoveFarTo ne
    // sait pas faire quand la destination ne change pas. Voir son en-tete.
    CoaEtatChasse etat = CoaEtatChasse::Choix;
    WorldPosition poste{};
    uint32 debutRoute = 0;
    uint32 debutGuet = 0;
    uint32 dureeGuetMs = 0;
    time_t prochainChoix = 0;

    // Lot 3. La proie est gardee par GUID et JAMAIS par pointeur : c'est le
    // patron de NewRpgOutdoorPvpAction, qui re-resout sa cible a chaque tick
    // depuis son spawnId (NewRpgOutdoorPvP.cpp:36-57). Un Unit* garde d'un tick
    // sur l'autre pend des que la proie se deconnecte ou change de grille.
    ObjectGuid proie;
    uint32 debutTraque = 0;
    uint32 dernierScanProie = 0;

    /*
     * REARMER LE COMPTEUR D'ENLISEMENT DE MoveFarTo, ET POURQUOI IL LE FAUT.
     *
     * MoveFarTo ne remet son etat d'enlisement a zero QUE si la destination
     * change : « if (dest != botAI->rpgInfo.moveFarPos) SetMoveFarTo(dest); »
     * (NewRpgBaseAction.cpp:52-56). Le piege 8 du dossier dit de rappeler
     * MoveFarTo avec la MEME destination a chaque tick, et c'est exact POUR UN
     * MEME VOYAGE ; mais un nouveau voyage vers la MEME destination n'est pas le
     * meme voyage, et c'est exactement ce que le lot 3 a rendu routinier :
     * GUET -> TRAQUE -> RETOUR ramene au poste d'ou l'on vient de partir.
     *
     * CE QUE CA PRODUISAIT. A l'arrivee au poste, rpgInfo.nearestMoveFarDis vaut
     * la distance du dernier progres (~20 yd, l'etat ROUTE basculant des
     * GetExactDist <= RayonPoste) et stuckTs date de cette arrivee. La traque
     * eloigne ensuite le bot de 40 a 200 yd SANS rien ecrire dans rpgInfo --
     * MoveTo(WorldObject*) n'y touche pas. Au RETOUR, chaque passe evalue
     * « disToDest + 5 < nearestMoveFarDis » (NewRpgBaseAction.cpp:98) avec
     * disToDest = 200, 175, 150... face a 20 : toutes fausses, donc
     * ++stuckAttempts a chaque passe. Et le reset par progres exigerait de
     * descendre sous nearestMoveFarDis - 5 (~15 yd) quand l'etat RETOUR sort
     * des RayonPoste = 20 yd : il ne pouvait JAMAIS avoir lieu. Les compteurs
     * s'additionnaient donc d'une traque a l'autre ; a la cinquieme,
     * stuckAttempts >= 5 et GetMSTimeDiffToNow(stuckTs) >= stuckTime (90 s,
     * NewRpgBaseAction.h:76, largement depasse par un guet de 120 a 300 s) : le
     * mercenaire etait TELEPORTE sur son poste (NewRpgBaseAction.cpp:117-119),
     * sous les yeux du joueur qu'il venait de suivre. C'est le piege 10 du
     * dossier -- un TeleportTo NU -- rentre par la porte de derriere, et c'est
     * precisement ce que l'en-tete de Traquer() declare inacceptable.
     *
     * POURQUOI SetMoveFarTo ET PAS TROIS AFFECTATIONS A LA MAIN. C'est l'API
     * prevue pour cela et elle n'ecrit que les quatre champs d'enlisement
     * (NewRpgInfo.cpp:86-92). Elle pose stuckTs = 0, ce qui semble armer
     * d'emblee « GetMSTimeDiffToNow(stuckTs) >= stuckTime » -- mais c'est sans
     * effet : elle pose AUSSI nearestMoveFarDis = FLT_MAX, et la toute premiere
     * evaluation du bloc d'enlisement prend alors la branche de progres
     * (disToDest + 5 < FLT_MAX est vrai pour toute distance finie), qui
     * reecrit stuckTs = getMSTime() et stuckAttempts = 0 avant que le compteur
     * n'ait pu atteindre 5. La branche de teleportation exige les DEUX
     * conditions (NewRpgBaseAction.cpp:104).
     *
     * SANS DANGER POUR LE RESTE. rpgInfo n'a qu'un seul ecrivain sur un
     * mercenaire : il n'a ni « grind » ni « new rpg » (AiFactory.cpp:719-723,
     * branche mercenaire), donc piege 9 sans objet tant que cela reste vrai.
     *
     * CE QUE CE REARMEMENT COUTE, ET CE QUI LE BORNE. Rendre une fenetre neuve a
     * chaque cycle, c'est repousser d'autant le repli d'enlisement de MoveFarTo.
     * Un bot dont le poste est devenu reellement inatteignable -- relief, tuile
     * manquante -- ne serait donc plus teleporte dessus. Ce n'est pas une
     * impasse : les etats ROUTE et RETOUR portent leur propre plafond,
     * DureeRouteMaxMs (600 s), au terme duquel Oublier() rend a CHOIX et un
     * autre poste est tire. La sortie existe, elle est simplement la notre et
     * non celle de l'amont -- et elle ne fait apparaitre aucun bot sous les yeux
     * d'un joueur.
     */
    void RearmerEnlisement()
    {
        botAI->rpgInfo.SetMoveFarTo(poste);
    }

    // Entree en ROUTE, depuis CHOIX, depuis REPLI, et depuis GUET quand le bot a
    // derive au-dela de pathFinderDis. Les trois ouvrent un voyage NEUF vers
    // `poste` : le compteur d'enlisement doit repartir de zero. Voir
    // RearmerEnlisement.
    void PasserEnRoute()
    {
        etat = CoaEtatChasse::Route;
        debutRoute = getMSTime();
        RearmerEnlisement();
    }

    // Entree en RETOUR. Elle rearme debutRoute : sans cela le plafond
    // DureeRouteMaxMs de l'etat RETOUR compterait le temps du voyage ALLER, et
    // un bot parti depuis neuf minutes n'aurait qu'une minute pour rentrer.
    // Et elle rearme le compteur d'enlisement, pour la raison ci-dessus : c'est
    // CE chemin-la qui, repete, finissait en teleportation.
    void PasserEnRetour()
    {
        etat = CoaEtatChasse::Retour;
        proie = ObjectGuid::Empty;
        debutRoute = getMSTime();
        RearmerEnlisement();
    }

    void Oublier()
    {
        etat = CoaEtatChasse::Choix;
        poste = WorldPosition();
        proie = ObjectGuid::Empty;
    }

    // Le seuil de decrochage, en pourcentage de PV. Le corps est CoaPvSousLeSeuil,
    // en tete de ce fichier : la chasse et le decrochage PvE lisent la meme
    // fonction et ne peuvent donc plus diverger. Voir son en-tete pour les deux
    // bords (0 et 100) que la premiere ecriture manquait.
    bool PvSousLeSeuil() const { return CoaPvSousLeSeuil(bot); }

    /*
     * LA TRAQUE. Un seul pas vers la proie, et rien d'autre.
     *
     * CE QU'ELLE NE FAIT PAS, et pourquoi. Elle n'emploie PAS MoveFarTo : cette
     * primitive garde une destination FIXE et, au bout de 5 essais et 90 s sans
     * progres, teleporte le bot dessus (NewRpgBaseAction.cpp, branche
     * stuckAttempts >= 5). Sur une proie qui bouge, cela ferait apparaitre un
     * mercenaire DANS le dos d'un joueur, ce qu'aucun reglage ne rattrape.
     * MovementAction::MoveTo(WorldObject*, distance) (MovementActions.cpp:752-787)
     * est le bon outil : il avance d'au plus spellDistance vers la cible et
     * s'arrete a `distance` d'elle, en recalculant a chaque tick.
     */
    bool Traquer()
    {
        // L'interrupteur peut s'eteindre au milieu d'une traque : il se relit a
        // chaud (playerbots rndbot reload) et l'etat, lui, survit dans
        // l'instance de l'action.
        if (!sPlayerbotAIConfig.wildPvpTraque)
        {
            AbandonnerLaTraque();
            return false;
        }

        if (GetMSTimeDiffToNow(debutTraque) >= DureeTraqueMaxMs)
        {
            AbandonnerLaTraque();
            return false;
        }

        Unit* cible = proie.IsEmpty() ? nullptr : botAI->GetUnit(proie);

        // PIEGE 11 : toutes les distances de ce module ignorent le mapId. La
        // carte se teste donc explicitement, avant toute distance.
        if (!cible || !cible->IsInWorld() || !cible->IsAlive() || !cible->IsPlayer() ||
            cible->GetMapId() != bot->GetMapId())
        {
            AbandonnerLaTraque();
            return false;
        }

        float const distance = bot->GetExactDist(cible);
        if (distance > PorteeDetection())
        {
            AbandonnerLaTraque();
            return false;
        }

        // INVARIANT 2, rejoue a chaque pas. Le filtre de niveau, lui, n'est pas
        // rejoue : sa decision est deterministe et STABLE pendant deux minutes
        // (ATTACK_DECISION_TIME_WINDOW, PossibleTargetsValue.cpp:28 et :115), ce
        // qui est plus long que DureeTraqueMaxMs. La rejouer chaque tick
        // couterait un balayage de grille de 100 yd pour rendre la meme reponse.
        if (!AttackersValue::IsPossibleTarget(cible, bot))
        {
            AbandonnerLaTraque();
            return false;
        }

        // INVARIANT 3, rejoue lui aussi a chaque pas : la proie peut ENTRER dans
        // un sanctuaire pendant la traque, et ni le test ci-dessus ni celui de
        // ChoisirProie ne le verraient -- leur seul test de lieu est
        // IsPvpProhibited, donc les listes de conf (AttackersValue.cpp:175-177),
        // quand le coeur, lui, refuse le coup des que target->IsInSanctuary()
        // (Unit.cpp:11341). Sans cette ligne, un bot suivait un joueur jusqu'a
        // la porte d'une aire sanctuaire absente des listes et s'y arretait,
        // muet, jusqu'a DureeTraqueMaxMs.
        if (sPlayerbotAIConfig.CoaLieuSansPvp(cible->GetZoneId(), cible->GetAreaId()))
        {
            AbandonnerLaTraque();
            return false;
        }

        // INVARIANT 1. A portee d'engagement, la chasse se tait : « attack enemy
        // player » (55) et « coa stealth » (56) doivent prendre le tick. On
        // n'attend pas d'y arriver pour passer en RETOUR -- isUseful rend deja
        // false des que « enemy player target » est garni, donc Execute n'est
        // meme plus appelee -- mais ce cas existe quand meme : |dz| >= 30 yd
        // fait refuser l'engagement (EnemyPlayerValue.cpp:320-322) alors que la
        // proie reste, elle, dans « nearest enemy players ».
        if (distance <= PorteeEngagement(cible))
        {
            PasserEnRetour();
            return false;
        }

        /*
         * MOVEMENT_COMBAT, ET CE N'EST PAS UN ORNEMENT.
         *
         * L'etat GUET, juste avant, pose un ForceToWait de 3 a 6 s, et
         * ForceToWait ecrit dans « last movement » avec MOVEMENT_NORMAL par
         * defaut (NewRpgBaseAction.cpp:285-291, NewRpgBaseAction.h:40). Or
         * MovementAction::MoveTo commence par IsWaitingForLastMove(priority)
         * (MovementActions.cpp:181-184), qui ne relache le verrou que si
         * `priority > lastMove.priority` (MovementActions.cpp:904-905) --
         * MOVEMENT_COMBAT devance MOVEMENT_NORMAL (LastMovementValue.h:18-25).
         *
         * Avec la priorite NORMALE, la traque demarrait donc avec jusqu'a six
         * secondes de retard : le verrou du guet qu'on vient de quitter la
         * bloquait. Six secondes, c'est la moitie de la distance entre un
         * guetteur et sa proie. La priorite de combat est aussi celle que
         * ReturnToPullPositionAction emploie pour le meme genre de mouvement
         * (PullActions.cpp:281-282).
         */
        return MoveTo(cible, PorteeEngagement(cible), MovementPriority::MOVEMENT_COMBAT);
    }

    /*
     * Fin de traque sans engagement visible. On repart en RETOUR et non en GUET,
     * pour une raison de mesure autant que de comportement : c'est par ce chemin
     * que passe le cas NORMAL. Quand la proie entre dans « enemy player target »,
     * isUseful rend false et Execute n'est plus appelee du tout ; le combat a
     * lieu, et la premiere fois que la chasse retrouve la parole, la proie est
     * morte, partie ou hors de portee. Ce que ce chemin voit n'est donc pas
     * « une traque ratee », c'est « une traque finie » -- dans les deux cas le
     * poste est ce vers quoi il faut rentrer.
     *
     * Si le poste n'est plus valable, Oublier() rend a CHOIX.
     */
    void AbandonnerLaTraque()
    {
        if (PosteEncoreValable())
            PasserEnRetour();
        else
            Oublier();
    }

    // Voir l'en-tete de PorteeEngagementForte : 60 yd si le bot a plus de PV que
    // la proie, 20 yd sinon (EnemyPlayerValue.cpp:315-317).
    //
    // GetExactDist est une distance de CENTRE a CENTRE, quand
    // Unit::IsWithinDist -- celle qu'emploie EnemyPlayerValue.cpp:318 -- retire
    // les rayons de combat. GetExactDist <= 60 implique donc IsWithinDist(60) :
    // on s'arrete un cheveu plus pres que le strict necessaire, jamais plus loin.
    float PorteeEngagement(Unit* cible) const
    {
        return bot->GetHealth() > cible->GetHealth() ? PorteeEngagementForte : PorteeEngagementFaible;
    }

    /*
     * INVARIANT 2 : « la selection de proie emploie le MEME predicat que
     * l'attaque ». Deux filtres, dans cet ordre, et aucun des deux n'est recopie.
     *
     * 1. AttackersValue::IsPossibleTarget (AttackersValue.h:24, statique et
     *    publique) : visibilite, faction, drapeaux, et les deux tests
     *    IsPvpProhibited -- sur la cible ET sur le bot -- qui sont exactement
     *    ceux par lesquels AttackAction.cpp:90-91 coupe A L'INSTANT DE FRAPPER.
     *    C'est ce filtre-la qui empeche le bot de marcher vers quelqu'un qu'il
     *    refusera de toucher (piege 6).
     *
     *    SON TROISIEME PARAMETRE NE SERT A RIEN, et on ne le passe donc pas :
     *    la declaration l'appelle `range` (AttackersValue.h:24) mais la
     *    DEFINITION le laisse SANS NOM, commente, et ne s'en sert nulle part
     *    (AttackersValue.cpp:130). Aucun filtre de distance n'en vient : la
     *    portee de la traque est la notre, PorteeDetection(), et rien d'autre.
     *
     *    IL A UN EFFET DE BORD, lu et assume : quand la cible est en lieu
     *    interdit au PvP, il ordonne AttackStop aux creatures controlees par le
     *    bot qui la visent (AttackersValue.cpp:175-193). C'est le meme appel que
     *    fait le chemin d'attaque ; l'appeler ici ne cree pas un comportement
     *    nouveau, il l'avance de quelques ticks.
     *
     * 2. Le filtre de niveau de PossibleTargetsValue.cpp:87-131. Il est DEJA
     *    APPLIQUE, et il ne faut donc rien faire pour l'obtenir :
     *    NearestEnemyPlayersValue::AcceptUnit commence par
     *    « if (!PossibleTargetsValue::AcceptUnit(unit)) return false; »
     *    (EnemyPlayerValue.cpp:169-172), et PossibleTargetsValue::AcceptUnit est
     *    IsPossibleTarget suivi du hachage FNV-1a et de ses quatre paliers.
     *
     *    CE QUI A ETE RETIRE, ET POURQUOI. La premiere ecriture lisait ici la
     *    valeur « possible targets » et intersectait, sur la foi d'un
     *    commentaire affirmant que NearestEnemyPlayersValue « redefinit »
     *    AcceptUnit et echappe donc au filtre de niveau. Lu dans le fichier,
     *    c'est l'inverse -- le commentaire amont de EnemyPlayerValue.cpp:169 dit
     *    en toutes lettres « Apply parent's filtering first (includes level
     *    difference checks) ». L'intersection etait donc un no-op DEMONTRABLE,
     *    et pas seulement redondant : « possible targets » porte a
     *    sightDistance = 100 yd (PossibleTargetsValue.h:19) contre
     *    grindDistance = 75 yd pour « nearest enemy players »
     *    (EnemyPlayerValue.h:20), les deux partagent le meme FindUnits
     *    (AnyUnfriendlyUnitInObjectRangeCheck) et le meme test de ligne de vue
     *    (NearestUnitsValue.cpp:14-20) -- proches est un sous-ensemble de
     *    possibles, l'intersection ne pouvait rien retirer.
     *
     *    CE QU'ELLE COUTAIT, EN REVANCHE. « possible targets » est un
     *    CalculatedValue a checkInterval = 1 (NearestUnitsValue.h:20-22) : il
     *    recalcule a CHAQUE Get. Chaque lecture etait un Cell::VisitObjects de
     *    100 yd ramenant toutes les unites hostiles -- creatures comprises, le
     *    filtre de niveau ne portant que sur les joueurs -- puis un
     *    IsPossibleTarget par candidat et un bot->IsWithinLOSInMap(unit), donc
     *    un lancer de rayon VMap, par candidat accepte. A 250 mercenaires
     *    concentres (RandomBotConcentrateInPlayerZone = 1), toutes les
     *    DelaiScanProieMs, pour un resultat mathematiquement garanti de contenir
     *    deja tous les `retenus`.
     *
     * 3. INVARIANT 3, ajoute par cette relecture : CoaLieuSansPvp sur la PROIE.
     *    Ni AcceptUnit ni IsPossibleTarget ne lisent le drapeau sanctuaire du
     *    DBC -- leur seul test de lieu est IsPvpProhibited, donc les listes de
     *    conf (EnemyPlayerValue.cpp:223, AttackersValue.cpp:175-177) -- alors
     *    que Unit::_IsValidAttackTarget refuse le coup des que
     *    « target->IsInSanctuary() » (Unit.cpp:11341). Le DBC marque 228 aires
     *    0x800 quand la conf en liste 21 : sans ce test, un guetteur retenait
     *    une proie postee dans l'une des 207 autres, marchait jusqu'a elle,
     *    puis restait colle a la frontiere sans pouvoir frapper. La traque
     *    n'aurait pas subi cette paralysie, elle l'aurait ORGANISEE.
     *
     *    PLUS STRICT QUE LE COEUR, ET C'EST LE BON SENS DE L'ECART.
     *    CoaLieuSansPvp couvre aussi la CAPITALE sur la zone, que
     *    _IsValidAttackTarget ne regarde pas : on renonce donc a traquer une
     *    proie que le coeur aurait peut-etre laisse frapper. C'est le meme
     *    arbitrage que pour le filtre de niveau -- le mercenaire peut renoncer a
     *    suivre une proie qu'il aurait frappee si elle etait venue a lui, il ne
     *    peut pas suivre une proie qu'il refuserait de frapper -- et c'est ce
     *    que l'invariant 3 demande en toutes lettres : TOUTE destination passe
     *    par le predicat du lot 1, sans exception pour celles qui bougent.
     *
     *    Cout : deux acces indexes dans sAreaTableStore par candidat retenu, et
     *    GetZoneId()/GetAreaId() sont les champs mis a jour paresseusement par
     *    UpdatePositionData (Object.cpp:3166-3180), pas une resolution de
     *    terrain.
     */
    ObjectGuid ChoisirProie()
    {
        // P-035 : sur un nom inconnu, GetValue rend nullptr. On ne dereference
        // jamais sans garde, meme pour un nom du contexte de base.
        Value<GuidVector>* valeurProches = context->GetValue<GuidVector>("nearest enemy players");
        if (!valeurProches)
            return ObjectGuid::Empty;

        GuidVector const proches = valeurProches->Get();
        if (proches.empty())
            return ObjectGuid::Empty;

        std::vector<ObjectGuid> retenus;
        retenus.reserve(proches.size());

        for (ObjectGuid const& candidat : proches)
        {
            Unit* unite = botAI->GetUnit(candidat);
            if (!unite || !unite->IsInWorld() || !unite->IsAlive() || !unite->IsPlayer())
                continue;

            if (unite->GetMapId() != bot->GetMapId())
                continue;

            // Une proie deja a portee d'engagement n'a pas besoin d'etre
            // traquee : c'est « attack enemy player » (55) qui doit la prendre.
            if (bot->GetExactDist(unite) <= PorteeEngagement(unite))
                continue;

            // INVARIANT 3 sur la PROIE. Voir le point 3 de l'en-tete : ni
            // AcceptUnit ni IsPossibleTarget ne lisent le sanctuaire du DBC, et
            // le coeur, lui, refuse le coup dessus (Unit.cpp:11341).
            if (sPlayerbotAIConfig.CoaLieuSansPvp(unite->GetZoneId(), unite->GetAreaId()))
                continue;

            // Redondant avec AcceptUnit, qui l'a deja applique par
            // PossibleTargetsValue::AcceptUnit (PossibleTargetsValue.cpp:42-43),
            // et garde pour deux raisons : il ne coute AUCUN balayage -- c'est
            // un predicat sur une unite deja en main -- et Traquer() le rejoue
            // a l'identique a chaque pas, les deux restant ainsi le meme test.
            if (!AttackersValue::IsPossibleTarget(unite, bot))
                continue;

            retenus.push_back(candidat);
        }

        if (retenus.empty())
            return ObjectGuid::Empty;

        /*
         * UNE AU HASARD, ET NON LA PLUS PROCHE. C'est un changement, et il
         * repare une affirmation fausse.
         *
         * La premiere ecriture retenait la plus proche en s'appuyant sur ceci :
         * « la curee que redoute le piege 26 est deja bornee en amont,
         * NearestEnemyPlayersValue::AcceptUnit refuse un candidat dont les
         * places d'attaquant sont prises ». Lu dans le fichier,
         * MercenaryAttackSlotFree (EnemyPlayerValue.cpp:120-166) ne compte que
         * les mercenaires DEJA EN COMBAT avec la cible : elle parcourt
         * enemy->GetCombatManager().GetPvPCombatRefs() -- qui ne naissent qu'au
         * premier coup PORTE, son propre en-tete le dit -- et
         * enemy->getAttackers(), peuplee aux AttackStart. Un mercenaire qui se
         * contente de TRAQUER n'est dans ni l'une ni l'autre. Le plafond ne mord
         * donc pas pendant la traque, et rien n'empechait N guetteurs de
         * designer le meme guid : trois mercenaires a moins de 75 yd les uns des
         * autres retenaient tous les trois le meme passant, le plus proche pour
         * chacun, et convergeaient dessus. C'est exactement la curee du piege 26,
         * organisee par la selection au lieu d'etre evitee.
         *
         * Le tirage uniforme est la meme reponse que pour le tirage de POSTE, et
         * pour la meme raison : c'est le seul moyen dont on dispose ici de
         * disperser, le plafond d'assaillants ne repondant qu'a l'engagement.
         * Il ne supprime pas la convergence, il la rend improbable a proportion
         * du nombre de proies visibles -- et il reste une LIMITE ASSUMEE quand
         * il n'y en a qu'une.
         *
         * Ce qu'on ne fait PAS : relire « nearest enemy players » a chaque pas
         * de Traquer() pour verifier que le creneau est encore libre. La valeur
         * recalcule a chaque Get (checkInterval = 1), ce serait un balayage de
         * 75 yd par tick et par traqueur -- le cout meme qu'on vient de retirer
         * a l'autre bout de cette fonction.
         */
        return retenus[urand(0, uint32(retenus.size()) - 1)];
    }

    /*
     * Un poste retenu se perime. DistanceMax n'etait verifiee qu'au TIRAGE
     * (TirerPoste) : ni ROUTE ni GUET ne la relisaient, ils n'invalidaient que
     * sur un changement de CARTE. Un mercenaire deplace de plusieurs milliers de
     * yards SUR LA MEME carte gardait donc son ancien poste -- et y revenait par
     * TELEPORTATION, puisque le repli d'enlisement de MoveFarTo est un
     * bot->TeleportTo(dest) (NewRpgBaseAction.cpp, branche stuckAttempts >= 5).
     *
     * CE QUE CA CASSAIT, et c'est le point : le lot 0 a pose
     * RandomBotConcentrateInPlayerZone = 1 pour rassembler les bots la ou sont
     * les vrais joueurs. Un mercenaire ainsi redeploye a 2200 yd de son vieux
     * hub n'y progresse jamais de 5 yd -- la branche de progres de MoveFarTo
     * n'est donc jamais prise -- et 90 s plus tard il est teleporte HORS de la
     * zone du joueur, de retour sur son poste d'avant. La concentration voulue
     * par le lot 0 etait defaite a chaque cycle de teleport.
     *
     * Mesure attendue apres correction : aucune ligne « [New RPG] Teleport ...
     * as it stuck when moving far » pour un mercenaire dont la distance au poste
     * a saute d'un coup.
     */
    bool PosteEncoreValable() const
    {
        if (poste == WorldPosition())
            return false;

        if (poste.GetMapId() != bot->GetMapId())
            return false;

        return bot->GetExactDist(poste) <= DistanceMax;
    }

    // INVARIANT 3. Une destination n'est retenue que si le PvP peut y avoir
    // lieu. Le predicat est celui du lot 1, PlayerbotAIConfig::CoaLieuSansPvp
    // (PlayerbotAIConfig.cpp:1122) : sanctuaire sur l'AIRE, capitale sur la
    // ZONE, puis les listes de conf. On ne le reecrit pas.
    //
    // La resolution se fait sur la CARTE, par GetZoneAndAreaId (Map.h:256) :
    // tester la seule zone raterait Ratchet, Booty Bay ou Anvilmar, aires
    // sanctuaires logees dans des zones ordinaires (piege 15).
    bool EstChassable(WorldLocation const& lieu)
    {
        Map* carte = bot->GetMap();
        if (!carte || carte->GetId() != lieu.GetMapId())
            return false;

        uint32 zone = 0;
        uint32 aire = 0;
        carte->GetZoneAndAreaId(bot->GetPhaseMask(), zone, aire, lieu.GetPositionX(), lieu.GetPositionY(),
                                lieu.GetPositionZ());

        // CE QUE CE GARDE PROTEGE VRAIMENT -- et ce qu'il ne protege pas.
        //
        // RECTIFICATION. Une version precedente de ce commentaire affirmait que
        // Map::GetAreaId rend 0 « quand la grille de terrain n'est pas chargee »
        // et que GetGridTerrainData travaille « sans chargement ». C'est
        // l'inverse, corps lu : Map::GetGridTerrainData(GridCoord) appelle
        // EnsureGridCreated (Map.cpp:1102-1115), qui appelle
        // MapGridManager::CreateGrid (Map.cpp:183-186), lequel prend le mutex
        // _gridLock et execute GridTerrainLoader::LoadTerrain()
        // (Grids/MapGridManager.cpp:5-30). La grille est donc CREEE et son
        // terrain lu sur disque a la demande, synchroniquement, depuis le fil de
        // carte -- et Map::UnloadGrid n'est appele que depuis Map::UnloadAll :
        // rien n'est relache avant l'arret de la carte. C'est un COUT, pas une
        // absence de reponse, et ce garde ne le borne pas.
        //
        // CE QUI LE BORNE REELLEMENT, corrige le 2026-09-24 apres revue : rien,
        // dans la branche des hubs, sinon le verrou de 120 s entre deux tirages
        // (VerrouTirageMs). Une revue precedente avait pose une constante
        // MaxTestsLieu puis l'avait retiree a bon escient ; ce renvoi lui avait
        // survecu et promettait une borne qui n'existe pas. Le cout d'un tirage
        // qui echoue est donc proportionnel au nombre de hubs de la carte,
        // une fois toutes les 120 s par bot.
        //
        // Ce garde ne couvre donc qu'un seul cas : GetAreaId n'a trouve ni WMO
        // ni aire de grille ET i_mapEntry->linked_zone vaut 0 lui aussi
        // (Map.cpp, fin de GetAreaId : « if (!areaId) areaId =
        // i_mapEntry->linked_zone; »), c'est-a-dire une tuile .map reellement
        // absente. Rare, mais alors on ne sait rien du lieu.
        //
        // Et « je ne sais rien » vaut NON pour une destination. CoaLieuSansPvp,
        // elle, ne conclut rien sur un id inconnu et rend faux -- c'est le bon
        // choix pour « ai-je le droit de frapper », ou il ne faut pas interdire
        // ce qu'on ne sait pas lire ; c'est le mauvais pour « dois-je marcher
        // jusque-la », ou l'inconnu doit etre ecarte. Un autre poste sera tire au
        // prochain passage.
        if (!aire)
            return false;

        return !sPlayerbotAIConfig.CoaLieuSansPvp(zone, aire);
    }

    // Rend true quand un poste a ete retenu. Compte « poste pris » a ce
    // moment-la, et seulement a ce moment-la.
    bool RetenirUnPoste(bool lePlusProche)
    {
        // Ne pas rejouer le tirage a chaque tick. GetTravelHubs recopie un
        // vecteur du cache (TravelMgr.cpp:4490-4496) et, sur un niveau encore
        // absent de ce cache, std::map::operator[] y INSERE -- depuis trois fils
        // de carte. Un tirage au plus toutes les RepliChoixSecondes rend le cout
        // invisible et ce risque negligeable.
        time_t const maintenant = time(nullptr);
        if (maintenant < prochainChoix)
            return false;

        prochainChoix = maintenant + RepliChoixSecondes;

        WorldPosition const tire = TirerPoste(lePlusProche);
        if (tire == WorldPosition())
        {
            // UN TIRAGE QUI N'A RIEN RENDU NE SE REJOUE PAS DANS 10 s.
            //
            // RepliChoixSecondes a ete dimensionne pour le cas ou le tirage
            // REUSSIT. Le cas qui coute, c'est celui ou il echoue TOUJOURS : un
            // mercenaire dans une zone sanctuaire sans AREA_FLAG_CAPITAL a
            // inCity = false, donc SelectRandomGrindPos ne retient que des
            // cellules de SA zone (NewRpgBaseAction.cpp, garde « !inCity && ...
            // GetZoneId(...) != bot->GetZoneId() ») -- toutes sanctuaires --,
            // EstChassable les rejette, et 10 s plus tard tout recommence a
            // l'identique, indefiniment.
            //
            // Et ce n'est pas un tirage bon marche : SelectRandomGrindPos
            // appelle bot->GetMap()->GetZoneId(...) pour CHAQUE cellule du
            // palier sur la bonne carte a moins de 2500 yd, soit une resolution
            // de terrain par cellule. Le dossier compte 98 mercenaires libres en
            // lieu inerte : a 10 s, cela ferait une dizaine de tirages par
            // seconde, en permanence, pour un resultat toujours vide. Le bot
            // n'attendait pas, il scannait.
            //
            // On recule donc le prochain essai a RepliEchecSecondes. C'est la
            // borne dont ce lot dispose : la boucle interne de
            // SelectRandomGrindPos est du code amont, hors de ce mandat.
            prochainChoix = maintenant + RepliEchecSecondes;
            return false;
        }

        poste = tire;
        CoaChasseComptePostePris();
        return true;
    }

    /*
     * LE CATALOGUE D'ABORD -- lot 4. Voir Ai/Coa/CoaPostes.h.
     *
     * CE QU'IL APPORTE PAR RAPPORT AUX HUBS. GetTravelHubs ne rend que des
     * AUBERGES, sauf dans neuf zones listees en dur (TravelMgr.cpp:4733-4744).
     * Le catalogue y ajoute les voleries, les cimetieres et les carrefours du
     * graphe de marche (playerbots_travelnode, 3 781 noeuds, present en base et
     * MORT en service), et chaque poste y porte sa carte, son palier, son camp
     * et son poids -- l'enrichissement mesure du dossier, x18 pour une auberge,
     * x9 pour une volerie, x3,4 pour un cimetiere.
     *
     * QUAND IL NE REND RIEN, ON NE S'ARRETE PAS. Table absente (c'est le cas
     * aujourd'hui : Updates.EnableDatabases = 0, le .sql s'applique a la main),
     * carte sans poste, aucun poste du bon palier a portee, ou tous les
     * candidats tires refuses par EstChassable : dans tous ces cas la fonction
     * rend WorldPosition() et l'appelant enchaine sur les hubs puis sur les
     * cellules de grind, c'est-a-dire exactement le comportement du lot 2.
     *
     * LE TIRAGE EST PONDERE ET SANS REMISE, et le sans-remise n'est pas un
     * raffinement : c'est ce qui borne le nombre d'appels a EstChassable, dont
     * chacun peut declencher la lecture disque d'une tuile .map sous le mutex de
     * grille (voir l'en-tete de RepliEchecSecondes). Sans lui, une boucle de
     * tirage avec remise pourrait retester cent fois le meme poste inerte.
     */
    WorldPosition TirerPosteCatalogue(bool lePlusProche, uint8& classe)
    {
        classe = COA_POSTE_SOURCES;  // « aucune » tant que rien n'est retenu

        CoaCataloguePostes const& catalogue = CoaCataloguePostes::Instance();
        if (!catalogue.EstCharge())
            return WorldPosition();

        std::vector<CoaPosteGuet> const* postes = catalogue.PourCarte(bot->GetMapId());
        if (!postes)
            return WorldPosition();

        // L'INTERRUPTEUR EST LU UNE SEULE FOIS POUR TOUT LE TIRAGE. Le relire
        // par candidat laisserait `facteurs` d'une taille differente de
        // `candidats` si la conf etait rechargee au milieu de la premiere passe,
        // et CoaNormaliserParClasse abandonnerait alors la ponderation sans un
        // mot. Une variable locale ferme la fenetre.
        bool const indexActif = IndexProiesActif();

        uint8 const niveau = bot->GetLevel();
        // 1 Alliance, 2 Horde -- meme encodage que la colonne `faction` du .sql.
        uint8 const camp = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;

        // PREMIERE PASSE, arithmetique seulement : palier, camp, distance. Aucun
        // test de terrain ici, pour la meme raison que dans TirerPoste -- ces
        // tests-la ecartent l'essentiel du catalogue pour le prix d'entiers.
        //
        // LA DISTANCE EST GARDEE, et elle ne l'etait pas. Elle etait calculee ici
        // puis jetee, et le comparateur du tri de repli la recalculait deux fois
        // par comparaison. Voir CoaCandidatPoste (Ai/Coa/CoaPostes.h) et le tri
        // du repli plus bas.
        std::vector<CoaCandidatPoste> candidats;
        candidats.reserve(postes->size());

        // LOT 5. Le facteur de presence de chaque candidat, aligne sur
        // `candidats` -- CoaNormaliserParClasse exige la meme taille, faute de
        // quoi il ignore la ponderation au lieu de sortir du tableau.
        //
        // IL N'EST REMPLI QUE POUR LE TIRAGE ORDINAIRE. La branche REPLI, plus
        // bas, ne normalise rien : elle trie par distance et prend le premier
        // chassable, parce qu'un bot en lieu sans PvP ne cherche pas la meilleure
        // chasse, il cherche la SORTIE. Y ajouter la presence des proies
        // reviendrait a lui faire traverser la carte pour sortir d'un sanctuaire.
        std::vector<uint32> facteurs;
        if (!lePlusProche && indexActif)
            facteurs.reserve(postes->size());

        for (CoaPosteGuet const& p : *postes)
        {
            if (niveau < p.palierMin || niveau > p.palierMax)
                continue;

            if (p.faction && p.faction != camp)
                continue;

            float const distance = bot->GetExactDist(p.x, p.y, p.z);
            if (distance > DistanceMax)
                continue;

            // LE PLANCHER N'EST PLUS TESTE ICI, et c'est la relecture qui l'a
            // trouve. Il portait sur la distance au Z BRUT du catalogue, AVANT
            // que PoserSurLeSol ne recale ce Z -- or la raison d'etre de
            // PoserSurLeSol est precisement que le Z des carrefours et des
            // cimetieres vient de la base et n'a JAMAIS ete verifie contre le
            // terrain. Un poste dont le Z stocke est 47 yd trop haut, a 19 yd
            // horizontalement du bot, donnait une distance testee de
            // sqrt(19^2 + 47^2) = 50,7 yd : il passait le plancher de 50. Pose
            // sur le sol, il se retrouvait a 19 yd, donc DEJA dans RayonPoste
            // (20 yd) : au tick suivant l'etat ROUTE voyait « arrive »,
            // CoaChasseComptePosteAtteint montait et ForceToWait immobilisait le
            // bot 2 a 5 minutes a l'endroit exact ou il etait -- mot pour mot la
            // pathologie que le plancher des cellules de grind, plus bas, a ete
            // ecrit pour tuer. La fenetre est etroite (dz entre 46 et 50 yd,
            // au-dela GetHeight abandonne) mais elle est reelle.
            //
            // Il est donc applique APRES PoserSurLeSol, dans la boucle de
            // tirage. CE QUE CELA COUTE : des candidats trop proches restent
            // dans la liste et peuvent consommer un tirage. C'est borne, et par
            // la donnee elle-meme -- depuis la correction de la fusion du
            // generateur, deux postes visibles par un meme camp sont a plus de
            // 50 yd l'un de l'autre, donc au plus une poignee peuvent tenir dans
            // le disque de 50 yd autour du bot.

            candidats.push_back(CoaCandidatPoste(distance, &p));

            // Sur le Z BRUT du catalogue, et c'est sans consequence : la cellule
            // de l'index ne depend que de X et Y (CoaIndexProies::CleDuLieu).
            // PoserSurLeSol, plus bas, ne change que le Z.
            //
            // FACTEUR 1 D'OFFICE SOUS LE PLANCHER, et c'est une correction du
            // lot 5 a lui-meme. Le plancher DistanceMin n'est applique qu'apres
            // le tirage (boucle plus bas, sur le Z recale) : un candidat trop
            // proche reste donc dans la liste et peut consommer un des
            // MaxTestsCatalogue essais pour etre rejete a coup sur. Le lot 4
            // bornait le degat par le NOMBRE (deux postes visibles par un meme
            // camp sont a plus de 50 yd l'un de l'autre, donc au plus une
            // poignee tiennent dans le disque de DistanceMin) ; ponderer ces
            // candidats-la en aurait augmente la PROBABILITE sans toucher a
            // cette borne, et le budget de tirage de leur classe serait parti
            // dans des postes injouables -- TirerPosteCatalogue rendant alors
            // WorldPosition() plus souvent, donc un repli sur les hubs ou sur
            // SelectRandomGrindPos, c'est-a-dire le comportement que le lot 4
            // devait remplacer.
            //
            // LE TEST EST 2D, ET LE SENS DE L'INEGALITE EST LA RAISON DU CHOIX
            // -- c'est la relecture qui l'a etabli, apres l'avoir d'abord ecrit
            // a l'envers. La distance 3D est toujours SUPERIEURE OU EGALE a la
            // distance 2D. Donc :
            //   - tout poste que le plancher rejettera (3D < DistanceMin) a
            //     forcement 2D < DistanceMin : il est ecarte ici. Le test ne
            //     laisse donc JAMAIS l'index peser sur un poste condamne, ce qui
            //     est exactement ce qu'on cherchait ;
            //   - la reciproque est fausse : un poste a 2D < DistanceMin peut
            //     avoir 3D >= DistanceMin et passer le plancher. Celui-la perd
            //     sa ponderation sans l'avoir merite.
            // Ce second cas demande plus de 43 yd de denivele apres PoserSurLeSol
            // -- qui recale le Z sur le SOL -- a moins de 50 yd horizontalement
            // du bot : une falaise ou une tour juste a cote. Le degat est alors
            // borne a un facteur ramene a 1, donc a un poste moins favorise,
            // jamais a un poste exclu : CoaNormaliserParClasse ramene de toute
            // facon a 1 tout poids effectif nul, 1 est le plancher du facteur, et
            // le poste reste tirable.
            //
            // Tester en 3D sur le Z BRUT n'etait pas une option : c'est
            // precisement ce que la premiere ecriture du plancher faisait, et le
            // pave ci-dessus dit pourquoi ce Z-la ment.
            //
            // Effet de bord bienvenu : ces candidats ne paient pas non plus la
            // lecture de l'index.
            if (!lePlusProche && indexActif)
            {
                bool const tropPres = bot->GetExactDist2d(p.x, p.y) < DistanceMin;
                facteurs.push_back(tropPres ? 1u
                                            : FacteurPresence(indexActif, p.carte, p.x, p.y));
            }
        }

        if (lePlusProche)
        {
            // REPLI : la distance prime sur le poids. Un poste x18 a 900 yd ne
            // sert a rien a un bot qui ne peut ni frapper ni etre frappe la ou
            // il est ; ce qu'il lui faut, c'est SORTIR.
            //
            // partial_sort ET NON sort, sur la distance DEJA CALCULEE. Deux
            // gaspillages que la relecture a trouves, et qui se cumulaient :
            //   - la liste entiere etait triee alors que la boucle qui suit n'en
            //     lit jamais plus de MaxTestsCatalogueRepli = 32 elements ;
            //   - le comparateur recalculait bot->GetExactDist -- une racine
            //     carree -- a chaque comparaison, soit deux par comparaison,
            //     alors que la premiere passe l'avait deja calculee.
            // Mesure sur le cas le plus dense du catalogue livre (carte 530,
            // niveau 70, 1500 yd : 118 candidats) : ~815 comparaisons, donc
            // ~1 630 GetExactDist, pour n'utiliser que 32 elements ; et sur les
            // 711 postes de cette carte, ~6 750 comparaisons donc ~13 500
            // racines. Le cout tombe a n + 32 log n comparaisons et ZERO racine
            // supplementaire. Ce n'etait pas un effondrement -- le verrou de
            // RetenirUnPoste empeche que ce soit par tick -- mais c'etait du
            // travail entierement evitable sur le fil de carte.
            //
            // LE MEME DEFAUT EXISTE au tri des hubs (TirerPoste, juste en
            // dessous). Il n'est pas corrige ici : c'est du lot 2, et la liste y
            // compte quelques dizaines d'entrees, pas plusieurs centaines.
            std::size_t const aTrier =
                std::min<std::size_t>(MaxTestsCatalogueRepli, candidats.size());
            std::partial_sort(candidats.begin(), candidats.begin() + aTrier, candidats.end(),
                              [](CoaCandidatPoste const& a, CoaCandidatPoste const& b)
                              { return a.first < b.first; });

            // CETTE BOUCLE EST BORNEE ELLE AUSSI, et elle ne l'etait pas a la
            // premiere ecriture -- c'est la relecture qui l'a trouve. Le tirage
            // ordinaire, juste en dessous, s'arrete a MaxTestsCatalogue ; ici le
            // parcours allait jusqu'au bout de la liste, soit jusqu'a plusieurs
            // centaines de resolutions de terrain DANS LE MEME TICK, chacune
            // pouvant faire lire une tuile .map sous le mutex de grille. La
            // forme recopiee etait celle du repli par les hubs, ou la liste
            // compte quelques dizaines d'entrees ; le catalogue en compte
            // plusieurs centaines, et la forme ne se transpose pas.
            //
            // BORNE PLUS LARGE QUE LE TIRAGE ORDINAIRE, et pour une raison :
            // ici la liste est TRIEE PAR DISTANCE, donc un refus ecarte le
            // meilleur candidat restant, pas un candidat quelconque. Et l'enjeu
            // n'est pas le meme : un tirage ordinaire rate coute un guet de
            // plus au meme endroit, un REPLI rate laisse le bot dans un lieu ou
            // il ne peut ni frapper ni etre frappe.
            for (std::size_t i = 0; i < aTrier; ++i)
            {
                CoaPosteGuet const* p = candidats[i].second;
                WorldPosition point(p->carte, p->x, p->y, p->z);
                if (PoserSurLeSol(point) && EstChassable(point))
                {
                    classe = p->classe;
                    return point;
                }
            }

            return WorldPosition();
        }

        // INVARIANT 3, et il s'applique MEME A UN POSTE DEJA FILTRE HORS LIGNE.
        // L'outil qui engendre le catalogue resout l'aire par la GRILLE 2D des
        // fichiers .map ; Map::GetAreaId, lui, consulte d'abord le VMAP, donc
        // les groupes WMO. Les deux peuvent differer a l'interieur d'un
        // batiment. Le catalogue est une PRESELECTION, pas une autorisation :
        // le seul juge reste CoaLieuSansPvp, appele ici comme pour toute autre
        // destination.
        //
        // ET LE Z EST RECALE, ce que le tirage des hubs ne fait pas. Les hubs
        // sont des positions de spawn de PNJ, deja valides -- le commentaire de
        // PoserSurLeSol le dit et c'est toujours vrai. Mais la grande majorite
        // du catalogue est faite de carrefours de routes et de cimetieres, dont
        // le Z vient de la base et n'a JAMAIS ete verifie contre le terrain du
        // serveur. Or le repli d'enlisement de MoveFarTo est un TeleportTo NU
        // (piege 10, NewRpgBaseAction.cpp:119-120) : un poste sous terre ou sous
        // l'eau finit par y teleporter le bot. PoserSurLeSol refuse l'eau,
        // refuse un sol introuvable, et repose le point 5 cm au-dessus du sol.
        //
        // CE QUE CELA COUTE AUX HUBS DU CATALOGUE, dit franchement : ils passent
        // eux aussi par ce filtre, et l'un d'eux -- un aubergiste a l'etage, un
        // quai -- peut s'y faire refuser alors qu'il est bon. La perte est
        // nulle : GetTravelHubs, juste en dessous, les propose sans ce filtre.
        // On ne perd donc jamais un hub, on gagne un sol sous chaque carrefour.
        //
        // MaxTestsCatalogue borne ce que ce refus peut couter. Il n'y a pas de
        // borne equivalente du cote des hubs, et c'est voulu : la liste des hubs
        // est courte et deja parcourue au plus une fois. Ici la liste peut
        // compter plusieurs centaines de postes, et un serveur dont les VMAP
        // desaccordent massivement avec la grille les testerait tous.
        //
        // ET LE TIRAGE EST NORMALISE PAR CLASSE avant d'etre pondere : sans
        // cela, le NOMBRE de carrefours ecrase le FACTEUR des auberges. Voir
        // CoaNormaliserParClasse (Ai/Coa/CoaPostes.h), qui porte la mesure.
        std::vector<CoaPostePese> peses;
        CoaNormaliserParClasse(candidats, peses, facteurs.empty() ? nullptr : &facteurs);

        for (uint32 essais = 0; essais < MaxTestsCatalogue; ++essais)
        {
            CoaPosteGuet const* p = CoaTirerPondere(peses);
            if (!p)
                break;

            WorldPosition point(p->carte, p->x, p->y, p->z);
            if (!PoserSurLeSol(point))
                continue;

            // LE PLANCHER, ICI ET PAS AVANT : sur le Z recale, le seul qui soit
            // celui de la destination reelle. Voir la premiere passe. Il est
            // inconditionnel parce qu'on n'atteint cette boucle que si
            // !lePlusProche -- la branche REPLI, plus haut, a deja rendu.
            if (bot->GetExactDist(point) < DistanceMin)
                continue;

            if (!EstChassable(point))
                continue;

            // LOT 5. Une SEULE relecture de l'index, sur le seul poste retenu.
            // Elle ne sert qu'a la mesure : le tirage, lui, est deja fait, et il
            // l'a ete avec le facteur calcule dans la premiere passe.
            //
            // SUR `indexActif` ET NON SUR IndexProiesActif() : la mesure doit
            // porter sur le regime dans lequel le tirage a eu lieu, pas sur
            // celui du moment ou on la prend. Un rechargement de conf entre les
            // deux ferait sinon compter un poste comme peuple alors qu'il a ete
            // tire a poids egaux.
            if (indexActif && ProiesAutour(point))
                CoaChasseComptePostePeuple();

            classe = p->classe;
            return point;
        }

        return WorldPosition();
    }

    WorldPosition TirerPoste(bool lePlusProche)
    {
        // L'INTERRUPTEUR EST LU UNE FOIS POUR TOUT CE TIRAGE, pour la meme
        // raison que dans TirerPosteCatalogue -- qui lit le sien : les deux
        // fonctions sont des tirages independants, chacun interieurement
        // coherent. Le relire par candidat dans la boucle des hubs laissait les
        // premiers hubs ponderes par l'index et les suivants a 1 si la conf
        // etait rechargee au milieu, donc un tirage sans remise fait sur une
        // demi-carte et biaise vers les hubs que GetTravelHubs rend en tete.
        bool const indexActif = IndexProiesActif();

        // LOT 4 : le catalogue passe devant, et lui seul compte « poste
        // catalogue ». Ce compteur est la seule facon de distinguer, dans
        // CoaBots.log, une chasse qui tire dans la table d'une chasse qui s'est
        // repliee sur les hubs -- les deux font monter « poste pris ».
        uint8 classe = COA_POSTE_SOURCES;
        WorldPosition const duCatalogue = TirerPosteCatalogue(lePlusProche, classe);
        if (duCatalogue != WorldPosition())
        {
            CoaChasseComptePosteCatalogue();
            // Et la CLASSE, sans laquelle une baisse de la metrique de preuve du
            // lot ne peut etre imputee a personne : « poste catalogue » monte
            // pareil pour une auberge (x18 mesure) et pour un carrefour (x1).
            CoaChasseComptePosteClasse(classe);
            return duCatalogue;
        }

        // MESURE DU DOSSIER (section 2.1) : les hubs sont le meilleur predicteur
        // de la presence des bots, x18 contre un temoin tire au hasard dans la
        // meme zone. MAIS ce cache ne contient que des AUBERGISTES, sauf dans
        // neuf zones listees en dur (TravelMgr.cpp:4733-4744) ou le maitre de vol
        // sert de hub. On ne tire donc pas « des lieux de passage », on tire
        // « des auberges » -- et beaucoup d'auberges sont en aire sanctuaire,
        // d'ou le filtre ci-dessous et le repli qui suit.
        std::vector<WorldLocation> const hubs = sTravelMgr.GetTravelHubs(bot);

        // PREMIERE PASSE : carte et distance seulement. Ces deux tests sont
        // arithmetiques et ne touchent pas au terrain. Rien d'autre ici : c'est
        // EstChassable qui coute, parce qu'il peut faire CREER une grille et
        // lire son terrain sur disque (voir son en-tete). D'ou cette premiere
        // passe, qui ecarte a peu de frais avant d'y venir.
        //
        // Il n'existe AUCUN plafond sur le nombre de EstChassable d'un tirage :
        // une revue avait pose MaxTestsLieu puis l'avait retiree, et ce renvoi
        // lui avait survecu. Corrige le 2026-09-24. Seul le verrou de 120 s
        // borne la frequence, pas le volume.
        std::vector<WorldLocation const*> candidats;
        candidats.reserve(hubs.size());

        for (WorldLocation const& lieu : hubs)
        {
            if (lieu.GetMapId() != bot->GetMapId())
                continue;

            float const distance = bot->GetExactDist(lieu);
            if (distance > DistanceMax)
                continue;

            // Le plancher ne vaut que pour le tirage ordinaire. En REPLI, le
            // point chassable le plus proche peut etre a vingt metres : c'est
            // exactement celui qu'on veut.
            if (!lePlusProche && distance < DistanceMin)
                continue;

            candidats.push_back(&lieu);
        }

        if (lePlusProche)
        {
            // REPLI : on veut le point chassable LE PLUS PROCHE. On trie par
            // distance croissante et on s'arrete au premier qui passe -- ce qui
            // rend le meme resultat qu'un balayage complet suivi d'un minimum,
            // pour le cout du seul prefixe inerte.
            std::sort(candidats.begin(), candidats.end(),
                      [this](WorldLocation const* a, WorldLocation const* b)
                      { return bot->GetExactDist(*a) < bot->GetExactDist(*b); });

            for (WorldLocation const* lieu : candidats)
                if (EstChassable(*lieu))
                    return WorldPosition(*lieu);
        }
        else if (!candidats.empty())
        {
            // Tirage au sort et non « le plus proche » : faire converger N
            // mercenaires sur le meme hub produirait exactement la curee que
            // MaxAttackersPerTarget = 1 devait empecher, et que son propre
            // en-tete dit ne pas empecher a l'ouverture
            // (EnemyPlayerValue.cpp:115-119). Le lot 5 le penche vers les hubs
            // peuples, sans jamais le rendre deterministe -- voir ci-dessous.
            //
            /*
             * LOT 5 : LE TIRAGE DES HUBS DEVIENT PONDERE PAR LA PRESENCE REELLE.
             *
             * CE QUE LE LOT 2 FAISAIT, ET POURQUOI ON NE LE GARDE PAS TEL QUEL.
             * Il tirait uniformement, par un Fisher-Yates paresseux arrete au
             * premier hub chassable. C'etait juste, mais aveugle : deux auberges
             * du meme palier avaient exactement le meme poids, que l'une soit
             * deserte et l'autre pleine. C'est precisement le trou que le lot 5
             * ferme -- au-dela de 75 yd, un mercenaire ne sait rien de ce qui
             * l'entoure.
             *
             * CE QU'ON FAIT A LA PLACE : un tirage PONDERE SANS REMISE, poids
             * 1 + min(proies, PlafondProies), donc entre 1 et 5. Le modele et le
             * raisonnement sont ceux de CoaTirerPondere (Ai/Coa/CoaPostes.h) :
             * la somme est recalculee a chaque tour parce que la liste retrecit,
             * et le poste tire est RETIRE de la liste, ce qui borne le nombre de
             * EstChassable a un par hub, comme avant.
             *
             * POURQUOI PONDERE ET NON PAS << LES PEUPLES D'ABORD >>. Une premiere
             * redaction partitionnait la liste en deux tranches, peuples puis
             * deserts. C'etait faux, et c'est la relecture qui l'a vu : quand un
             * seul hub de la liste est peuple, TOUS les mercenaires du meme
             * palier et du meme camp a portee y vont, sans exception. C'est
             * exactement la convergence que le lot 2 avait ecrit son tirage
             * uniforme pour eviter, et la curee que MaxAttackersPerTarget = 1 ne
             * sait pas empecher a l'ouverture (EnemyPlayerValue.cpp:115-119, son
             * propre en-tete le dit). Un rapport borne a 5:1 penche sans
             * decider : le hub desert reste tirable, et deux mercenaires qui
             * tirent au meme instant ne vont pas forcement au meme endroit.
             *
             * INDEX ETEINT : tous les poids valent 1. Un tirage pondere sans
             * remise a poids egaux construit une permutation UNIFORME : la loi du
             * hub retenu est donc exactement celle du lot 2. Ce qui change, et il
             * faut le dire, c'est la CONSOMMATION du generateur -- un urand par
             * tour ici, un par tour la-bas, mais sur des bornes differentes : les
             * deux versions ne rendent pas la meme suite de tirages, seulement la
             * meme loi.
             *
             * CE QUE CA COUTE : un appel a ProiesAutour par candidat -- au plus
             * 32 sequences de sondage, soit quelques dizaines de chargements
             * atomiques relaxed, aucun acces terrain -- et une somme par tour.
             * La liste des hubs d'un palier compte quelques dizaines d'entrees ;
             * le tirage complet est donc de l'ordre du millier d'additions au
             * pire, au plus une fois toutes les RepliEchecSecondes pour le bot
             * qui ne trouve rien.
             */
            std::vector<std::pair<uint32, WorldLocation const*>> peses;
            peses.reserve(candidats.size());
            for (WorldLocation const* lieu : candidats)
                peses.push_back({ FacteurPresence(indexActif, lieu->GetMapId(),
                                                  lieu->GetPositionX(), lieu->GetPositionY()),
                                  lieu });

            while (!peses.empty())
            {
                // SOMME SATURANTE, comme CoaTirerPondere : un debordement ne
                // planterait pas, il fausserait le tirage en silence.
                uint32 somme = 0;
                for (auto const& pese : peses)
                {
                    if (somme > 0xFFFFFFFFu - pese.first)
                    {
                        somme = 0xFFFFFFFFu;
                        break;
                    }
                    somme += pese.first;
                }

                // Ne peut pas arriver -- tout facteur vaut au moins 1 -- mais un
                // urand sur une somme nulle deviendrait urand(0, 0xFFFFFFFF).
                if (!somme)
                    break;

                // urand borne INCLUS des deux cotes : on tire dans [0, somme-1],
                // sans quoi la valeur `somme` sortirait de la boucle cumulee sans
                // avoir rien choisi.
                uint32 tire = urand(0, somme - 1);
                std::size_t choisi = peses.size() - 1;
                for (std::size_t i = 0; i < peses.size(); ++i)
                {
                    if (tire < peses[i].first)
                    {
                        choisi = i;
                        break;
                    }
                    tire -= peses[i].first;
                }

                std::swap(peses[choisi], peses.back());
                std::pair<uint32, WorldLocation const*> const retenu = peses.back();
                peses.pop_back();

                if (EstChassable(*retenu.second))
                {
                    // facteur > 1 equivaut a << l'index a vu au moins une proie
                    // NON MERCENAIRE dans le bloc 2x2 du hub >> : le facteur
                    // vaut 1 + min(proies exogenes, 4), et ne depasse 1 que la.
                    // Le qualificatif compte : un hub ou ne se tiennent que des
                    // chasseurs ne fait plus monter ce compteur, ce qui est
                    // precisement ce qui rendait la metrique auto-confirmante.
                    if (retenu.first > 1)
                        CoaChasseComptePostePeuple();

                    return WorldPosition(*retenu.second);
                }
            }
        }

        // Aucun hub chassable a portee. Les cellules de grind du palier donnent
        // un point de plein air -- sans valeur predictive sur la presence des
        // proies (mesure : 25 % contre 22 % pour un temoin), mais c'est un
        // deplacement reel plutot qu'aucun. Teste lui aussi : INVARIANT 3.
        //
        // Limite assumee, lue dans le code : SelectRandomGrindPos
        // (NewRpgBaseAction.cpp:964-1020) reste dans la zone du bot sauf en
        // capitale. Son autre limite, le Z, est corrigee juste en dessous.
        WorldPosition repli = SelectRandomGrindPos(bot);
        if (repli == WorldPosition() || repli.GetMapId() != bot->GetMapId())
            return WorldPosition();

        // LE MEME PLANCHER QUE POUR LES HUBS, qui manquait ici. Ni TirerPoste ni
        // SelectRandomGrindPos ne le portaient : cette derniere ne filtre que
        // par carte, zone et 2500 yd -- contrairement a SelectRandomCampPos,
        // qui, elle, ecarte les points a moins de 50 yd du bot
        // (NewRpgBaseAction.cpp, garde « if (bot->GetExactDist(loc) < 50.0f)
        // continue; »).
        //
        // CE QUE SON ABSENCE PRODUISAIT. Les cellules de grind sont sur un pas
        // de 50 yd (TravelMgr.cpp:4690-4692) : une cellule a moins de 20 yd du
        // bot est un tirage possible. On passait en ROUTE puis, au meme tick,
        // « bot->GetExactDist(poste) <= RayonPoste » (20 yd) etait deja vrai :
        // GUET, CoaChasseComptePosteAtteint(), et ForceToWait immobilisait le
        // bot 2 a 5 minutes a l'endroit exact ou il etait. Les deux compteurs
        // « poste pris » et « poste atteint » -- la seule preuve prevue que ce
        // lot fonctionne -- montaient pour un bot qui n'avait pas parcouru un
        // metre, et qui etait desormais PLUS immobile qu'avant le lot.
        //
        // Le plancher ne s'applique pas au repli de sortie de sanctuaire, pour
        // la meme raison que pour les hubs : en REPLI, tout point chassable est
        // bon a prendre, aussi proche soit-il.
        //
        // ET IL EST TESTE APRES PoserSurLeSol, pas avant : le Z d'une cellule de
        // grind est arrondi au pas de 50 yd sur les TROIS axes (piege 20), donc
        // la distance au Z brut peut depasser DistanceMin la ou la distance a la
        // destination REELLE ne le depasse pas. Tester avant, c'etait rouvrir
        // exactement la pathologie que ce plancher ferme -- et sur la population
        // ou le Z est le plus faux. Meme reordonnancement que dans la boucle de
        // tirage du catalogue.
        if (!PoserSurLeSol(repli))
            return WorldPosition();

        if (!lePlusProche && bot->GetExactDist(repli) < DistanceMin)
            return WorldPosition();

        if (!EstChassable(repli))
            return WorldPosition();

        // LOT 5, ET C'EST UNE CORRECTION DU LOT A LUI-MEME. « poste peuple »
        // n'etait joignable que sur deux des quatre chemins de TirerPoste -- le
        // tirage ordinaire du catalogue et celui des hubs -- alors que « poste
        // pris » monte pour TOUS les postes retenus (RetenirUnPoste). Le ratio
        // annonce dans l'en-tete des compteurs melangeait donc des tirages ou
        // l'index avait ete consulte et des tirages ou il ne pouvait
        // structurellement pas l'etre, et la conclusion aurait porte sur la
        // qualite de l'index alors que la cause etait ailleurs. Aujourd'hui le
        // chemin qui manquait le plus est justement celui-ci : le catalogue
        // n'etant pas applique en base (Updates.EnableDatabases = 0),
        // TirerPosteCatalogue rend WorldPosition() d'emblee et une large part
        // des postes retenus vient de SelectRandomGrindPos.
        //
        // RESTENT HORS DU COMPTE les deux replis de sortie de lieu sans PvP
        // (lePlusProche), et c'est voulu : ils ne sont pas ponderes du tout --
        // un bot en sanctuaire cherche la SORTIE, pas la meilleure chasse -- et
        // les compter reviendrait a mesurer l'index la ou il n'a pas servi.
        // L'en-tete de « poste peuple » (Ai/Class/Coa/CoaAiObjectContext.cpp)
        // nomme donc le denominateur exact : les tirages ORDINAIRES.
        if (!lePlusProche && indexActif && ProiesAutour(repli))
            CoaChasseComptePostePeuple();

        return repli;
    }

    /*
     * PIEGE 20. Les positions de locsPerLevelCache sont arrondies au pas de
     * 50 yd sur les TROIS axes, Z compris (TravelMgr.cpp:4690-4692, :4824-4827) :
     * une cellule peut etre a 25 yd sous terre ou en l'air.
     *
     * POURQUOI CA COMPTE ICI, alors que MoveFarTo sait marcher vers un point
     * approximatif. Parce que son repli anti-enlisement, lui, ne marche pas : au
     * bout de 5 essais et 90 s il fait un bot->TeleportTo(dest) NU, sans test de
     * sol, d'eau ni de faction (NewRpgBaseAction.cpp:119-120, contre
     * RandomPlayerbotMgr.cpp:1668-1675 qui, lui, verifie). Une destination sous
     * terre finit donc par y teleporter le bot.
     *
     * La correction reprend exactement la forme de RandomTeleport
     * (RandomPlayerbotMgr.cpp:1668-1675) : refuser l'eau, refuser un sol
     * introuvable, puis poser le point 5 cm au-dessus du sol trouve. Les hubs ne
     * passent pas par la : ce sont des positions de spawn de PNJ, deja valides.
     */
    bool PoserSurLeSol(WorldPosition& point)
    {
        Map* carte = bot->GetMap();
        if (!carte || carte->GetId() != point.GetMapId())
            return false;

        float const x = point.GetPositionX();
        float const y = point.GetPositionY();
        float const z = point.GetPositionZ();

        if (carte->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
            return false;

        float const sol = carte->GetHeight(bot->GetPhaseMask(), x, y, z + 0.5f);
        if (sol <= INVALID_HEIGHT)
            return false;

        // LE REFUS DE L'EAU SE REJOUE SUR LE Z CORRIGE, et c'est la seule
        // verification qui compte : le test ci-dessus porte sur le Z ARRONDI,
        // celui-la meme dont l'en-tete de cette fonction dit qu'il peut etre faux
        // de 25 yd. Le point qu'on retient, lui, est sol + 0,05.
        //
        // CE QUI PASSAIT AU TRAVERS. Une cellule dont le Z arrondi tombe 20 yd
        // SOUS le fond d'un lac : le premier IsInWater rend false (le point est
        // dans la roche, pas dans le liquide), GetHeight rend le fond du lac, et
        // le point retenu etait le fond du lac + 0,05 -- sous l'eau. EstChassable
        // l'acceptait, MoveFarTo le retenait, et 90 s plus tard son repli
        // d'enlisement y teleportait le bot. C'est exactement le cas que cette
        // fonction existe pour empecher.
        //
        // Note : RandomPlayerbotMgr.cpp, dont la forme a ete recopiee, porte la
        // meme faiblesse. La recopier fidelement ne la rend pas correcte ici, ou
        // le point teleporte est choisi par nous.
        float const zPose = 0.05f + sol;
        if (carte->IsInWater(bot->GetPhaseMask(), x, y, zPose, bot->GetCollisionHeight()))
            return false;

        point.setZ(zPose);
        return true;
    }
};

/*
 * LE DECROCHAGE PvE -- lot 3, section 2.2 du dossier.
 *
 * CE QU'IL REPARE. Le mercenaire riposte deja : la chaine
 * ThreatMgr -> « attackers » -> « dps target » -> « dps assist » (50,0) est
 * intacte chez lui (DpsAssistStrategy.cpp:12-13, posee AVANT la branche
 * mercenaire en AiFactory.cpp:640-644), et le dossier l'a mesuree -- huit
 * mercenaires solo vus en combat sur une creature en 420 s. Ce qu'il n'a pas,
 * c'est le CONTRAIRE : aucune strategie « flee », « runaway » ni « return » ne
 * lui est posee, et la riposte n'a AUCUN garde-fou de niveau ni de rang, la ou
 * GrindTargetValue en avait deux (:95 pour l'ecart de niveau, :98-101 pour les
 * rangs elite). Un mercenaire qui se fait mordre par un elite de dix niveaux
 * au-dessus se bat jusqu'a la mort.
 *
 * POURQUOI UNE ACTION ET PAS UN DECLENCHEUR DE COMBAT. Le moteur de COMBAT ne
 * devient courant que quand le bot attaque pour de bon : les deux seuls
 * ChangeEngine(BOT_STATE_COMBAT) de tout l'arbre sont AttackAction.cpp:199 et
 * PullActions.cpp:94 (grep exhaustif). Tant que la creature frappe sans que le
 * bot ait riposte, bot->IsInCombat() est VRAI et le moteur courant est encore
 * le NON-COMBAT -- PlayerbotAI.cpp:1718-1725 le dit en toutes lettres, en
 * nettoyant « current target » dans ce cas precis. Une action poussee par la
 * strategie « coa chasse » a donc la parole a cet instant-la, et c'est le seul
 * instant qui compte : a 50,5 elle passe AVANT « dps assist » (50,0) et le bot
 * n'engage jamais le combat qu'il ne doit pas prendre.
 *
 * CE QU'ELLE NE COUVRE PAS, ET IL FAUT LE DIRE. Une fois AttackAction passee,
 * le moteur courant est le moteur de COMBAT, ou cette strategie non-combat n'a
 * plus la parole. Un combat qui tourne mal APRES l'engagement n'est donc rompu
 * qu'au retour au moteur non-combat (« invalid target » -> « drop target » a 99,
 * CombatStrategy.cpp:20-26), c'est-a-dire quand la cible meurt ou devient
 * invalide. Fermer ce trou demande une strategie de type COMBAT et une pose
 * dans AiFactory::AddDefaultCombatStrategies : ce n'est pas dans ce lot.
 *
 * LA BANDE, ET POURQUOI ELLE EST SI ETROITE :
 *   56,0  « coa stealth »          -- la furtivite garde la main
 *   55,0  « attack enemy player »  -- le PvP garde la main, TOUJOURS
 *   50,5  <- ici
 *   50,0  « dps assist » / « tank assist » -- la riposte PvE, qu'on evince
 * Un mercenaire ne decroche JAMAIS d'un joueur : c'est son gagne-pain. Le
 * predicat ci-dessous le refuse explicitement, et la pertinence le refuserait
 * de toute facon.
 *
 * CE QUE LA FENETRE COUPE VRAIMENT, ET CE N'EST PAS « ce qui est sous 50,5 ».
 * La liste de pertinences ci-dessus est celle du moteur NON-COMBAT, et elle ne
 * dit pas le principal : tant que cette action gagne le tick, le moteur de
 * COMBAT n'est jamais mis en service. Les deux seuls
 * ChangeEngine(BOT_STATE_COMBAT) de l'arbre sont AttackAction.cpp:199 et
 * PullActions.cpp:94 (grep exhaustif) ; PlayerbotAI::DoNextAction, lui, ne
 * bascule QUE vers BOT_STATE_DEAD et BOT_STATE_NON_COMBAT
 * (PlayerbotAI.cpp:1705, :1714). Une fenetre de decrochage prive donc le bot de
 * TOUT le moteur de combat -- soins, defensives, interruptions -- et pas
 * seulement de « dps assist ». C'est ce qui rend le choix des declencheurs
 * ci-dessous decisif : une rupture ouverte a tort coute au bot toute sa
 * capacite a se defendre, pendant DureeMaxMs.
 *
 * ET LA FUITE NE FUIT PAS. Le pas vaut sPlayerbotAIConfig.fleeDistance, 5,0 yd
 * par defaut (PlayerbotAIConfig.cpp:116) : une creature au contact le reste. Ce
 * que cette action obtient reellement est donc « ne plus alimenter ce combat »,
 * pas « s'en extraire ». Consigne ici plutot que suppose ailleurs : c'est la
 * raison pour laquelle les declencheurs exigent desormais une PREUVE que le
 * combat est perdu, et non un simple indice qu'il est difficile.
 */
class CoaDecrochageAction : public MovementAction
{
public:
    CoaDecrochageAction(PlayerbotAI* botAI) : MovementAction(botAI, "coa decrochage") {}

    static constexpr float Pertinence = 50.5f;

    // Duree maximale d'un decrochage, et ce n'est pas un detail de confort :
    // cette action rend TRUE tant qu'elle tient, donc elle affame tout ce qui
    // est sous 50,5 -- « dps assist » le premier, ce qui est le but, mais aussi
    // la chasse elle-meme. Sans ce plafond, un mercenaire poursuivi par un
    // elite qui ne le lache pas fuirait jusqu'a sa mort sans jamais rendre un
    // coup. Passe ce delai il se rebat : mal, mais il se bat.
    static constexpr uint32 DureeMaxMs = 20 * 1000;

    // Age au-dela duquel le marqueur de fenetre est tenu pour perime et rearme.
    // Voir le garde correspondant dans isUseful.
    static constexpr uint32 OubliMs = 5 * 60 * 1000;

    // Ecart de niveau au-dela duquel on rompt, recopie de
    // GrindTargetValue.cpp:95 (« (int)unit->GetLevel() - (int)bot->GetLevel() > 4 »).
    static constexpr int32 EcartNiveauMax = 4;

    // Ecart de niveau A PARTIR DUQUEL le RANG elite suffit a rompre. Zero : une
    // creature d'elite ou de rare de niveau egal ou superieur. En dessous de
    // son niveau, un elite reste un combat que le bot gagne, et le rang seul ne
    // doit pas suffire -- voir CombatPerduOuInterdit, qui explique pourquoi
    // « can fight elite » ne peut pas trancher pour un mercenaire libre.
    static constexpr int32 EcartNiveauEliteMin = 0;

    bool isUseful() override
    {
        // Les deux interrupteurs, dans cet ordre : le decrochage est une piece
        // de la chasse, il ne s'allume pas sans elle.
        if (!sPlayerbotAIConfig.wildPvpChasse || !sPlayerbotAIConfig.wildPvpDecrochage)
            return false;

        if (!bot->IsAlive() || bot->IsInFlight() || bot->InBattleground())
            return false;

        // LE REARMEMENT DE LA FENETRE EST ICI, et il ne peut pas etre ailleurs :
        // hors combat, tous les tests qui suivent rendent false, et Execute
        // n'est jamais appelee. Si le compteur ne se remettait pas a zero a cet
        // endroit, un bot ayant deja decroche 20 s une fois dans sa vie ne
        // decrocherait plus jamais.
        if (!bot->IsInCombat())
        {
            debutDecrochage = 0;
            return false;
        }

        if (botAI->HasGameClientMaster())
            return false;

        if (bot->GetLevel() < sPlayerbotAIConfig.wildPvpMinLevel)
            return false;

        if (!sPlayerbotAIConfig.IsMercenary(bot->GetGUID().GetRawValue()))
            return false;

        /*
         * LA FENETRE, ET SON GARDE-FOU CONTRE UN MARQUEUR PERIME.
         *
         * Le rearmement d'en haut n'a lieu QUE si cette isUseful est atteinte
         * hors combat -- et le moteur s'arrete a la premiere action qui
         * s'execute (Engine.cpp:229-236). Rien ne garantit donc qu'elle le soit :
         * « coa stealth » (56) et « attack enemy player » (55) sont au-dessus.
         * Un marqueur laisse par un combat d'il y a une heure condamnerait le
         * bot a ne plus jamais decrocher, sans un mot.
         *
         * Au-dela d'OubliMs, le marqueur ne peut plus appartenir au combat en
         * cours : aucun combat de mercenaire ne dure cela sans une seule
         * interruption. On le rearme plutot que de s'y fier.
         */
        if (debutDecrochage)
        {
            uint32 const ecoule = GetMSTimeDiffToNow(debutDecrochage);
            if (ecoule >= OubliMs)
                debutDecrochage = 0;
            else if (ecoule >= DureeMaxMs)
                return false;
        }

        // JAMAIS CONTRE UN JOUEUR. Deux verrous, parce qu'ils ne voient pas la
        // meme chose : « enemy player target » est la proie CONVOITEE, et
        // getAttackers() la liste des unites qui frappent le bot en ce moment
        // (Unit.h:902, le meme conteneur que lit MyAttackerCountValue,
        // AttackerCountValues.cpp:11). Un mercenaire qui fuit un joueur n'est
        // plus un mercenaire.
        if (Value<Unit*>* convoitee = context->GetValue<Unit*>("enemy player target"))
            if (convoitee->Get())
                return false;

        for (Unit* agresseur : bot->getAttackers())
        {
            if (!agresseur)
                continue;

            if (agresseur->IsPlayer())
                return false;

            // Le familier d'un joueur n'est pas un joueur, mais le combat qu'il
            // ouvre en est un. GetOwner rend nullptr pour une creature libre.
            if (Unit* maitre = agresseur->GetOwner())
                if (maitre->IsPlayer())
                    return false;
        }

        return CombatPerduOuInterdit();
    }

    bool Execute(Event /*event*/) override
    {
        /*
         * INVARIANT 1, DEUX SORTIES AVANT TOUT EFFET.
         *
         * (a) PAS D'AGRESSEUR ATTEIGNABLE. AgresseurLePlusProche ecarte ce qui
         *     est mort, hors monde ou sur une autre carte -- et bot->IsInCombat()
         *     peut rester vrai apres, le temps que le coeur referme la
         *     rencontre. La premiere ecriture rendait true quand meme : elle
         *     gagnait le tick a 50,5 pendant 20 s en n'appelant meme pas
         *     MoveAway, ce que l'invariant 1 interdit en toutes lettres. Le
         *     raisonnement du commentaire plus bas -- ne pas propager l'echec
         *     d'un pas -- ne couvrait que le cas ou le pas existe.
         *
         * (b) LE BOT NE PEUT PAS BOUGER. IsMovingAllowed est botAI->CanMove()
         *     (MovementActions.cpp:915-918) : enracine, etourdi, sous controle.
         *     Un decrochage qui ne deplace rien n'a aucun effet, et il coute
         *     alors tout le moteur de COMBAT (voir l'en-tete de cette classe).
         *     Rendre la main a « dps assist » (50,0) est strictement meilleur :
         *     un bot immobile qui ne frappe pas est un bot qui meurt.
         *
         * Les deux sorties sont AVANT l'armement de la fenetre : un decrochage
         * qui n'a pas eu lieu ne consomme pas la fenetre et ne se compte pas.
         */
        Unit* agresseur = AgresseurLePlusProche();
        if (!agresseur)
            return false;

        if (!IsMovingAllowed())
            return false;

        /*
         * UN DECROCHAGE = UNE FENETRE, ET DONC UN INCREMENT.
         *
         * CoaChasseCompteDecrochage etait appele a CHAQUE Execute, alors que
         * RecordUsage incremente `tried` a chaque appel (CoaAiObjectContext.cpp,
         * corps de RecordUsage). Le compteur ne mesurait donc pas des ruptures
         * mais des TICKS de fuite : une seule rupture de 20 s avec
         * AiPlayerbot.ReactDelay = 100 en produisait de l'ordre de cent a deux
         * cents. Ses deux freres du meme lot comptent, eux, une TRANSITION
         * (CoaChasseCompteTraque a l'entree en TRAQUE, CoaChasseCompteRetour a
         * l'arrivee au poste) : les trois n'etaient plus comparables, et c'est
         * precisement le rapport traque/retour/decrochage qui doit servir de
         * preuve que le lot fonctionne. Un exploitant lisant « decrochage 0/200 »
         * a cote de « traque 0/12 » aurait conclu a un emballement du garde-fou
         * et eteint l'interrupteur.
         */
        if (!debutDecrochage)
        {
            debutDecrochage = getMSTime();
            CoaChasseCompteDecrochage();
        }

        /*
         * LACHER LA CIBLE. Meme sequence que DropTargetAction
         * (ChooseTargetActions.cpp:59-70), a une ligne pres : on n'appelle PAS
         * ChangeEngine(BOT_STATE_NON_COMBAT). Cette action n'est poussee que par
         * une strategie STRATEGY_TYPE_NONCOMBAT, donc elle ne s'execute que
         * lorsque le moteur non-combat est DEJA le moteur courant ; l'appel
         * n'aurait rien a changer, et PlayerbotAI::ChangeEngine ecrit une ligne
         * de journal a chaque passage.
         */
        if (Value<Unit*>* cible = context->GetValue<Unit*>("current target"))
            cible->Set(nullptr);

        bot->SetTarget(ObjectGuid::Empty);
        bot->SetSelection(ObjectGuid());
        bot->AttackStop();

        /*
         * S'ELOIGNER -- MAIS PAS SUR UN TICK DEJA VERROUILLE.
         *
         * MoveAway essaie jusqu'a neuf directions autour de l'oppose de
         * l'agresseur et appelle Map::CheckCollisionAndGetValidCoords AVANT
         * chaque MoveTo (MovementActions.cpp:1535-1537, :1556-1558) ; c'est
         * MoveTo, lui, qui refuse sur IsWaitingForLastMove
         * (MovementActions.cpp:181-184) -- donc APRES la requete de collision.
         * Sur un tick ou le verrou MOVEMENT_COMBAT pose par le pas precedent
         * court encore, les neuf requetes VMap/MMap etaient de la pure perte,
         * repetees a chaque tick d'IA pendant toute la fenetre. Le verrou se
         * teste ici, une fois, pour zero requete.
         *
         * Le pas lui-meme vaut fleeDistance (5,0 yd) : voir l'en-tete de cette
         * classe, qui dit ce que cela vaut vraiment.
         */
        if (!IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
            MoveAway(agresseur, sPlayerbotAIConfig.fleeDistance, false);

        /*
         * RENDRE TRUE MEME SI LE PAS A ECHOUE, et c'est voulu.
         *
         * MoveAway rend false sur un simple verrou de deplacement
         * (IsWaitingForLastMove, relu par MovementAction::MoveTo). Si on
         * propageait ce false, le moteur descendrait d'un cran dans le panier et
         * trouverait « dps assist » (50,0), qui re-engagerait la creature au
         * tick suivant : le bot lacherait sa cible et la reprendrait
         * alternativement, sans jamais ni fuir ni combattre. Le decrochage doit
         * TENIR le tick tant qu'il dure, et c'est DureeMaxMs qui le borne --
         * pas l'echec d'un pas.
         *
         * Ce que cela affame, dit franchement, et la premiere ecriture n'en
         * disait que la moitie : dans le moteur NON-COMBAT, tout ce qui est
         * sous 50,5, pendant au plus 20 s -- la chasse (2,0) etant de toute
         * facon eteinte en combat par son propre invariant 1, et
         * « food » / « drink » impossibles en combat. Mais AUSSI, et c'est le
         * principal, TOUT LE MOTEUR DE COMBAT : soins, defensives,
         * interruptions, pas seulement « dps assist ». Voir l'en-tete de cette
         * classe, qui en donne la preuve (les deux seuls
         * ChangeEngine(BOT_STATE_COMBAT) de l'arbre sont hors d'atteinte tant
         * que cette action gagne le tick). C'est ce qui justifie les deux
         * sorties en tete d'Execute et le durcissement des declencheurs de
         * CombatPerduOuInterdit : une fenetre ouverte a tort coute cher.
         */
        return true;
    }

private:
    uint32 debutDecrochage = 0;

    /*
     * LE GARDE-FOU, recopie de GrindTargetValue.cpp:95-101 et retourne.
     *
     * La-bas il servait a ne pas CHOISIR une cible trop forte ; ici il sert a
     * ROMPRE avec un adversaire trop fort, parce que la riposte, elle, ne
     * choisit rien : TargetValue::FindTarget n'itere que sur « attackers »
     * (TargetValue.cpp:41-43), c'est-a-dire sur ce qui frappe deja le bot.
     *
     * Le test de niveau porte sur ce qui N'EST PAS un joueur -- c'est la
     * condition « && !unit->GetGUID().IsPlayer() » de GrindTargetValue.cpp:95.
     * Ici elle est redondante (isUseful a deja refuse tout agresseur joueur),
     * mais on la garde : elle dit pourquoi le seuil vaut 4 et pas autre chose.
     */
    bool CombatPerduOuInterdit()
    {
        /*
         * LES PV SEULS NE DISENT PAS QU'UN COMBAT EST PERDU, et la premiere
         * ecriture rompait dessus sans rien regarder d'autre.
         *
         * Le cas qui le montre : un mercenaire a 29 % de PV (seuil par defaut
         * 30) face a une creature de son niveau, non elite, a 10 % de PV. Il
         * gagne au prochain coup ; il lachait sa cible, reculait de 5 yd par
         * passe et encaissait pendant DureeMaxMs sans rendre un coup ni pouvoir
         * se soigner -- « food » et « drink » sont impossibles en combat
         * (NonCombatActions.cpp, EatAction::isPossible : « if (bot->IsInCombat())
         * return false »), et le moteur de COMBAT etait hors circuit (voir
         * l'en-tete de cette classe). A l'expiration, « dps assist » reprenait
         * le combat avec beaucoup moins de PV qu'au depart. Le decrochage
         * rendait la mort PLUS probable dans le cas meme pour lequel il a ete
         * ecrit.
         *
         * La preuve qu'on exige desormais est la plus simple qui soit vraie :
         * un agresseur vivant qui tient MIEUX que le bot, en pourcentage.
         */
        bool const perdant = PvSousLeSeuil() && AgresseurMieuxPortant();

        /*
         * « CAN FIGHT ELITE » NE PEUT PAS ARBITRER UN MERCENAIRE LIBRE, et il
         * faut le dire ici parce que le nom fait croire le contraire.
         * CanFightEliteValue::Calculate COMMENCE par bot->GetGroup()
         * (MaintenanceValues.cpp:54-57) : un mercenaire sans groupe -- le cas
         * normal, HasGameClientMaster ayant deja ete refuse par isUseful --
         * rend donc TOUJOURS faux. Sur la foi de ce faux, la premiere ecriture
         * rompait des qu'un agresseur avait rank > CREATURE_ELITE_NORMAL, rare
         * de niveau egal compris : toute la population des rares declenchait un
         * decrochage, alors qu'un rare de son niveau se tue.
         *
         * On le garde -- un mercenaire chef d'un groupe de bots existe, et pour
         * lui la valeur a un sens -- mais il ne porte plus la decision : le rang
         * ne rompt que si la creature est AUSSI d'un niveau au moins egal a
         * celui du bot. C'est le pendant, pour le rang, de ce que EcartNiveauMax
         * est pour le niveau : un critere de combat qu'on n'aurait pas du
         * prendre, et non une difficulte passagere.
         */
        bool const peutElite = PeutCombattreElite();

        for (Unit* agresseur : bot->getAttackers())
        {
            if (!agresseur || !agresseur->IsAlive())
                continue;

            if (!agresseur->GetGUID().IsPlayer() &&
                int32(agresseur->GetLevel()) - int32(bot->GetLevel()) > EcartNiveauMax)
                return true;

            if (Creature* creature = agresseur->ToCreature())
                if (CreatureTemplate const* modele = creature->GetCreatureTemplate())
                    if (modele->rank > CREATURE_ELITE_NORMAL && !peutElite &&
                        int32(creature->GetLevel()) - int32(bot->GetLevel()) >= EcartNiveauEliteMin)
                        return true;
        }

        return perdant;
    }

    // Un agresseur vivant dont le POURCENTAGE de PV depasse celui du bot.
    //
    // Le pourcentage et non les PV absolus : c'est la seule des deux mesures qui
    // reste comparable entre un bot de 5000 PV et une creature de 300. Et une
    // borne INFERIEURE, pas une preuve : elle ignore les degats par seconde des
    // deux cotes, si bien qu'un bot a 29 % face a un petit agresseur a 90 %
    // rompra alors qu'il l'aurait tue. On l'assume, parce que l'ecart avec
    // l'ecriture precedente va dans le bon sens : celle-la rompait sur les PV du
    // bot SEULS, donc aussi contre une creature a 10 % de PV qu'il abattait au
    // coup suivant -- et lui offrait alors vingt secondes sans riposte.
    //
    // Unit::GetHealthPct rend 0 sur un PVmax nul (Unit.h:1112), ce qui ecarte de
    // lui-meme les cas degeneres sans test supplementaire.
    bool AgresseurMieuxPortant() const
    {
        float const pctBot = bot->GetHealthPct();

        for (Unit* agresseur : bot->getAttackers())
        {
            if (!agresseur || !agresseur->IsAlive())
                continue;

            if (agresseur->GetHealthPct() > pctBot)
                return true;
        }

        return false;
    }

    // Le MEME seuil et le MEME corps que CoaChasseAction::PvSousLeSeuil : un
    // seul reglage, une seule notion de « combat perdu ». La duplication des
    // deux corps jumeaux a ete remplacee par l'appel unique a CoaPvSousLeSeuil
    // (en tete de ce fichier) le jour ou ce corps a gagne ses deux bords.
    bool PvSousLeSeuil() const { return CoaPvSousLeSeuil(bot); }

    // « can fight elite » est le meme arbitre que celui de
    // GrindTargetValue.cpp:100 (CanFightEliteValue, MaintenanceValues.h:78-84).
    // Garde P-035 : sur un nom inconnu GetValue rend nullptr.
    bool PeutCombattreElite()
    {
        Value<bool>* valeur = context->GetValue<bool>("can fight elite");
        return valeur && valeur->Get();
    }

    Unit* AgresseurLePlusProche()
    {
        Unit* plusProche = nullptr;
        float meilleure = 0.0f;

        for (Unit* agresseur : bot->getAttackers())
        {
            if (!agresseur || !agresseur->IsInWorld() || !agresseur->IsAlive())
                continue;

            // PIEGE 11 : la carte d'abord, la distance ensuite.
            if (agresseur->GetMapId() != bot->GetMapId())
                continue;

            float const distance = bot->GetExactDist(agresseur);
            if (!plusProche || distance < meilleure)
            {
                plusProche = agresseur;
                meilleure = distance;
            }
        }

        return plusProche;
    }
};

/*
 * La chasse est une strategie A ELLE, et non un noeud greffe dans CoaBuffStrategy.
 *
 * POURQUOI. Trois raisons, dans cet ordre :
 *   - « coa buff » est posee sur TOUS les bots de classe CoA (AiFactory.cpp:640-642),
 *     mercenaires ou non : y greffer la chasse ferait evaluer l'action sur les
 *     241 bots ordinaires pour qu'elle reponde non ;
 *   - on ne pourrait pas eteindre la chasse sans perdre les buffs, ce que le
 *     dossier reproche deja a l'embuscade (section 2.3) ;
 *   - une strategie nommee APPARAIT dans la liste rendue par le port 8888
 *     (commande « strategy »). C'est la seule facon de prouver qu'elle a ete
 *     posee : un nom de strategie sans createur ne produit RIEN, sans erreur ni
 *     journal (Engine.cpp:366-381, piege 3), et « nc » et « buff » sont dans ce
 *     cas depuis toujours sans que personne l'ait vu.
 *
 * L'ACTION EST POUSSEE PAR DEFAUT, SANS DECLENCHEUR. Engine::PushDefaultActions
 * (Engine.cpp:518-526) la met dans le panier a chaque tick. Le declencheur
 * « often » de CoaBuffStrategy ne conviendrait pas : c'est un RandomTrigger de
 * probabilite 5 (TriggerContext.h:349, GenericTriggers.cpp:322-333), qui ne
 * s'arme qu'un tick sur cinq -- il multiplierait par cinq la duree de chaque
 * voyage. La chasse n'en devient pas gourmande pour autant : elle rend false des
 * qu'elle n'a rien a faire, et « move random » (1,5) reprend alors la main.
 *
 * ELLE N'EST PAS ENREGISTREE DANS CoaStrategyFactoryInternal, et ce n'est pas un
 * detail : cette fabrique-la supporte les FRERES, et Engine::addStrategy retire
 * du moteur tous les freres de la strategie posee (Engine.cpp:365-379). Y mettre
 * « coa chasse » faisait tomber « coa buff », posee sur le meme moteur
 * non-combat, et avec elle la seule amorce de l'embuscade. Elle a donc sa propre
 * fabrique sans freres, CoaChasseStrategyFactoryInternal -- voir son en-tete
 * dans CoaAiObjectContext.cpp.
 */
class CoaChasseStrategy : public Strategy
{
public:
    CoaChasseStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "coa chasse"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }

    /*
     * QUATRE ACTIONS PAR DEFAUT, ET AUCUN DECLENCHEUR.
     *
     * « coa decrochage » (50,5) et « coa chasse » (2,0) sont les deux moities du
     * comportement : l'une rompt un combat PvE perdu ou interdit, l'autre
     * voyage, guette et traque. Elles ne peuvent pas se gener -- la chasse rend
     * false des que bot->IsInCombat(), le decrochage n'est utile que dans ce cas.
     *
     * « drink » (4,2) et « food » (4,1) sont RECOPIEES de
     * GrindingStrategy::getDefaultActions (GrindingStrategy.cpp:10-16), et c'est
     * une reparation, pas un ajout : le mercenaire les a perdues en perdant
     * « grind » (AiFactory.cpp:758-760, branche `else`). Les deux valeurs 4,2 et
     * 4,1 sont celles de l'amont, au chiffre pres, et elles priment la chasse
     * (2,0) -- ce qui est exactement ce qu'on veut apres un decrochage.
     *
     * CE QUE CETTE REPARATION COUVRE VRAIMENT. Le premier jet ecrivait que sans
     * elles le mercenaire « ne se restaure plus qu'a 45 % de PV et 15 % de
     * mana », en citant les declencheurs « low health » / « low mana » de
     * UseFoodStrategy. C'est l'autre branche. UseFoodStrategy::InitTriggers en a
     * DEUX (UseFoodStrategy.cpp:14-23) et celle qui s'applique en service est
     * « if (botAI->HasCheat(BotCheatMask::food)) », qui pose « medium health » et
     * « high mana » : AiPlayerbot.BotCheats vaut « food,taxi,raid » par defaut
     * (PlayerbotAIConfig.cpp:550-556). Le seuil reel est donc mediumHealth = 65 %
     * et highMana = 65 % (PlayerbotAIConfig.cpp:130, :134), pas 45 % et 15 %. La
     * bande que ces deux lignes ajoutent est 65-100 %, pas 45-100 %.
     *
     * ET ELLE A UN COUT, QUI N'AVAIT PAS ETE ECRIT. En mode triche,
     * EatAction::Execute fait bot->SetStandState(UNIT_STAND_STATE_SIT) puis
     * botAI->SetNextCheckDelay(18000 * (100 - PV%) / 100)
     * (NonCombatActions.cpp:110-125) : a 80 % de PV, 3,6 s SANS AUCUN TICK D'IA.
     * PlayerbotAI::UpdateAI ne fait que decrementer nextAICheckDelay
     * (PlayerbotAI.cpp:263-268) ; rien ne le raccourcit a l'ouverture d'un
     * combat. Un guetteur assis perd donc, a chaque accrochage, plusieurs
     * secondes de « attack enemy player » et de riposte. C'est assume -- un bot
     * qui chasse a 65 % de PV meurt plus souvent qu'un bot qui mange -- mais
     * c'est une contrepartie, pas un pur gain, et elle se lit ici.
     *
     * Elles sont posees ici et non par un declencheur parce que c'est ainsi que
     * GrindingStrategy les posait : ce sont des actions par DEFAUT, dont les
     * declencheurs (« food » / « drink ») portent leurs propres conditions de
     * soif et de faim.
     */
    std::vector<NextAction> getDefaultActions() override
    {
        /*
         * L'INTERRUPTEUR EST TESTE ICI AUSSI, et pas seulement dans les isUseful.
         *
         * Engine::PushDefaultActions rappelle getDefaultActions A CHAQUE TICK
         * (Engine.cpp:518-526) : ce test est donc relu a chaud comme les autres,
         * sans reconnecter les bots. Il ne sert a rien pour « coa chasse » et
         * « coa decrochage », dont l'isUseful porte deja le meme garde -- il
         * sert pour « drink » et « food », qui n'en ont pas et qui ne sont PAS
         * a nous.
         *
         * Sans lui, poser Chasse = 0 ne rendrait plus le comportement d'avant :
         * un mercenaire de classe CoA se mettrait a manger des 99 % de PV
         * (EatAction::isUseful, NonCombatActions.cpp:134 : « health < 100 »)
         * alors qu'il n'a plus « grind ». C'est precisement la reparation
         * voulue -- mais elle doit s'allumer avec le reste, pas avant.
         */
        if (!sPlayerbotAIConfig.wildPvpChasse)
            return {};

        /*
         * ET LE MEME TRI QUE LES isUseful, POUR LA MEME RAISON.
         *
         * « coa chasse » et « coa decrochage » portent chacune, dans leur
         * isUseful, les gardes HasGameClientMaster / wildPvpMinLevel /
         * IsMercenary. « drink » et « food » n'en ont aucun -- et ce ne sont pas
         * nos actions : EatAction::isUseful vaut « health < 100 » et
         * DrinkAction::isUseful vaut « mana < 100 » (NonCombatActions.cpp:75-77,
         * :131). Poser les deux sous le seul interrupteur de chasse les laissait
         * a 4,1 et 4,2 sur un bot qui n'est plus un mercenaire libre.
         *
         * Le cas est reel, c'est le piege 29 du dossier : les strategies sont
         * posees a l'initialisation de l'IA et jamais reevaluees
         * (AiFactory.cpp:709-713). Un joueur qui recrute par « .playerbot bot
         * add » un bot de classe CoA dont le hachage en fait un mercenaire
         * eteint « coa chasse » et « coa decrochage » -- HasGameClientMaster
         * rend vrai -- mais gardait « food » et « drink » a chaque tick. Le
         * compagnon s'asseyait pour manger des 99 % de PV, ce qui evince
         * « follow » (1,0, FollowMasterStrategy.cpp:9-13), et restait plante
         * derriere son maitre a la moindre egratignure. Le meme trou s'ouvre sur
         * un changement de MercenaryPercent a chaud (piege 33).
         *
         * getDefaultActions est rappelee A CHAQUE TICK par
         * Engine::PushDefaultActions (Engine.cpp:518-526) : ces deux tests se
         * relisent donc a chaud, exactement comme ceux des isUseful.
         *
         * wildPvpMinLevel n'est PAS repete ici, et c'est voulu : la pose de
         * « coa chasse » l'exige deja au niveau de AiFactory.cpp:716-718, un bot
         * ne perd pas de niveau, et la reparation food/drink -- rendre ce que la
         * perte de « grind » a pris -- ne depend d'aucun seuil de niveau.
         */
        // Strategy n'herite que de PlayerbotAIAware (Strategy.h:67) : elle porte
        // botAI, pas le `bot` protege de AiObject. Le joueur se lit donc par
        // PlayerbotAI::GetBot (PlayerbotAI.h:541).
        if (botAI->HasGameClientMaster())
            return {};

        Player* const joueur = botAI->GetBot();
        if (!joueur || !sPlayerbotAIConfig.IsMercenary(joueur->GetGUID().GetRawValue()))
            return {};

        return {
            NextAction("coa decrochage", CoaDecrochageAction::Pertinence),
            NextAction("drink", 4.2f),
            NextAction("food", 4.1f),
            NextAction("coa chasse", CoaChasseAction::Pertinence),
        };
    }
};

#endif
