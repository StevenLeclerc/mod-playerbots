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
 */

#ifndef _PLAYERBOT_COACOMPAGNONS_H
#define _PLAYERBOT_COACOMPAGNONS_H

#include "Define.h"

#include <string>

class ChatHandler;
class Player;

namespace CoaCompagnons
{
// N'importe quel fil. Vrai si ce bot (guid bas) est au registre d'un humain (eveille ou endormi, pas « retire »).
bool EstCompagnon(uint32 botGuid);

// FIL MONDE, au demarrage (RandomPlayerbotMgr::Init, avant le premier tirage) : lit le registre, retire les bots
// supprimes, purge les lignes d'un humain supprime, et remet TOUT le registre en sommeil.
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
}  // namespace CoaCompagnons

#endif
