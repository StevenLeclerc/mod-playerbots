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
    Inconnu     // valeur recue que ce binaire ne connait pas : toujours refusee
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
};

#endif
