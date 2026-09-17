/*
 * Grants a CoA bot the talents a player of its level could have - no more.
 *
 * Background: bots never get a specialization through the character
 * advancement path, because that runs through the client and they have none.
 * The core abilities of a spec hang off talent nodes though. Measured across
 * nine bots on 14 Sep 2026: 12 of 50 core abilities. They run on the bare
 * class starter kit.
 *
 * The order in which points are spent lives in CoaTalentPlan.h and is generated
 * from the builder data. It honours thresholds, prerequisites and rank counts.
 */

#ifndef PLAYERBOTS_COATALENTAPPLY_H
#define PLAYERBOTS_COATALENTAPPLY_H

class Player;

// Idempotent: spells that are already known are skipped. Safe to call on every
// login and every level up.
void ApplyCoaTalentPlan(Player* bot);

#endif
