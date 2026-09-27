/*
 * CoA -- le Directeur, lot D1 : ce que le jeu lui offre.
 *
 * Conception : coa-project/docs/CONCEPTION-directeur.md (v3), § 6.3, § 14.
 *
 * DEUX CHOSES, ET UNE SEULE REGLE.
 *
 * 1. Le port 8888 cesse de lire les objets de jeu depuis son fil reseau (P-137).
 *    Chaque requete est deposee dans une file, executee par le FIL MONDE dans
 *    WorldScript::OnUpdate -- apres l'attente des cartes, donc sans course --, et
 *    le fil reseau attend la reponse, deux secondes au plus. La requete `monde`
 *    rend en une ligne l'etat de tous les personnages en jeu, bots et humains :
 *    c'est la perception rapide du Directeur.
 *
 * 2. Les commandes en jeu `.directeur note|ask|stop|reprise`. Elles deposent la
 *    demande dans `coa_dir_demande` (acore_characters) ; le service externe la
 *    lit. `stop` pose en plus un drapeau d'arret par personnage, persiste dans
 *    `coa_dir_arret` et relu au demarrage : c'est le jeu, pas le service, qui le
 *    tient (§ 6.3), et tout verbe futur devra le consulter (EstArrete).
 *
 * 3. (Lot D2a) Le bus de commandes du service, et le verbe `treve` : voir plus
 *    bas, et coa-project/docs/SPEC-directeur-D2.md § 3-4.
 *
 * 4. (Lot D2c) Le verbe `suspendre_mercenaire`, et son mode RPG : § 5.
 *
 * 5. (Lot D2b) Les verbes `reaffecter` et `alleger` : une affectation (un
 *    disque) en deux phases, TRAJET puis SEJOUR, lue par la teleportation
 *    aleatoire, new rpg et la chasse : § 6.
 *
 * 6. (Lot D3) Les verbes `repondre`, `peupler`, `faire_parler`,
 *    `heure_du_jour`, `meteo`, la requete `joueur,<guid>` et le refus `occupe` :
 *    coa-project/docs/SPEC-directeur-D3.md, § 2.8, § 3 a § 6.
 *
 * LA REGLE : un seul proprietaire par etat (P-134). Le drapeau d'arret n'est
 * ecrit que par ce fichier. Aucun champ du coeur ni de playerbots n'est touche.
 */

#ifndef COA_DIRECTEUR_H
#define COA_DIRECTEUR_H

#include "Define.h"
#include <string>

class Map;
class Player;
struct Position;

namespace CoaDirecteur
{
// Depuis n'importe quel fil. Bloque au plus DELAI_REPONSE_MS ; rend "timeout"
// si le fil monde n'a pas repondu, "busy" si la file est pleine.
std::string SoumettreRequete(std::string const& requete);

// Fil monde seulement : WorldScript::OnUpdate.
void ViderRequetes();

// Fil monde seulement : WorldScript::OnStartup.
void ChargerArrets();

// Tout verbe du Directeur qui vise un personnage doit d'abord poser la question.
bool EstArrete(uint32 guidLow);

// ------------------------------------------------------------------ lot D2a
//
// LE BUS DE COMMANDES (SPEC-directeur-D2.md § 4). Le service externe insere
// des commandes dans acore_characters.coa_dir_commande ; le fil monde les lit
// en asynchrone, les controle, pose SES consignes et les acquitte. Le magasin
// de consignes n'a qu'un ecrivain, le fil monde ; les fils de carte le lisent
// (SousTreve) par un compteur atomique puis un verrou partage. Aucun etat de
// playerbots ni du coeur n'est ecrit (P-134) : le composant concerne LIT la
// consigne (PossibleTargetsValue::AcceptUnit pour la treve).
//
// Tout est inerte tant que AiPlayerbot.Directeur.Bus vaut 0 (defaut).

// Fil monde, OnStartup, apres ChargerArrets : teste la table, ecrit
// coa_dir_instance. Table absente : bus inerte pour la vie du processus.
void DemarrerBus();

// Fil monde, OnUpdate : drainage, levees, echeances, bilan.
void TraiterBus(uint32 diff);

// Fil monde, OnUpdate, MEME SI le bus est eteint : ecrit les arrets en base,
// au plus une fois par seconde et par joueur, le DERNIER etat (correctif C2).
void Persister();

// Tout fil, chemin chaud : `guidLow` est-il sous une treve du Directeur ?
// (aucun bot n'initie de combat contre lui ; la riposte reste permise).
bool SousTreve(uint32 guidLow);

// ------------------------------------------------------------------ lot D2c
//
// Verbe `suspendre_mercenaire` (SPEC-directeur-D2.md § 5). Deux effets, un
// seul proprietaire chacun ; ni le drapeau FFA ni IsMercenary ne sont touches.

// Tout fil, chemin chaud : le BOT `guidLow` est-il un mercenaire suspendu ?
// Lu par PossibleTargetsValue::AcceptUnit : il ne convoite alors aucun joueur
// (l'autodefense, testee avant, reste permise).
bool MercenaireSuspendu(uint32 guidLow);

// Tout fil : le bot est-il suspendu EN MODE RPG ? Lu par la fabrique
// (AiFactory::AddDefaultNonCombatStrategies), proprietaire de la composition
// des strategies : vrai, elle compose celle d'un bot ordinaire. Le fil monde
// appelle ResetStrategies a la pose et a la levee pour qu'elle la relise.
bool MercenaireEnRpg(uint32 guidLow);

// ------------------------------------------------------------------ lot D2b
//
// Verbes `reaffecter` et `alleger` (SPEC-directeur-D2.md § 6) : meme effet cote
// jeu, seul le choix du service differe. Le bot recoit un disque ; il s'y rend
// par ses propres actions, SANS quetes (TRAJET) ; arrive, il reprend son
// parcours normal (SEJOUR), seule la teleportation aleatoire restant
// suspendue. Seul le fil monde ecrit la phase (suivi de chaque seconde) ; les
// proprietaires existants la LISENT :
//   - RandomPlayerbotMgr::RandomTeleport et RandomBotLevelMgr::AdjustBotToRange :
//     aucune teleportation aleatoire sous affectation (TRAJET et SEJOUR) ;
//   - NewRpgStatusUpdateAction::Execute : la laisse (TRAJET, hors du disque) ;
//   - NewRpgGoGrindAction::Execute : pas de quete en route (TRAJET) ;
//   - NewRpgBaseAction::MoveFarTo : pas de saut d'enlisement sous les yeux
//     d'un humain (TRAJET) ;
//   - CoaChasseAction::PosteEncoreValable / RetenirUnPoste : le poste est dans
//     le disque (TRAJET et SEJOUR).

enum class CoaDirPhase : uint8
{
    Trajet,
    Sejour
};

struct Affectation
{
    uint32 id = 0;      // commande qui l'a posee
    uint32 carte = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, rayon = 0.0f;   // z : sol trouve a la pose
    uint32 finS = 0;    // GameTime::GetGameTime(), en secondes
    CoaDirPhase phase = CoaDirPhase::Trajet;
    // Etape d'itineraire (action `E-...`, option Directeur.Etapes) : ne passe
    // jamais en SEJOUR, reste en TRAJET jusqu'au remplacement ou l'echeance.
    bool etape = false;
    // Lot D3 (verbe `peupler`, SPEC-directeur-D3.md § 4.2) : affectation « de
    // presence », pres du joueur qui l'a demandee. Figee a la pose. Deux
    // lecteurs : NewRpgBaseAction::MoveFarTo (le saut d'enlisement est TOUJOURS
    // retenu, en trajet comme en sejour : le bot ne se teleporte jamais sous les
    // yeux du joueur) et NewRpgStatusUpdateAction::Execute (laisse de sejour,
    // option Directeur.PresenceLaisseYd).
    bool presence = false;
    // Compagnons (27/09, option Directeur.Compagnons) : presence `peupler`
    // d'action `C-...`, posee APRES une presence du meme proprietaire. Le
    // centre SUIT le joueur (le fil monde le recale chaque seconde, seul
    // ecrivain) ; le bot se tient a COMPAGNON_MIN_YD-COMPAGNON_MAX_YD de lui
    // (NewRpgStatusUpdateAction::Execute, proprietaire de rpgInfo). Figee a
    // la pose. `rayon` vaut alors COMPAGNON_MAX_YD.
    bool compagnon = false;
};

// Compagnons : la bande ou le bot se tient autour du joueur, et le point vise
// quand il en sort (sur la droite joueur -> bot).
constexpr float COMPAGNON_MIN_YD = 15.0f;
constexpr float COMPAGNON_MAX_YD = 30.0f;
constexpr float COMPAGNON_CIBLE_YD = 22.0f;

// Tout fil, chemin chaud : le bot a-t-il une affectation en cours ? `sortie`
// nul : seulement la question. Sans affectation, un chargement atomique.
bool AffectationDe(uint32 guidLow, Affectation* sortie);

// Le point (carte, x, y) est-il dans le disque ? Arithmetique seule.
bool DansLeDisque(Affectation const& a, uint32 carte, float x, float y);

// Compagnons : le point ou un bot en (bx, by) se place pres du joueur (le
// centre de `a`) : sur la droite centre -> bot, a COMPAGNON_CIBLE_YD, sol sec
// (P-132) ; sinon la meme distance a +-35 puis +-70 degres ; sinon le centre
// (le sol du joueur). Meme contrat de fil que TirerDansLeDisque : `carte` est
// la carte DU BOT, lue depuis son propre fil. -> faux si repli sur le centre.
bool PointDApproche(Map* carte, Affectation const& a, float bx, float by, float& x, float& y, float& z);

// Tout fil, chemin chaud : PACTE de compagnon (option Directeur.Compagnons).
// Vrai si le bot `botLow` porte un pacte pour le joueur `uniteLow`, ou si
// `uniteLow` est un autre bot pacte pour le MEME joueur. Lu par
// PossibleTargetsValue::AcceptUnit, apres l'autodefense : un compagnon ne
// convoite ni son joueur ni les autres compagnons de ce joueur (un joueur High
// Risk porte le FFA, comme ~80 % des bots, 27/09 : sans groupe, rien d'autre ne
// l'en garde a 15-30 yd). Pose par le fil monde a la pose de TOUTE presence
// (etape, finale, `C-`), il SURVIT a sa levee (groupe, calmer, stop,
// remplacement) jusqu'a la fin de l'affectation + PACTE_MARGE_S, prolonge
// tant que le bot est dans le groupe du joueur ; il tombe aussi a la
// deconnexion du joueur, au bus eteint, a l'option eteinte. Sans pacte : un
// chargement atomique.
bool Pacte(uint32 botLow, uint32 uniteLow);

// Tire un point dans le disque, sol trouve et hors de l'eau (P-132), 6 essais,
// sinon le centre (valide a la pose). LAISSE DE HAUTEUR (26/09) : le sol est
// cherche depuis a.z + 2, puis a.z + 6 ; un point dont le sol s'ecarte de plus de 6 yd de a.z
// n'est garde que sous le ciel ouvert (sonde depuis max(sol, a.z) + 50) et en pente d'au plus 1 depuis le centre
// (sinon c'est le terrain brut sous une ville ou un pont : essai perdu, compte
// dans `tirages_refuses_hauteur`, qui cumule la laisse new rpg ET la chasse). `carte` : la carte DU BOT, lue depuis son
// propre fil de carte (ou le fil monde) ; nulle ou autre carte : le centre.
// `depuis` non nul : un point a plus de `distanceMax` (plan x, y) de `depuis`
// est rejete comme un point sans sol, dans la meme limite d'essais. La chasse
// s'en sert : le trajet est borne au CENTRE (1 400 yd), un point du disque peut
// en etre a 1 400 + rayon, au-dela de CoaChasseAction::DistanceMax. Le repli
// (le centre) n'est PAS reverifie : a l'appelant de le faire.
struct TirageDisque
{
    uint8 essais = 0;           // tirages faits (hors portee compris)
    uint8 refusesHauteur = 0;   // ... dont refuses par la laisse de hauteur
    bool repli = true;          // aucun point garde : le centre
};
TirageDisque TirerDansLeDisque(Map* carte, Affectation const& a, float& x, float& y, float& z,
                               Position const* depuis = nullptr, float distanceMax = 0.0f);

// Tout fil, chemin chaud : le point (carte, x, y) est-il dans le disque d'une
// affectation levee EN TRAJET depuis moins de TRAJET_LEVE_S secondes (ou en
// TRAJET et echue, pas encore levee), et hors du disque de l'affectation en
// cours s'il y en a une ? Le fil monde pose l'etat a chaque fin d'affectation
// en TRAJET (toutes raisons : lever, remplace, echeance, abandon, mort, stop,
// bus eteint) et le retire a l'echeance ; il n'ecrit JAMAIS rpgInfo (P-134).
// C'est l'IA du bot, proprietaire de rpgInfo, qui lit et remet son GO_GRIND a
// IDLE (NewRpgStatusUpdateAction::Execute, NewRpgBaseAction::MoveFarTo). Sans
// etat pose : un chargement atomique.
// `debutMs` : rpgInfo.startT du GO_GRIND (getMSTime a son ecriture). Seul un
// GO_GRIND ecrit AVANT la levee est vise : celui qu'un bot choisit ensuite, meme
// dans l'ancien disque, est le sien (sans cela, un bot arrive a une etape y
// serait remis a IDLE en boucle pendant TRAJET_LEVE_S).
bool TrajetLeveVers(uint32 guidLow, uint32 carte, float x, float y, uint32 debutMs);

// ------------------------------------------------------------------ lot D2b, suite
//
// TROIS COMPORTEMENTS QUI NE VALENT QUE QUAND LE DIRECTEUR A DEMANDE LE
// DEPLACEMENT. Chaque lecteur commence par la meme question, « ce bot a-t-il
// une affectation en TRAJET ? », qui sans affectation est UN chargement
// atomique : hors de ce cas, le code d'origine s'execute a l'identique.
//
// 1. TRAJET PACIFIQUE (option Directeur.TrajetPacifique). Vrai si le bot a une
//    affectation en TRAJET et se trouve sur la carte du disque (un champ de
//    bataille, une arene, une instance sont d'autres cartes : jamais pacifique
//    la-bas). Lu la ou une cible est CHOISIE, apres l'autodefense :
//    GrindTargetValue::FindTargetForGrinding (apres la boucle des assaillants),
//    EnemyPlayerValue::Calculate (etape 2 seule ; references de combat et
//    assaillants du groupe intacts), CoaChasseAction::ChoisirProie (traque).
//    Fil de carte du bot : le bot est lu sur son propre fil.
bool EnTrajetPacifique(Player const* bot);

// 2. RELEVE AU CIMETIERE (option Directeur.ReleveCimetiere). Le suivi du fil
//    monde, en levant une affectation pour `mort` alors qu'elle etait en
//    TRAJET, pose l'etat `mort_en_trajet` de ce bot (seul ecrivain : le fil
//    monde ; il tombe a la resurrection, a la deconnexion ou apres
//    ReleveSecondes). Lu par FindCorpseAction et ReviveFromCorpseAction : le
//    fantome va a l'esprit-guerisseur au lieu du cadavre. Tout fil.
bool MortEnTrajet(uint32 guidLow);

// Compteurs du bilan `coa directeur bus:`. Atomiques relaxes : incrementes
// depuis les fils de carte. L'ordre ne compte pas ; le bilan les nomme.
enum CoaDirCompteur : uint8
{
    COA_DIR_RECUES,
    COA_DIR_EXECUTEES,
    COA_DIR_REFUSEES,
    COA_DIR_EXPIREES,
    COA_DIR_LEVEES,
    COA_DIR_REMPLACEES,
    COA_DIR_ECARTE_TREVE,
    COA_DIR_ECARTE_SUSPENSION,
    COA_DIR_TELEPORTS_SUSPENDUS,
    COA_DIR_LAISSES_RPG,
    COA_DIR_TRAJETS_ARRIVES,
    COA_DIR_TRAJETS_ABANDONNES,
    COA_DIR_QUETES_TRAJET_EVITEES,
    COA_DIR_ENLISEMENTS_DIRIGES,
    COA_DIR_POSTES_AFFECTES,
    COA_DIR_RECONSTRUCTIONS,
    COA_DIR_RECONSTRUCTIONS_DIFFEREES,
    COA_DIR_REFUS_ARRET,
    COA_DIR_REFUS_BUDGET,
    COA_DIR_DIVERGENCES,
    COA_DIR_AFFECTATIONS_LEVEES_MORT,
    COA_DIR_AFFECTATIONS_LEVEES_HORS_LIGNE,
    COA_DIR_AFFECTATIONS_LEVEES_FIN_SUSPENSION,
    COA_DIR_ENLISEMENTS_RETENUS,
    COA_DIR_ECHUES,
    COA_DIR_ECARTES_TRAJET,          // choix de cible refuses en trajet pacifique
    COA_DIR_MORTS_EN_TRAJET,         // etats mort_en_trajet poses
    COA_DIR_RELEVES_RESSUSCITES,     // ... tombes parce que le bot est de nouveau en vie
    COA_DIR_RELEVES_LOIN_DU_CORPS,   // ... et relevees a plus de RELEVE_LOIN_YD du lieu de la mort
    COA_DIR_ETAPES_ARRIVEES,         // etapes d'itineraire atteintes (sans sejour)
    COA_DIR_TIRAGES_REFUSES_HAUTEUR, // points du disque refuses par la laisse de hauteur
    COA_DIR_RPG_REMIS_IDLE,          // GO_GRIND vers un trajet leve remis a IDLE par l'IA
    // Lot D3 (SPEC-directeur-D3.md § 3.3), en fin de la ligne bilan :
    COA_DIR_REPONSES,                // lignes `repondre` envoyees
    COA_DIR_PAROLES_LIVREES,         // paroles `faire_parler` dites
    COA_DIR_PAROLES_PERDUES,         // ... perdues a la livraison (raison au journal)
    COA_DIR_PEUPLEMENTS,             // affectations de presence posees
    COA_DIR_ARRIVEES_PRESENCE,       // ... arrivees dans leur disque (sejour)
    COA_DIR_ENLISEMENTS_PRESENCE,    // sauts d'enlisement retenus pour une presence
    COA_DIR_CIELS,                   // ciels poses (heure_du_jour)
    COA_DIR_METEOS,                  // meteos posees
    COA_DIR_REAFFIRMATIONS_METEO,    // paquets meteo renvoyes (reaffirmation)
    COA_DIR_OCCUPEES,                // affectations refusees `occupe` (§ 3.6)
    COA_DIR_LAISSES_PRESENCE,        // laisses de sejour d'un bot de presence
    // Compagnons (27/09), en fin de la ligne bilan :
    COA_DIR_COMPAGNONS,              // presences `C-` posees
    COA_DIR_APPROCHES,               // approches du joueur tirees par l'IA (bande 15-30 yd quittee)
    COA_DIR_ECARTE_PACTE,            // cibles ecartees par un pacte de compagnon
    COA_DIR_NB_COMPTEURS
};
void Compter(CoaDirCompteur c);

// ------------------------------------------------------------------ lot D3
//
// SPEC-directeur-D3.md, § 2.8, § 3, § 4.2, § 5.2, § 6.2-6.3. Tout est inerte
// tant que le verbe n'est pas dans AiPlayerbot.Directeur.Verbes, et tant que la
// migration 2026_09_26_01_coa_directeur_d3.sql n'est pas appliquee (garde de
// colonnes au demarrage : sans elle, refus `colonnes_d3`). Nouveaux etats, TOUS
// au fil monde seul, sans verrou : la file de paroles, les ciels et les meteos
// par joueur (un seul ecrivain, un seul lecteur : le fil monde -- Consignes et
// `joueur,<guid>` du 8888 y sont executes, P-137). L'affectation de presence
// (`peupler`) vit dans le magasin des affectations, meme verrou, meme patron.
//
// Requete neuve du 8888 (lecture seule, fil monde) :
//   joueur,<guid> -> guid,regle,equipe,race,classe,niveau,groupe,mj,bg,ffa,pvp,arret,ciel,meteo
// `absent` si le personnage n'est pas en jeu, `invalid` si mal formee OU bus eteint.
}  // namespace CoaDirecteur

void AddSC_coa_directeur();

#endif
