/*
 * CoA -- le Directeur, lot D2a : la SOURCE des commandes, vue du jeu.
 *
 * Specification : coa-project/docs/SPEC-directeur-D2.md, § 3.
 *
 * POURQUOI UNE INTERFACE. Le transport retenu pour D2 est une file MySQL
 * (table acore_characters.coa_dir_commande), lue en asynchrone par le fil
 * monde. Le commanditaire a demande Redis ou AMQP ; ni l'un ni l'autre n'est
 * installe (mesure le 25/09, § 3.1), et les adopter veut dire un demon de plus,
 * une dependance liee au worldserver et une modification du CMake. On garde donc
 * MySQL, mais DERRIERE cette interface : brancher Redis ou AMQP plus tard, c'est
 * ecrire une nouvelle CoaDirSource, sans toucher ni aux verbes, ni aux
 * controles, ni au magasin de consignes (CoaDirecteur.cpp). Le code des verbes
 * ne voit jamais de SQL : il recoit une CoaDirCommande et rend un statut.
 *
 * TOUT SE PASSE SUR LE FIL MONDE : Demarrer (OnStartup), Sonder et Acquitter
 * (OnUpdate, et HandleStop qui y tourne aussi). Une implementation ne doit
 * JAMAIS bloquer dans Sonder : le fil monde porte tout le serveur.
 *
 * En-tete seul, volontairement : un .cpp neuf exigerait de relancer cmake
 * (CollectSourceFiles a la configuration, § 2.2). L'implementation MySQL vit
 * dans CoaDirecteur.cpp, espace de noms anonyme.
 */

#ifndef COA_DIRECTEUR_SOURCE_H
#define COA_DIRECTEUR_SOURCE_H

#include "Define.h"

#include <functional>
#include <string>

enum class CoaDirVerbe : uint8
{
    Treve,
    Reaffecter,
    Alleger,
    SuspendreMercenaire,
    Lever,
    // Lot D3 (SPEC-directeur-D3.md § 3.3), AVANT Inconnu :
    Peupler,        // affectation « de presence » pres du joueur qui demande (§ 4.2)
    FaireParler,    // une parole d'un bot, en file sur le fil monde (§ 5.2)
    Repondre,       // une ligne systeme au joueur, prefixee [Directeur] (§ 2.8)
    HeureDuJour,    // le ciel d'un seul joueur (§ 6.2)
    Meteo,          // la meteo d'un seul joueur, reaffirmee (§ 6.3)
    Inconnu     // valeur recue que ce binaire ne connait pas : toujours refusee
};

// Colonne `canal` (migration D3). Vide ou NULL : Aucun ; valeur que ce binaire
// ne connait pas : Inconnu (refus `bornes`).
enum class CoaDirCanal : uint8
{
    Aucun,
    Systeme,
    Dire,
    Crier,
    Chuchoter,
    Zone,
    Inconnu
};

struct CoaDirCommande
{
    uint32 id = 0;
    CoaDirVerbe verbe = CoaDirVerbe::Inconnu;
    uint32 cible = 0;        // guid bas ; pour Lever : id de commande, 0 = tout
    uint32 joueur = 0;       // humain autour duquel on agit, 0 = aucun
    uint32 dureeS = 0;
    uint32 carte = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, rayon = 0.0f;
    uint32 epoqueMonde = 0;  // epoque du monde lue par l'emetteur
    bool expiree = false;    // expire_le deja passe au moment de la lecture
    std::string action;      // colonne `action` (A-xxxx du service, E-xxxx : etape), vide si NULL
    // Colonne `origine` (migration D2 : regle, ask, arc, manuel), lue depuis D3
    // pour le refus `occupe` (SPEC-directeur-D3.md § 3.6).
    std::string origine;
    // Colonnes D3 (migration 2026_09_26_01_coa_directeur_d3.sql), lues seulement
    // si la garde de colonnes les a vues au demarrage (§ 3.2) ; sinon a leur
    // valeur par defaut, et tout verbe D3 est refuse `colonnes_d3`.
    std::string texte;                         // repondre, faire_parler : UTF-8
    CoaDirCanal canal = CoaDirCanal::Aucun;    // faire_parler
    int32 valeur = 0;                          // heure_du_jour : minute du ciel ; meteo : WeatherState
    float intensite = 0.0f;                    // meteo : ]0, 1[
    uint32 delaiS = 0;                         // faire_parler : livraison apres execution
};

class CoaDirSource
{
public:
    virtual ~CoaDirSource() = default;

    // Fil monde, OnStartup. Rend faux si le transport est indisponible (table
    // absente, demon injoignable...) : le bus reste alors inerte pour toute la
    // vie du processus, et plus rien n'est lu.
    virtual bool Demarrer(uint32 epoqueMonde, bool bus) = 0;

    // Fil monde. Livre, dans l'ordre, les commandes pretes ; ne bloque jamais.
    // Une commande livree ne l'est plus jamais dans la vie du processus.
    virtual void Sonder(uint32 diff, std::function<void(CoaDirCommande const&)> const& recevoir) = 0;

    // Fil monde, bus eteint. Livre ce qui a DEJA ete lu (lecture en vol comprise,
    // quand elle aboutit), sans rien lire de neuf. Sans elle, une commande lue
    // juste avant l'extinction resterait en memoire et serait executee au
    // rallumage, des heures plus tard, sur la foi d'une echeance evaluee avant.
    virtual void Vidanger(std::function<void(CoaDirCommande const&)> const& recevoir) = 0;

    // Fil monde, asynchrone. Premier accuse d'une commande livree : ne
    // s'applique que si elle est encore en attente.
    virtual void Acquitter(uint32 id, char const* statut, char const* raison) = 0;

    // Fil monde, asynchrone. Statut final d'une consigne deja executee (levee,
    // remplacee). Ne doit jamais ecraser un statut final deja pose, et doit
    // l'emporter sur un Acquitter du meme id arrive en retard.
    virtual void Clore(uint32 id, char const* statut, char const* raison) = 0;

    // Fil monde. L'interrupteur general vient de changer (rechargement de conf).
    virtual void SignalerBus(bool /*bus*/) { }

    // Fil monde, apres Demarrer. Les colonnes du lot D3 (texte, canal, valeur,
    // intensite, delai_s) ET l'ENUM de `verbe` a dix valeurs sont-ils en base ?
    // Faux : les verbes D3 sont refuses `colonnes_d3`, les verbes D2 passent.
    virtual bool ColonnesD3() const { return false; }
};

#endif
