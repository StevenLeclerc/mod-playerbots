/*
 * Conquest of Azeroth — client de l'oracle de decision Laya. Voir l'en-tete
 * pour le protocole et la regle de non-blocage.
 */

#include "CoaLayaOracle.h"

#include "CoaLayaEtat.h"

#include "Action.h"
#include "AiObjectContext.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Timer.h"
#include "Unit.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{
    constexpr size_t TAILLE_DATAGRAMME = 4096;

    // L'oracle refuse au-dela de 12 options. On s'arretait a 8 pour borner le
    // datagramme et le cout d'inference.
    //
    // RELEVE DU 2026-09-23, QUI A FAIT LEVER LA BORNE. Une fois les duellistes
    // montes au niveau 60, la capture du fil montre 8 options sur TOUTES les
    // requetes : la borne etait atteinte a chaque tour. Or le canal sort ne
    // classe pas ses candidats — il leur donne tous le poids 0,0 — donc la
    // troncature gardait les huit PREMIERS du kit, toujours les memes, et
    // cachait le reste au modele en permanence. Mesurer l'effet de l'oracle
    // sur une rotation dont un tiers ne lui est jamais montre n'a pas de sens.
    //
    // A 12, tout le kit d'un niveau 60 passe. C'est la limite de l'oracle
    // lui-meme, pas une valeur choisie.
    constexpr size_t MAX_OPTIONS = 12;

}

// Le protocole reserve ces trois caracteres. Un nom qui en contiendrait
// decalerait le decoupage cote oracle : on les neutralise plutot que d'inventer
// un echappement. Publique parce que la lecture du cache doit appliquer
// exactement la meme regle que l'emission, sans quoi rien ne s'apparie.
std::string CoaLayaOracle::LibelleSur(std::string const& brut, size_t maxLongueur)
{
    std::string sortie;
    sortie.reserve(std::min(brut.size(), maxLongueur));
    for (char c : brut)
    {
        if (sortie.size() >= maxLongueur)
            break;
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '|' || c == ';' || c == '=' || u < 0x20 || u > 0x7E)
            sortie.push_back(' ');
        else
            sortie.push_back(c);
    }
    if (sortie.find_first_not_of(' ') == std::string::npos)
        return "option";
    return sortie;
}

CoaLayaOracle& CoaLayaOracle::Instance()
{
    static CoaLayaOracle instance;
    return instance;
}

CoaLayaOracle::~CoaLayaOracle() { Arreter(); }

void CoaLayaOracle::Demarrer()
{
    if (_actif.load(std::memory_order_acquire))
        return;
    if (!sPlayerbotAIConfig.layaEnabled)
        return;

    std::string const& point = sPlayerbotAIConfig.layaEndpoint;
    size_t const sep = point.rfind(':');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= point.size())
    {
        LOG_ERROR("playerbots", "coa laya: AiPlayerbot.Laya.Endpoint invalide ('{}'), attendu <ip>:<port>. "
                                "L'oracle reste eteint.", point);
        return;
    }

    std::string const hote = point.substr(0, sep);
    int const port = atoi(point.substr(sep + 1).c_str());
    if (port <= 0 || port > 65535)
    {
        LOG_ERROR("playerbots", "coa laya: port invalide dans '{}'. L'oracle reste eteint.", point);
        return;
    }

    sockaddr_in adresse{};
    adresse.sin_family = AF_INET;
    adresse.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, hote.c_str(), &adresse.sin_addr) != 1)
    {
        LOG_ERROR("playerbots", "coa laya: '{}' n'est pas une adresse IPv4 litterale. "
                                "Aucune resolution DNS n'est faite ici, elle bloquerait le thread monde. "
                                "L'oracle reste eteint.", hote);
        return;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        LOG_ERROR("playerbots", "coa laya: socket() a echoue ({}). L'oracle reste eteint.", strerror(errno));
        return;
    }

    // connect() sur un socket UDP ne cause aucun echange : il fixe seulement le
    // correspondant, ce qui permet d'emettre avec send() et surtout de ne
    // recevoir que de lui.
    if (connect(sock, reinterpret_cast<sockaddr*>(&adresse), sizeof(adresse)) != 0)
    {
        LOG_ERROR("playerbots", "coa laya: connect() a echoue ({}). L'oracle reste eteint.", strerror(errno));
        close(sock);
        return;
    }

    // Le thread receveur doit pouvoir constater l'ordre d'arret : sans delai de
    // garde, recv() l'attendrait indefiniment et l'arret du serveur bloquerait.
    timeval attente{};
    attente.tv_sec = 0;
    attente.tv_usec = 200000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &attente, sizeof(attente));

    _sock = sock;
    _arret.store(false, std::memory_order_release);
    _actif.store(true, std::memory_order_release);
    _receveur = std::thread(&CoaLayaOracle::Recevoir, this);

    LOG_INFO("playerbots", "coa laya: oracle arme sur {} — elite {} %, periode {} ms, age max {} ms",
             point, sPlayerbotAIConfig.layaElitePercent, sPlayerbotAIConfig.layaPeriodMs,
             sPlayerbotAIConfig.layaMaxAgeMs);
}

void CoaLayaOracle::Arreter()
{
    if (!_actif.exchange(false, std::memory_order_acq_rel))
        return;
    _arret.store(true, std::memory_order_release);
    if (_receveur.joinable())
        _receveur.join();
    if (_sock >= 0)
    {
        close(_sock);
        _sock = -1;
    }
    LOG_INFO("playerbots", "coa laya: oracle eteint — {}", Compteurs());
}

void CoaLayaOracle::Recevoir()
{
    std::vector<char> tampon(TAILLE_DATAGRAMME);
    while (!_arret.load(std::memory_order_acquire))
    {
        ssize_t lu = recv(_sock, tampon.data(), tampon.size(), 0);
        if (lu <= 0)
        {
            // EAGAIN/EWOULDBLOCK : le delai de garde a expire, on repasse par
            // le test d'arret. Toute autre erreur est traitee pareil : un
            // datagramme perdu n'est pas une panne.
            continue;
        }
        Integrer(tampon.data(), static_cast<size_t>(lu));
    }
}

void CoaLayaOracle::Integrer(char const* datagramme, size_t taille)
{
    _recues.fetch_add(1, std::memory_order_relaxed);
    std::string texte(datagramme, taille);

    if (texte.compare(0, 4, "err ") == 0)
    {
        _erreurs.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("playerbots", "coa laya: l'oracle refuse une requete: {}", texte);
        return;
    }
    if (texte.compare(0, 4, "ans ") != 0)
    {
        _illisibles.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    size_t const finEntete = texte.find('|');
    if (finEntete == std::string::npos)
    {
        _illisibles.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    unsigned long long lu = 0;
    uint32 seq = 0;
    if (sscanf(texte.c_str(), "ans %llu %u", &lu, &seq) != 2)
    {
        _illisibles.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Le canal voyage dans le bit de poids faible du seq (voir l'en-tete).
    uint64 const cle = Cle(static_cast<uint64>(lu), seq & 1u);

    size_t const finChoix = texte.find('|', finEntete + 1);
    std::string const champ = texte.substr(finEntete + 1,
        finChoix == std::string::npos ? std::string::npos : finChoix - finEntete - 1);

    std::vector<std::pair<std::string, float>> probabilites;
    size_t debut = 0;
    while (debut < champ.size())
    {
        size_t const fin = champ.find(';', debut);
        std::string const morceau = champ.substr(debut, fin == std::string::npos ? std::string::npos : fin - debut);
        size_t const egal = morceau.rfind('=');
        if (egal != std::string::npos && egal > 0)
        {
            char* reste = nullptr;
            float const valeur = strtof(morceau.c_str() + egal + 1, &reste);
            // Une valeur hors [0,1] ou non finie signale un oracle casse : on
            // jette toute la reponse plutot que de ponderer avec du bruit.
            if (reste == morceau.c_str() + egal + 1 || !std::isfinite(valeur) || valeur < 0.0f || valeur > 1.0f)
            {
                _illisibles.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            probabilites.emplace_back(morceau.substr(0, egal), valeur);
        }
        if (fin == std::string::npos)
            break;
        debut = fin + 1;
    }

    if (probabilites.empty())
    {
        _illisibles.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    uint32 const maintenant = getMSTime();
    std::lock_guard<std::mutex> tenu(_verrou);
    auto trouve = _cache.find(cle);
    if (trouve == _cache.end())
    {
        // Une reponse pour un bot dont on n'a rien demande : l'oracle parle a
        // quelqu'un d'autre, ou le bot s'est deconnecte entre-temps.
        _desordre.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    Entree& entree = trouve->second;

    // PIEGE MESURE SUR LE BANC DE L'ORACLE : quand un bot redemande pendant que
    // la machine travaille, une reponse deja en vol peut arriver APRES une plus
    // recente. N'accepter qu'un seq strictement superieur au dernier retenu est
    // ce qui empeche de rejouer une decision perimee. Comparer au dernier seq
    // EMIS ne conviendrait pas : on perdrait la reponse encore valable pendant
    // tout le temps de vol de la suivante.
    if (seq <= entree.seqRecu && entree.seqRecu != 0)
    {
        _desordre.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (GetMSTimeDiffToNow(entree.msEmission) > sPlayerbotAIConfig.layaMaxAgeMs)
    {
        _perimees.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    entree.seqRecu = seq;
    entree.msReception = maintenant;
    entree.probabilites = std::move(probabilites);
}

void CoaLayaOracle::Demander(uint64 guid, uint32 canal, std::string const& etat,
                             std::vector<std::pair<std::string, float>> const& candidats,
                             std::vector<std::string> const& descriptions,
                             bool avecNoul)
{
    if (!_actif.load(std::memory_order_acquire) || candidats.size() < 2)
        return;

    uint64 const cle = Cle(guid, canal);
    uint32 seq = 0;
    {
        std::lock_guard<std::mutex> tenu(_verrou);
        Entree& entree = _cache[cle];
        // Le compteur avance de deux en deux pour laisser le bit du canal
        // intact ; la monotonie du seq reste donc vraie canal par canal.
        seq = (++entree.seqEmis << 1) | (canal & 1u);
        entree.msEmission = getMSTime();

        // Un bot qui se deconnecte laisse son entree derriere lui, et les bots
        // tournent en permanence sur ce serveur. Sans ce balayage le cache ne
        // redescend jamais. Une fois tous les 512 envois suffit : c'est de
        // l'entretien, pas une urgence.
        if ((entree.seqEmis & 0x1FF) == 0)
        {
            for (auto it = _cache.begin(); it != _cache.end();)
            {
                if (it->first != cle && GetMSTimeDiffToNow(it->second.msEmission) > 60 * IN_MILLISECONDS)
                    it = _cache.erase(it);
                else
                    ++it;
            }
        }
    }

    std::string message;
    message.reserve(512);
    message += "req ";
    message += std::to_string(guid);
    message += ' ';
    message += std::to_string(seq);
    message += '|';
    // L'etat passe par le meme assainisseur que les libelles. Il ne contient
    // en principe aucun des trois caracteres reserves, mais il est desormais
    // construit a partir de noms de specialisation et de valeurs de contexte
    // (CoaLayaEtat.cpp) : une seule de ces sources qui changerait decalerait
    // tout le decoupage cote oracle, sans erreur visible ici.
    message += LibelleSur(etat, 800);
    message += '|';

    // Deux noms differents peuvent devenir identiques une fois assainis et
    // tronques ; l'oracle rejette alors la requete entiere pour option en
    // double. On ecarte le doublon ici plutot que de perdre la decision.
    std::vector<std::string> nomsPoses;
    size_t posees = 0;
    size_t index = 0;
    for (auto const& candidat : candidats)
    {
        size_t const i = index++;
        if (posees >= MAX_OPTIONS)
            break;
        std::string const nom = LibelleSur(candidat.first);
        if (std::find(nomsPoses.begin(), nomsPoses.end(), nom) != nomsPoses.end())
            continue;
        nomsPoses.push_back(nom);
        if (posees)
            message += ';';
        message += nom;
        message += '=';
        // La description ne pretend pas savoir ce que l'action fait : elle
        // donne le nom, que le modele lit, et la priorite que le moteur lui a
        // deja attribuee. Inventer une semantique par correspondance de nom
        // serait une regle de plus a maintenir, et fausse le jour ou une action
        // est renommee.
        if (i < descriptions.size() && !descriptions[i].empty())
        {
            message += LibelleSur(descriptions[i], 200);
        }
        else
        {
            message += nom;
            message += ". Priorite moteur ";
            message += std::to_string(static_cast<int>(candidat.second));
            message += '.';
        }
        ++posees;
    }
    if (posees < 2)
        return;

    // La question annexe double le cout d'inference (27 ms contre 14 mesures
    // sur M1). On ne la pose que quand elle sert.
    if (avecNoul)
        message += "|Le bot risque-t-il de mourir dans les prochaines secondes ?";

    if (message.size() > TAILLE_DATAGRAMME)
        message.resize(TAILLE_DATAGRAMME);

    // MSG_DONTWAIT : si le tampon d'emission du noyau est plein, on echoue tout
    // de suite plutot que d'endormir le thread monde. Un datagramme perdu est
    // exactement le cas de repli prevu.
    ssize_t const envoye = send(_sock, message.c_str(), message.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (envoye < 0)
        _echecsEnvoi.fetch_add(1, std::memory_order_relaxed);
    else
        _emises.fetch_add(1, std::memory_order_relaxed);
}

float CoaLayaOracle::Probabilite(uint64 guid, uint32 canal, std::string const& libelle) const
{
    if (!_actif.load(std::memory_order_acquire))
        return -1.0f;

    // Meme nettoyage qu'a l'emission, sinon rien ne s'apparie.
    std::string const action = LibelleSur(libelle);

    std::lock_guard<std::mutex> tenu(_verrou);
    auto trouve = _cache.find(Cle(guid, canal));
    if (trouve == _cache.end() || trouve->second.probabilites.empty())
    {
        _lecturesVides.fetch_add(1, std::memory_order_relaxed);
        return -1.0f;
    }
    if (GetMSTimeDiffToNow(trouve->second.msReception) > sPlayerbotAIConfig.layaMaxAgeMs)
    {
        _lecturesVides.fetch_add(1, std::memory_order_relaxed);
        return -1.0f;
    }
    for (auto const& paire : trouve->second.probabilites)
    {
        if (paire.first == action)
        {
            _lecturesServies.fetch_add(1, std::memory_order_relaxed);
            return paire.second;
        }
    }
    _lecturesVides.fetch_add(1, std::memory_order_relaxed);
    return -1.0f;
}

float CoaLayaOracle::MeilleureProbabilite(uint64 guid, uint32 canal) const
{
    std::lock_guard<std::mutex> tenu(_verrou);
    auto trouve = _cache.find(Cle(guid, canal));
    if (trouve == _cache.end() || trouve->second.probabilites.empty())
        return -1.0f;
    if (GetMSTimeDiffToNow(trouve->second.msReception) > sPlayerbotAIConfig.layaMaxAgeMs)
        return -1.0f;
    float meilleure = 0.0f;
    for (auto const& paire : trouve->second.probabilites)
        meilleure = std::max(meilleure, paire.second);
    return meilleure;
}

bool CoaLayaOracle::PeutRedemander(uint64 guid, uint32 canal) const
{
    std::lock_guard<std::mutex> tenu(_verrou);
    auto trouve = _cache.find(Cle(guid, canal));
    if (trouve == _cache.end())
        return true;
    return GetMSTimeDiffToNow(trouve->second.msEmission) >= sPlayerbotAIConfig.layaPeriodMs;
}

void CoaLayaOracle::Oublier(uint64 guid)
{
    std::lock_guard<std::mutex> tenu(_verrou);
    for (uint32 canal = 0; canal <= 1; ++canal)
        _cache.erase(Cle(guid, canal));
}

std::string CoaLayaOracle::Compteurs() const
{
    size_t suivis = 0;
    {
        std::lock_guard<std::mutex> tenu(_verrou);
        suivis = _cache.size();
    }
    std::string sortie;
    sortie.reserve(256);
    sortie += "emises=" + std::to_string(_emises.load(std::memory_order_relaxed));
    sortie += " recues=" + std::to_string(_recues.load(std::memory_order_relaxed));
    sortie += " perimees=" + std::to_string(_perimees.load(std::memory_order_relaxed));
    sortie += " desordre=" + std::to_string(_desordre.load(std::memory_order_relaxed));
    sortie += " illisibles=" + std::to_string(_illisibles.load(std::memory_order_relaxed));
    sortie += " refus=" + std::to_string(_erreurs.load(std::memory_order_relaxed));
    sortie += " echecs_envoi=" + std::to_string(_echecsEnvoi.load(std::memory_order_relaxed));
    sortie += " vetos=" + std::to_string(_vetos.load(std::memory_order_relaxed));
    sortie += " muets=" + std::to_string(_muets.load(std::memory_order_relaxed));
    sortie += " choix_sorts=" + std::to_string(_choix.load(std::memory_order_relaxed));
    sortie += " lectures_servies=" + std::to_string(_lecturesServies.load(std::memory_order_relaxed));
    sortie += " lectures_vides=" + std::to_string(_lecturesVides.load(std::memory_order_relaxed));
    sortie += " bots_suivis=" + std::to_string(suivis);
    return sortie;
}

/* ------------------------------------------------------------------ */
/* Le multiplicateur                                                    */
/* ------------------------------------------------------------------ */

std::string CoaLayaMultiplier::DecrireEtat()
{
    // Un seul constructeur d'etat pour les deux canaux : voir CoaLayaEtat.h.
    // La version qui vivait ici decrivait six faits ; la campagne de duels a
    // montre que c'etait trop peu pour que le modele puisse arbitrer quoi que
    // ce soit. Elle est remplacee, pas doublee : deux descriptions divergentes
    // etaient deja le defaut qu'on vient de payer.
    return CoaDecrireEtatLaya(botAI, bot, nullptr);
}

// Appele une fois par tour par Engine::DoNextAction, avec la file COMPLETE des
// candidats, avant tout depilement. C'est le seul endroit du moteur ou le lot
// de choix existe reellement (P-089) : GetValue n'en voit qu'un par tour.
void CoaLayaMultiplier::ObserveQueue(std::list<ActionBasket*> const& candidats)
{
    CoaLayaOracle& oracle = CoaLayaOracle::Instance();
    if (!oracle.Actif() || candidats.size() < 2)
        return;

    // NE PAS CONSULTER QUAND RIEN NE PEUT PARTIR.
    //
    // PlayerbotAI::CanCastSpell (PlayerbotAI.cpp:3526) refuse TOUT sort des que
    // le bot porte UNIT_STATE_LOST_CONTROL — etourdi, confus, en fuite, en saut
    // ou en charge. Demander au modele d'arbitrer pendant ces fenetres, c'est
    // lui faire choisir entre des actions dont aucune ne partira : la reponse
    // encombre le cache, elle sera lue au tour suivant, et elle dilue le signal
    // sans jamais rien changer.
    //
    // Ce n'est pas une micro-optimisation. Mesure de la session voisine sur
    // l'echantillon complet des refus du coeur : SPELL_FAILED_STUNNED pese
    // 41 549 refus, de loin le premier poste. Une part notable du temps de
    // combat se passe donc sous controle.
    //
    // Le bot mort est ecarte pour la meme raison, et par prudence : le moteur
    // de combat ne devrait pas tourner, mais rien ne le garantit ici.
    if (!bot->IsAlive() || bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
    {
        oracle.CompterMuet();
        return;
    }

    uint64 const guid = bot->GetGUID().GetRawValue();
    if (!oracle.PeutRedemander(guid, CoaLayaOracle::CANAL_ACTION))
        return;

    std::vector<std::pair<std::string, float>> lot;
    lot.reserve(candidats.size());
    for (ActionBasket* panier : candidats)
    {
        if (!panier)
            continue;
        ActionNode* noeud = panier->getAction();
        if (!noeud)
            continue;
        lot.emplace_back(noeud->getName(), panier->getRelevance());
    }
    if (lot.size() < 2)
        return;

    // La file n'est PAS triee : Queue::All rend l'ordre d'insertion, le
    // classement se fait a chaque Peek. On trie donc ici, pour que la troncature
    // a MAX_OPTIONS ecarte les candidats secondaires et non les urgences.
    std::sort(lot.begin(), lot.end(),
              [](auto const& a, auto const& b) { return a.second > b.second; });

    oracle.Demander(guid, CoaLayaOracle::CANAL_ACTION, DecrireEtat(), lot);
}

float CoaLayaMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    CoaLayaOracle& oracle = CoaLayaOracle::Instance();
    if (!oracle.Actif())
        return 1.0f;

    // Les urgences du moteur sont hors d'atteinte : un modele qui a mal lu une
    // phrase ne doit pas pouvoir empecher un bot de se soigner.
    float const pertinence = action->getRelevance();
    if (pertinence >= static_cast<float>(sPlayerbotAIConfig.layaVetoMaxRelevance))
        return 1.0f;

    // Lire le cache, et rien que le cache. L'emission a lieu dans ObserveQueue.
    uint64 const guid = bot->GetGUID().GetRawValue();
    float const proba = oracle.Probabilite(guid, CoaLayaOracle::CANAL_ACTION, action->getName());
    if (proba < 0.0f)
        return 1.0f;
    float const meilleure = oracle.MeilleureProbabilite(guid, CoaLayaOracle::CANAL_ACTION);
    if (meilleure <= 0.0f)
        return 1.0f;

    // Dans le doute, on laisse le moteur faire : le veto ne tombe que si le
    // modele ecarte franchement cette action de son favori.
    float const seuil = meilleure * (static_cast<float>(sPlayerbotAIConfig.layaVetoRatio) / 100.0f);
    if (proba >= seuil)
        return 1.0f;

    // Un modele qui deraille ne doit pas pouvoir paralyser un bot : au plus
    // layaVetoMax refus par fenetre de layaPeriodMs.
    if (_fenetreVetos == 0 || GetMSTimeDiffToNow(_fenetreVetos) >= sPlayerbotAIConfig.layaPeriodMs)
    {
        _fenetreVetos = getMSTime();
        _vetos = 0;
    }
    if (_vetos >= sPlayerbotAIConfig.layaVetoMax)
        return 1.0f;

    ++_vetos;
    oracle.CompterVeto();
    return 0.0f;
}
