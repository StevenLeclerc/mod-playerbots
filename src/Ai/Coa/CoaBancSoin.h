/*
 * Conquest of Azeroth -- banc grandeur nature des soins, lots S0 (traces) et S1 (commande de banc).
 *
 * Conception : coa-project/docs/CONCEPTION-banc-soigneurs.md (le § 9 fixe l'interface : options, commande,
 * epreuves, formats de trace) ; recherche : coa-project/docs/RECHERCHE-banc-soigneurs-20261013.md.
 *
 * Options : AiPlayerbot.CoaSoin.* (conf/playerbots.conf.dist), lues par sConfigMgr a chaque commande (`.reload
 * config` suffit). Banc = 0 (defaut) : la commande refuse, aucun essai ne peut exister, et tout ce qui suit sort
 * sur un chargement atomique (aucun essai = aucune trace, aucune garde).
 *
 *   .coa soin banc <epreuve> <essai> <nom[:spe]> [<nom[:spe]> ...]
 *   .coa soin arret <essai>
 */

#ifndef _PLAYERBOT_COABANCSOIN_H
#define _PLAYERBOT_COABANCSOIN_H

#include "Define.h"

#include <string>

class Player;

namespace CoaBancSoin
{
// TOUT FIL, chemin chaud. Vrai : le bot `guidLow` est membre d'un essai en cours (de sa preparation a sa fin).
// Aucun essai : un seul chargement atomique. Lu par les gardes du module (EnsureCoaSpecialization, AiFactory,
// PlayerbotAI::AllowActive, RandomPlayerbotMgr, arene 1v1) pour ne pas defaire ce que le banc a pose.
bool MembreEngage(uint32 guidLow);

// Fil de l'IA du bot (fil de carte). Une ligne `coa soin action:` si `bot` est membre d'un essai en cours, BORNEE :
// une ligne identique (meme action, resultat, sort, code) au plus une fois par seconde et par bot, et au plus
// ACTIONS_PAR_SECONDE lignes par essai et par seconde. `action` : heal, hot, group_heal, defensive ; `resultat` :
// lance, rien, refus. Aucun essai : un chargement atomique.
void TracerAction(Player* bot, char const* action, char const* resultat, uint32 sort, uint32 code);

// Corrections du banc du 17/10/2026 (coa-project/docs/DIAG-equilibrage-classes-20261017.md, B-1 a B-7).

// TOUT FIL. B-2 : vrai si `guidLow` est membre d'un essai en cours ET AiPlayerbot.CoaSoin.Potions = 0 (defaut) -- les
// actions « healing potion », « mana potion » et « healthstone » lui sont refusees (UseItemAction::isPossible).
bool PotionsInterdites(uint32 guidLow);

// TOUT FIL. B-3 : vrai si le dernier `.coa soin banc` avait AiPlayerbot.CoaSoin.AttenteAvantCombat > 0 ; la composition
// des membres d'essai (AiFactory) garde alors « coa buff » hors combat.
bool BuffsAvantCombat();

// Fil de l'IA. B-4 : une ligne `coa soin action: ... resultat=lance` pour un sort lance HORS des actions de soin (attack,
// aoe, taunt, dispel, interrupt ; CastFirst), si `bot` est membre d'un essai en cours. Debit borne a part des actions de
// soin. Aucun essai : un chargement atomique.
void TracerLancer(Player* bot, char const* action, uint32 sort);

// Fil de l'IA. B-4 : une ligne `coa soin liste:` -- les defensifs et les soins sur soi qu'une action de survie a recus
// (CoaIaDefensifs, CoaIaSoinsSurSoi), ids separes par des virgules, suffixe « r » si le sort est en recharge -- seulement
// quand le texte change pour ce bot et cette action. Aucun essai : un chargement atomique.
void TracerListe(Player* bot, char const* action, std::string const& defensifs, std::string const& soins);
}  // namespace CoaBancSoin

void AddSC_coa_banc_soin();

#endif
