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
 * 7. (Lot D4c) Les verbes `prime` et `chasser`, la garde de colonnes D4 et les
 *    morts tracees d'humains (`coa directeur mort:`, option TracerMorts) :
 *    coa-project/docs/SPEC-directeur-D4.md, § 3.2-3.4, § 6.2-6.3, et la voie
 *    retenue pour D4-0 (§ 10) : un chasseur EN TRAJET est compose en mode RPG.
 *
 * LA REGLE : un seul proprietaire par etat (P-134). Le drapeau d'arret n'est
 * ecrit que par ce fichier. Aucun champ du coeur ni de playerbots n'est touche.
 */

#ifndef COA_DIRECTEUR_H
#define COA_DIRECTEUR_H

#include "Define.h"
#include <string>
#include <vector>

class Map;
class Player;
class Unit;
class WorldLocation;
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
// Lot D4c : vrai AUSSI pour un chasseur (verbe `chasser`) dont l'affectation
// est EN TRAJET -- la marche vers la scene est celle de new rpg (etapes E-,
// laisse, trajet pacifique), la seule que les mesures aient montree ; a
// l'arrivee (SEJOUR), faux : la fabrique lui rend sa chasse. Le fil monde
// appelle ResetStrategies a chaque bascule (pose, arrivee, fin en trajet).
bool MercenaireEnRpg(uint32 guidLow);

// ------------------------------------------------------------------ lot D4c
//
// Verbe `prime` (SPEC-directeur-D4.md § 6.2) : magasin `primes`, cle le guid du
// joueur, ECRIT par le fil monde seul sous le verrou exclusif des consignes,
// LU par les fils de carte sous le verrou partage apres un compteur atomique.
// Verbe `chasser` (§ 6.3) : une affectation (meme magasin, meme suivi que
// `reaffecter`), drapeau Affectation::chasseur.
//
// Tout fil, chemin chaud : le BOT `botLow` est-il un chasseur designe, dont la
// prime court encore ? `joueur` (non nul) recoit le guid du joueur prime. Sans
// chasseur : UN chargement atomique. Lu par la chasse (GUET : proie lue par son
// guid ; traque plus longue), NearestEnemyPlayersValue::AcceptUnit (aucune
// autre proie ; meute bornee a Directeur.PrimeMeute) et
// PossibleTargetsValue::AcceptUnit (filtre de niveau saute pour ce couple).
// `id` (non nul) recoit l'id de la commande `chasser` en cours (journal).
// Reserve de chasseurs (02/10, CONCEPTION-reserve-chasseurs.md § 2.5) : FAUX
// aussi quand le joueur prime est AFK (PLAYER_FLAGS_AFK) -- pour TOUTE prime,
// reserve ou non (revue M1). L'AFK n'est pas lu ici
// (fil de carte : le joueur peut etre sur une autre carte, P-137) : le fil monde
// le recopie chaque seconde dans la prime (SuivrePrimes), et ce lecteur ne lit
// que cette copie, sous le verrou partage.
bool ProieImposee(uint32 botLow, uint32* joueur, uint32* id = nullptr);

// Reserve de chasseurs (02/10). Tout fil, chemin chaud : le BOT `botLow` est-il
// un chasseur designe dont la prime court, AFK ou non ? TOUT chasseur de prime,
// de la reserve ou non (revue M1, voulu par le commanditaire) : protection AFK,
// aucune creature choisie, decrochage en combat valent pour tous ; seuls la
// traque sans attente ni borne et la composition de chasse en trajet sont
// propres a la reserve (`reserve`). `afk` (non nul) recoit
// l'etat AFK du prime (copie du fil monde, au plus 1 s de retard) ; `reserve`
// (non nul) dit si le chasseur est de la RESERVE (guilde de
// Directeur.ReserveGuildes, figee a la pose de l'affectation, au fil monde).
// ProieImposee == ChasseurDe && !afk. Sans chasseur : un chargement atomique.
// Lecteurs : CoaChasseAction (tenue a distance d'un prime AFK, traque sans
// borne et sans attente d'un chasseur de reserve), NearestEnemyPlayersValue
// (un chasseur dont le prime est AFK ne convoite personne, riposte permise),
// GrindTargetValue (aucune creature choisie par un chasseur).
// Reserve de chasseurs (02/10, revue m2). Tout fil : le bot a-t-il ete PREPARE
// (`chasser` `P-` fait : niveau ou carte) il y a moins de 60 s ? Lu par
// RandomPlayerbotMgr::RandomTeleport : jamais de teleportation aleatoire entre la
// preparation et le saut du service. Aucune preparation faite : un chargement.
bool PrepareRecemment(uint32 guidLow);

// Revue M4 (02/10). Tout fil, chemin chaud : le bot est-il AU REPOS apres une
// preparation hors prime `P-0` (30 min, ou jusqu'a la pose d'un `chasser`) ? Lu par
// PossibleTargetsValue::AcceptUnit : il ne convoite alors aucun joueur, riposte
// permise. PrepareRecemment le couvre aussi. Sans repos : un chargement atomique.
bool EnRepos(uint32 guidLow);

bool ChasseurDe(uint32 botLow, uint32* joueur = nullptr, uint32* id = nullptr, bool* afk = nullptr,
                bool* reserve = nullptr);

// ------------------------------------------------------------------ banc d'essai de prime (03/10/2026)
//
// Option Directeur.PrimeBanc (guids bas de BOTS, vide par defaut). Le fil monde la
// relit chaque seconde (SuivreBancs) et en tient une COPIE sous le verrou des
// consignes ; tout fil la lit ici, apres un compteur atomique (sans banc : un
// chargement). Un bot de banc est la cible d'un banc de prime : il est traite comme
// un humain prime pour la prime SEULEMENT (ExecuterPrime, SuivrePrimes, ligne
// `mort:`, `recompenser`) ; lecteurs hors Directeur : AiFactory (composition
// immobile), PossibleTargetsValue (il ne convoite personne, riposte permise),
// RandomPlayerbotMgr (ni teleportation aleatoire, ni Randomize). Un humain liste
// n'est JAMAIS traite en banc (Humain() est teste par le Directeur avant).
bool CibleDeBanc(uint32 guidLow);
// 04/10 : bot prepare par `P-0` hors prime (cible de contrat) : immobile (« stay » + riposte)
// le temps de son repos. Tout fil (AiFactory), apres un compteur atomique.
bool CibleImmobile(uint32 guidLow);

// ------------------------------------------------------------------ retraite (03/10/2026)
//
// A TOUTE fin de prime, les chasseurs de la RESERVE qui l'ont servie (vivants ou
// morts, encore en ligne, sans autre affectation `chasser`) se retirent : repos
// (aucune convoitise, aucune teleportation aleatoire), releve au cimetiere s'ils
// sont morts, puis, hors de la vue de tout humain (SAUT_HUMAIN_YD), un saut cache
// loin (ChercherPointCarte, 600-1 200 yd). Etat ecrit par le fil monde seul. Tout
// fil, chemin chaud : le bot `botLow` est-il en retraite ? `fuirDe` (non nul) recoit
// le guid de l'humain le plus proche a eviter (0 : aucun). Lu par CoaChasseAction,
// qui l'eloigne a pied (RETRAITE_YD). Sans retraite : un chargement atomique.
bool EnRetraite(uint32 botLow, uint32* fuirDe = nullptr);
// Distance a laquelle un chasseur en retraite s'eloigne de l'humain le plus proche
// (CoaChasse.h) : au-dela de SAUT_HUMAIN_YD (150, CoaDirecteur.cpp), garde du saut.
constexpr float RETRAITE_YD = 175.0f;

// ------------------------------------------------------------------ embuscade (02/10)
//
// Option Directeur.Embuscade (defaut 1). UN ETAT PAR PRIME, « assaut donne »,
// ECRIT par le fil monde seul (SuivreEmbuscades, chaque seconde, cartes a
// l'arret : il lit le joueur et ses chasseurs, P-137), sous le verrou exclusif
// des consignes, avec la PLACE de chaque chasseur (indice dans
// EMBUSCADE_ANGLES_DEG, attribuee a son arrivee, rendue a son depart). Les fils
// de carte le LISENT (Embuscade) sous le verrou partage, apres le compteur
// atomique des chasseurs. Avant l'assaut, aucune attaque d'initiative du chasseur
// sur son prime (NearestEnemyPlayersValue::AcceptUnit) ; la riposte reste.
struct EtatEmbuscade
{
    uint32 joueur = 0;       // guid bas du prime
    uint32 id = 0;           // commande `chasser` en cours (journal)
    uint32 prime = 0;        // commande `prime` (id de la prime : borne des replis)
    uint32 plafond = 1;      // chasseurs de la prime, au plus (meute de l'assaut)
    bool reserve = false;
    bool assaut = false;     // l'assaut de CETTE prime est donne
    char raison = '-';       // a, b, c, d (voir SuivreEmbuscades)
    uint8 place = 0xFF;      // 0xFF : pas encore attribuee (la seconde suivante)
};

// Angles d'attente, en degres, relatifs a l'orientation du joueur (0 = devant) :
// l'arriere puis les flancs, hors du cone de 120 degres devant lui.
constexpr uint8 EMBUSCADE_PLACES = 8;
constexpr float EMBUSCADE_ANGLES_DEG[EMBUSCADE_PLACES] = {180.0f, 120.0f, 240.0f, 150.0f, 210.0f, 90.0f, 270.0f, 165.0f};

// Tout fil, chemin chaud : le BOT `botLow` est-il un chasseur dont la prime court,
// joueur non AFK, option Directeur.Embuscade ET chasse allumees (WildPvp.Chasse et
// WildPvp.Traque) ? Sans chasseur : un chargement atomique. `sortie` (non nul)
// n'est ecrite que si la reponse est vraie.
bool Embuscade(uint32 botLow, EtatEmbuscade* sortie);

// Le point d'attente d'un chasseur : a `distance` du joueur (jx, jy, jz), sur
// l'angle ABSOLU `angle` (radians), sol sec a +-6 yd du joueur (P-132, P-145),
// pas sous un toit, en ligne de vue du joueur (modeles). Essaie l'angle, puis
// +-15 et +-30 degres, a distance, distance - 5 et distance + 5 ; -> faux si
// aucun (x, y, z restent le premier candidat, non verifie). `carte` : la carte
// DU BOT, depuis son fil (GetGridHeight peut creer la grille, comme
// TirerDansLeDisque). Arithmetique et sondes de terrain seulement.
bool PointDEmbuscade(Map* carte, float jx, float jy, float jz, float angle, float distance, float& x, float& y,
                     float& z);

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

// Missions de l'animation (30/09, option Directeur.Missions) : ce que le bot
// vient faire au lieu de sa presence. Portee par la FINALE `Q-` (colonne
// `texte`, parametre `valeur`) ; ouverte : un evenement de zone (vortex, etude
// a venir) sera un type de plus. Aucune : presence ordinaire, code d'avant.
enum class CoaDirMission : uint8
{
    Aucune,
    Quete,      // valeur : id de la quete, incomplete au journal du bot
    Chasse,     // un camp de creatures de son niveau au centre du disque
    Recolte,    // valeur : metier (182 herboristerie, 186 minage), noeuds au centre
    Passage,    // il traverse la scene, puis s'en va a l'echeance
    Bande,      // valeur : guid du CHEF (une presence du meme joueur) ; le centre suit le chef arrive
    Evenement   // lot F des evenements (06/10) : valeur 0 ; missionParam = guid du JOUEUR dont il sert
                // l'evenement (option Directeur.EvenementBots ; eteinte : degradee en Chasse)
};

// Missions : la laisse d'un bot en mission s'etend a rayon + MISSION_LAISSE_MARGE_YD ;
// un suiveur de bande se tient a BANDE_MAX_YD de son chef, et vise BANDE_CIBLE_YD.
constexpr float MISSION_LAISSE_MARGE_YD = 30.0f;
constexpr float BANDE_MAX_YD = 15.0f;
constexpr float BANDE_CIBLE_YD = 6.0f;
// Laisse d'un suiveur : au-dela de BANDE_CIBLE_YD + 10 yd (arrivee de GO_GRIND).
constexpr float BANDE_LAISSE_YD = 25.0f;

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
    // Teleportation cachee (27/09, option Directeur.TeleportCache) : presence
    // `peupler` d'action `T-<n>`, une ETAPE (option Directeur.Etapes exigee).
    // Le bot a ete TELEPORTE dans son disque a la pose, par le fil monde, hors
    // de la vue du joueur (ExecuterAffectation). `sautMs` : getMSTime() au saut,
    // meme horloge que rpgInfo.startT. Lecteur : NewRpgStatusUpdateAction::Execute,
    // qui remet a IDLE un statut RPG choisi AVANT le saut (il menerait le bot
    // loin du point). Figes a la pose.
    bool saut = false;
    uint32 sautMs = 0;
    // Lot D4c (verbe `chasser`, SPEC-directeur-D4.md § 6.3) : un MERCENAIRE
    // designe chasseur du joueur `joueur` de l'affectation, qui porte une
    // prime. Fige a la pose. Lecteurs : NewRpgBaseAction::MoveFarTo (saut
    // d'enlisement TOUJOURS retenu, comme une presence), la fabrique par
    // MercenaireEnRpg (EN TRAJET, le chasseur est compose en mode RPG : c'est
    // new rpg qui le mene, etape par etape -- voie retenue apres l'echec de
    // D4-0), et ProieImposee (chasse, « nearest enemy players », filtre de
    // niveau).
    bool chasseur = false;
    // Reserve de chasseurs (02/10) : chasseur dont la guilde est dans
    // Directeur.ReserveGuildes, lu au fil monde A LA POSE et fige (les fils de
    // carte ne lisent jamais la chaine de conf). Faux hors `chasser`. Lecteurs :
    // ChasseurEnTrajet (un chasseur de reserve n'est JAMAIS compose en mode RPG :
    // il garde sa chasse en trajet), EnTrajetPacifique (jamais pacifique : il ne
    // choisit de toute facon que son prime), ChasseurDe (traque sans attente au
    // poste, sans borne de duree).
    bool reserve = false;
    // Missions (30/09, option Directeur.Missions) : posee avec la FINALE d'une
    // presence (`peupler` `Q-`), lue en SEJOUR par NewRpgStatusUpdateAction
    // (statuts tires, laisse), NewRpgWanderRandomAction (errance dans le
    // disque). Le fil monde seul l'ecrit : a la pose, puis quand elle devient
    // caduque (quete finie, chef parti : degradee en Chasse). Pour une Bande,
    // le centre (x, y, z) suit le chef, recale par le meme suivi.
    CoaDirMission mission = CoaDirMission::Aucune;
    uint32 missionParam = 0;
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
                               Position const* depuis = nullptr, float distanceMax = 0.0f,
                               bool hauteurStricte = false);
// Missions (30/09) : `hauteurStricte` vrai, un sol a plus de TIRAGE_ECART_Z (6 yd)
// du centre est TOUJOURS refuse, ciel ouvert et pente douce compris : la laisse
// de sejour d'une presence en mission ne mene plus un bot au flanc d'une colline
// (mesure du 30/09 : points a z + 40 d'un centre en plaine). Le trajet garde la
// regle d'avant (un chemin peut monter).

// Missions, bande : le point ou un suiveur en (bx, by) rejoint son chef (le centre
// de `a`) : sur la droite centre -> suiveur, a `distance`, sol a +-6 yd du centre ;
// sinon +-35 puis +-70 degres ; sinon le centre. Meme contrat de fil que
// PointDApproche (qui l'appelle a COMPAGNON_CIBLE_YD). -> faux si repli.
bool PointPres(Map* carte, Affectation const& a, float bx, float by, float distance, float& x, float& y, float& z);

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

// 3. LE SAUT (reutilise par le saut d'enlisement de NewRpgBaseAction::MoveFarTo
//    et par la teleportation cachee `T-` d'ExecuterAffectation) : retire les
//    auras rompues par une teleportation, puis Player::TeleportTo. Un seul code
//    pour les deux sauts. Le fil de l'appelant : celui du bot (MoveFarTo) ou le
//    fil monde, cartes a l'arret (ExecuterAffectation, P-137).
bool Sauter(Player* bot, WorldLocation const& dest);

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
    // Teleportation cachee (27/09), en fin de la ligne bilan :
    COA_DIR_TELEPORTS_CACHES,        // presences `T-` posees : bot teleporte hors de la vue du joueur
    COA_DIR_SAUTS_REMIS_IDLE,        // statuts RPG d'avant le saut remis a IDLE par l'IA (NewRpgAction)
    // Lot D4c (SPEC-directeur-D4.md § 3.4), en fin de la ligne bilan :
    COA_DIR_PRIMES,                  // primes posees
    COA_DIR_CHASSEURS,               // affectations `chasser` posees (etapes comprises)
    COA_DIR_PRIMES_TOMBEES,          // fins `tombe` (le joueur prime est mort)
    COA_DIR_PRIMES_ECHUES,           // fins a l'echeance
    COA_DIR_PRIMES_LEVEES,           // autres fins (lever, arret, bus eteint, hors ligne, regle, defi)
    COA_DIR_PROIES_IMPOSEES,         // traques lancees sur le joueur prime (une par traque)
    COA_DIR_TRAQUES_PRIME,           // ... arrivees a portee d'engagement
    COA_DIR_MEUTE_PERMISE,           // candidats admis par la meute au-dela de MaxAttackersPerTarget
    COA_DIR_MORTS_TRACEES,           // lignes `coa directeur mort:` ecrites
    COA_DIR_MORTS_PERDUES,           // evenements de mort perdus (file pleine)
    // 30/09, en fin de la ligne bilan :
    COA_DIR_TELEPORTS_LOINTAINS,     // presences `L-` posees : bot teleporte a 250-350 yd du joueur
    COA_DIR_PAROLES_HORS_VUE,        // dire/crier differes : le joueur n'a pas le locuteur a son client
    // Missions (30/09), en fin de la ligne bilan :
    COA_DIR_MISSIONS,                // finales `Q-` posees avec une mission
    COA_DIR_MISSIONS_IGNOREES,       // ... ignorees (option eteinte, type inconnu)
    COA_DIR_MISSIONS_DEGRADEES,      // missions devenues caduques (quete finie, chef parti) : Chasse
    COA_DIR_PATROUILLES,             // errances d'un bot en mission vers un point a plat de son disque
    COA_DIR_BANDES_RECALEES,         // centres de suiveurs recales sur leur chef
    COA_DIR_BANDES_REJOINTES,        // suiveurs repartis rejoindre leur chef (laisse de bande)
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
//   joueur,<guid> -> guid,regle,equipe,race,classe,niveau,groupe,mj,bg,ffa,pvp,arret,ciel,meteo,afk
// (`afk`, 15e champ, 02/10 : 1 si PLAYER_FLAGS_AFK, EN FIN de ligne)
// `absent` si le personnage n'est pas en jeu, `invalid` si mal formee OU bus eteint.

// ------------------------------------------------------------------ lot F des evenements (06/10/2026)
//
// LES BOTS PARTICIPENT A L'EVENEMENT (coa-project/docs/EVENEMENTS-lot-F.md ; grimoire § 2.7, § 5.2).
// INSCRIPTION (G1) : la FINALE `Q-` d'une presence `peupler` portant la mission `evenement` (texte
// `evenement`, valeur 0), option Directeur.EvenementBots ET Directeur.Missions. L'affectation EST le
// registre : « ce bot sert l'evenement du joueur `missionParam`, au point (x, y), rayon `rayon` ». Elle
// en garde tout le cycle de vie (trajet, sejour, laisse rayon + MISSION_LAISSE_MARGE_YD, levee par
// `lever`, echeance, mort, joueur hors ligne, stop, bus eteint). Un bot DEJA proche du joueur peut etre
// inscrit : l'inscription est exemptee du refus `a_vue` et de la borne basse du centre (40 yd du joueur :
// son centre est le point de l'evenement), option allumee seulement.
// CIBLES (G2, G3) : le fil monde publie chaque seconde les invoques VIVANTS et non retires du lot D par joueur (copie
// sous un verrou propre). CibleDEvenement, appelee par GrindTargetValue::FindTargetForGrinding APRES les
// assaillants et AVANT le trajet pacifique, rend le plus proche invoque de l'evenement servi : a
// EVENEMENT_ENGAGEMENT_YD du bot au plus, dans la laisse (rayon + 30 yd du centre), attaquable et en
// vue -- SANS les filtres de niveau (+4), de rang, de gris, de hauteur, d'assistance, ni la borne de
// 30 yd des statuts RPG actifs. Fil de carte du bot (meme carte que l'invoque : GetCreature par le bot).
// Sans inscription publiee : UN chargement atomique, puis le code d'origine.
constexpr float EVENEMENT_ENGAGEMENT_YD = 50.0f;
Unit* CibleDEvenement(Player* bot);

// Bots ACTIFS (07/10/2026, coa-project/docs/EVENEMENTS-bots-actifs.md). Retour du commanditaire apres la
// Faille du 06/10 (23:07-23:11 Paris) : « Ils m'ont regarde mourir sans bouger ». LU : CibleDEvenement n'est
// interrogee que par `grind target`, donc par « no target » -> « attack anything » (4,0) du moteur HORS combat,
// que AttackAnythingAction::isUseful coupe des que le bot est en combat (bot->IsInCombat()) ; en combat, et hors
// combat par « dps assist » (50,0) / « tank assist », la cible vient de la valeur « attackers », qui ne contient
// que les assaillants du BOT et de son groupe : un invoque qui frappe le joueur (hors groupe) n'y est jamais.
// AssaillantsDEvenement est appelee par AttackersValue::Calculate pour un bot INSCRIT (meme garde que
// CibleDEvenement) et AJOUTE aux assaillants, avant le tri d'origine (RemoveNonThreating : vivant, attaquable,
// en vue) : (b) DEFENSE d'abord -- les invoques de l'evenement dont la victime est le joueur servi ou un autre
// inscrit du meme evenement, a EVENEMENT_DEFENSE_YD du bot et a EVENEMENT_DEFENSE_CENTRE_YD du centre ; s'il y
// en a, eux SEULS (ils deviennent la cible du choix d'origine, DpsTargetValue / TankTargetValue) ; sinon (a)
// tout invoque a EVENEMENT_ENGAGEMENT_YD du bot et dans la laisse (rayon + 30). Un combat legitime n'est pas
// interrompu : ses assaillants restent dans la liste, le choix d'origine les departage. Fil de carte du bot.
// Sans inscription publiee : UN chargement atomique.
constexpr float EVENEMENT_DEFENSE_YD = 80.0f;
constexpr float EVENEMENT_DEFENSE_CENTRE_YD = 100.0f;
// (c) jamais plante : un inscrit en SEJOUR qui n'a rien a frapper rejoint le joueur servi (point a
// EVENEMENT_RALLIEMENT_CIBLE_YD de lui, cote bot) s'il en est a plus de EVENEMENT_RALLIEMENT_YD et que le joueur
// est dans la laisse ; jamais de repos assis (NewRpgAction.cpp).
constexpr float EVENEMENT_RALLIEMENT_YD = 15.0f;
constexpr float EVENEMENT_RALLIEMENT_CIBLE_YD = 8.0f;
void AssaillantsDEvenement(Player* bot, std::vector<Unit*>& sortie);
// Le joueur servi par un inscrit en sejour, s'il est sur la carte du bot, vivant, et dans la laisse : sa position.
// -> faux sinon (pas inscrit, pas en sejour, joueur absent, mort, hors laisse). Fil de carte du bot.
bool JoueurDEvenement(Player* bot, float& x, float& y, float& z);
}  // namespace CoaDirecteur

void AddSC_coa_directeur();

#endif
