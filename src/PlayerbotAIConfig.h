/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTAICONFIG_H
#define PLAYERBOTS_PLAYERBOTAICONFIG_H

#include "DBCEnums.h"
#include "SharedDefines.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

enum class BotCheatMask : uint32
{
    none = 0,
    taxi = 1,
    gold = 2,
    health = 4,
    mana = 8,
    power = 16,
    raid = 32,
    food = 64,
    maxMask = 128
};

enum class HealingManaEfficiency : uint8
{
    VERY_LOW = 1,
    LOW = 2,
    MEDIUM = 4,
    HIGH = 8,
    VERY_HIGH = 16,
    SUPERIOR = 32
};

enum class ShowHideCosmetic : uint8
{
    ALWAYS_HIDE = 0,
    ALWAYS_SHOW = 1,
    RANDOMIZE = 2
};

enum class AutoPartyBuffMode : uint8
{
    DISABLED = 0,
    RAID_ONLY = 1,
    GROUP_OR_RAID = 2
};

enum NewRpgStatus : int
{
    //Initial Status
    RPG_IDLE = 0,
    RPG_GO_GRIND = 1,
    RPG_GO_CAMP = 2,
    // Exploring nearby
    RPG_WANDER_RANDOM = 3,
    RPG_WANDER_NPC = 4,
    // Do Quest (based on quest status)
    RPG_DO_QUEST = 5,
    // Travel

    RPG_TRAVEL_FLIGHT = 6,
    // Taking a break
    RPG_REST = 7,
    RPG_OUTDOOR_PVP = 8,
    RPG_STATUS_END = 9
};

#define MAX_SPECNO 20

// One level range/bucket used by the random bot level brackets sub-feature (see RandomBotLevelMgr).
struct LevelBracketConfig
{
    uint8 lower = 1;
    uint8 upper = 80;
    uint8 pct = 0;
};

class PlayerbotAIConfig
{
public:
    static PlayerbotAIConfig& instance()
    {
        static PlayerbotAIConfig instance;

        return instance;
    }

    bool Initialize();
    void LoadRandomBotLevelConfig();
    bool IsInRandomAccountList(uint32 id);
    bool IsInRandomQuestItemList(uint32 id);
    bool IsPvpProhibited(uint32 zoneId, uint32 areaId);
    bool IsInPvpProhibitedZone(uint32 id);
    bool IsInPvpProhibitedArea(uint32 id);

    // Vrai quand le PvP ne peut PAS avoir lieu a cet endroit -- les deux sources
    // reunies : les drapeaux du DBC et les listes de conf. Voir la definition.
    bool CoaLieuSansPvp(uint32 zoneId, uint32 areaId);

    bool enabled;
    bool disabledWithoutRealPlayer;
    bool EnableICCBuffs;
    bool allowAccountBots, allowGuildBots, allowTrustedAccountBots;
    bool randomBotGuildNearby, randomBotInvitePlayer, inviteChat;
    uint32 globalCoolDown, reactDelay, maxWaitForMove, disableMoveSplinePath, maxMovementSearchTime, expireActionTime,
        dispelAuraDuration, passiveDelay, repeatDelay, errorDelay, rpgDelay, sitDelay, returnDelay, lootDelay;
    bool dynamicReactDelay;
    float sightDistance, spellDistance, reactDistance, grindDistance, lootDistance, shootDistance, fleeDistance,
        tooCloseDistance, meleeDistance, followDistance, whisperDistance, contactDistance, aoeRadius, rpgDistance,
        targetPosRecalcDistance, farDistance, healDistance, aggroDistance;
    uint32 criticalHealth, lowHealth, mediumHealth, almostFullHealth;
    uint32 lowMana, mediumMana, highMana;
    bool autoSaveMana;
    uint32 saveManaThreshold;
    AutoPartyBuffMode autoGreaterBlessings;
    AutoPartyBuffMode autoPartyBuffs;
    bool tellWhenMissingBuffReagents;
    uint32 missingBuffReagentMessageCooldown;
    bool forceRebuffOnReadyCheck;
    uint32 forceRebuffMarginSecs;
    bool autoAvoidAoe;
    float maxAoeAvoidRadius;
    std::set<uint32> aoeAvoidSpellWhitelist;
    bool tellWhenAvoidAoe;
    std::set<uint32> disallowedGameObjects;
    std::set<uint32> attunementQuests;
    std::set<uint32> unobtainableItems;

    uint32 openGoSpell;
    bool randomBotAutologin;
    bool botAutologin;
    std::string randomBotMapsAsString;
    float probTeleToBankers;
    bool enableWeightTeleToCityBankers;
    int weightTeleToStormwind;
    int weightTeleToIronforge;
    int weightTeleToDarnassus;
    int weightTeleToExodar;
    int weightTeleToOrgrimmar;
    int weightTeleToUndercity;
    int weightTeleToThunderBluff;
    int weightTeleToSilvermoonCity;
    int weightTeleToShattrathCity;
    int weightTeleToDalaran;
    std::vector<uint32> randomBotMaps;
    std::vector<uint32> randomBotQuestItems;
    std::vector<uint32> randomBotAccounts;
    std::vector<uint32> randomBotSpellIds;
    std::vector<uint32> randomBotQuestIds;
    uint32 randomBotTeleportDistance;
    float randomGearLoweringChance;
    int32 randomGearQualityLimit;
    int32 randomGearScoreLimit;
    bool preferClassArmorType;
    bool preferredSpecWeapons;
    float randomBotMinLevelChance, randomBotMaxLevelChance;
    float randomBotRpgChance;
    uint32 minRandomBots, maxRandomBots;
    uint32 randomBotUpdateInterval, randomBotCountChangeMinInterval, randomBotCountChangeMaxInterval;
    uint32 minRandomBotInWorldTime, maxRandomBotInWorldTime;
    uint32 minRandomBotRandomizeTime, maxRandomBotRandomizeTime;
    uint32 minRandomBotChangeStrategyTime, maxRandomBotChangeStrategyTime;
    uint32 minRandomBotReviveTime, maxRandomBotReviveTime;
    uint32 minRandomBotTeleportInterval, maxRandomBotTeleportInterval;
    uint32 permanentlyInWorldTime;
    uint32 minRandomBotPvpTime, maxRandomBotPvpTime;
    uint32 randomBotsPerInterval;
    uint32 randomBotPrintStatsInterval;
    uint32 minRandomBotsPriceChangeInterval, maxRandomBotsPriceChangeInterval;
    uint32 disabledWithoutRealPlayerLoginDelay, disabledWithoutRealPlayerLogoutDelay;
    bool randomBotJoinLfg;

    // Professions
    bool enableFishingWithMaster;
    uint32 classMatchingProfessionChance;
    float fishingDistanceFromMaster, fishingDistance, endFishingWithMaster;

    // chat
    bool randomBotTalk;
    bool randomBotEmote;
    bool randomBotSuggestDungeons;
    bool enableBroadcasts;
    bool enableGreet;
    bool randomBotSayWithoutMaster;

    uint32 broadcastChanceMaxValue;
    std::string broadcastWorldChannelName;
    uint32 zoneChannelId;

    uint32 broadcastToGuildGlobalChance;
    uint32 broadcastToWorldGlobalChance;
    uint32 broadcastToGeneralGlobalChance;
    uint32 broadcastToTradeGlobalChance;
    uint32 broadcastToLFGGlobalChance;
    uint32 broadcastToLocalDefenseGlobalChance;
    uint32 broadcastToWorldDefenseGlobalChance;
    uint32 broadcastToGuildRecruitmentGlobalChance;

    uint32 broadcastChanceLootingItemPoor;
    uint32 broadcastChanceLootingItemNormal;
    uint32 broadcastChanceLootingItemUncommon;
    uint32 broadcastChanceLootingItemRare;
    uint32 broadcastChanceLootingItemEpic;
    uint32 broadcastChanceLootingItemLegendary;
    uint32 broadcastChanceLootingItemArtifact;

    uint32 broadcastChanceQuestAccepted;
    uint32 broadcastChanceQuestUpdateObjectiveCompleted;
    uint32 broadcastChanceQuestUpdateObjectiveProgress;
    uint32 broadcastChanceQuestUpdateFailedTimer;
    uint32 broadcastChanceQuestUpdateComplete;
    uint32 broadcastChanceQuestTurnedIn;

    uint32 broadcastChanceKillNormal;
    uint32 broadcastChanceKillElite;
    uint32 broadcastChanceKillRareelite;
    uint32 broadcastChanceKillWorldboss;
    uint32 broadcastChanceKillRare;
    uint32 broadcastChanceKillUnknown;
    uint32 broadcastChanceKillPet;
    uint32 broadcastChanceKillPlayer;

    uint32 broadcastChanceLevelupGeneric;
    uint32 broadcastChanceLevelupTenX;
    uint32 broadcastChanceLevelupMaxLevel;

    uint32 broadcastChanceSuggestInstance;
    uint32 broadcastChanceSuggestQuest;
    uint32 broadcastChanceSuggestGrindMaterials;
    uint32 broadcastChanceSuggestGrindReputation;
    uint32 broadcastChanceSuggestSell;
    uint32 broadcastChanceSuggestSomething;

    uint32 broadcastChanceSuggestSomethingToxic;

    uint32 broadcastChanceSuggestToxicLinks;
    std::string toxicLinksPrefix;
    uint32 toxicLinksRepliesChance;

    uint32 broadcastChanceSuggestThunderfury;
    uint32 thunderfuryRepliesChance;

    uint32 broadcastChanceGuildManagement;

    uint32 guildRepliesRate;

    bool randomBotJoinBG;
    bool randomBotAutoJoinBG;

    std::string randomBotAutoJoinICBrackets;
    std::string randomBotAutoJoinEYBrackets;
    std::string randomBotAutoJoinAVBrackets;
    std::string randomBotAutoJoinABBrackets;
    std::string randomBotAutoJoinWSBrackets;

    uint32 randomBotAutoJoinBGICCount;
    uint32 randomBotAutoJoinBGEYCount;
    uint32 randomBotAutoJoinBGAVCount;
    uint32 randomBotAutoJoinBGABCount;
    uint32 randomBotAutoJoinBGWSCount;

    uint32 randomBotAutoJoinArenaBracket;

    uint32 randomBotAutoJoinBGRatedArena2v2Count;
    uint32 randomBotAutoJoinBGRatedArena3v3Count;
    uint32 randomBotAutoJoinBGRatedArena5v5Count;

    uint32 randomBotTeleLowerLevel, randomBotTeleHigherLevel;
    std::map<uint32, std::pair<uint32, uint32>> zoneBrackets;
    bool logInGroupOnly, logValuesPerTick;
    bool fleeingEnabled;
    bool summonAtInnkeepersEnabled;
    std::string combatStrategies, nonCombatStrategies;
    std::string randomBotCombatStrategies, randomBotNonCombatStrategies;
    bool applyInstanceStrategies;
    uint32 randomBotMinLevel, randomBotMaxLevel;
    float randomChangeMultiplier;

    // std::string premadeLevelSpec[MAX_CLASSES][10][91]; //lvl 10 - 100
    // ClassSpecs classSpecs[MAX_CLASSES];

    std::string premadeSpecName[MAX_CLASSES][MAX_SPECNO];
    std::string premadeSpecGlyph[MAX_CLASSES][MAX_SPECNO];
    std::vector<uint32> parsedSpecGlyph[MAX_CLASSES][MAX_SPECNO];
    std::string premadeSpecLink[MAX_CLASSES][MAX_SPECNO][MAX_LEVEL];
    std::string premadeHunterPetLink[3][21];
    std::vector<std::vector<uint32>> parsedSpecLinkOrder[MAX_CLASSES][MAX_SPECNO][MAX_LEVEL];
    std::vector<std::vector<uint32>> parsedHunterPetLinkOrder[3][21];
    uint32 randomClassSpecProb[MAX_CLASSES][MAX_SPECNO];
    uint32 randomClassSpecIndex[MAX_CLASSES][MAX_SPECNO];

    std::string commandPrefix, commandSeparator;
    std::string randomBotAccountPrefix;
    uint32 randomBotAccountCount;
    bool randomBotRandomPassword;
    bool deleteRandomBotAccounts;
    uint32 randomBotGuildCount, randomBotGuildSizeMax;
    bool deleteRandomBotGuilds;
    // --- PvP mercenaire (CoA) ---------------------------------------------
    // Une fraction des bots sont marques FFA et attaquent n'importe qui, y
    // compris les autres bots. Le reste du serveur garde le PvP faction contre
    // faction habituel. A 0, comportement d'origine strictement inchange.
    bool wildPvpEnabled;
    uint32 wildPvpMercenaryPercent;
    bool wildPvpStealthAmbush;
    uint32 wildPvpMinLevel;
    bool wildPvpBotsFightBots;

    // Cible et agresseur sont deux choses distinctes. Le coeur exige le
    // drapeau FFA DES DEUX COTES pour deux joueurs de meme faction
    // (Unit::GetReactionTo, Unit.cpp:7472) : sans ce reglage, un mercenaire ne
    // peut attaquer qu'un autre mercenaire, soit un bot sur cinq. A 1, tous les
    // bots aleatoires portent le drapeau et deviennent donc des proies
    // possibles, sans devenir pour autant des chasseurs : leur choix de cible
    // reste celui d'un bot ordinaire, faction contre faction.
    bool wildPvpAllBotsAreTargets;

    // CoA, 18/10/2026 (Mulgore « jonche de cadavres ») : avec AllBotsAreTargets, chaque bot porte le FFA et le
    // grind d'un bot ORDINAIRE prenait les bots de sa faction pour des monstres (71 a 97 % des morts JcJ).
    // SeulMercenaireMemeFaction (defaut 1) : seul un mercenaire convoite un joueur de SA faction (riposte, champ de
    // bataille, arene et duel restent permis). TreveTous (defaut 1) : la treve apres une mort JcJ
    // (TruceAfterDeathSec) protege la victime de TOUS les bots, plus seulement des mercenaires. BotsPvpMinLevel
    // (defaut 0 : eteint) : un bot aleatoire sans maitre, de ce niveau ou plus, hors lieu sans JcJ, porte le
    // drapeau JcJ ordinaire -- le JcJ entre factions est ouvert entre bots, sans FFA.
    bool wildPvpSeulMercenaireMemeFaction;
    bool wildPvpTreveTous;
    uint32 wildPvpBotsPvpMinLevel;

    // Nombre maximum de mercenaires qui peuvent engager la MEME cible. A 1, un
    // duel. C'est la borne qui empeche la meute, sans laquelle chaque
    // mercenaire decide seul et trois d'entre eux repondent trois fois oui.
    // A 0, aucune limite.
    uint32 wildPvpMaxAttackersPerTarget;

    // Recompense d'experience par victime, exprimee en FRACTION du palier de
    // niveau en cours : xp = GetXPForLevel(niveau) / divisor. A 4, quatre
    // victimes de son niveau pour passer un palier, a tous les niveaux. A 0,
    // aucune experience n'est versee.
    uint32 wildPvpKillXpDivisor;

    // Garde-fous anti-farm. Delai avant qu'une MEME victime repaie, plafond de
    // victimes payantes par heure toutes victimes confondues, et treve de
    // ciblage accordee a qui vient de mourir (sinon le mercenaire attend au
    // cimetiere). A 0, chacun est desactive.
    uint32 wildPvpKillRepeatCooldown;
    uint32 wildPvpKillsPerHourCap;
    uint32 wildPvpTruceAfterDeathSec;

    // Plafond de montee indexe sur la mediane des niveaux des bots en ligne :
    // un mercenaire cesse de gagner de l'experience au-dela de
    // mediane + offset. Il patiente, il ne redescend pas. Sans lui, il finit
    // par n'avoir plus aucune proie a sa portee, sauf le joueur humain.
    bool wildPvpLevelCapEnabled;
    int32 wildPvpLevelCapMedianOffset;

    // La montee de niveau teleporte un bot vers une zone de son niveau
    // (AutoMaintenanceOnLevelupAction::AutoTeleportForLevel). Pour un
    // mercenaire qui rode, c'est le faire disparaitre au milieu de sa chasse.
    bool wildPvpNoTeleportOnLevelUp;

    // Un mercenaire ne quete pas et ne farme pas : il echange "grind" et la
    // strategie de quete contre "move random". Son agression sur les joueurs
    // passe par "pvp", deja presente dans le jeu de base de tout bot.
    // Verifie en jeu par le port 8888, voir AiFactory.cpp et P-056.
    bool wildPvpMercenariesSkipPve;

    // La chasse : le mercenaire choisit un poste de guet, y va, et l'y tient
    // (strategie « coa chasse », Ai/Coa/CoaChasse.h). Sans elle il n'a que
    // « move random », qui ne le deplace pas. Eteint par defaut : c'est le
    // levier de montee en charge, le cout CPU de MoveFarTo a 250 mercenaires
    // n'ayant jamais ete mesure. Lu a chaque tick par l'action : le changer a
    // chaud prend effet sans reconnexion des bots.
    bool wildPvpChasse;

    // Lot 3. La TRAQUE : une proie deja dans « nearest enemy players » (75 yd)
    // mais hors de portee d'engagement (60 yd, ou 20 s'il a moins de PV qu'elle,
    // EnemyPlayerValue.cpp:315-317) -- le mercenaire va la chercher au lieu
    // d'attendre qu'elle passe a portee. Separee de wildPvpChasse parce qu'elle
    // coute autre chose : un balayage de grille de 75 yd par guetteur et par
    // DelaiScanProieMs, la ou le voyage coute des calculs de chemin.
    bool wildPvpTraque;

    // Lot 3. Le DECROCHAGE PvE : le mercenaire rompt un combat contre une
    // creature quand il le perd (PV sous le seuil ci-dessous) ou quand il
    // n'aurait jamais du le prendre (ecart de niveau > 4 ou rang elite, le
    // garde-fou de GrindTargetValue.cpp:95-101, que la riposte n'a pas). Il ne
    // rompt JAMAIS contre un joueur. Eteint par defaut : le besoin lui-meme est
    // une hypothese non mesuree (section 5 du dossier de conception).
    bool wildPvpDecrochage;

    // Le seuil, en POURCENTAGE de points de vie. Il sert a TROIS endroits, et
    // c'est voulu : le decrochage PvE l'emploie pour rompre ; l'etat RETOUR de
    // la chasse pour renoncer a rentrer au poste et en tirer un autre (le bot se
    // restaure alors en route, « food » et « drink » etant a 4,1 et 4,2 contre
    // 2,0 pour la chasse) ; et L'ENTREE EN TRAQUE, ou un guetteur sous le seuil
    // ne part pas. Ce troisieme usage manquait ici avant le 2026-09-24.
    //
    // LE TROISIEME EST UN PIEGE DE MESURE : monte haut, ce reglage eteint la
    // traque sans un mot, et le compteur « traque » reste a zero. A 100 -- le
    // plafond impose plus bas dans Initialize() -- plus aucune traque ne part,
    // points de vie pleins compris.
    //
    // A 0, aucun des trois ne se declenche sur les PV -- le garde-fou de niveau
    // et de rang, lui, reste actif.
    uint32 wildPvpDecrochagePct;

    // Lot 5. L'INDEX << OU SONT LES PROIES >> (CoaIndexProies, Bot/MercenaryRewards.h).
    // Une table de cellules de 250 yd par carte, rebatie toutes les 30 s dans le
    // balayage que MercenaryRewards::RecomputeMedian fait deja, lue sans verrou
    // par la chasse a l'etat CHOIX pour PONDERER le tirage du poste de guet vers
    // les lieux ou il y avait du monde. Eteint par defaut, et l'interrupteur
    // porte les DEUX cotes : eteint, le balayage ne remplit rien et le tirage se
    // comporte exactement comme au lot 4.
    //
    // Ne fait rien si wildPvpChasse vaut 0 : il n'y a alors personne pour lire
    // l'index. Le laisser a 1 dans ce cas ne coute que le remplissage.
    bool wildPvpIndexProies;

    // Hotel des ventes des bots (Ai/Coa/CoaHotelDesVentes.cpp, lots 1 a 3).
    // FAISABILITE-bots-hotel-des-ventes.md § 6-7. Eteint par defaut. Lues par
    // le fil monde seul (RandomPlayerbotMgr::UpdateAIInternal), comme le
    // `.reload config` qui les ecrit -- SAUF coaHvEnable, coaHvCourrier et
    // coaHvTeleportVisible, lus aussi depuis un fil de carte (testes avant le
    // garde de fil ; le dernier par DestinationsCommissaire, que
    // AutoMaintenanceOnLevelupAction atteint via RandomTeleportForLevel) : sans
    // course, car l'ecriture (Initialize) se fait fils de carte a l'arret.
    bool coaHvEnable;                  // 0 : AvantDestruction et ReleverCourrier sortent aussitot
    bool coaHvSimulation;              // 1 : journal seul ; 0 : depot REEL (lot 2)
    bool coaHvCourrier;                // lot 3 : releve des courriers d'HV des bots (exige Enable)
    uint32 coaHvCourrierMaxParPasse;   // courriers effaces au plus par passe
    uint32 coaHvMaxParMinute;          // depots (simules ou reels) au plus par minute ; 0 = aucun
    float coaHvCoefArtisanatQ1Min;     // class 7, qualite 1 : prix = SellPrice x [min ; max]
    float coaHvCoefArtisanatQ1Max;
    float coaHvCoefArtisanatQ23Min;    // class 7, qualite 2-3
    float coaHvCoefArtisanatQ23Max;
    float coaHvCoefEquipVertMin;       // class 2/4, qualite 2
    float coaHvCoefEquipVertMax;
    float coaHvCoefEquipBleuMin;       // class 2/4, qualite 3
    float coaHvCoefEquipBleuMax;
    float coaHvCoefRecetteMin;         // class 9, qualite 1-3
    float coaHvCoefRecetteMax;
    uint32 coaHvMiseMinPct;            // mise a prix, en % du prix d'achat immediat
    uint32 coaHvMiseMaxPct;
    uint32 coaHvBruitPct;              // +/- sur le prix unitaire
    uint32 coaHvSousEnchereMinPct;     // sous le plus bas des bots du meme modele
    uint32 coaHvSousEnchereMaxPct;
    float coaHvPlancherCoef;           // prix unitaire >= SellPrice x ce coefficient
    uint32 coaHvMaxParBotParPassage;
    uint32 coaHvMaxParPasse;           // budget global par passe de UpdateAIInternal
    uint32 coaHvMaxActivesParBot;
    uint32 coaHvMaxParMaison;          // encheres de BOTS par maison (les humaines a part)
    uint32 coaHvMaxParModele;          // encheres de bots par modele et par maison
    uint32 coaHvBilanSecondes;         // periode de la ligne `coa hv:` ; 0 = jamais
    bool coaHvTeleportVisible;         // lot 4 : apres un depot reel, branche ville -> commissaire-priseur
    uint32 coaHvTeleportVisiblePct;    // lot 4 : part (0-100) des bots marques envoyes au commissaire
    // Ressources d'evenement a l'HV (12/10/2026, CONCEPTION-ressources-rares-evenements.md § 4.2, lot 4). Exigent
    // Enable. LUES AUSSI DEPUIS LES FILS DE CARTE (RessourceProtegee : DestroyItemsVisitor, ItemUsageValue) : sans
    // course pour la meme raison que coaHvEnable (Initialize ecrit, fils de carte a l'arret).
    bool coaHvRessources;              // 0 : rien ne change ; 1 : ressources prioritaires, place reservee, protegees
    bool coaHvRessourcesVenteAnticipee; // 1 : ProcessBot depose les ressources sans attendre le vidage des sacs
    float coaHvRessourcesCoefMin;      // prix = SellPrice x [min ; max] (plafond BuyPrice x n - 1 inchange)
    float coaHvRessourcesCoefMax;
    uint32 coaHvRessourcesMaxParMaison;  // encheres de ressources de BOTS par maison, EN PLUS de MaxParMaison
    uint32 coaHvRessourcesMaxParModele;  // encheres de bots par ressource et par maison
    uint32 coaHvRessourcesGarderMaxPiles; // piles gardees au plus par vidage de sacs (au-dela : detruites)

    // Directeur, lot D2a (Ai/Coa/CoaDirecteur.cpp, SPEC-directeur-D2.md § 2.3).
    // TOUT EST ETEINT PAR DEFAUT : absentes de la conf en service, ces options
    // valent leur defaut et le bus ne lit rien. Lues par le fil monde seul.
    bool directeurBus;                        // interrupteur general
    std::string directeurVerbes;              // "treve,..." ; lever toujours accepte si bus
    uint32 directeurBusIntervalleMs;          // cadence de sondage de la file
    uint32 directeurMaxCommandesParMinute;    // seau a jetons
    uint32 directeurMaxTreves;                // treves actives au plus
    // Lot D2c (SPEC § 5) : inertes tant que `suspendre_mercenaire` n'est pas
    // dans directeurVerbes.
    uint32 directeurMaxSuspensions;           // suspensions actives au plus
    bool directeurSuspensionModeRpg;          // 0 : garde sa chasse ; 1 : mode RPG
    // Lot D2b (SPEC § 6) : inertes tant que `reaffecter` ou `alleger` n'est
    // pas dans directeurVerbes.
    uint32 directeurMaxAffectations;          // affectations actives au plus
    uint32 directeurTrajetMaxSecondes;        // trajet non arrive : abandonne au-dela
    // Lot D2b, suite (25/09) : trois comportements qui ne valent QUE sous une
    // affectation en TRAJET, tous eteints par defaut (Ai/Coa/CoaDirecteur.h).
    bool directeurTrajetPacifique;            // en trajet : aucune cible choisie, riposte seule
    bool directeurReleveCimetiere;            // mort en trajet : releve a l'esprit-guerisseur
    uint32 directeurReleveSecondes;           // duree de l'etat mort_en_trajet (plafond)
    bool directeurEtapes;                     // action `E-...` : etape d'itineraire, pas de sejour
    // Lot D3 (SPEC-directeur-D3.md § 8) : inertes tant que le verbe concerne
    // n'est pas dans directeurVerbes.
    uint32 directeurMaxParolesParMinute;      // faire_parler : paroles acceptees par minute, tous bots
    uint32 directeurMeteoReaffirmationSecondes; // meteo : renvoi du paquet au joueur
    uint32 directeurPresenceLaisseYd;         // peupler : laisse de sejour (0 = aucune)
    // Compagnons (27/09) : presence `C-` pres du joueur et pacte ; eteint par
    // defaut (une `C-` est alors refusee `compagnon_eteint`, aucun pacte lu).
    bool directeurCompagnons;
    // Teleportation cachee (27/09) : presence `T-` teleportee hors de la vue du
    // joueur ; eteinte par defaut (une `T-` est alors refusee `teleport_eteint`).
    bool directeurTeleportCache;
    // Saut lointain (30/09) : presence `L-` teleportee a 250-350 yd du joueur, hors
    // de sa portee de vue (100 yd), sans exiger la ligne de vue coupee ; eteint par
    // defaut (une `L-` est alors refusee `lointain_eteint`). Exige TeleportCache.
    bool directeurTeleportLointain;
    // Paroles (30/09) : `dire`/`crier` differes tant que le joueur n'a pas le
    // locuteur a son client (au plus 60 s, puis `perdue hors_vue`) ; eteint par
    // defaut (la parole part comme avant, la ligne dit `vu=0`).
    bool directeurParoleAVue;
    // Missions (30/09) : la FINALE `Q-` d'une presence d'animation porte une
    // mission (colonne `texte` : quete, chasse, recolte, passage, bande ;
    // `valeur` : quete, metier ou chef). En SEJOUR, le bot ne tire plus que ce
    // qui le garde au lieu de sa mission, et sa laisse s'y centre. Eteint par
    // defaut : la mission est ignoree (journal `etat=ignoree`), rien ne change.
    bool directeurMissions;
    // Lot F des evenements (06/10/2026) : la mission `evenement` d'une finale `Q-` inscrit le
    // bot a l'evenement du joueur (cibles : les invoques du lot D ; CoaDirecteur::CibleDEvenement).
    // Eteint par defaut : la mission est degradee en `chasse` (le bot reste au lieu, rien d'autre).
    bool directeurEvenementBots;
    // Lot S (08/10/2026) : un bot INSCRIT a l'evenement d'un joueur compte ce joueur parmi ses cibles de soin
    // (« party member to heal ») et de buff/dissipation des classes CoA (NearbyGroup), bien qu'il ne soit pas
    // dans son groupe. Eteint par defaut : rien ne change (CoaDirecteur::JoueurASoigner rend nul).
    bool directeurEvenementSoins;
    // Vrai groupe (09/10/2026, coa-project/docs/EVENEMENTS-groupe.md) : un bot INSCRIT a l'evenement d'un joueur,
    // arrive au lieu (sejour), REJOINT le groupe de ce joueur (cree, joueur chef, s'il n'en a pas ; ajoute s'il en
    // mene un qui a de la place) et le quitte a la fin de l'evenement. Eteint par defaut : rien ne change.
    bool directeurEvenementGroupe;
    // Fin d'evenement (13/10/2026, remontee R-20261013-1757) : a la fin de l'evenement, les inscrits RESTENT dans
    // le groupe du joueur au plus ce nombre de secondes, pour qu'il pille les corps tapes par le groupe (le droit
    // de butin d'un corps tape en groupe est attache au GROUPE : Player::isAllowedToLoot). Ils le quittent avant si
    // le joueur quitte le groupe (bouton natif), se deconnecte, s'eloigne au-dela de ...AttenteYd, ou si un nouvel
    // evenement du joueur commence sans eux. 0 : comportement d'avant (sortie a la fin).
    uint32 directeurEvenementGroupeAttenteS;
    uint32 directeurEvenementGroupeAttenteYd;
    // Lot D4c (coa-project/docs/SPEC-directeur-D4.md § 3.3, § 3.4) : inertes tant
    // que `prime` et `chasser` ne sont pas dans directeurVerbes ; TracerMorts
    // eteint par defaut (les crochets de mort rendent la main au premier test).
    bool directeurTracerMorts;                // ligne `coa directeur mort:` pour toute mort d'humain
    uint32 directeurMaxPrimes;                // primes actives au plus
    uint32 directeurMaxChasseursParPrime;     // plafond de `valeur` d'une prime
    uint32 directeurPrimeMeute;               // mercenaires engages ensemble sur le prime, au plus
    uint32 directeurPrimeTraqueSecondes;      // traque d'un chasseur vers son prime
    uint32 directeurPrimeDetectionYd;         // portee a laquelle un chasseur lit son prime
    // Embuscade des chasseurs de prime (02/10, apres l'arc 5) : approche discrete
    // (a pied sous 300 yd, furtive des 120 yd ou a couvert hors du cone de 120 deg
    // devant le joueur), point d'attente propre a chaque chasseur a 25-35 yd,
    // ASSAUT coordonne par prime (fil monde : CoaDirecteur SuivreEmbuscades),
    // aucune attaque d'initiative sur le prime avant l'assaut, repli borne.
    // Specialisation d'embuscade pour les membres de la reserve. Vrai par
    // defaut ; 0 rend le comportement d'avant (traque P-167), a chaud.
    bool directeurEmbuscade;
    // Reserve de chasseurs (02/10, coa-project/docs/CONCEPTION-reserve-chasseurs.md) :
    // identifiants de guilde separes par des virgules ("12,13"). Un mercenaire dont
    // GetGuildId() y figure est un « chasseur de reserve ». Vide (defaut) : aucun.
    // Lue par EstGuildeDeReserve / GuildesDeReserve. Le Directeur la lit au fil
    // monde (pose `chasser`, preparation `P-`) et fige le resultat dans
    // l'affectation, que les fils de carte lisent. D'AUTRES lecteurs la lisent
    // depuis le fil du bot (actions et declencheurs de guilde, PlayerbotFactory::
    // InitGuild via AssignToGuild) : sans course, parce que la chaine n'est
    // reecrite que par `reload config`, au fil monde, pendant que les cartes ne
    // tournent pas (World::Update : les commandes avant sMapMgr->Update).
    std::string directeurReserveGuildes;
    // La guilde `guildId` est-elle de la reserve ? Lecture STRICTE de
    // directeurReserveGuildes (un morceau mal forme, vide compris : aucune
    // reserve), la meme que le service (emetteur.lire_reserve_guildes). guildId
    // nul : jamais. GuildesDeReserve : la liste lue (vide si mal formee).
    bool EstGuildeDeReserve(uint32 guildId) const;
    std::vector<uint32> GuildesDeReserve() const;
    // Lot X0 (03/10/2026, coa-project/docs/ENQUETE-niveau-reserve.md § 6.1) : vrai, un
    // membre de la reserve (EstGuildeDeReserve) ne recoit plus l'XP de victime JcJ du
    // mercenaire (MercenaryRewards::ComputeReward). Faux (defaut) : comportement d'avant.
    // Interrupteur PROPRE : couper X0 ne touche pas a la reserve (prime, embuscade,
    // guildes), que directeurReserveGuildes porte seule. Inerte si la reserve est vide.
    bool directeurReserveSansXpMercenaire;
    // Recompenses des primes, lot R3 (03/10/2026, coa-project/docs/CONCEPTION-recompenses-primes.md) :
    // inertes tant que `recompenser` n'est pas dans directeurVerbes. Liste blanche des objets
    // qu'un courrier de recompense peut porter (« 1397885,… », lecture stricte : un morceau mal
    // forme, aucun objet) et plafond d'argent d'UNE commande, en cuivres.
    std::string directeurRecompenseObjets;
    uint32 directeurRecompenseMaxCuivre;
    // Traqueur de contrat, lot T1 (12/10/2026, coa-project/docs/CONCEPTION-primes-traqueur-bots.md) :
    // l'addon CoABounty demande `COABH\tTRAQUE` (chuchotement d'addon a soi) ; le serveur repond
    // (prefixe COABT) la distance et la direction de la cible du contrat ACTIF du joueur. Faux
    // (defaut) : la demande est consommee sans reponse, l'addon n'affiche rien.
    bool directeurTraqueur;
    uint32 directeurTraqueurIntervalleMs;     // une lecture par joueur au plus toutes les N ms (1000-60000)
    // Lot T3 (12/10/2026) : lignes `W` (avis de recherche, table coa_dir_avis ecrite par contrats.py) dans
    // l'onglet. Faux (defaut) : aucune requete sur la table (elle peut ne pas exister).
    bool directeurAvis;
    // Lots T4-T5 (12/10/2026) : REACTIONS des cibles de contrat (params.reaction de l'arc `contrat`, tiree par
    // contrats.py) : `defense` (engage le chasseur a vue), `fuite_gardes` (fuit une fois, ses gardes l'engagent).
    // Faux (defaut) : aucune lecture, la cible reste passive (C1).
    bool directeurReactions;
    // Lot T6 (13/10/2026) : `fuite_monture` (monte, fuit loin, descend et attend ; deux fuites), `soudoiement` (offre
    // un pot-de-vin au chasseur), et la seconde fuite de `fuite_gardes`. Faux (defaut) : ces deux noms sont inconnus
    // (cible passive, journalise une fois) et `fuite_gardes` fuit une seule fois, comme T5.
    bool directeurReactionsT6;
    // Lot D5 « l'imprevu », partie jeu (15/10/2026, coa-project/docs/ETUDE-directeur-D5-imprevu.md § 3.1, § 3.2 ;
    // code dans Ai/Coa/CoaDirecteurImprevu.inc). Faux (defaut) : les verbes `attirer` et `soigner` sont refuses
    // `attirer_eteint` / `soigner_eteint` (meme listes dans Directeur.Verbes), le registre se vide a la seconde.
    bool directeurAttirer;
    bool directeurSoigneurs;
    // Banc D5 avec les bots (15/10/2026) : `attirer` admet un bot aleatoire comme `joueur`, `soigner` comme patient
    // (lignes marquees `banc=1`). Faux (defaut) : un humain est exige, a l'identique.
    bool directeurImprevuBanc;
    // Lot C des evenements (04/10/2026, coa-project/docs/EVENEMENTS-grimoire-directeur.md § 2.1-2.3) :
    // inertes tant que `evenement` n'est pas dans directeurVerbes. LISTES BLANCHES FERMEES des ids
    // qu'une commande `evenement` peut jouer (« id,id », lecture stricte : un morceau mal forme et
    // AUCUN id n'est permis). Defauts : les ids notes ✔ au banc de rendu du 04/10
    // (coa-project/docs/EVENEMENTS-banc-rendu-verdicts.md).
    std::string directeurEvenementAmbiances;   // sorts de filtre d'ecran, poses sur le joueur
    std::string directeurEvenementSons;        // SoundEntries.dbc, sous-action `son` (au joueur, direct)
    std::string directeurEvenementSonsPoint;   // `son:point` (positionnel) : coupure DBC >= 100 yd seulement
    std::string directeurEvenementMusiques;    // SoundEntries.dbc (type 28)
    std::string directeurEvenementKits;        // SpellVisualKit.dbc (effets instantanes)
    std::string directeurEvenementSorts;       // aures cosmetiques durables, posees par un declencheur
    std::string directeurEvenementSortsHaut;   // les memes, permises au SEUL lieu `haut` (45213) ; vide
    uint32 directeurEvenementReaffirmationSecondes;   // ambiance : repose de l'aure (5-25 s)
    uint32 directeurEvenementHauteurYd;               // lieu `haut` : declencheur au-dessus du sol (3-40)
    uint32 directeurEvenementDistanceYd;              // point a au plus N yd du joueur (10-100)
    uint32 directeurEvenementDeclencheursParJoueur;   // declencheurs vivants par joueur (1-16)
    // Lot D des evenements (05/10/2026, grimoire § 2.5-2.6, § 2.10) : sous-actions invoquer, ordre,
    // retirer. ETEINTES tant que directeurEvenementApparitions est faux (retirer excepte).
    bool directeurEvenementApparitions;               // interrupteur d'invoquer et d'ordre (defaut faux)
    std::string directeurEvenementCreatures;          // entrees permises (plage 9100000-9109999 imposee)
    std::string directeurEvenementSortsArrivee;       // sorts d'arrivee permis (7398 « naissance »)
    bool directeurEvenementArriveeCiel;               // arrivee `ciel` (pose en hauteur, descente) ; faux : au sol
    // Autonomie des evenements (11/10/2026, coa-project/docs/CONCEPTION-autonomie-evenements.md § 5) : un BOT non
    // mercenaire peut servir d'ANCRE (joueur de reference) a `evenement` aux lieux point/haut, a invoquer/ordre et a la
    // presence `peupler` F-/T-/L-/Q- (CoaDirecteur.cpp, AncreDEvenement). Faux (defaut) : humain seul, comme avant.
    bool directeurEvenementAncreBot;
    // Ressources rares des evenements (11/10/2026, coa-project/docs/CONCEPTION-ressources-rares-evenements.md § 3.1,
    // decision D4a) : au tirage du butin d'une creature, une ressource 9100100-9100149 n'est gardee que si sa tranche
    // est celle du NIVEAU DE LA CREATURE (CoaDirecteurRessources.inc). Faux (defaut) : rien n'est touche.
    bool directeurRessourcesNiveauMonstre;
    // Butin des bots aux evenements (12/10/2026, CONCEPTION-ressources-rares-evenements.md § 4.1, lot 3, D2 et D6).
    // directeurButinObjets : les ressources d'evenement (ids d'item_template), lues par le ramassage, la trace ET
    // l'HV des bots (coaHvRessources). Lues depuis les fils de carte ; ecrites par Initialize seul (fils a l'arret).
    std::vector<uint32> directeurButinObjets;
    bool directeurButinBots;    // 1 : un bot d'un groupe en butin libre ramasse SA part des ressources d'un corps d'evenement
    bool directeurButinTrace;   // 1 : ligne `coa directeur butin:` a chaque ressource ramassee (humain ou bot)
    uint32 directeurEvenementInvoquesParJoueur;       // invoques vivants par joueur (1-24)
    uint32 directeurEvenementInvocationDistanceYd;    // point d'apparition a au plus N yd du joueur (20-250)
    // Banc d'essai de prime (03/10/2026, coa-project/outils/banc-prime.py) : guids bas
    // de BOTS separes par des virgules ("4242,4243"). Un bot liste est traite COMME UN
    // HUMAIN PRIME, pour la prime seulement (verbes `prime`, `chasser`, `recompenser`,
    // ligne `coa directeur mort:`) : sa regle est le FFA de playerbots hors lieu sans
    // PvP au lieu de l'aura High Risk ; il ne convoite personne (riposte permise), ne
    // bouge pas (composition `stay`), n'est ni teleporte ni re-randomise. Vide (defaut) :
    // rien ne change. Lecture STRICTE, comme ReserveGuildes (un morceau mal forme, vide
    // compris : AUCUN bot). Lue au FIL MONDE seul (CoaDirecteur, chaque seconde), qui
    // en tient une copie pour les fils de carte (CoaDirecteur::CibleDeBanc).
    std::string directeurPrimeBanc;
    std::vector<uint32> PrimeBancGuids() const;

    // Oracle de decision Laya (voir Ai/Coa/CoaLayaOracle.h). Eteint par defaut :
    // sans lui le moteur se comporte exactement comme avant.
    bool layaEnabled;
    uint32 layaElitePercent;      // part des MERCENAIRES qui deviennent des elites
    std::string layaEndpoint;     // <ipv4>:<port>, aucune resolution DNS
    uint32 layaMaxAgeMs;          // au-dela, la reponse en cache est ignoree
    uint32 layaPeriodMs;          // delai minimal entre deux demandes pour un bot
    uint32 layaVetoRatio;         // en %, du favori du modele ; en dessous, veto
    uint32 layaVetoMaxRelevance;  // au-dessus, l'action est hors d'atteinte du veto
    uint32 layaVetoMax;           // refus au maximum par fenetre de layaPeriodMs
    bool layaVeto;                // canal action : le multiplicateur-veto (mesure sans effet)
    bool layaSorts;               // canal sort : l'oracle ordonne les sorts d'attaque
    bool layaCibles;              // canal cible : l'oracle choisit qui frapper, a l'acquisition
    // Part des RENCONTRES de combat que l'oracle a le droit de piloter, en %.
    // Le tirage se fait a l'ouverture de la rencontre, dans CoaRegistreCombat,
    // et ne depend de rien de ce qui precede : c'est ce qui fait passer la
    // mesure de l'observation a l'experience. 100 = comportement d'avant la
    // greffe, 0 = l'oracle ne pilote plus rien.
    uint32 layaTirageAuSort;

    // Un bot est mercenaire ou non de facon STABLE dans le temps : la decision
    // derive de son GUID, pas d'un tirage. Sans cela un bot changerait de camp
    // a chaque appel, ce qui rendrait le monde illisible.
    bool IsMercenary(uint64 botGuid) const;
    bool IsLayaElite(uint64 botGuid) const;

    std::vector<uint32> pvpProhibitedZoneIds;
    std::vector<uint32> pvpProhibitedAreaIds;
    bool fastReactInBG;

    bool randombotsWalkingRPG;
    bool randombotsWalkingRPGInDoors;
    uint32 minEnchantingBotLevel;
    uint32 limitEnchantExpansion;
    uint32 limitGearExpansion;
    uint32 randombotStartingLevel;
    bool enablePeriodicOnlineOffline;
    float periodicOnlineOfflineRatio;
    bool gearscorecheck;
    bool randomBotPreQuests;
    bool botSendMailEnabled;

    bool guildTaskEnabled;
    uint32 minGuildTaskChangeTime, maxGuildTaskChangeTime;
    uint32 minGuildTaskAdvertisementTime, maxGuildTaskAdvertisementTime;
    uint32 minGuildTaskRewardTime, maxGuildTaskRewardTime;
    uint32 guildTaskAdvertCleanupTime;
    uint32 guildTaskKillTaskDistance;

    uint32 iterationsPerTick;

    std::mutex m_logMtx;
    bool enableAutoTradeOnItemMention;
    std::vector<std::string> tradeActionExcludedPrefixes;
    std::vector<std::string> allowedLogFiles;
    std::unordered_map<std::string, std::pair<FILE*, bool>> logFiles;

    std::vector<std::string> botCheats;
    uint32 botCheatMask = 0;

    struct worldBuff
    {
        uint32 spellId;
        uint32 factionId;
        uint32 classId;
        uint32 specId;
        uint32 minLevel;
        uint32 maxLevel;
    };

    std::vector<worldBuff> worldBuffs;

    uint32 commandServerPort;
    bool perfMonEnabled;
    bool summonWhenGroup;
    ShowHideCosmetic randomBotShowHelmet;
    ShowHideCosmetic randomBotShowCloak;
    bool randomBotFixedLevel;
    bool disableRandomLevels;
    float randomBotXPRate;
    uint32 randomBotAllianceRatio;
    uint32 randomBotHordeRatio;
    bool disableDeathKnightLogin;
    bool limitTalentsExpansion;
    uint32 botActiveAlone;
    uint32 BotActiveAloneDurationSeconds;
    uint32 BotActiveAloneForceWhenInRadius;
    bool BotActiveAloneForceWhenInZone;
    bool BotActiveAloneForceWhenInMap;
    bool BotActiveAloneForceWhenIsFriend;
    bool BotActiveAloneForceWhenInGuild;
    bool botActiveAloneSmartScale;
    uint32 botActiveAloneSmartScaleDiffLimitfloor;
    uint32 botActiveAloneSmartScaleDiffLimitCeiling;
    uint32 botActiveAloneSmartScaleWhenMinLevel;
    uint32 botActiveAloneSmartScaleWhenMaxLevel;

    bool freeMethodLoot;
    int32 lootNeedRollLevel;
    bool lootGreedRollLevel;
    bool lootRollRecipe;
    bool lootRollDisenchant;
    std::string autoPickReward;
    bool autoEquipUpgradeLoot;
    float equipUpgradeThreshold;
    bool twoRoundsGearInit;
    bool syncQuestWithPlayer;
    bool syncQuestForPlayer;
    bool dropObsoleteQuests;
    bool allowLearnTrainerSpells;
    bool autoPickTalents;
    bool autoUpgradeEquip;
    int32 hunterWolfPet;
    int32 defaultPetStance;
    int32 petChatCommandDebug;
    bool autoLearnTrainerSpells;
    bool autoLearnTradeskillSpells;
    bool coaSpecRotations;
    uint32 coaHealerManaReserve;
    uint32 coaCasterManaReserve;
    // --- CoaArene IA (10/10/2026, Ai/Class/Coa/CoaAreneIa.inc) : tout a 0/faux par
    // defaut, la strategie « coa arene » n'est alors meme pas composee.
    bool coaAreneIaCommune;      // L0 : toutes classes CoA, en arene et en duel
    bool coaAreneIaRanger;       // L1 : Ranger a distance
    bool coaAreneMondeOuvert;    // etend L0/L1 au un contre un JcJ du monde ouvert (banc)
    uint32 coaAreneTirage;       // % d'engagements ou l'IA s'applique (tirage par engagement)
    bool coaAreneIaPiliers;      // L2 : Ranger en ARENE, casser la ligne de vue derriere un obstacle (defaut faux)
    bool coaAreneIaSurvie;       // survie d'arene 1v1 et duel (11/10) : paliers, soins sur soi, dissipation rentable (defaut vrai)
    // --- fin CoaArene IA
    // --- IA soins/survie des bots CoA (16/10/2026, docs/DIAG-banc-soins-*-20261016.md, Ai/Class/Coa/CoaAiObjectContext.cpp).
    // Chaque option garde un changement de comportement ; toutes VRAIES par defaut, a 0 l'ancien comportement revient.
    bool coaIaClassement;        // lot 1 : classement des sorts (boucliers sur allie, surcharges, soin d'autrui, Babify)
    bool coaIaPostureRole;       // lot 2 : une specialisation tank prend la posture qui ouvre ses defensifs
    bool coaIaSurvie;            // lot 3 : survie de tous les roles (paliers 65/45/30 %, perte rapide, soin sur soi, vol de vie)
    bool coaIaSurviePotion;      // lot 3 : potion de soin des 45 % de vie quand aucun defensif n'est pret
    bool coaIaReserveSoigneur;   // lot 4 : un soigneur en groupe n'attaque en mana que groupe plein et mana haute
    uint32 coaIaReserveSoigneurMana; // lot 4 : seuil de mana (%) de la reserve ci-dessus
    bool coaIaHotUnique;         // lot 4 : un HoT a cible unique n'est pas deplace s'il tient ailleurs, sauf urgence
    bool coaIaAuraSurLanceur;    // lot 4 : « deja pose » lu sur le lanceur quand l'aura du sort est sur le lanceur
    bool coaIaTriSoins;          // lot 4 : soins tries par deficit de la cible et rendement en mana
    bool coaIaUtiliteAlignee;    // lot 4 : isUseful des soins et defensifs aligne sur ce que CastFirst refusera
    bool coaIaFormeMaudite;      // lot 5 : Bloodmage sous lowHealth en combat -> forme maudite, puis ses defensifs de forme
    bool coaIaVenomancerSoin;    // lot 5 : Venomancer sous 35 % en combat -> quitte sa forme, se soigne, la reprend
    bool coaIaSoigneurResteEnCombat; // C1 (16/10) : soigneur en combat, cible invalide effacee sans quitter le moteur de combat
    bool coaIaLeverServiteur;    // I7 (17/10) : Necromancer a Life Force sans serviteur en leve un hors combat (en plus de Survie)
    bool coaIaReserveDefensif;   // I8 (17/10) : sous MediumHealth, mana du defensif et d'un soin gardee (en plus de Survie)
    uint32 coaIaReserveDefensifSeuil; // D1 (19/10) : vie (%) sous laquelle la reserve I8 s'arme ; 100 = des le combat, 0 = MediumHealth
    bool coaIaReserveToutesRessources; // D1 (19/10) : reserve I8 dans la ressource du defensif (energie, rage...), pas la mana seule
    bool coaIaSoigneurNeFrappePas; // D2 (19/10) : soigneur en groupe, membre sous MediumHealth -> ni ressource ni GCD en attaque
    bool coaIaCoutEffectif;      // D2 (19/10) : DropManaSpells lit le cout du sort effectivement lance (EffectiveSpell)
    bool coaIaSurvieAttendEtourdi; // D8 (19/10) : bot etourdi, ses sorts de soin et de survie refuses ne sont pas « prets »
    bool coaIaSoinCoefficientScript; // I15 (19/10) : estimation des soins du Tinker avec le coefficient ajoute par le module
    bool coaIaAttaqueQuiSoigne;  // I17 (19/10) : la reserve C4 et D2 gardent les attaques qui soignent un allie (Heretic)
    bool coaIaProtegerAllie;     // I17 (19/10) : soigneur, « coa protection » pose Void Shield / Hand of Yogg-Saron sur l'allie
    bool coaIaTracerEcarts;      // D7 (19/10) : banc des soins, ligne « rien » avec le dernier ecart silencieux de CastFirst
    bool coaIaPretCommeCastFirst; // D7 (19/10) : CoaIaUnPret s'arrete a la recharge globale comme CastFirst (tours a vide)
    uint32 coaIaAttaqueQuiSoigneMana; // ia5 (20/10) : I17 ne garde une attaque qui soigne payee en mana qu'au-dessus de ce % de mana ; 0 = toujours
    bool coaIaReserveSurviePalier; // ia5 (20/10) : la survie garde la ressource du defensif d'un palier plus bas (Embrace sous 30 %...)
    bool coaGarderPassifs;       // ia5 (20/10) : Randomize (CancelAuras) garde les auras passives du bot (Harvester, talents...)
    bool coaVolZoneVolable;      // R-20261020-1622 (20/10) : monture volante et vol des bots seulement en zone ou le vol est permis
    bool coaSansFlottement;      // R-20261020-1622 (20/10) : aucun sort qui fait flotter le bot ou ralentit sa chute (classement)
    bool coaTracerVol;           // R-20261020-1622 (20/10) : ligne « coa vol: » quand un bot passe en vol ou est projete
    uint32 coaFamilierDefensif;  // R-20261020-2207 (20/10) : familier (Pet) passif d'un bot CoA -> defensif ; 0 avant, 1 Chien d'ombre, 2 tous
    bool coaIaPostureSurvie;     // ia5 (20/10) : non-tank en combat sous MediumHealth -> la posture qui ouvre ses defensifs, retour hors combat
    // --- fin IA soins/survie
    uint32 lootLogMinQuality;
    bool autoDoQuests;
    bool enableNewRpgStrategy;
    std::unordered_map<NewRpgStatus, uint32> RpgStatusProbWeight;
    bool syncLevelWithPlayers;
    bool randomBotConcentrateInPlayerZone;
    bool autoLearnQuestSpells;
    bool autoTeleportForLevel;
    bool randomBotGroupNearby;
    int32 enableRandomBotTrading;
    uint32 tweakValue;  // Debugging config

    uint32 randomBotArenaTeamMaxRating;
    uint32 randomBotArenaTeamMinRating;
    uint32 randomBotArenaTeamMinLevel;  // CoA (17/10/2026, porte d'Armageddon) : niveau minimal d'un bot en equipe d'arene (70 avant)
    uint32 randomBotArenaTeam2v2Count;
    uint32 randomBotArenaTeam3v3Count;
    uint32 randomBotArenaTeam5v5Count;
    bool deleteRandomBotArenaTeams;

    uint32 selfBotLevel;
    int32 botTextLocale;  // -1: follow connected players' client locale, 0-8: force this locale
    bool downgradeMaxLevelBot;
    bool equipAndSpecPersistence;
    int32 equipAndSpecPersistenceLevel;
    int32 groupInvitationPermission;
    bool keepAltsInGroup = false;
    bool KeepAltsInGroup() const { return keepAltsInGroup; }
    bool allowSummonInCombat;
    bool allowSummonWhenMasterIsDead;
    bool allowSummonWhenBotIsDead;
    int reviveBotWhenSummoned;
    bool botRepairWhenSummon;
    bool autoInitOnly;
    bool resetInstanceIdForAltBots;
    float autoInitEquipLevelLimitRatio;
    int32 maxAddedBots;
    int32 addClassCommand;
    int32 addClassAccountPoolSize;
    int32 maintenanceCommand;
    bool altMaintenanceAttunementQs,
            altMaintenanceBags,
            altMaintenanceAmmo,
            altMaintenanceFood,
            altMaintenanceReagents,
            altMaintenanceConsumables,
            altMaintenancePotions,
            altMaintenanceTalentTree,
            altMaintenancePet,
            altMaintenancePetTalents,
            altMaintenanceClassSpells,
            altMaintenanceAvailableSpells,
            altMaintenanceSkills,
            altMaintenanceReputation,
            altMaintenanceSpecialSpells,
            altMaintenanceMounts,
            altMaintenanceGlyphs,
            altMaintenanceKeyring,
            altMaintenanceGemsEnchants;
    int32 autoGearCommand, autoGearCommandAltBots, autoGearQualityLimit, autoGearScoreLimit;
    int32 autoGearBisCommand;

    uint32 useGroundMountAtMinLevel;
    uint32 useFastGroundMountAtMinLevel;
    uint32 useFlyMountAtMinLevel;
    uint32 useFastFlyMountAtMinLevel;

    // stagger flightpath takeoff
    uint32 botTaxiDelayMin;
    uint32 botTaxiDelayMax;
    uint32 botTaxiGapMs;
    uint32 botTaxiGapJitterMs;

    std::string const GetTimestampStr();
    bool hasLog(std::string const fileName)
    {
        return std::find(allowedLogFiles.begin(), allowedLogFiles.end(), fileName) != allowedLogFiles.end();
    };
    bool openLog(std::string const fileName, char const* mode = "a");
    bool isLogOpen(std::string const fileName)
    {
        auto it = logFiles.find(fileName);
        return it != logFiles.end() && it->second.second;
    }
    void log(std::string const fileName, char const* str, ...);

    void loadWorldBuff();

    static std::vector<std::vector<uint32>> ParseTempTalentsOrder(uint32 cls, std::string temp_talents_order);
    static std::vector<std::vector<uint32>> ParseTempPetTalentsOrder(uint32 spec, std::string temp_talents_order);

    bool restrictHealerDPS = false;
    std::vector<uint32> restrictedHealerDPSMaps;
    bool IsRestrictedHealerDPSMap(uint32 mapId) const;

    std::vector<uint32> excludedHunterPetFamilies;

    // Random bot level brackets (periodic redistribution across per-faction level ranges). See
    // RandomBotLevelMgr; percentages here are the as-configured values, not the runtime working copy.
    bool levelBracketsEnabled;
    uint32 levelBracketsCheckFrequency;
    uint32 levelBracketsFlaggedCheckFrequency;
    uint32 levelBracketsFlaggedProcessLimit;
    bool levelBracketsIgnoreGuildWithRealPlayers;
    bool levelBracketsIgnoreArenaTeamBots;
    bool levelBracketsIgnoreFriendListed;
    std::vector<std::string> levelBracketsExcludeNames;
    uint8 levelBracketsNumRanges;
    std::vector<LevelBracketConfig> levelBracketsAlliance;
    std::vector<LevelBracketConfig> levelBracketsHorde;
    bool levelBracketsDynamicDistribution;
    float levelBracketsRealPlayerWeight;
    bool levelBracketsSyncFactions;
    bool levelBracketsFreshStart;
    bool levelBracketsFreshStartSpread;

    // Random bot level reset (reset random bots reaching max level). See RandomBotLevelMgr.
    bool resetBotLevelEnabled;
    uint8 resetBotLevelMaxLevel;
    uint8 resetBotLevelResetTo;
    uint8 resetBotLevelSkipFrom;
    uint8 resetBotLevelSkipTo;
    uint8 resetBotLevelChance;
    bool resetBotLevelScaledChance;
    bool resetBotLevelRestrictTimePlayed;
    uint32 resetBotLevelMinTimePlayed;
    uint32 resetBotLevelPlayedTimeCheckFrequency;
    bool resetBotLevelIgnoreGuildWithRealPlayers;
    std::vector<std::string> resetBotLevelExcludeNames;

    // ------------------------------------------------------------ Arene1v1
    // CoA (10/10/2026, coa-project/docs/CONCEPTION-arene-1v1.md ; Ai/Coa/CoaArene1v1.cpp) :
    // escarmouche 1 contre 1 non classee, un humain contre un bot Ranger « gladiateur »
    // mis a son niveau. Tout est inerte tant que arene1v1Enabled est faux (defaut) : ni
    // file 11, ni option de dialogue, ni maintien en ligne.
    bool arene1v1Enabled;
    uint32 arene1v1NiveauMin;              // niveau minimal du joueur (defaut 10)
    // Guids bas des gladiateurs, separes par des virgules ("4242,4243"). Lecture STRICTE
    // (un morceau mal forme, vide compris : AUCUN gladiateur). Lue au FIL MONDE seul.
    std::string arene1v1Gladiateurs;
    bool arene1v1MaintenirEnLigne;         // reconnecter un gladiateur hors ligne (toutes les 5 min)
    // Secondes pendant lesquelles le gladiateur reste visible dans l'arene apres la fin tant
    // que le joueur y est (defaut 30, 0 = depart immediat, borne a 110 : le coeur expulse a 120 s).
    uint32 arene1v1DelaiDepartS;
    // CoA (10/10/2026 soir) : classes CoA (12-32) admises comme gladiateurs, ids separes par des
    // virgules ; vide = toutes. Lue au FIL MONDE seul (inscription, dialogue).
    std::string arene1v1Classes;
    // Trace de combat des gladiateurs (« coa arene trace: », CoaAreneIa.inc) : une ligne par
    // tentative de sort et une par seconde, 5 lignes/s au plus par gladiateur. Defaut 1 : elle ne
    // concerne que les gladiateurs engages dans une arene 1v1 en cours.
    bool arene1v1Trace;
    // Banc d'arene 1v1 BOT CONTRE BOT (`.coa arene banc`, CoaArene1v1.cpp ; coa-project/outils/banc-arene.py).
    // Defaut faux : la commande refuse (`desactive_banc`), rien d'autre ne change.
    bool arene1v1Banc;

    // ------------------------------------------------------------ CoaCompagnons
    // CoA (25/10/2026, coa-project/docs/CONCEPTION-bots-compagnons-directeur.md, lot 1 ; Ai/Coa/CoaCompagnons.cpp) :
    // chaque personnage humain a ses bots attitres (allies de sa faction, vis-a-vis de l'autre), reveilles a sa
    // connexion, endormis apres son depart. Relues a chaque recharge (Initialize), SAUF Enable : CoaCompagnons.cpp le fige
    // a sa premiere lecture (revue du 26/10, M1). Enable faux (defaut) : la table coa_compagnon n'est meme pas lue,
    // EstCompagnon rend toujours faux, aucun effet.
    bool coaCompagnonsEnable;
    uint32 coaCompagnonsSeuilNiveau;       // niveau minimal de l'humain (defaut 10)
    uint32 coaCompagnonsAllies;            // allies par personnage (defaut 1 depuis la revision du 26/10, borne a 5)
    uint32 coaCompagnonsVisAVis;           // vis-a-vis par personnage (defaut 0 depuis la revision du 26/10, borne a 5)
    uint32 coaCompagnonsGrace;             // secondes entre le depart de l'humain et le sommeil (defaut 900)
    uint32 coaCompagnonsRenouvellement;    // secondes entre deux renouvellements de « add » (defaut 120, 30..600)
    uint32 coaCompagnonsEchelonnement;     // secondes entre deux reveils (defaut 2, 0..60)
    uint32 coaCompagnonsMaxEveilles;       // plafond global de compagnons eveilles (defaut 40 ; 0 = aucun reveil)
    uint32 coaCompagnonsEcartBas;          // a la constitution, jamais plus de N niveaux sous l'humain (defaut 5)
    // Revision du 26/10/2026 (conception, en tete) : une reserve de bots par tranche de niveau (1-10 ... 51-60) ET par
    // faction, toujours les memes, reveillee des qu'un humain de la tranche est en jeu, endormie Grace secondes apres
    // le dernier. Niveau fige (PLAYER_FLAGS_NO_XP_GAIN, memes exemptions que les compagnons). Enable fige au demarrage.
    bool coaReserveEnable;                 // defaut faux : table coa_reserve non lue, aucun effet
    uint32 coaReserveParTranche;           // bots par tranche et par faction (defaut 8, borne a 30)
    uint32 coaReserveMaxEveilles;          // plafond global de bots de reserve eveilles (defaut 48 ; 0 = aucun reveil)
    uint32 coaReserveSeuilConstitution;    // reserve completee quand elle compte moins de N membres (defaut = ParTranche)
    // Lot 3 (27/10/2026) : le compagnon eveille rattrape le niveau de son humain (GiveLevel + sorts, comme `.coa niveau`),
    // jamais au-dessus (PLAYER_FLAGS_NO_XP_GAIN des qu'il l'a rejoint). Equipement refait si l'ecart atteint N niveaux.
    bool coaCompagnonsRattrapage;          // defaut faux
    uint32 coaCompagnonsRattrapageEquipement;  // ecart a partir duquel l'equipement est refait (defaut 3)
    // Souvenirs (27/10/2026, Ai/Coa/CoaSouvenirs.cpp) : ce que l'humain a vecu (morts, quetes, moments durs, recoltes,
    // exploits, niveaux), seul ou avec son compagnon ; le compagnon eveille lui en chuchote un de temps en temps.
    bool coaSouvenirsEnable;               // defaut faux : rien n'est note ni dit, la table n'est pas lue
    uint32 coaSouvenirsPremiereParole;     // secondes entre le reveil du compagnon et sa premiere parole (defaut 300)
    uint32 coaSouvenirsIntervalleMin;      // secondes entre deux paroles, au moins (defaut 1200)
    uint32 coaSouvenirsIntervalleMax;      // et au plus (defaut 2400)

private:
    PlayerbotAIConfig() = default;
    ~PlayerbotAIConfig() = default;

    PlayerbotAIConfig(PlayerbotAIConfig const&) = delete;
    PlayerbotAIConfig& operator=(PlayerbotAIConfig const&) = delete;

    PlayerbotAIConfig(PlayerbotAIConfig&&) = delete;
    PlayerbotAIConfig& operator=(PlayerbotAIConfig&&) = delete;
};

#define sPlayerbotAIConfig PlayerbotAIConfig::instance()

#endif
