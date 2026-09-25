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
 * LA REGLE : un seul proprietaire par etat (P-134). Le drapeau d'arret n'est
 * ecrit que par ce fichier. Aucun champ du coeur ni de playerbots n'est touche.
 */

#ifndef COA_DIRECTEUR_H
#define COA_DIRECTEUR_H

#include "Define.h"
#include <string>

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
}  // namespace CoaDirecteur

void AddSC_coa_directeur();

#endif
