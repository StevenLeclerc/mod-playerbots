/*
 * Conquest of Azeroth -- bots compagnons du Directeur (« visages connus »), lot 1.
 *
 * Conception : coa-project/docs/CONCEPTION-bots-compagnons-directeur.md (§ 3 a 6, § 15).
 *
 * Chaque personnage HUMAIN de niveau >= AiPlayerbot.CoaCompagnons.SeuilNiveau a un registre de bots attitres, toujours
 * les memes : des allies de sa faction et des vis-a-vis de l'autre faction. Ils se reveillent quand il se connecte et
 * s'endorment Grace secondes apres son depart (tout de suite s'il change de personnage sur le meme compte). Endormis,
 * ils portent un evenement « logout » de PermanentlyInWorldTime : hors du tirage d'AddRandomBots, redemarrage compris.
 * Le registre est rattache au PERSONNAGE, pas au compte.
 *
 * UN SEUL AUTEUR (P-134) : ce fichier ecrit la table `coa_compagnon` de la base personnages ; le Directeur, le panneau
 * et les outils la LISENT. Toute demande de changement passe par `.coa compagnon ...`.
 *
 * FILS. Tout s'execute au FIL MONDE (crochets de connexion et de deconnexion, OnUpdate du WorldScript, commandes,
 * RandomPlayerbotMgr::Init), SAUF EstCompagnon, lu aussi depuis un fil de carte (ProcessBot(Player*) par
 * RandomBotUpdateAction) : il lit un ensemble sous verrou partage, que seul le fil monde modifie.
 *
 * OPTION ETEINTE (defaut) : ChargerEtEndormir ne lit rien, EstCompagnon rend faux, tous les crochets sortent d'entree.
 *
 * REVISION DU 26/10/2026 : RESERVES PAR TRANCHE. Une reserve de bots par tranche de niveau (1-10 ... 51-60, tranche =
 * (niveau - 1) / 10) ET par faction, toujours les memes (table `coa_reserve`, meme fichier, meme auteur). Un humain de
 * la tranche, quel que soit son camp, reveille les deux reserves de sa tranche ; elles s'endorment Grace secondes apres
 * le depart du dernier humain de la tranche. Niveau fige (PLAYER_FLAGS_NO_XP_GAIN) ; tranche trop maigre completee par
 * montee en niveau au premier reveil. Option AiPlayerbot.CoaCompagnons.Reserve.Enable (0 par defaut, figee au
 * demarrage). Les exemptions du lot 1 valent pour eux : EstCompagnon rend vrai pour un bot de reserve.
 */

#ifndef _PLAYERBOT_COACOMPAGNONS_H
#define _PLAYERBOT_COACOMPAGNONS_H

#include "Define.h"

#include <string>

class ChatHandler;
class Player;

namespace CoaCompagnons
{
// N'importe quel fil. Vrai si ce bot (guid bas) est au registre d'un humain OU dans une reserve de tranche (eveille ou
// endormi, pas « retire ») : c'est la question de toutes les exemptions. Suit les Enable FIGES au demarrage.
bool EstCompagnon(uint32 botGuid);
// N'importe quel fil. Vrai si ce bot est dans une reserve de tranche (pas « retire ») : niveau fige (NO_XP_GAIN).
bool EstReserve(uint32 botGuid);
// FIL DE L'APPELANT. Vrai pendant que CoaCompagnons.cpp place lui-meme ce bot (RandomTeleportGrindForLevel d'un bot de
// reserve de l'autre faction) : RandomTeleport laisse alors passer ce bot malgre EstCompagnon.
bool PlacementAutorise(uint32 botGuid);

// FIL MONDE, au demarrage (RandomPlayerbotMgr::Init, avant le premier tirage) : lit le registre, retire les bots
// supprimes, purge les lignes d'un humain supprime, et remet TOUT le registre en sommeil. Init repasse ici a chaque
// recharge de la conf : AiPlayerbot.CoaCompagnons.Enable est fige au premier appel, et un registre deja charge est
// garde tel quel (seul nouvel essai : table absente au premier passage).
void ChargerEtEndormir();

// FIL MONDE. Crochets de RandomPlayerbotMgr::OnPlayerLogin / OnPlayerLogout, HUMAINS seulement.
void HumainConnecte(Player* humain);
void HumainDeconnecte(Player* humain);

// FIL MONDE. RandomPlayerbotMgr signale qu'un compagnon a quitte le jeu sans qu'on l'ait endormi (presence expiree,
// echec de connexion) : il redevient « dort », et sera reveille au renouvellement suivant si son humain est la.
void BotSorti(uint32 botGuid);

// FIL MONDE, a chaque tour du monde (PlayerbotsWorldScript::OnUpdate) : reveils echelonnes, renouvellement,
// delais de grace, releve.
void MettreAJour(uint32 diff);

// FIL MONDE. Commandes `.coa compagnon liste|eveiller|endormir <humain>` (CoaDuelCommandScript.cpp).
bool CommandeListe(ChatHandler* handler, std::string const& nom);
bool CommandeEveiller(ChatHandler* handler, std::string const& nom);
bool CommandeEndormir(ChatHandler* handler, std::string const& nom);

// FIL MONDE. Commandes `.coa reserve liste [tranche] | eveiller <tranche> | endormir <tranche> | constituer`.
// `arg` : tranche 0 a 5, ou « 1-10 » ... « 51-60 » ; vide pour liste et constituer.
bool CommandeReserveListe(ChatHandler* handler, std::string const& arg);
bool CommandeReserveEveiller(ChatHandler* handler, std::string const& arg);
bool CommandeReserveEndormir(ChatHandler* handler, std::string const& arg);
bool CommandeReserveConstituer(ChatHandler* handler);
}  // namespace CoaCompagnons

#endif
