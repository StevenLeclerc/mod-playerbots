/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COAAIOBJECTCONTEXT_H
#define _PLAYERBOT_COAAIOBJECTCONTEXT_H

#include "AiObjectContext.h"
#include "ObjectGuid.h"

#include <ctime>
#include <unordered_map>
#include <vector>

class PlayerbotAI;
class SpellInfo;

/*
 * One ability of a bot's kit: an ability of its class that it has reached and really
 * knows, kept between ticks by CoaAiObjectContext below.
 *
 * `firstSpellId` is the first rank of the spell, which is what tells two ranks of the
 * same ability apart from two different abilities: only the highest rank reached is
 * ever cast.
 */
struct CoaKnownAbility
{
    SpellInfo const* info;
    uint16 kind;
    uint32 dispelMask;
    uint32 firstSpellId;
};

/*
 * CoaArene IA (lots L0 et L1, 10/10/2026, Ai/Class/Coa/CoaAreneIa.inc) : l'etat d'un
 * engagement JcJ (arene, duel, et en option le un contre un du monde ouvert), propre a
 * un bot. Ecrit et lu par la seule mise a jour d'IA de ce bot (fil monde), donc sans
 * verrou, comme benchedSpells. Inerte tant qu'aucune option AiPlayerbot.CoaArene.* n'est
 * armee : rien ne l'ecrit.
 */
// Survie d'arene (11/10, match id=10) : fenetre du « pire coup recent » note par le crochet de degats (CoaArene1v1.cpp)
// et lu par CoaAreneMaj (CoaAreneIa.inc).
constexpr uint32 CoaAreneSurviePireCoupMs = 15000;

struct CoaAreneEtat
{
    uint32 majMs = 0;              // getMSTime() de la derniere mise a jour (0 : jamais)
    uint8 cadre = 0;               // CoaAreneCadre (CoaAreneIa.inc) ; 0 = aucun
    bool preparation = false;      // arene portes fermees, ou duel en compte a rebours
    bool vuCeTick = false;         // l'engagement est vu au tick courant (hors grace) : condition de L1
    bool actif = false;            // engagement en cours, delai de grace compris
    bool bras = false;             // tire a l'ouverture de l'engagement : IA appliquee ou non
    // Instance d'arene de l'engagement (0 : duel ou monde ouvert). Une autre instance vue pendant le delai de grace
    // clot l'engagement (bilan) et en ouvre un neuf (banc du 10/10 : deux matchs a < 10 s fusionnes en un bilan).
    uint32 instance = 0;
    uint32 debutMs = 0;
    uint32 dernierContactMs = 0;   // dernier tick ou l'engagement a ete vu (hors preparation)
    ObjectGuid adversaire;
    // Recul en cours (L1) : fin prevue, distance de depart, dernier essai.
    uint32 reculFinMs = 0;         // 0 : aucun recul en cours
    uint32 reculDebutMs = 0;
    uint32 dernierEssaiReculMs = 0;
    float reculDistAvant = 0.0f;
    float reculVise = 0.0f;
    uint32 prochaineOuvertureMs = 0;
    uint32 prochainePreparationMs = 0;
    uint32 dernierTenirMs = 0;
    uint32 dernierImpossibleMs = 0;
    // Recul (revue du 10/10 soir) : distance que la poursuite ralentie permet d'atteindre, et la visee d'origine.
    float reculViseMax = 0.0f;
    // Retour a portee et en vue (L1, 10/10 soir) : dernier essai, journal, mouvement en cours.
    uint32 dernierRetourMs = 0;
    uint32 dernierJournalRetourMs = 0;
    bool retourEnCours = false;
    // Burst : depuis quand la pile est pleine sans fenetre (0 : pas pleine).
    uint32 pilePleineDepuisMs = 0;
    // L2, ligne de vue (CoaArene.IaPiliers) : cachette en cours, raison, pauses et cumul.
    uint32 pilierDebutMs = 0;      // 0 : aucune cachette en cours
    uint8 pilierRaison = 0;        // 1 : vie basse ; 2 : sorts en recharge
    uint32 derniereSortiePilierMs = 0;
    uint32 dernierEssaiPilierMs = 0;
    uint32 dernierJournalPilierMs = 0;
    uint32 pilierCumulMs = 0;
    // Trace des gladiateurs (AiPlayerbot.Arene1v1.Trace, 10/10 soir) : decision prise une fois par instance d'arene,
    // debit borne par seconde (CoaAreneIa.inc, CoaAreneTraceActive).
    uint32 traceInstance = 0;      // instance d'arene de la decision (0 : aucune)
    bool traceOui = false;         // ce bot est un gladiateur engage dans cette arene 1v1
    uint32 traceAuraPile = 0;      // aura a charges que le kit consomme (Advantage du Ranger) ; 0 : aucune
    uint32 traceSeconde = 0;       // getMSTime() / 1000 de la fenetre de debit
    uint8 traceLignes = 0;         // lignes ecrites dans cette seconde
    uint32 traceOmises = 0;        // lignes tues par le debit depuis la derniere ecrite
    uint32 traceTickMs = 0;        // derniere ligne « tick »
    // Defensif d'arene (10/10 soir) : vie releve toutes les 2 s, pour reconnaitre un burst subi.
    float pvRef = 100.0f, pvPrec = 100.0f;
    uint32 pvRefMs = 0;
    // Bilan de l'engagement, ecrit a sa cloture.
    uint32 echantillons = 0, auContact = 0, dansLaPlage = 0;
    uint32 reculs = 0, reculsReussis = 0, ruptures = 0, bursts = 0, ouvertures = 0;
    uint32 retours = 0, piliers = 0;
    float pvMin = 100.0f;
    // Banc d'arene 1v1, mode cacheur (CoaAreneIa.inc ; `.coa arene banc ... cacheur=A|B`) : decision lue une fois
    // par instance (CoaArene1v1::CacheurDeBanc), deplacement vers une cachette en cours, essais, dernier tir.
    uint32 cacheurInstance = 0;    // instance d'arene de la decision (0 : aucune)
    bool cacheurOui = false;
    uint32 cacheurEnRouteMs = 0;   // 0 : aucun deplacement vers une cachette en cours
    uint32 cacheurEssaiMs = 0;
    uint32 cacheurTirMs = 0;
    uint32 cacheurJournalMs = 0;   // debit de la ligne « cacheur_aucun »
    uint32 cacheurCaches = 0, cacheurTirs = 0, cacheurAucun = 0, cacheurVu = 0;
    // Familier (11/10, CoaAreneIa.inc « coa arene familier ») : prochain essai d'invocation (0 : tout de suite), kit
    // sans aucune invocation permanente (lu une fois par engagement : le kit ne change pas pendant un match),
    // invocations reussies (bilan).
    uint32 prochainFamilierMs = 0;
    bool familierAucun = false;
    uint32 familiers = 0;
    // Revue c2 : un familier a ete vu vivant (pour reconnaitre sa perte), et l'instant de cette perte (0 : aucune).
    bool familierVu = false;
    uint32 familierPerduMs = 0;
    // 11/10 (Necromancer 453, match id=30 : aucune ligne « familier » ni « familier_refus ») : debit de la ligne
    // « familier_attente » (pourquoi le declencheur reste muet), une par raison toutes les 5 s.
    uint32 familierAttenteMs = 0;
    uint8 familierAttenteRaison = 0;
    // Lot 19 (11/10, ouvertures) : monture de classe lue une fois par engagement (0 : aucune, ou pas d'attaque montee),
    // essais d'attaque montee a portee sans rien lancer (au troisieme, le bot descend), debit de « ouverture_refus ».
    bool montureLue = false;
    uint32 montureClasseId = 0;
    uint8 ouvertureMonteeEssais = 0;
    uint32 ouvertureMonteeDebutMs = 0;  // premier tick monte portes ouvertes (plafond CoaAreneOuvertureMonteeMaxMs)
    uint32 ouvertureRefusMs = 0;
    uint8 ouvertureRefusRaison = 0;
    // Combat contre un humain (11/10, CoaAreneIa.inc, arene 1v1 et duel, CoaArene.IaCommune) : interruption,
    // dissipation sur soi, fenetre de burst apres un controle. Bilan : compteurs ci-dessous.
    // Incantation adverse suivie par CoaAreneMaj (les deux bras) : identite de la derniere vue (sort et adresse du
    // Spell, jamais dereferencee), pour compter chaque incantation une fois ; echec d'interruption deja note pour elle.
    uint32 incantationAdvSort = 0;
    uintptr_t incantationAdvSpell = 0;
    // Banc du 11/10 (tour 7) : l'incantation dont l'interruption a deja ete jugee (meme identite sort + adresse), et si
    // elle est comptee ratee. Une ratee suivie d'un succes sur la MEME incantation (recharge globale finie 250 ms plus
    // tard) est decomptee : avant, elle restait ratee (4 cas sur 24 au tour 7). Remis a zero par CoaAreneMaj a chaque
    // nouvelle incantation vue.
    uint32 interruptionJugeeSort = 0;
    uintptr_t interruptionJugeeSpell = 0;
    bool interruptionJugeeRatee = false;
    uint32 interruptionsRattrapees = 0;
    uint32 prochaineInterruptionMs = 0;
    uint32 prochainDispelSoiMs = 0;
    // Fenetre de burst en cours : aura de controle qui l'a ouverte, fin prevue (getMSTime), un sort deja lance dedans.
    uint32 fenetreAura = 0;
    uint32 fenetreFinMs = 0;
    bool fenetreExploitee = false;
    uint32 incantationsVues = 0, soinsVus = 0, interruptions = 0, interruptionsSoin = 0, interruptionsRatees = 0;
    uint32 dispelsSoi = 0, fenetresVues = 0, fenetresBurst = 0, fenetreSorts = 0;
    // Choix tactiques (11/10, match id=89 ; CoaAreneRefusTactique) : sorts sans effet ecartes (bilan), et derniere
    // trace de refus par sort (une ligne toutes les 2 s par sort au plus, sinon la trace serait noyee).
    uint32 refusTactiques = 0;
    std::unordered_map<uint32, uint32> refusTactiqueTraceMs;
    // Retour en urgence (11/10, match id=90) : derniere relance hors du delai d'une seconde.
    uint32 retourUrgenceMs = 0;
    // Survie d'arene (11/10, AiPlayerbot.CoaArene.IaSurvie ; CoaAreneIa.inc « coa arene survie ») : palier courant
    // (0 aucun, 1 pression, 2 danger, 3 critique) et sa raison, recalcules par CoaAreneMaj ; sorts lances par palier
    // (index 1 a 3), soins sur soi ; fuite hors de vue en palier critique (en cours, dernier essai, fin, cumul).
    uint8 survieNiveau = 0;
    char const* survieRaison = "-";
    uint32 survieSorts[4] = { 0, 0, 0, 0 };
    uint32 soinsSoi = 0;
    uint32 survieFuiteDebutMs = 0;     // 0 : aucune fuite en cours
    uint32 survieFuiteEssaiMs = 0;
    uint32 survieFuiteFinMs = 0;
    uint32 survieFuiteCumulMs = 0;
    uint32 survieFuites = 0;
    // Pire coup subi recemment (11/10, match id=10 : un tick de Melt Reality a retire 38 % de vie d'un coup), en % de la
    // vie maximale, et son heure ; note par le crochet de degats (CoaArene1v1.cpp), lu par CoaAreneMaj pour hausser les
    // seuils des paliers : seuil danger courant (CoaAreneSurvieDangerPv ou plus).
    float pireCoupPct = 0.0f;
    uint32 pireCoupMs = 0;
    float survieSeuilDanger = 50.0f;
    uint32 survieControles = 0;        // controles lances par la survie faute de soin ou de defensif (bilan)
    uint32 survieControleMs = 0;       // dernier controle de secours (lot 18 : au plus un par CoaAreneSurvieControleFenetreMs)
    uint32 survieArretMs = 0;          // dernier « survie_arret » (au plus un toutes les 2 s)
    uint32 retourVuDepuisMs = 0;       // retour d'un tireur : adversaire en vue sans interruption depuis (0 : pas en vue)
    // Dissipation sur soi, politique de cout (IaSurvie) : attente apres un succes (3 -> 6 -> 12 s, plafond 15 s, doublee
    // a chaque reapplication vue dans les 4 s), derniere dissipation reussie, DoT qu'elle visait, reapplication deja
    // comptee ; refus par raison (1 pv haut, 2 mana basse, 3 pas rentable, 4 reapplication), comptes au plus une fois
    // par raison et par 2 s.
    uint32 dispelAttenteMs = 0;
    uint32 dernierDispelMs = 0;
    std::vector<uint32> dispelRetires;
    bool dispelReapplicationVue = false;
    uint32 dispelRefus[5] = { 0, 0, 0, 0, 0 };
    uint32 dispelRefusMs[5] = { 0, 0, 0, 0, 0 };
    // Sorts ecartes faute de ressource (CoaPeutPayer) : bilan, et derniere trace par sort (une toutes les 5 s au plus).
    uint32 refusRessource = 0;
    std::unordered_map<uint32, uint32> refusRessourceTraceMs;
    // Lot 20 (13/10) : soins ou boucliers deja poses ecartes (CoaSoinDejaPose), generateurs de ressource lances par la
    // survie, soins a aura retenus par la reserve de mana du meilleur outil de survie -- bilan (par sort et par 5 s pour
    // les deux refus), et derniere trace par sort.
    uint32 dejaPose = 0, survieGenerateurs = 0, survieReserveMana = 0;
    std::unordered_map<uint32, uint32> dejaPoseTraceMs;
    std::unordered_map<uint32, uint32> reserveManaTraceMs;
    // Etendards et autres sorts au sol de soutien poses sur soi (« coa arene soutien ») : prochain essai, poses.
    uint32 prochainSoutienMs = 0;
    uint32 soutiens = 0;
    // Source du premier degat subi hors de vue de l'adversaire (gladiateurs traces seulement) : guet ouvert par
    // CoaAreneMaj quand l'adversaire sort de la vue, rempli par le crochet de degats de CoaArene1v1.cpp (meme fil : la
    // carte du bot), ecrit dans le journal par CoaAreneMaj. Genre : 1 joueur (l'adversaire), 2 familier ou unite qu'il
    // controle, 3 autre. `sourceSort` : sort du coup (0 : coup blanc ou inconnu), `sourcePeriodique` : tick de DoT.
    uint32 sourceGuetDepuisMs = 0;
    bool sourceNotee = false;
    bool sourceAEcrire = false;
    uint8 sourceGenre = 0;
    uint32 sourceSort = 0;
    bool sourcePeriodique = false;
    bool sourceFurtif = false;         // le bot etait furtif au moment du coup
    uint32 sourceDegats = 0;
    uint32 sourceEntree = 0;           // entree de creature de l'attaquant (familier), 0 pour un joueur
    uint32 sourceMs = 0;               // delai entre la perte de vue (ouverture du guet) et le coup
    uint32 sourcesHorsVue = 0;
    // Sort de degats annonce par ModifySpellDamageTaken juste avant DealDamage (meme tour de carte) : pour nommer le sort
    // du coup que OnDamage recoit sans lui.
    uint32 sortSubiAttente = 0;
    ObjectGuid sortSubiAttaquant;
    uint32 sortSubiMs = 0;
};

/*
 * Fallback combat context for Conquest of Azeroth custom classes (ids 12 and above).
 *
 * mod-playerbots ships one hand written context per vanilla class. A character whose
 * class id is outside 1-11 falls through to the plain AiObjectContext, which carries
 * movement, questing and social behaviour but no combat rotation at all: such bots
 * greet, follow and hand in quests, then stand still in a fight.
 *
 * Rather than writing one context per custom class (each vanilla one is roughly two
 * thousand lines), this context rotates through the class abilities listed in
 * ascension_custom_class_spell that the bot has learned. It therefore covers every
 * custom class at once, including any the server author adds later, at the cost of
 * not knowing class specific rotations.
 */
class CoaAiObjectContext : public AiObjectContext
{
public:
    CoaAiObjectContext(PlayerbotAI* botAI);

    static void BuildSharedContexts();
    static void BuildSharedStrategyContexts(SharedNamedObjectContextList<Strategy>& strategyContexts);
    static void BuildSharedActionContexts(SharedNamedObjectContextList<Action>& actionContexts);
    static void BuildSharedTriggerContexts(SharedNamedObjectContextList<Trigger>& triggerContexts);
    static void BuildSharedValueContexts(SharedNamedObjectContextList<UntypedValue>& valueContexts);

    static SharedNamedObjectContextList<Strategy> sharedStrategyContexts;
    static SharedNamedObjectContextList<Action> sharedActionContexts;
    static SharedNamedObjectContextList<Trigger> sharedTriggerContexts;
    static SharedNamedObjectContextList<UntypedValue> sharedValueContexts;

    // Spells this bot set aside after a failure that will not clear on the next tick (wrong
    // target state, shapeshift...), with the time they may be tried again. One per bot, only
    // touched by the bot's own AI update, so no locking.
    std::unordered_map<uint32, time_t> benchedSpells;

    // The bot's kit: the abilities of its class it has reached and knows, without any
    // filter on what they do.
    //
    // Ten actions and triggers ask for a filtered view of this list on every tick of
    // every bot (isUseful of the attack, area, three heals, taunt, defensive and dispel
    // actions, the two triggers, then Execute). Building it walks the ~350 abilities of
    // the class with one Player::HasSpell and one SpellMgr::GetSpellInfo each, so it was
    // ~3500 lookups per bot and per tick, all giving the same answer. It is now built
    // once and filtered.
    //
    // Rebuilt when the bot's level or the size of its spell book changes, and in any
    // case after CoaKitLifeSeconds, so that a change neither of those two sees (a spell
    // learned and another unlearned between two ticks) cannot be missed for long.
    //
    // Like benchedSpells: one per bot, only touched by that bot's own AI update, so no
    // locking.
    std::vector<CoaKnownAbility> knownKit;
    time_t knownKitBuilt = 0;   // 0 while the kit has never been built
    uint32 knownKitSpells = 0;  // size of the spell book when it was built
    uint8 knownKitLevel = 0;

    // CoaArene IA : voir CoaAreneEtat plus haut.
    CoaAreneEtat arene;
};

#endif
