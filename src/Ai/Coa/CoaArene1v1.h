/*
 * Conquest of Azeroth -- arene 1 contre 1 non classee (escarmouche) : un joueur
 * humain contre un bot Ranger, le « gladiateur », mis a son niveau.
 *
 * Conception : coa-project/docs/CONCEPTION-arene-1v1.md. Le detail (file 11, type
 * d'arene 1, appariement, garde-fous, fils) est en tete de CoaArene1v1.cpp.
 *
 * Options : AiPlayerbot.Arene1v1.* (conf/playerbots.conf.dist). Enabled = 0 (defaut) :
 * rien n'est declare, rien ne s'affiche, rien ne tourne.
 */

#ifndef _PLAYERBOT_COAARENE1V1_H
#define _PLAYERBOT_COAARENE1V1_H

#include "Define.h"

#include <string>

class Player;

namespace CoaArene1v1
{
// Type d'arene (Battleground::GetArenaType) et file de combat (BattlegroundQueueTypeId)
// propres au 1 contre 1. 11 : premiere valeur libre apres BATTLEGROUND_QUEUE_5v5 (10),
// sous MAX_BATTLEGROUND_QUEUE_TYPES (20) ; verifiee libre au demarrage.
constexpr uint8 TYPE_ARENE = 1;
constexpr uint32 FILE_ARENE = 11;

// FIL MONDE SEULEMENT (option de dialogue, commande MJ). Inscrit `joueur` et un
// gladiateur libre prepare a son niveau dans la file 11. Rend faux et remplit `refus`
// (code court, deja explique au joueur) si rien n'a ete mis en file.
bool Inscrire(Player* joueur, std::string const& origine, std::string& refus);

// TOUT FIL. Le bot `guidLow` est-il un gladiateur engage (inscrit, en file, invite ou
// en arene) ? Lu par le Directeur (CoaDirecteur.cpp, garde du bus) pour ne jamais
// affecter, preparer ni deplacer un gladiateur engage. Aucun match : un chargement
// atomique.
bool GladiateurEngage(uint32 guidLow);

// Fil de l'IA du bot (fil de carte de l'arene) ou fil monde. Vrai : `bot` est le gladiateur
// d'une arene 1v1 TERMINEE (STATUS_WAIT_LEAVE) dont le depart est encore retenu
// (AiPlayerbot.Arene1v1.DelaiDepartS, joueur encore present). Lu par
// BGStatusAction::LeaveBG et BGLeaveAction : le gladiateur reste visible apres la fin (le coeur
// ressuscite tout le monde a EndBattleground), sans disparition brutale, immunise aux joueurs.
// C'est le suivi de CoaArene1v1 (fil monde) qui libere le depart et fait partir le bot a
// l'echeance ; faux dans tout autre cas (rien n'est retenu hors de ce match).
bool RetenirDepart(Player* bot);
}  // namespace CoaArene1v1

void AddSC_coa_arene1v1();

#endif
