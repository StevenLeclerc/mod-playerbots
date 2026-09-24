/*
 * Conquest of Azeroth — registre des rencontres de combat.
 *
 * POURQUOI CE FICHIER EXISTE, ET CE QU'IL REMPLACE
 *
 * Trois jours de mesure de l'oracle Laya ont compare des TAUX HORAIRES entre
 * deux groupes de bots : kills par heure, niveaux par heure, part du temps
 * passe mort. Tout est sorti « dans le bruit ». Le calcul de puissance, fait
 * trop tard, a montre pourquoi : il aurait fallu que l'oracle DOUBLE le nombre
 * de kills pour etre vu (P-101). 43 % des bots ne tuent jamais, et 96 % des
 * kills viennent de 75 bots sur 242. Moyenner un taux horaire sur une
 * population qui se bat une minute par heure dilue le signal jusqu'a
 * l'invisible.
 *
 * Ce registre mesure donc la RENCONTRE, pas l'heure. Une rencontre est une
 * tranche de temps pendant laquelle un bot est en combat : elle a une duree,
 * un cout en points de vie, une issue. C'est l'unite ou l'effet d'une rotation
 * se voit, s'il existe.
 *
 * LE PLAN D'EXPERIENCE QU'IL PERMET, ET QUI VAUT MIEUX QUE LE PRECEDENT
 *
 * Chaque rencontre est etiquetee par la couverture Laya : la part des ticks ou
 * une reponse fraiche du canal SORT etait disponible. Comme l'oracle repond
 * par intermittence — periode d'une seconde, reponse servant le tour suivant,
 * pertes reseau —, un MEME bot produit des rencontres servies et des
 * rencontres non servies.
 *
 * On compare donc un bot A LUI-MEME. Tout ce qui le distingue durablement d'un
 * autre — sa classe, son niveau, son equipement, l'endroit ou il se bat —
 * disparait de la comparaison. C'est incomparablement plus puissant que
 * d'opposer deux groupes de cent bots dont on espere qu'ils se ressemblent.
 *
 * CE QU'IL NE MESURE PAS, ET POURQUOI
 *
 * Les degats infliges. Les lire demanderait un hook sur Unit::DealDamage,
 * c'est-a-dire une modification du coeur et un cout a chaque coup porte. La
 * duree de la rencontre et le cout en points de vie disent deja l'essentiel
 * d'une rotation efficace : tuer plus vite et encaisser moins.
 *
 * LE TIRAGE AU SORT PAR RENCONTRE — CE QUI TRANSFORME L'OBSERVATION EN EXPERIENCE
 *
 * Etiqueter les rencontres par la couverture Laya reste une OBSERVATION, et
 * elle est biaisee. Une reponse fraiche n'existe au debut d'un combat que si le
 * bot s'est battu juste avant : la categorie « servie » selectionne donc les
 * ENCHAINEMENTS de combats, pas un echantillon au hasard. Mesure : les
 * rencontres servies ont plus d'assaillants (+0,16, significatif) et
 * appartiennent a des bots qui comptent 45,7 rencontres en moyenne contre 83,6
 * pour les autres.
 *
 * Le registre tire donc au sort, A L'OUVERTURE DE CHAQUE RENCONTRE, si elle
 * sera pilotee par l'oracle ou laissee au tourniquet d'origine. Le tirage ne
 * depend de rien de ce qui precede : ni du combat d'avant, ni de l'etat du
 * cache, ni du reseau. C'est l'assignation aleatoire qui manquait.
 *
 * Le tirage est DETERMINISTE, pas `rand()` : un hachage FNV-1a du couple
 * (debutMs, guid) compare au seuil `AiPlayerbot.Laya.TirageAuSort`. Meme
 * journal, meme conclusion, rejouable. Voir TirerPilotage dans le .cpp pour la
 * forme exacte du hachage et les mesures d'equirepartition qui l'ont choisie.
 *
 * Le seuil vaut 100 par defaut : la greffe est alors SANS EFFET, et
 * OrdonnerParLaya n'interroge meme pas le registre.
 *
 * CE QU'IL COUTE
 *
 * Un appel par bot et par tick, qui ne fait qu'un test booleen tant que rien ne
 * change d'etat. Rien n'est ecrit hors des transitions d'entree et de sortie de
 * combat. Le registre ne suit que les mercenaires, et seulement quand l'oracle
 * est arme : sans cela il ne fait rien du tout.
 *
 * LE VERROU, ET POURQUOI IL A FALLU L'AJOUTER
 *
 * `Observer` est appele depuis `PlayerbotAI::UpdateAI`, lui-meme appele depuis
 * le hook `OnPlayerAfterUpdate` de `Player::Update` — donc depuis les
 * **MapUpdate.Threads** fils de mise a jour de carte, dix en service. Deux bots
 * sur deux cartes differentes touchaient donc `_encours` EN MEME TEMPS, sans
 * aucune synchronisation : une insertion qui declenche un rehash pendant qu'un
 * autre fil parcourt un seau est une corruption de tas, pas une approximation.
 *
 * C'est exactement le defaut qui a fait segfaulter ce serveur le 2026-09-20
 * (P-050, `AscensionResourceService` et ses deux conteneurs globaux sans
 * verrou). Le registre le portait depuis sa creation ; `EstPilotee` ajoutait un
 * troisieme chemin de lecture, ce qui a rendu la chose visible.
 *
 * Le verrou n'est pris qu'APRES les deux tests d'echappement (oracle arme,
 * bot mercenaire), et la ligne de journal est ecrite EN DEHORS : la section
 * critique ne contient que la manipulation de la table.
 */

#ifndef PLAYERBOTS_COAREGISTRECOMBAT_H
#define PLAYERBOTS_COAREGISTRECOMBAT_H

// Define.h porte les alias uint8/uint32/uint64 du coeur : <cstdint> seul ne
// les declare pas, ce sont des typedef AzerothCore et non des types standard.
#include "Define.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

class Player;
class PlayerbotAI;

class CoaRegistreCombat
{
public:
    static CoaRegistreCombat& Instance();

    // Appele a chaque tick, pour chaque bot, depuis PlayerbotAI::UpdateAI.
    // Ne fait rien tant que l'etat de combat ne change pas.
    void Observer(Player* bot, PlayerbotAI* botAI);

    // L'horodatage d'ouverture de la rencontre en cours, ou 0 si aucune.
    // Sert a dire au modele depuis combien de temps le combat dure : une
    // rotation ne se joue pas pareil a la premiere seconde et a la trentieme.
    // Meme verrou, meme cout qu'EstPilotee : une lecture de table.
    uint32 DebutMs(uint64 guid) const;

    // Vrai si une rencontre est EN COURS pour ce bot et qu'elle est du bras
    // pilote. Hors combat — donc sans rencontre ouverte — rend false.
    bool EstPilotee(uint64 guid) const;

    // Le bot quitte le monde. La rencontre en cours est JETEE, pas close : la
    // cloture lit le Player (points de vie, classe, kills) et l'appel vient du
    // destructeur de PlayerbotAI, ou cet objet est en train de disparaitre.
    // Sans cet oubli, l'entree resterait pour toujours et la reconnexion
    // heriterait d'un debutMs, d'un pvDebut et d'un bras de tirage perimes.
    void Oublier(uint64 guid);

    std::string Compteurs() const;

private:
    CoaRegistreCombat() = default;
    CoaRegistreCombat(CoaRegistreCombat const&) = delete;
    CoaRegistreCombat& operator=(CoaRegistreCombat const&) = delete;

    struct Rencontre
    {
        uint32 debutMs = 0;
        uint8 pvDebut = 0;
        uint8 pvMin = 100;
        uint8 niveau = 0;
        uint32 assaillantsMax = 0;
        // QUI A OUVERT LE COMBAT — le champ le plus demande, et le seul qui
        // distingue un mercenaire qui CHASSE d'un mercenaire qui SUBIT. Les
        // deux produisent aujourd'hui la meme ligne, et avec 68 % de
        // mercenaires immobiles, l'hypothese par defaut est qu'ils subissent.
        //
        // Lu a l'OUVERTURE, pas au plus fort du combat : `assaillantsMax` est
        // un maximum sur toute la rencontre et ne dit rien de son declenchement.
        // A zero assaillant et une victime deja designee, le bot a initie.
        uint32 assaillantsOuverture = 0;
        bool victimeOuverture = false;
        // L'AIRE ET LA ZONE — DEUX IDENTIFIANTS, ET C'EST LE PIEGE.
        //
        // Le PvP mercenaire est impossible en sanctuaire et en capitale : les
        // deux posent `pvpInfo.IsInNoPvPArea`, qui fait sortir
        // `Player::UpdateFFAPvPState`. Mais ils ne se testent PAS sur le meme
        // identifiant, et la premiere version de ce champ lisait l'aire pour
        // les deux :
        //
        //   AREA_FLAG_SANCTUARY (0x800) -> sur l'AIRE  (PlayerUpdates.cpp:1277)
        //   AREA_FLAG_CAPITAL   (0x100) -> sur la ZONE (PlayerUpdates.cpp:1370)
        //
        // Une capitale n'a aucun bit de sanctuaire, et une sous-aire d'une
        // capitale ne porte pas forcement le bit capitale : lire l'aire seule
        // aurait rendu `capitale=0` pour un bot debout dans Orgrimmar des
        // qu'il se tenait dans une sous-aire. Symetriquement, lire la zone
        // seule ne voit aucun sanctuaire — Valley of Trials est une sous-aire
        // de Durotar, qui n'en est pas un.
        //
        // Sans ces deux champs, une moyenne melange des lieux ou la mecanique
        // peut fonctionner et des lieux ou elle ne peut pas : deux populations,
        // une seule moyenne.
        // CE QUE « capitale » RECOUVRE, ET CE N'EST PAS CE QUE LE MOT EVOQUE.
        // 31 zones portent AREA_FLAG_CAPITAL dans le DBC de ce royaume, pas
        // huit : les huit capitales de faction, plus Shattrath, Dalaran,
        // Twisting Nether, Dun Kazad, Unused Monastery et une vingtaine de
        // zones custom Ascension (10000-10331). Plus 125 sous-aires qui
        // portent aussi le bit, sans etre des zones.
        // Ecarter « les capitales » d'un depouillement ecarte donc tout cela.
        uint32 aire = 0;
        uint32 aireFlags = 0;
        uint32 zone = 0;
        uint32 zoneFlags = 0;
        // CONTRE QUI LE BOT S'EST BATTU. Sans ce compteur, une rencontre contre
        // un joueur et une contre une creature rendent la MEME ligne : un banc
        // cense opposer des bots a des paquets de monstres peut les opposer
        // entre eux sans que le journal le distingue. Soupconne sur le lot
        // « melee » du 2026-09-24, invérifiable faute de cette donnee.
        //
        // On compte les ticks, pas une seule observation : une rencontre change
        // de cible en cours de route, et « il a tape un joueur au moins une
        // fois » ne dit pas la meme chose que « il a passe sa rencontre sur un
        // joueur ».
        uint32 ticksJoueur = 0;
        uint32 ticksCreature = 0;
        uint32 ticks = 0;
        uint32 sondages = 0;        // ticks ou la couverture a ete sondee
        uint32 ticksServis = 0;     // sondages ou une reponse Laya fraiche existait
        // La meme mesure, bornee aux premieres secondes. Voir le commentaire
        // de PAS_DE_SONDAGE dans le .cpp : la couverture sur la rencontre
        // ENTIERE ne peut pas servir a classer la rencontre, elle depend de sa
        // duree. Celle-ci est mesuree avant que l'issue ne soit jouee.
        uint32 sondagesDebut = 0;
        uint32 servisDebut = 0;
        uint32 killsDebut = 0;
        bool mort = false;
        // Le bras de l'experience, tire UNE SEULE FOIS a l'ouverture et jamais
        // revu. `true` par defaut : une rencontre mal formee vaut le
        // comportement d'avant la greffe, pas son inverse.
        bool pilote = true;
    };

    // Tire le bras de la rencontre. Statique et sans etat : la reponse ne
    // depend que du couple donne et du seuil de configuration.
    static bool TirerPilotage(uint64 guid, uint32 debutMs);

    // Appelee HORS verrou, sur une copie de la rencontre deja retiree de la
    // table : elle ecrit dans le journal, ce qui n'a rien a faire dans une
    // section critique partagee par dix fils.
    void Clore(Player* bot, Rencontre const& r, char const* raison);

    mutable std::mutex _verrou;
    std::unordered_map<uint64, Rencontre> _encours;
    std::atomic<uint64> _ouvertes{0};
    std::atomic<uint64> _closes{0};
    std::atomic<uint64> _ignorees{0};
    std::atomic<uint64> _pilotees{0};
    // LE TIRAGE SE VERIFIE PAR BRAS, A CHAQUE ETAPE. Le journal du 2026-09-24
    // montrait 62 % de rencontres pilotees la ou le tirage en promet 50, dans
    // toutes les tranches de duree, avec onze bots sur quatre-vingt-dix-neuf
    // au-dela de 90/10. La fonction de tirage est pourtant equitable : simulee
    // a 49,9-50,0 % quelle que soit la granularite de l'horloge, et zero bot
    // sur trois cents au-dela de 90/10. Quelque chose ecarte donc des
    // rencontres APRES le tirage, et rien ne disait lesquelles.
    //
    // Ces trois compteurs repondent : combien ouvertes, combien jetees pour
    // trop courtes, combien oubliees a la deconnexion — bras par bras.
    std::atomic<uint64> _ignoreesPilotees{0};
    std::atomic<uint64> _oubliees{0};
    std::atomic<uint64> _oublieesPilotees{0};
};

#endif
