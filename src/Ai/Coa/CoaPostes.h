/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COAPOSTES_H
#define _PLAYERBOT_COAPOSTES_H

#include "DatabaseEnv.h"
// DatabaseEnv.h ne fait que DECLARER Field et ResultSet (DatabaseEnvFwd.h:24,
// :26). Les deux en-tetes ci-dessous sont ceux qui les DEFINISSENT, et sans eux
// ce fichier ne compile que dans les .cpp qui les incluent deja par ailleurs --
// une dette qui se paierait au premier nouvel appelant.
#include "Field.h"
#include "Log.h"
#include "QueryResult.h"
#include "Random.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/*
 * LE CATALOGUE DE POSTES DE GUET -- lot 4 de docs/CONCEPTION-mercenaire-chasseur.md.
 *
 * CE QU'IL EST. Une table de la base ACORE_WORLD, engendree HORS LIGNE par
 * coa-project/outils/engendrer-postes-guet.py, chargee une fois au demarrage et
 * ensuite LUE SEULEMENT. Elle contient des lieux de passage -- auberges,
 * voleries, cimetieres, carrefours du graphe de marche -- deja filtres de tout
 * lieu sans PvP, deja bornes en niveau et en camp.
 *
 * POURQUOI UNE TABLE, ET PAS GetTravelHubs. Le lot 2 tire ses postes dans
 * TravelMgr::GetTravelHubs, et ce cache est la meilleure source VIVANTE : la
 * mesure du dossier lui donne x18 contre un temoin. Mais il ne contient QUE des
 * aubergistes, sauf dans neuf zones listees en dur (TravelMgr.cpp:4733-4744),
 * et le projet a par ailleurs 3 781 noeuds de route et 15 041 aretes en base
 * (playerbots_travelnode / _link) qu'AUCUN code ne lit en service : leur unique
 * chargeur, TravelMgr::LoadQuestTravelTable (TravelMgr.cpp:1817), n'a aucun
 * appelant, et la ligne « >> Loaded {} travelNodes. » (TravelNode.cpp:2290)
 * n'apparait dans aucun journal. Le catalogue est la facon d'en tirer parti
 * sans reveiller ce chargeur, dont le cout au demarrage n'est pas mesure
 * (loadNodeStore + generateAll sur 1,4 M points).
 *
 * POURQUOI UN FICHIER SANS .cpp, encore. Meme raison que CoaChasse.h, et elle
 * n'a pas change : un .cpp NEUF n'est pas dans
 * /opt/coa/build-main/compile_commands.json tant que cmake n'a pas ete rejoue,
 * donc outils/verifier-syntaxe.py ne saurait pas le verifier ; et les sources du
 * module sont ramassees par un file(GLOB) au moment de la CONFIGURATION
 * (src/cmake/macros/AutoCollect.cmake:28), donc un .cpp ajoute sans
 * reconfigurer n'est pas compile et l'erreur n'apparait qu'a l'edition de liens.
 * Cet en-tete est inclus par deux .cpp deja compiles : Script/Playerbots.cpp
 * (qui le charge) et Ai/Class/Coa/CoaAiObjectContext.cpp (qui le lit, a travers
 * CoaChasse.h). Toutes ses fonctions sont donc `inline`, et l'unique instance
 * vit dans un static de fonction inline -- une seule pour tout le binaire, que
 * la norme garantit (C++20, -std=gnu++20 dans la ligne de compilation reelle).
 *
 * LE FICHIER SQL N'EST PAS APPLIQUE TOUT SEUL. Le serveur en service a
 * Updates.EnableDatabases = 0 (worldserver.conf) : le metteur a jour de base ne
 * tourne pas. Tant que l'exploitant n'a pas joue le .sql a la main, la table
 * n'existe pas -- et c'est le cas nominal a traiter, pas un cas d'erreur. D'ou
 * la sonde d'existence ci-dessous, et d'ou le repli integral sur le lot 2.
 */

/*
 * LA CLASSE D'UN POSTE -- colonne `source` du .sql.
 *
 * ELLE N'ETAIT PAS LUE, et c'est ce que la relecture a trouve. La requete de
 * Charger ne la demandait pas et la structure ne la portait pas : le chargeur
 * ne savait donc pas distinguer une auberge d'un carrefour, alors que tout
 * l'interet mesure du catalogue est dans cette distinction. Deux consequences,
 * toutes deux payees :
 *
 *   1. LE TIRAGE ETAIT ECRASE PAR LE NOMBRE. Le poids est porte par le POSTE ;
 *      or le catalogue compte ~1 500 carrefours pour ~90 auberges. A la carte 0
 *      et au niveau 20, les 100 carrefours a portee totalisaient 43 % du poids
 *      contre 25 % pour les 7 auberges : le facteur 18:1 mesure entre les deux
 *      sources etait renverse par le rapport de population 14:1. L'esperance
 *      d'enrichissement du poste tire tombait a ~6,8, contre ~18 au lot 2, ou
 *      TirerPoste ne rendait que des hubs. Le lot pouvait donc FAIRE BAISSER la
 *      metrique meme qu'il se donne pour preuve.
 *
 *   2. RIEN NE PERMETTAIT DE LE VOIR. Le lot n'ajoutait qu'un compteur agrege,
 *      « poste catalogue », qui monte pareil pour une auberge et pour un
 *      carrefour : CoaBots.log ne pouvait pas montrer d'ou venait la baisse.
 *
 * CE QUE LA CLASSE SERT DESORMAIS : un BUDGET DE TIRAGE par classe
 * (CoaNormaliserParClasse, plus bas) et un compteur par classe
 * (CoaChasseComptePosteClasse, Ai/Coa/CoaChasse.h).
 *
 * L'ORDRE DES VALEURS EST CELUI DE CoaPoidsDeClasse : les deux tables se lisent
 * ensemble, et COA_POSTE_SOURCES est leur taille commune.
 */
enum CoaSourcePoste : uint8
{
    COA_POSTE_AUBERGE = 0,
    COA_POSTE_VOLERIE,
    COA_POSTE_CIMETIERE,
    COA_POSTE_ROUTE,
    COA_POSTE_SOURCES
};

/*
 * LE POIDS DE LA CLASSE : l'enrichissement mesure du dossier (section 2.1) x 10,
 * contre un temoin tire au hasard dans la meme zone -- auberge x18, volerie x9,
 * cimetiere x3,4, carrefour x1 (aucune mesure, donc le niveau du temoin).
 *
 * CES VALEURS SONT LE MEME FAIT QUE POIDS dans
 * coa-project/outils/engendrer-postes-guet.py, ecrit deux fois. Si l'une des
 * deux tables bouge, l'autre doit bouger : le generateur s'en sert pour le poids
 * de base d'un poste, le chargeur pour le budget de sa classe.
 */
constexpr uint32 CoaPoidsDeClasse[COA_POSTE_SOURCES] = { 180, 90, 34, 10 };

struct CoaPosteGuet
{
    uint32 id = 0;
    uint32 carte = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 zone = 0;
    uint32 aire = 0;
    uint8 palierMin = 1;
    uint8 palierMax = 80;
    // 0 les deux camps, 1 Alliance seule, 2 Horde seule. Meme encodage que la
    // colonne `faction` du .sql, dont l'en-tete le dit aussi. Depuis la
    // correction de la fusion du generateur, ce n'est plus la faction de la
    // SOURCE mais le DROIT DE TIRAGE : un carrefour neutre retenu par un seul
    // camp -- parce que l'autre a mieux a moins de 50 yd -- y porte le numero de
    // ce camp. Le test de lecture, lui, ne change pas.
    uint8 faction = 0;
    // Classe du poste, lue dans la colonne `source`. Voir CoaSourcePoste.
    uint8 classe = COA_POSTE_ROUTE;
    // Poids DANS SA CLASSE : l'enrichissement mesure de la classe (x 10) module
    // par la densite de points de quete, d'un facteur x1,25 par tranche de dix
    // points a moins de 75 yd, deux tranches au plus. IL NE SE COMPARE PAS
    // ENTRE CLASSES -- le tirage repartit un budget par classe, voir
    // CoaNormaliserParClasse. Il sert de poids de tirage, jamais de seuil.
    uint32 poids = 1;
};

class CoaCataloguePostes
{
public:
    static inline CoaCataloguePostes& Instance()
    {
        static CoaCataloguePostes unique;
        return unique;
    }

    /*
     * CHARGEMENT, UNE SEULE FOIS, DEPUIS LE FIL DU MONDE.
     *
     * CETTE FONCTION N'EST PAS UN « reload ». Elle ECRIT dans parCarte (clear
     * puis push_back) alors que PourCarte est lue depuis les TROIS fils de
     * carte. Elle n'est sans course que parce qu'elle tourne a OnStartup,
     * c'est-a-dire avant qu'un seul bot n'existe. La rappeler a chaud -- par une
     * commande GM, par exemple -- serait un comportement indefini, pas une
     * lecture perimee. Rendre le rechargement possible demanderait un verrou
     * partage ou un echange de pointeur atomique ; ce lot ne le fait pas, et ce
     * commentaire est la pour qu'on ne le croie pas fait.
     *
     * Appele par PlayerbotsWorldScript::OnStartup (Script/Playerbots.cpp), sur
     * le patron de mod-coa-challenges (CoA.Challenges.Scripts.cpp:1896-1900 :
     * WORLDHOOK_ON_STARTUP, EnsureTables puis LoadChallengeDefinitions).
     *
     * CE QU'ON N'A PAS RECOPIE DE CE PATRON, ET POURQUOI. mod-coa-challenges
     * porte son propre CREATE TABLE (CoA.Challenges.Definitions.cpp:50), mais il
     * est ETEINT PAR DEFAUT : garde par sConfigMgr->GetOption<bool>(
     * "CoAChallenges.AutoCreateSchema", false) (:46-47), et son propre
     * commentaire le dit « Dev fallback only ». Un chargeur qui recopierait ce
     * CREATE en le croyant actif creerait une table VIDE et ferait taire
     * l'absence de donnees. Ici il n'y a pas de CREATE du tout : le schema
     * appartient au .sql engendre, et une table absente est un etat annonce.
     *
     * LA SONDE D'EXISTENCE N'EST PAS UN CONFORT. Sans elle,
     * WorldDatabase.Query sur une table absente rend nullptr APRES avoir emis
     * une erreur SQL dans les journaux, a chaque demarrage, sur tout serveur qui
     * n'a pas encore joue le .sql -- c'est-a-dire sur celui-ci aujourd'hui. On
     * pose donc la question a information_schema d'abord, ce qui distingue « pas
     * de table » (normal, on se replie) de « table vide » (anormal, on le dit).
     */
    inline void Charger()
    {
        parCarte.clear();
        total = 0;
        charge = false;
        uint32 inconnues = 0;

        QueryResult existe = WorldDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.TABLES"
            " WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'playerbots_coa_poste_guet'");
        if (!existe || (*existe)[0].Get<uint64>() == 0)
        {
            LOG_INFO("playerbots",
                     "[CoA chasse] table playerbots_coa_poste_guet absente : la chasse tire "
                     "ses postes dans GetTravelHubs (comportement du lot 2).");
            return;
        }

        QueryResult r = WorldDatabase.Query(
            "SELECT id, map, x, y, z, zoneId, areaId, palierMin, palierMax, faction, poids, source"
            " FROM playerbots_coa_poste_guet WHERE actif = 1");
        if (!r)
        {
            LOG_ERROR("playerbots",
                      "[CoA chasse] playerbots_coa_poste_guet existe mais ne rend aucune ligne "
                      "active : la chasse se replie sur GetTravelHubs.");
            return;
        }

        do
        {
            Field* f = r->Fetch();
            CoaPosteGuet p;
            p.id = f[0].Get<uint32>();
            p.carte = f[1].Get<uint16>();
            p.x = f[2].Get<float>();
            p.y = f[3].Get<float>();
            p.z = f[4].Get<float>();
            p.zone = f[5].Get<uint16>();
            p.aire = f[6].Get<uint16>();
            p.palierMin = f[7].Get<uint8>();
            p.palierMax = f[8].Get<uint8>();
            p.faction = f[9].Get<uint8>();
            p.poids = f[10].Get<uint16>();
            p.classe = ClasseDeSource(f[11].Get<std::string>(), inconnues);

            // UN POIDS NUL SORTIRAIT DU TIRAGE SANS UN MOT. Le tirage est
            // proportionnel au poids : un poste a 0 ne serait jamais tire tout
            // en occupant une ligne « active ». On le ramene a 1 -- si
            // l'exploitant veut le retirer, la colonne pour cela est `actif`.
            if (!p.poids)
                p.poids = 1;

            // Un palier renverse (min > max) ne peut satisfaire aucun niveau :
            // le poste serait charge et jamais tire. On le redresse plutot que
            // de le jeter, et on ne le signale pas ligne par ligne -- le compte
            // final suffit a voir qu'une regeneration a derape.
            if (p.palierMin > p.palierMax)
                std::swap(p.palierMin, p.palierMax);

            parCarte[p.carte].push_back(p);
            ++total;
        } while (r->NextRow());

        charge = total != 0;
        LOG_INFO("playerbots", "[CoA chasse] catalogue de postes : {} postes actifs sur {} cartes.",
                 total, uint32(parCarte.size()));

        // UNE SOURCE ILLISIBLE NE SE TAIT PAS. Elle est comptee dans la classe
        // la plus faible (route), donc elle ne peut jamais gonfler le tirage --
        // mais un catalogue regenere avec un vocabulaire nouveau doit se voir
        // dans le journal, pas se deviner a la lecture des compteurs.
        if (inconnues)
            LOG_ERROR("playerbots",
                      "[CoA chasse] {} postes de colonne `source` inconnue : comptes en "
                      "« route ». Le vocabulaire attendu est auberge, volerie, cimetiere, route.",
                      inconnues);
    }

    /*
     * `source` -> CoaSourcePoste. Le vocabulaire est celui du generateur
     * (coa-project/outils/engendrer-postes-guet.py, table POIDS) et de l'en-tete
     * du .sql, qui l'enonce ligne a ligne.
     *
     * UNE VALEUR INCONNUE N'EST PAS REJETEE : le poste est garde et compte comme
     * une ROUTE, c'est-a-dire dans la classe au budget le plus faible. C'est le
     * seul repli qui ne peut pas mentir en faveur du poste -- le rejeter
     * retirerait de la couverture en silence, le compter en auberge lui
     * donnerait 18 fois le budget d'un lieu dont on ne sait rien. Le compte est
     * remonte a l'appelant, qui le journalise.
     */
    static inline uint8 ClasseDeSource(std::string const& source, uint32& inconnues)
    {
        if (source == "auberge")
            return COA_POSTE_AUBERGE;
        if (source == "volerie")
            return COA_POSTE_VOLERIE;
        if (source == "cimetiere")
            return COA_POSTE_CIMETIERE;
        if (source == "route")
            return COA_POSTE_ROUTE;

        ++inconnues;
        return COA_POSTE_ROUTE;
    }

    inline bool EstCharge() const { return charge; }
    inline uint32 Total() const { return total; }

    /*
     * LECTURE. Rend nullptr quand la carte n'a aucun poste.
     *
     * ELLE NE PEUT PAS ETRE operator[], et c'est la raison d'etre de cette
     * fonction. Les appelants sont les actions de chasse, qui tournent sur TROIS
     * fils de carte ; std::unordered_map::operator[] INSERE une entree vide sur
     * une cle absente, c'est-a-dire ecrit dans le conteneur -- une course de
     * donnees a chaque bot qui se trouve sur une carte sans poste. C'est le meme
     * piege que RetenirUnPoste documente pour std::map::operator[] du cote de
     * GetTravelHubs. find() ne mute rien, et apres Charger() le conteneur n'est
     * plus jamais ecrit : la lecture concurrente est alors sans course.
     */
    inline std::vector<CoaPosteGuet> const* PourCarte(uint32 carte) const
    {
        auto it = parCarte.find(carte);
        return it == parCarte.end() ? nullptr : &it->second;
    }

private:
    CoaCataloguePostes() = default;

    std::unordered_map<uint32, std::vector<CoaPosteGuet>> parCarte;
    uint32 total = 0;
    bool charge = false;
};

/*
 * UN CANDIDAT DE LA PREMIERE PASSE : (distance au bot, poste).
 *
 * LA DISTANCE EST CALCULEE UNE FOIS ET GARDEE. La premiere ecriture la calculait
 * dans la premiere passe puis la JETAIT, et le comparateur du tri de repli la
 * recalculait deux fois par comparaison -- soit O(n log n) racines carrees pour
 * ordonner une liste dont on ne lit jamais plus de MaxTestsCatalogueRepli = 32
 * elements. Mesure sur le cas le plus dense du catalogue livre (carte 530,
 * niveau 70, rayon 1500 yd : 118 candidats) : ~815 comparaisons, donc ~1 630
 * GetExactDist, pour n'en utiliser que 32. Voir TirerPosteCatalogue.
 */
using CoaCandidatPoste = std::pair<float, CoaPosteGuet const*>;

/*
 * UN CANDIDAT PESE : (poids EFFECTIF deja normalise par classe, poste).
 * Voir CoaNormaliserParClasse, qui le fabrique, et CoaTirerPondere, qui le
 * consomme.
 */
struct CoaPostePese
{
    uint32 poids = 0;
    CoaPosteGuet const* poste = nullptr;
};

/*
 * LE BUDGET DE CLASSE -- la correction du defaut que la relecture a nomme
 * « le poids est porte par le POSTE et non par la CLASSE ».
 *
 * CE QUI N'ALLAIT PAS. Le poids du .sql est celui d'un poste ; le tirage etait
 * proportionnel a la somme de ces poids, donc a la POPULATION de chaque classe.
 * Le catalogue compte ~1 500 carrefours pour ~90 auberges : le facteur 18:1
 * mesure entre les deux sources etait renverse par le rapport de population
 * 14:1, et le poste tire etait un carrefour dans 43 % des cas a la carte 0 au
 * niveau 20, contre 25 % pour une auberge. Esperance d'enrichissement : ~6,8,
 * contre ~18 au lot 2, ou TirerPoste ne rendait que des hubs.
 *
 * CE QU'ON FAIT. Chaque CLASSE presente parmi les candidats recoit un budget
 * proportionnel a CoaPoidsDeClasse, et ce budget est reparti entre SES postes
 * au prorata de leur poids. La part d'une classe dans le tirage ne depend donc
 * plus du tout de son nombre de postes a portee -- seulement de son
 * enrichissement mesure -- pendant que le poids du .sql continue de departager
 * les postes A L'INTERIEUR d'une classe (c'est la, et seulement la, que la
 * densite de points de quete joue son x1,5).
 *
 * UNE CLASSE ABSENTE NE COUTE RIEN : son budget n'est pas distribue, et les
 * classes presentes se partagent le tirage dans leur propre rapport. Un bot qui
 * n'a que des carrefours a portee tire donc un carrefour, comme il se doit.
 *
 * LOT 5 : LE FACTEUR DE PRESENCE, ET POURQUOI IL ENTRE **AVANT** LA
 * NORMALISATION ET PAS APRES. `facteurs`, quand il est fourni, porte un
 * multiplicateur par candidat -- l'index des proies (CoaIndexProies,
 * Bot/MercenaryRewards.h) le tire du nombre de proies recensees autour du poste.
 * Il est applique au poids du POSTE, donc AVANT que la classe ne soit
 * normalisee, et la consequence est exactement celle qu'on veut :
 *
 *   - la part de chaque CLASSE dans le tirage ne bouge pas d'un iota. Elle vaut
 *     CoaPoidsDeClasse[classe] / somme des classes presentes, quelle que soit la
 *     valeur des facteurs -- puisque la somme de la classe est recalculee avec
 *     eux. La correction documentee ci-dessus tient donc intacte ;
 *   - a l'INTERIEUR d'une classe, un poste ou l'index a vu du monde passe devant
 *     un poste desert.
 *
 * L'appliquer APRES la normalisation aurait defait cette correction en silence :
 * une classe qui compte beaucoup de postes peuples aurait regagne la part que le
 * budget lui retire.
 *
 * `facteurs` doit avoir la MEME TAILLE que `candidats` ou etre nul ; toute autre
 * taille est ignoree, facteur 1 partout. Une taille discordante serait un defaut
 * d'appelant, et un acces hors borne serait pire que l'absence de ponderation.
 *
 * ARITHMETIQUE ENTIERE, ET SANS DEBORDEMENT POSSIBLE. Le facteur d'echelle
 * evite que la division entiere n'ecrase les petites classes. Majoration du
 * produit intermediaire : poids <= 65535 (SMALLINT UNSIGNED) multiplie par un
 * facteur borne a CoaFacteurPresenceMax = 16, soit 1 048 560 ; Echelle = 4096 ;
 * CoaPoidsDeClasse <= 180. Le produit vaut au plus
 * 65535 x 16 x 4096 x 180 = 773 082 316 800, soit 7,73e11 -- calcule en uint64,
 * dont le maximum est 1,8e19, donc une marge de 2,4e7. Une redaction precedente
 * annoncait 7,7e14 : la conclusion tenait, le chiffre etait faux d'un facteur
 * mille, et il etait presente comme une verification faite -- de quoi faire
 * conclure a 24 000 fois de marge une session qui voudrait relever
 * CoaFacteurPresenceMax. Majoration du resultat : la somme de tous les poids
 * effectifs vaut au plus Echelle * (180 + 90 + 34 + 10) = 1 286 144 (celle-la
 * est exacte), et elle ne depend PAS des facteurs, donc tres loin du plafond
 * saturant de CoaTirerPondere.
 *
 * UN POIDS EFFECTIF NUL EST RAMENE A 1, pour la meme raison qu'au chargement :
 * un poste a 0 resterait dans la liste sans jamais pouvoir etre tire.
 */

// Plafond du multiplicateur de presence. Il n'est pas la pour eviter un
// debordement -- la majoration ci-dessus le montre -- mais pour BORNER CE QUE
// L'INDEX PEUT DIRE. L'index est un instantane de 30 s d'une population qui se
// deplace (dossier, section 5) : lui laisser multiplier un poids par mille
// ferait converger tous les mercenaires d'un palier sur la cellule la plus
// peuplee du dernier balayage, c'est-a-dire exactement la curee que
// MaxAttackersPerTarget = 1 ne sait pas empecher a l'ouverture
// (EnemyPlayerValue.cpp:115-119, son propre en-tete le dit).
constexpr uint32 CoaFacteurPresenceMax = 16;

inline void CoaNormaliserParClasse(std::vector<CoaCandidatPoste> const& candidats,
                                   std::vector<CoaPostePese>& sortie,
                                   std::vector<uint32> const* facteurs = nullptr)
{
    constexpr uint64 Echelle = 4096;

    bool const pondere = facteurs && facteurs->size() == candidats.size();

    // LE FACTEUR EST BORNE ICI, PAS CHEZ L'APPELANT, et les deux bornes comptent.
    // Le plancher a 1 : un facteur nul ferait tomber a zero la somme d'une classe
    // dont TOUS les postes seraient deserts, et la boucle suivante les ecarterait
    // tous -- la classe disparaitrait du tirage au lieu d'y peser moins. Le
    // plafond : c'est lui qui rend vraie la majoration de debordement de
    // l'en-tete, au lieu d'en faire un contrat que l'appelant peut rompre.
    auto facteurDe = [&](std::size_t i) -> uint64
    {
        if (!pondere)
            return 1;
        uint32 const brut = (*facteurs)[i];
        return brut < 1 ? 1 : (brut > CoaFacteurPresenceMax ? CoaFacteurPresenceMax : brut);
    };

    uint64 sommeParClasse[COA_POSTE_SOURCES] = { 0 };
    for (std::size_t i = 0; i < candidats.size(); ++i)
    {
        CoaCandidatPoste const& c = candidats[i];
        if (c.second->classe < COA_POSTE_SOURCES)
            sommeParClasse[c.second->classe] += uint64(c.second->poids) * facteurDe(i);
    }

    sortie.clear();
    sortie.reserve(candidats.size());
    for (std::size_t i = 0; i < candidats.size(); ++i)
    {
        CoaCandidatPoste const& c = candidats[i];
        uint8 const classe = c.second->classe < COA_POSTE_SOURCES ? c.second->classe
                                                                  : uint8(COA_POSTE_ROUTE);
        uint64 const somme = sommeParClasse[classe];
        if (!somme)
            continue;  // ne peut pas arriver : tout poids vaut au moins 1 au chargement

        uint64 effectif =
            uint64(c.second->poids) * facteurDe(i) * Echelle * CoaPoidsDeClasse[classe] / somme;
        if (!effectif)
            effectif = 1;

        sortie.push_back(CoaPostePese{ uint32(effectif), c.second });
    }
}

/*
 * TIRAGE PONDERE SANS REMISE, sur une liste de candidats deja peses.
 *
 * `restants` est CONSOMME : le poste tire en est retire avant d'etre rendu.
 * L'appelant peut donc rappeler tant que la liste n'est pas vide, sans jamais
 * retomber sur le meme poste -- c'est ce qui borne le nombre de tests de lieu,
 * chacun coutant une resolution de terrain, mutex de grille tenu.
 *
 * ET C'EST TOUT CE QUE LE SANS-REMISE PROTEGE : la MEME ENTREE. Deux entrees
 * distinctes au meme endroit seraient tirees deux fois de suite comme s'il
 * s'agissait de deux lieux -- raison pour laquelle la separation des postes est
 * garantie a la GENERATION, et par camp (voir fusionner dans
 * coa-project/outils/engendrer-postes-guet.py).
 *
 * Rend nullptr, et seulement, quand il n'y a plus rien a tirer.
 *
 * POURQUOI PAS urand(0, total) : urand borne inclus des DEUX cotes
 * (Random.h). Un tirage dans [0, total] pourrait rendre `total` lui-meme et
 * sortir de la boucle de somme cumulee sans avoir rien choisi. On tire donc
 * dans [0, total - 1], et `total` est garanti non nul parce que tout poids
 * effectif vaut au moins 1 (CoaNormaliserParClasse).
 *
 * POURQUOI LA SOMME EST RECALCULEE A CHAQUE APPEL. Parce que la liste retrecit :
 * garder une somme d'un appel sur l'autre imposerait de la decrementer du poids
 * retire, et un seul oubli sur ce chemin donnerait un tirage hors borne qui ne
 * se voit pas -- il choisit simplement toujours le dernier. Le cout est une
 * addition par element, sur une liste de quelques centaines, une fois par
 * RepliChoixSecondes et par bot.
 */
inline CoaPosteGuet const* CoaTirerPondere(std::vector<CoaPostePese>& restants)
{
    if (restants.empty())
        return nullptr;

    // SOMME SATURANTE. Les poids effectifs sont bornes par construction (voir
    // CoaNormaliserParClasse) et le total tient tres largement dans un uint32.
    // Mais un debordement ici ne planterait pas, il fausserait le tirage en
    // silence, ce qui est pire : on plafonne plutot que de supposer.
    uint32 total = 0;
    for (CoaPostePese const& p : restants)
    {
        if (total > 0xFFFFFFFFu - p.poids)
        {
            total = 0xFFFFFFFFu;
            break;
        }
        total += p.poids;
    }

    // Ne peut pas arriver -- tout poids effectif vaut au moins 1 -- mais un
    // urand sur un total nul deviendrait urand(0, 0xFFFFFFFF), silencieusement.
    if (!total)
        return nullptr;

    uint32 tire = urand(0, total - 1);
    std::size_t choisi = restants.size() - 1;
    for (std::size_t i = 0; i < restants.size(); ++i)
    {
        if (tire < restants[i].poids)
        {
            choisi = i;
            break;
        }
        tire -= restants[i].poids;
    }

    std::swap(restants[choisi], restants.back());
    CoaPosteGuet const* p = restants.back().poste;
    restants.pop_back();
    return p;
}

#endif
