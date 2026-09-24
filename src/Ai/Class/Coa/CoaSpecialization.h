/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COASPECIALIZATION_H
#define _PLAYERBOT_COASPECIALIZATION_H

#include "Define.h"

#include <limits>
#include <string>

class Player;

enum class CoaRole : uint8
{
    Dps,
    Tank,
    Heal
};

// How a CoA character fights, from the ascensionsidekick.com role of its specialization.
enum class CoaStyle : uint8
{
    Melee,   // strikes in melee range
    Ranged,  // physical damage from range: bows, guns, thrown weapons
    Caster   // spells from range
};

// Primary stats of a CoA specialization, as a mask.
enum CoaStat : uint8
{
    COA_STAT_STRENGTH  = 0x01,
    COA_STAT_AGILITY   = 0x02,
    COA_STAT_INTELLECT = 0x04,
    COA_STAT_SPIRIT    = 0x08,
    COA_STAT_STAMINA   = 0x10
};

// Armor a CoA class is proficient with. CoA grants every armor proficiency of a class at
// character creation (acore_world.playercreateinfo_spell_custom, required level 0), so this
// never depends on the character's level, unlike the level 40 step of the WotLK classes.
struct CoaArmorProficiency
{
    uint8 heaviestArmor;  // ITEM_SUBCLASS_ARMOR_*
    bool usesShield;
};

// Armor proficiencies of one of the 21 CoA classes, or nullptr for any other class.
CoaArmorProficiency const* GetCoaArmorProficiency(uint8 playerClass);

// Role of a Conquest of Azeroth character, from its active specialization. Dps when it has none.
CoaRole GetCoaRole(Player const* player);

// Fighting style and primary stats of a CoA character's specialization. Before it has one
// (under level 10), those shared by most specializations of its class.
CoaStyle GetCoaStyle(Player const* player);
uint8 GetCoaPrimaryStats(Player const* player);

// Gives a random bot of a CoA class a specialization once it reaches level 10, the way a
// player picks one in the client. Returns true when a specialization was just chosen.
bool EnsureCoaSpecialization(Player* bot);

// Spends a random bot's Character Advancement points the way a player of its specialization
// would: one ability essence at every even level from 10 (class tree), one talent essence at every
// odd level from 11 (specialization tree), in the order of the ascensionsidekick.com level build.
// Returns the number of entries raised.
uint32 ApplyCoaTalents(Player* bot);

// Palier d'equipement demande a la selection.
//
// POURQUOI RELATIF AU VIVIER, ET JAMAIS ABSOLU. Mesure du 2026-09-23 sur les 500 bots
// en ligne (outils/sonde-palier-equipement.py, qui rejoue GetEquipGearScore sur
// character_inventory) : l'indice d'equipement mediant vaut 0 au niveau 4, 4 au niveau 8,
// 7 au niveau 16, 13 au niveau 27. Un seuil ecrit en dur - « indice >= 12 » - serait
// inatteignable en dessous du niveau 20 et gagne d'avance au-dela. Il n'existe aucune
// table serveur de l'indice attendu par niveau : le seul point de comparaison interrogeable
// est le vivier lui-meme.
//
// POURQUOI DES PERCENTILES ET NON UN POURCENTAGE DU MAXIMUM. Un seuil « >= 80 % du
// meilleur » est l'otage d'un seul bot : au niveau 21, mesure, le vivier va de 4 a 18 avec
// une mediane a 6 ; 80 % de 18 vaut 14,4 et ne laisse que le sommet, alors qu'au niveau 22
// le meme calcul (80 % de 13 = 10,4) en laisse la moitie. Un percentile ne bouge pas quand
// un seul individu s'envole. Il ne divise par rien non plus : c'est un indice dans un
// tableau trie, donc le cas « tout le vivier est nu » (indice 0, reel jusqu'au niveau 4)
// n'a pas besoin d'un garde contre la division par zero.
//
// CE QU'ILS GARANTISSENT. Un percentile est toujours atteint par au moins un element :
// un palier ne peut jamais vider un vivier non vide. La colonne « >=q3 » de la mesure le
// verifie sur les 37 niveaux peuples : elle ne vaut jamais 0.
enum class CoaGearTier : uint8
{
    Any,     // aucun filtre - c'est exactement le comportement d'avant ce lot
    Median,  // indice >= mediane du vivier retenu
    Top      // indice >= quartile superieur du vivier retenu
};

// Criteres de recrutement. Les valeurs par defaut reproduisent, au geste pres, ce que la
// commande faisait avant ce lot : delta de niveau non borne, aucun palier, refabrication
// autorisee. C'est ce qui permet au panneau en service de continuer a jouer sa forme de
// commande sans rien changer.
struct CoaRecruitOptions
{
    int32 levelDeltaLow = std::numeric_limits<int32>::min();
    int32 levelDeltaHigh = std::numeric_limits<int32>::max();
    CoaGearTier tier = CoaGearTier::Any;
    // Faux : on prend le bot tel qu'il est, avec sa vie. Vrai : on autorise le repli qui
    // le refabrique au niveau du maitre - ce que Randomize(false) detruit est documente
    // dans CoaSpecialization.cpp, au-dessus du bloc.
    bool rebuild = true;

    bool IsDefault() const
    {
        return levelDeltaLow == std::numeric_limits<int32>::min() &&
               levelDeltaHigh == std::numeric_limits<int32>::max() && tier == CoaGearTier::Any && rebuild;
    }

    bool HasLevelDelta() const
    {
        return levelDeltaLow != std::numeric_limits<int32>::min() ||
               levelDeltaHigh != std::numeric_limits<int32>::max();
    }
};

// Ce que la selection a vu, pour que l'appelant puisse dire sur QUEL critere elle a coince.
// « 2 sur 4 » ne renseigne personne : il faut savoir s'il manque des soigneurs ou si c'est
// le palier qui vide le vivier.
struct CoaRecruitReport
{
    uint32 seen = 0;          // bots aleatoires parcourus
    uint32 unavailable = 0;   // morts, en combat, deja en groupe, en vol, en instance, non CoA...
    uint32 wrongRole = 0;     // la classe ne porte aucune specialisation du role demande
    uint32 outsideDelta = 0;  // ecart de niveau hors des bornes demandees
    uint32 belowTier = 0;     // ecartes par le palier d'equipement
    uint32 eligible = 0;      // restants apres le palier

    // Indices d'equipement du vivier, connus seulement quand des criteres ont ete demandes :
    // les calculer coute 17 lectures d'inventaire par candidat, et le chemin par defaut - celui
    // que le panneau en service joue - n'a rien a payer pour une option qu'il n'utilise pas.
    bool gearKnown = false;
    uint32 gearMin = 0;
    uint32 gearMedian = 0;
    uint32 gearMax = 0;
    uint32 gearFloor = 0;   // le seuil effectivement applique
    uint32 pickedGear = 0;  // indice du bot retenu, en ABSOLU : sur un vivier d'un seul
                            // bot, le palier dit toujours « maximum » et n'apprend rien.

    bool picked = false;
    std::string pickedName;  // le nom du bot retenu : la ligne d'indice le porte, et le lire
                             // ici evite de le re-extraire du message rendu
    bool rebuilt = false;  // vrai si le repli a refabrique le bot, ce qui est irreversible
};

// Brings a free random bot able to fill `role` into the master's group, teleported next to
// him. Par defaut le bot est pris TEL QU'IL EST : c'est `options.rebuild` qui autorise le
// repli refabriquant, et lui seul. Returns false with the reason in `message` when no bot
// can be recruited; `report` est rempli dans tous les cas, succes comme echec.
bool RecruitCoaBot(Player* master, CoaRole role, CoaRecruitOptions const& options, CoaRecruitReport& report,
                   std::string& message);

// Surcharge de compatibilite : criteres par defaut, rapport jete. Elle existe pour que les
// appelants d'avant ce lot n'aient rien a changer.
bool RecruitCoaBot(Player* master, CoaRole role, std::string& message);

#endif
