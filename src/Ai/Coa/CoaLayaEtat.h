/*
 * Conquest of Azeroth — la description de situation envoyee a l'oracle Laya.
 *
 * POURQUOI CE FICHIER EXISTE
 *
 * Les deux canaux de l'oracle — « quelle action » et « quel sort » — decrivaient
 * chacun la situation de leur cote, et les deux descriptions avaient diverge.
 * Celle du canal sort ecrivait encore « Ressource: 0% » pour toute classe sans
 * mana et « Cible a 0% de vie, a 0 metres » en l'absence de cible : deux
 * mensonges que la version action avait deja corriges le 2026-09-22. Un
 * encodeur ne signale pas une entree absurde, il en tire une reponse absurde.
 *
 * Il n'y a donc plus qu'un seul constructeur d'etat, ici.
 *
 * CE QUE LA CAMPAGNE DE DUELS A MONTRE, ET QUI A DICTE LE CONTENU
 *
 * 2016 manches de duel, 947 decidees : l'oracle n'avait AUCUN effet mesurable
 * sur l'ordre des sorts (-0,7 point +/- 3,9, effet detectable 5,6 points).
 * Le controle adverse a nomme la cause la plus probable : le modele recevait
 * SIX faits — vie, ressource, niveau, vie de la cible, distance, nombre
 * d'assaillants — alors que les conditions d'emploi redigees pour les sorts
 * en reclament une quinzaine. On ne peut pas demander a un modele d'arbitrer
 * entre un sort de zone et un sort mono-cible sans jamais lui dire combien
 * d'ennemis sont groupes, ni de concentrer le soigneur adverse sans jamais lui
 * dire que la cible en est un.
 *
 * REGLE DE REDACTION : QUE DES FAITS, JAMAIS DE CONSIGNE
 *
 * On ecrit « la cible est un joueur soigneur », jamais « il faut la focaliser ».
 * Souffler la reponse revient a reecrire la rotation en francais dans un
 * fichier C++, c'est-a-dire a se passer du modele tout en payant son cout.
 *
 * CE QUE CA COUTE
 *
 * Un appel au plus par bot et par `AiPlayerbot.Laya.PeriodMs` (1 s par defaut),
 * et seulement pour les bots d'elite quand l'oracle est arme. Les parcours les
 * plus lourds — densite d'ennemis autour de la cible, etat du groupe — sont
 * bornes : ils lisent des valeurs de contexte deja calculees et mises en cache
 * par le moteur pour ses propres besoins.
 */

#ifndef PLAYERBOTS_COALAYAETAT_H
#define PLAYERBOTS_COALAYAETAT_H

#include <string>

class Player;
class PlayerbotAI;
class Unit;

// La situation du bot, en une ligne de texte lisible.
//
// `cible` peut etre nullptr : la phrase le dit alors franchement plutot que
// d'ecrire des zeros. Quand elle vaut nullptr, la valeur de contexte
// « current target » est consultee en dernier recours.
std::string CoaDecrireEtatLaya(PlayerbotAI* botAI, Player* bot, Unit* cible);

#endif
