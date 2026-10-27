/*
 * Conquest of Azeroth -- souvenirs du compagnon (27/10/2026). Voir CoaSouvenirs.cpp.
 *
 * Le serveur note ce que vit chaque humain (morts, quetes finies, moments durs, recoltes, elites et rares abattus,
 * niveaux pris), « ensemble » quand son compagnon eveille etait a cote ; le compagnon eveille lui en chuchote un de
 * temps en temps. Option AiPlayerbot.CoaCompagnons.Souvenirs.Enable ; table `coa_compagnon_souvenir`.
 */

#ifndef _PLAYERBOT_COASOUVENIRS_H
#define _PLAYERBOT_COASOUVENIRS_H

#include "Define.h"

#include <ctime>

class Creature;
class Player;
class Quest;

namespace CoaSouvenirs
{
// N'IMPORTE QUEL FIL (crochets du PlayerScript, fils de carte) : l'evenement est mis en file, sous verrou, et traite
// au fil monde par MettreAJour. L'appelant a deja verifie l'option et que `humain` est humain (CoaCompagnons::EstHumain).
void Mort(Player* humain);
void TueParCreature(Creature* tueur, Player* humain);
void QueteFinie(Player* humain, Quest const* quete);
void Recolte(Player* humain, uint32 competence);
void CreatureTuee(Player* humain, Creature* victime);
// Coup fatal d'un COMPAGNON (l'appelant a verifie CoaCompagnons::EstCompagnon) : l'exploit va a son humain, resolu au
// fil monde.
void CreatureTueeParCompagnon(Player* compagnon, Creature* victime);
void NiveauPris(Player* humain);

// FIL MONDE. Appeles par CoaCompagnons (HumainConnecte / HumainDeconnecte / MettreAJour, une fois par seconde).
void Arrivee(Player* humain, time_t maintenant);
void Depart(uint32 humainGuid);
void MettreAJour(time_t maintenant);
}  // namespace CoaSouvenirs

#endif
