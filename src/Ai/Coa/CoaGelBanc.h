/*
 * Conquest of Azeroth -- gel d'un bot pendant le banc de garde G1 de l'artisanat (T2-1).
 *
 * Le banc (mod-ascension-compat, `.artisanat banc garde`, AscensionArtisanat.cpp) pose sur le bot la cle
 * CustomData « CoaGelBanc » le temps d'une serie de mesures, et la retire a `rendre`, a l'echeance (filet de
 * securite du module, fil monde) ; elle disparait d'elle-meme a la deconnexion et au redemarrage (CustomData
 * n'est jamais sauvegarde). Tant qu'elle est posee :
 *   - EquipAction::EquipItem ne change aucune piece (EquipAction.cpp) ;
 *   - RandomPlayerbotMgr::ProcessBot(uint32) et ProcessBot(Player*) ne font rien pour ce bot : ni mise a jour,
 *     ni refonte, ni revive, ni teleportation, ni deconnexion de rotation ; RandomTeleport le refuse aussi
 *     (RandomPlayerbotMgr.cpp) ;
 *   - PlayerbotAI::UpdateAIInternal ne lance aucune action HORS combat : le bot reste sur place, ne cherche ni
 *     cible ni trajet ; en combat (son adversaire l'attaque), il se bat et se soigne normalement.
 * Un bot mort pendant le gel n'est ressuscite ni par son IA ni par le gestionnaire : le banc le ressuscite
 * (`.revive`), sinon il attend `rendre` ou l'echeance (au plus 15 min apres la derniere action du banc).
 * Non couverts : Directeur (Reequiper, Preparer), arene, duel ; le banc detecte une piece retiree (portees).
 * Sans la cle : aucun effet. Ecritures : fil monde seulement (commande de console, WorldScript), cartes a
 * l'arret ; lectures : fil de carte du bot ou fil monde.
 */

#ifndef _PLAYERBOT_COAGELBANC_H
#define _PLAYERBOT_COAGELBANC_H

#include "DataMap.h"
#include "Player.h"

inline bool CoaGelBanc(Player const* bot)
{
    return bot && bot->CustomData.Get<DataMap::Base>("CoaGelBanc") != nullptr;
}

#endif
