/*
 * Conquest of Azeroth — l'oracle choisit QUI le mercenaire d'elite frappe.
 *
 * POURQUOI CETTE GREFFE, APRES TROIS CAMPAGNES NULLES
 *
 * L'oracle Laya a ete mesure trois fois sur le choix du SORT, dans trois
 * situations differentes, avec un instrument dont la puissance est etablie :
 *
 *   duel apparie niveau 60, 2 327 duels tranches ...... -1,1 point +/- 2,6
 *   solo contre paquets de monstres, 2 312 rencontres .. +3,1 point +/- 3,7
 *   groupe de cinq complet, 2 990 rencontres ........... +0,6 point +/- 3,4
 *
 * Rien, nulle part, y compris a trois ennemis et plus. Et le resultat ne dit
 * PAS que le modele choisit mal : il dit que ce choix-la n'a pas de
 * consequence. L'oracle ne reordonnait que des sorts que le moteur avait deja
 * juges lancables, dans des rotations courtes ou l'ordre de tir change peu
 * l'issue.
 *
 * Le choix de la CIBLE, lui, en a une. Concentrer le soigneur adverse plutot
 * que taper le premier venu se voit en trois secondes, et se mesure.
 *
 * QUAND L'ORACLE DECIDE, ET POURQUOI PAS TOUT LE TEMPS
 *
 * UNIQUEMENT A L'ACQUISITION — quand le bot n'a pas encore de victime.
 *
 * Laisser l'oracle rearbitrer a chaque tour produirait un bot qui change de
 * cible toutes les secondes et ne tue personne : l'etat change apres le premier
 * coup (la cible a maintenant un assaillant), donc la reponse change, donc la
 * cible change, et ainsi de suite. Ce n'est pas une hypothese prudente, c'est
 * la forme meme de la boucle.
 *
 * La regle « seulement quand `bot->GetVictim()` est nul » n'a besoin d'aucun
 * etat, ne peut pas osciller, et correspond a la decision qui interesse
 * vraiment : sur qui j'ouvre. Changer de cible EN COURS de combat est une
 * seconde decision, qui se mesurera separement si celle-ci donne quelque chose.
 *
 * ETAT AU 2026-09-24 : EN SERVICE, JAMAIS ARME, JAMAIS MESURE — ET C'EST VOULU
 *
 * `AiPlayerbot.Laya.Cibles` vaut false par defaut et n'a jamais ete mis a true.
 * Ce n'est pas un oubli : trois campagnes sur le choix du SORT n'ont rien
 * donne, et allumer une quatrieme greffe pour toute la population avant de
 * l'avoir mesuree serait exactement la faute qu'on vient d'eviter trois fois.
 *
 * Ce qui reste a faire, dans cet ordre :
 *   1. rallumer l'oracle sur le Mac (`outils/laya-oracle.py`), puis verifier le
 *      pont avec `outils/sonde-laya-pont.py` AVANT de conclure quoi que ce soit ;
 *   2. armer `Laya.Cibles = 1` et `Laya.TirageAuSort = 50` ;
 *   3. lancer le banc de groupe (`outils/banc-terrain.py --lot groupe`), qui est
 *      le seul a fournir plusieurs cibles dont un soigneur ;
 *   4. depouiller par `outils/analyse-rencontres-par-bras.py --lot groupe`, et
 *      lire d'abord le compteur `cibles_changees` du releve `coa laya:` : a
 *      zero, la greffe est branchee et sans effet, et tout ecart mesure serait
 *      du bruit qu'on attribuerait a tort au modele.
 *
 * LE REPLI EST EXACT, COMME PARTOUT AILLEURS
 *
 * Oracle eteint, reponse perimee, moins de deux candidats, bot non elite,
 * rencontre non tiree au sort : la fonction rend la cible du moteur, inchangee.
 * Rien dans ce fichier ne peut faire attaquer un bot qui n'aurait pas attaque.
 * Il ne peut que substituer une cible a une autre, parmi des candidats que le
 * moteur jugeait deja possibles.
 */

#ifndef PLAYERBOTS_COACHOIXCIBLE_H
#define PLAYERBOTS_COACHOIXCIBLE_H

class Player;
class PlayerbotAI;
class Unit;

// Rend la cible a frapper : celle de l'oracle s'il a une reponse fraiche et
// utilisable, sinon `choixMoteur`, inchangee. Ne rend jamais nullptr quand
// `choixMoteur` ne l'est pas.
Unit* CoaChoisirCibleParLaya(PlayerbotAI* botAI, Player* bot, Unit* choixMoteur);

#endif
