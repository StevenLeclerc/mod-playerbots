/*
 * Conquest of Azeroth — client de l'oracle de decision Laya.
 *
 * Un petit modele de decision typee (Laya, portage MLX) tourne sur une machine
 * Apple Silicon joignable par Tailscale. Le serveur lui decrit la situation
 * d'un bot d'elite et les actions que le moteur vient de retenir ; il repond
 * une probabilite par action. Ces probabilites ponderent la pertinence des
 * actions candidates, via un Multiplier ordinaire.
 *
 * LA REGLE QUI PRIME SUR TOUT LE RESTE : le thread monde n'attend JAMAIS.
 * L'emission est un sendto non bloquant, la lecture ne touche qu'un cache
 * alimente par un thread a part. Aucune fonction de ce fichier appelee depuis
 * le moteur ne peut se bloquer sur le reseau.
 *
 * Le repli est l'absence de reponse : machine eteinte, endormie, hors reseau
 * ou saturee, le cache perime, le multiplicateur rend 1.0 et le bot se comporte
 * exactement comme si rien n'existait. C'est pourquoi le transport est UDP et
 * non HTTP : il n'y a aucun etat de connexion a reprendre, et rien a traiter
 * comme une erreur.
 *
 * Protocole, une ligne de texte par sens (voir outils/laya-oracle.py) :
 *
 *   req <guid> <seq>|<etat>|<opt>=<desc>;<opt>=<desc>;...|<noul>;<noul>
 *   ans <guid> <seq>|<opt>=<proba>;...|<proba>;<proba>
 *   err <guid> <seq>|<raison>
 */

#ifndef PLAYERBOTS_COALAYAORACLE_H
#define PLAYERBOTS_COALAYAORACLE_H

#include "Multiplier.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class Action;
class PlayerbotAI;

class CoaLayaOracle
{
public:
    static CoaLayaOracle& Instance();

    // Ouvre le socket et lance le thread receveur. Idempotent, sans effet si
    // AiPlayerbot.Laya.Enabled vaut 0. Appele une fois au chargement de la conf.
    void Demarrer();
    void Arreter();

    bool Actif() const { return _actif.load(std::memory_order_acquire); }

    // Emis depuis le thread monde. Ne bloque jamais : le datagramme part ou il
    // est perdu, les deux conviennent.
    void Demander(uint64 guid, std::string const& etat,
                  std::vector<std::pair<std::string, float>> const& candidats);

    // Rend la probabilite de cette action pour ce bot, ou -1.0f si aucune
    // reponse fraiche n'est disponible. Ne touche que le cache.
    float Probabilite(uint64 guid, std::string const& action) const;

    // La plus forte probabilite de la derniere reponse retenue, ou -1.0f.
    // Sert a situer une action par rapport au favori du modele.
    float MeilleureProbabilite(uint64 guid) const;

    // Le veto est prononce par le multiplicateur ; l'oracle le compte, pour
    // que le releve dise combien de fois le modele a change le cours des choses.
    void CompterVeto() { _vetos.fetch_add(1, std::memory_order_relaxed); }

    void Oublier(uint64 guid);

    // Delai minimal entre deux demandes pour un meme bot, en ms.
    bool PeutRedemander(uint64 guid) const;

    std::string Compteurs() const;

private:
    CoaLayaOracle() = default;
    ~CoaLayaOracle();
    CoaLayaOracle(CoaLayaOracle const&) = delete;
    CoaLayaOracle& operator=(CoaLayaOracle const&) = delete;

    struct Entree
    {
        uint32 seqEmis = 0;        // derniere requete envoyee
        uint32 msEmission = 0;
        uint32 seqRecu = 0;        // derniere reponse RETENUE
        uint32 msReception = 0;
        std::vector<std::pair<std::string, float>> probabilites;
    };

    void Recevoir();               // corps du thread receveur
    void Integrer(char const* datagramme, size_t taille);

    int _sock = -1;
    std::thread _receveur;
    std::atomic<bool> _actif{false};
    std::atomic<bool> _arret{false};

    mutable std::mutex _verrou;
    std::unordered_map<uint64, Entree> _cache;

    std::atomic<uint64> _emises{0};
    std::atomic<uint64> _recues{0};
    std::atomic<uint64> _perimees{0};     // reponse arrivee trop tard, jetee
    std::atomic<uint64> _desordre{0};     // reponse plus ancienne que la retenue
    std::atomic<uint64> _illisibles{0};
    std::atomic<uint64> _erreurs{0};      // datagramme "err" renvoye par l'oracle
    std::atomic<uint64> _echecsEnvoi{0};
    std::atomic<uint64> _vetos{0};
    // mutable : Probabilite() est const — elle ne touche que le cache — mais
    // doit pouvoir compter ce qu'elle sert. fetch_add n'est pas const.
    mutable std::atomic<uint64> _lecturesServies{0};
    mutable std::atomic<uint64> _lecturesVides{0};
};

/*
 * Le point de greffe dans le moteur.
 *
 * CE QU'UN Multiplier PEUT FAIRE, ET CE QU'IL NE PEUT PAS. La lecture de
 * Engine::DoNextAction (Bot/Engine/Engine.cpp, ~l.165-215) est sans appel :
 * le moteur DEPILE d'abord l'action la plus pertinente, PUIS applique les
 * multiplicateurs a elle seule, puis l'execute et sort de la boucle. Le
 * classement entre candidats a donc deja eu lieu quand on est appele.
 *
 * Un multiplicateur ne reclasse rien. Son seul effet observable est le
 * franchissement de zero : a `relevance <= 0` l'action est traitee comme
 * IMPOSSIBLE, ses alternatives sont empilees et le moteur passe au candidat
 * suivant. Rendre 1,4 ou 0,6 ne change strictement RIEN — et `setRelevance`
 * prend un uint32, ce qui tronquerait de toute facon les nuances.
 *
 * Ce multiplicateur est donc un VETO, pas une ponderation.
 *
 * ET IL NE PEUT PAS DECOUVRIR LES CANDIDATS SEUL (P-089). `GetValue` n'est
 * appele que sur les actions que le moteur a reellement essayees, et il
 * s'arrete a la premiere qui s'execute : une par tour, le plus souvent. La
 * premiere version accumulait ces actions a travers les tours jusqu'a en avoir
 * deux — elle demandait donc au modele d'arbitrer entre des actions qui
 * n'avaient jamais ete disponibles en meme temps. Mesure en service : 0,36
 * requete par seconde pour 18 bots, la ou 18 etaient attendues.
 *
 * D'ou `ObserveQueue`, appele une fois par tour par le moteur avec la file
 * complete, avant tout depilement. C'est de la que vient desormais le lot de
 * candidats, et de nulle part ailleurs.
 *
 * Donc, dans l'ordre :
 *
 *   1. ObserveQueue recoit la file complete ; si le delai est ecoule, la
 *      situation et les candidats partent ;
 *   2. GetValue lit le cache ;
 *   3. il rend 0 — et seulement dans ce cas le bot fera autre chose — quand le
 *      modele juge cette action nettement moins bonne que son favori.
 *
 * La demande d'un tour sert le tour SUIVANT : attendre la reponse dans le tour
 * courant reviendrait a bloquer le thread monde.
 *
 * TROIS GARDE-FOUS, parce qu'un veto est une arme plus brutale qu'un poids :
 *   - jamais au-dessus de Laya.VetoMaxRelevance : les urgences du moteur sont
 *     hors d'atteinte du modele ;
 *   - seulement si p(action) < p(favori) x Laya.VetoRatio : dans le doute, on
 *     laisse le moteur faire ;
 *   - au plus Laya.VetoMax refus par fenetre de Laya.PeriodMs, pour qu'un
 *     modele qui deraille ne puisse pas paralyser un bot.
 */
class CoaLayaMultiplier : public Multiplier
{
public:
    CoaLayaMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "coa laya") {}

    float GetValue(Action* action) override;
    void ObserveQueue(std::list<ActionBasket*> const& candidats) override;

private:
    std::string DecrireEtat();

    uint32 _fenetreVetos = 0;     // debut de la fenetre de comptage, en ms
    uint32 _vetos = 0;            // refus deja prononces dans cette fenetre
};

#endif
