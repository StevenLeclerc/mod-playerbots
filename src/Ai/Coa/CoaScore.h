/*
 * Conquest of Azeroth -- score d'efficacite des bots, lot L1.
 * Conception : coa-project/docs/CONCEPTION-score-efficacite-bots.md
 *
 * Une ligne `coa score:` par rencontre de combat de chaque bot aleatoire d'une classe CoA, avec ce
 * que la ligne `coa rencontre:` de CoaRegistreCombat ne porte pas : la specialisation, le role, la
 * table de talents suivie, les DEGATS infliges (aux joueurs et aux creatures), les SOINS effectifs,
 * les degats subis, les victimes. Lue par coa-project/outils/score-bots.py.
 *
 * POURQUOI A PART DE CoaRegistreCombat
 *   Le registre ne suit que les mercenaires, sous l'oracle Laya, et porte du travail non commite
 *   d'un autre chantier. Le score suit tous les bots CoA et ne touche a rien de cela.
 *
 * LES CROCHETS, TOUS VERIFIES APPELES PAR LE COEUR (10/10/2026)
 *   PlayerScript::OnPlayerUpdate (PlayerUpdates.cpp:315)  ouverture / fermeture, PV minimum
 *   UnitScript::OnDamage        (Unit.cpp:1006)           degats infliges et subis
 *   UnitScript::OnHeal          (Unit.cpp:8472)           soin EFFECTIF (apres ModifyHealth)
 *   OnPlayerCreatureKill / OnPlayerCreatureKilledByPet / OnPlayerPVPKill / OnPlayerLogout
 *   LA MORT se lit dans OnPlayerUpdate (!IsAlive a la fermeture) : OnPlayerJustDied arrive APRES
 *   la fermeture (KillPlayer suit OnPlayerUpdate), il ne sert pas.
 *   PIEGE EVITE : OnPlayerEnterCombat et OnPlayerLeaveCombat sont declares dans PlayerScript.h mais
 *   le coeur ne les appelle NULLE PART. Un score bati dessus n'aurait rien ecrit, sans un mot.
 *
 * FILS ET VERROUS
 *   Aucun verrou. Les compteurs d'une rencontre vivent dans le Player du bot (Object::CustomData).
 *   Une carte est mise a jour par un seul fil, et l'attaquant, la victime et le bot sont sur la meme
 *   carte : seul le fil de cette carte touche ces compteurs. Le seul etat partage est un compteur
 *   atomique de rencontres ouvertes, qui fait sortir OnDamage/OnHeal en un chargement quand le score
 *   est eteint.
 *
 * INTERRUPTEUR : AiPlayerbot.Coa.Score.Enable (0 par defaut), relu au plus une fois par seconde.
 */
#ifndef _PLAYERBOT_COASCORE_H
#define _PLAYERBOT_COASCORE_H

void AddSC_coa_score();

#endif
