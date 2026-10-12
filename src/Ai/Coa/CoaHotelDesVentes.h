/*
 * Conquest of Azeroth — les bots aleatoires et l'hotel des ventes.
 *
 * CE QUE CA FAIT (LOTS 1 A 3)
 *
 * Le butin d'artisanat et l'equipement vert ou bleu d'un bot aleatoire sont
 * DETRUITS a chaque Refresh, Randomize ou Revive (ClearInventory,
 * PlayerbotFactory.cpp). Le chantier les met en vente juste avant : il parcourt
 * les sacs, filtre, calcule le prix selon la grille validee le 2026-10-10, puis
 *   - Simulation = 1 (defaut, lot 1) : JOURNALISE ce qu'il aurait poste, sans
 *     toucher ni l'HV ni l'inventaire ;
 *   - Simulation = 0 (lot 2) : cree l'enchere sur le patron du coeur
 *     (AuctionHouseHandler.cpp, HandleAuctionSellItem), sans caution.
 * Lot 3 (Courrier = 1) : ReleverCourrier encaisse l'or et detruit les objets
 * des courriers d'HV deja livres au bot (ventes, invendus), puis les efface.
 * Lot 4 (TeleportVisible = 1) : apres un depot reel, si le teleport qui suit
 * choisit la ville, le bot va devant un commissaire-priseur de son camp
 * (DestinationsCommissaire) au lieu d'un banquier.
 * Specification : coa-project/docs/FAISABILITE-bots-hotel-des-ventes.md, § 6-7.
 *
 * LE FIL D'EXECUTION — LA CONTRAINTE QUI COMMANDE TOUT
 *
 * AuctionHouseMgr n'a aucun verrou : on ne le lit (et, au lot 2, on ne l'ecrit)
 * que depuis le FIL MONDE, quand les fils de carte sont a l'arret
 * (MapMgr::Update attend m_updater.wait() avant que World::Update n'appelle
 * OnWorldUpdate).
 *
 * Or `RandomPlayerbotMgr::ProcessBot(Player*)` a DEUX appelants :
 *   - `ProcessBot(uint32)`, appele par `UpdateAIInternal`, fil monde ;
 *   - `RandomBotUpdateAction::Execute`, une ACTION de bot, donc un fil de
 *     carte : la strategie « default » (WorldPacketHandlerStrategy) porte le
 *     declencheur passe-plat « random bot update », actif des que la valeur du
 *     meme nom est vraie — et ProcessBot(uint32) la met a vrai sans jamais la
 *     remettre a faux.
 * Le dossier de faisabilite (§ 3) ne voyait que le premier.
 *
 * D'ou `PasseMonde` : un garde pose en tete de UpdateAIInternal (fil monde
 * seul) leve un drapeau thread_local. AvantDestruction ne fait rien sans ce
 * drapeau ; elle compte seulement l'appel ecarte (« hors_fil_monde » au bilan).
 * Un fil de carte ne voit jamais le drapeau d'un autre fil.
 */

#ifndef PLAYERBOTS_COAHOTELDESVENTES_H
#define PLAYERBOTS_COAHOTELDESVENTES_H

#include "Define.h"

#include <vector>

class Player;
class WorldLocation;

namespace CoaHotelDesVentes
{
    // A poser en tete de RandomPlayerbotMgr::UpdateAIInternal, et NULLE PART
    // AILLEURS : il affirme « ce fil est le fil monde ». Ouvre aussi une passe
    // (budget global de depots remis a plein, index de l'HV oublie) et publie
    // le bilan periodique `coa hv:`.
    class PasseMonde
    {
    public:
        PasseMonde();
        ~PasseMonde();
        PasseMonde(PasseMonde const&) = delete;
        PasseMonde& operator=(PasseMonde const&) = delete;

    private:
        bool _precedent;
    };

    // Juste avant une destruction des sacs (Revive, Randomize, Refresh) dans
    // RandomPlayerbotMgr::ProcessBot(Player*). `motif` : chaine litterale
    // (« revive », « randomize », « refresh »), recopiee dans le journal.
    // Sort aussitot si AiPlayerbot.Coa.HotelDesVentes.Enable = 0.
    // Ne fait RIEN hors du fil monde (voir l'en-tete).
    void AvantDestruction(Player* bot, char const* motif);

    // Lot 3 : releve des courriers d'HV (MAIL_AUCTION, livres) d'un bot
    // aleatoire en ligne. A appeler dans RandomPlayerbotMgr::ProcessBot(Player*).
    // Sort aussitot si Enable = 0 ou Courrier = 0. Ne fait RIEN hors du fil
    // monde : la boite aux lettres (Player::m_mail) est aussi touchee par le
    // fil monde (AuctionHouseMgr::Update) et par le fil de carte du bot.
    void ReleverCourrier(Player* bot);

    // Lot 4, volet visible (TeleportVisible). A appeler dans la branche ville
    // de RandomPlayerbotMgr::RandomTeleportForLevel, a la place de
    // TravelMgr::GetCityLocations. Rend vrai et remplit `locs` (vide a
    // l'entree) avec les commissaires-priseurs du camp du bot si, et seulement
    // si : Enable = 1, TeleportVisible = 1, on est sur le fil monde, le bot a
    // fait un depot REEL dans la passe en cours (marque posee par
    // AvantDestruction, consommee ici, effacee en fin de passe), le tirage
    // TeleportVisiblePct passe, et au moins un commissaire convient (camp,
    // niveau). Sinon rend faux et l'appelant garde le banquier.
    // RandomTeleportForLevel a aussi des appelants sur fil de carte
    // (AutoMaintenanceOnLevelupAction) : la, rend faux sans rien toucher.
    bool DestinationsCommissaire(Player* bot, std::vector<WorldLocation>& locs);

    // RESSOURCES D'EVENEMENT (12/10/2026, coa-project/docs/CONCEPTION-ressources-rares-evenements.md § 4.2, lot 4).
    // Option AiPlayerbot.Coa.HotelDesVentes.Ressources (defaut 0, exige Enable) ; liste des objets :
    // AiPlayerbot.Directeur.ButinObjets. Allumee :
    //   - categorie propre (qualite 4 comprise), coefficients Ressources.CoefMin/Max x SellPrice, meme plafond
    //     marchand (BuyPrice x n - 1), plancher, bruit et sous-enchere ;
    //   - EN PRIORITE : triees avant tout autre candidat du bot ;
    //   - PLACE RESERVEE : hors de MaxParMaison / MaxParModele / MaxActivesParBot, bornees par
    //     Ressources.MaxParMaison / MaxParModele ; les autres objets gardent tout MaxParMaison ;
    //   - une ligne `coa hv ressource:` par ressource retenue, et des champs ressources_* EN FIN de `coa hv:`.

    // Vente anticipee (Ressources.VenteAnticipee, defaut 0) : a appeler dans RandomPlayerbotMgr::ProcessBot(Player*),
    // apres ReleverCourrier. Si le bot porte une ressource et passe les gardes d'AvantDestruction (fil monde, bot
    // aleatoire sans groupe ni maitre humain ni affectation, hors BG et instance), il depose ses ressources SEULES
    // (motif « ressources »), sans attendre le prochain vidage de sacs. Hors fil monde : ne fait rien.
    void VendreRessources(Player* bot);

    // Tout fil : la ressource est-elle protegee du vidage des sacs (DestroyItemsVisitor de PlayerbotFactory) et
    // gardee par le bot (ItemUsageValue -> ITEM_USAGE_KEEP : ni vendue au marchand, ni detruite sac plein) ?
    // Vrai seulement si Enable = 1, Simulation = 0 et Ressources = 1 : en simulation rien n'est vendu, la garder
    // ne ferait que remplir les sacs.
    bool RessourceProtegee(uint32 itemId);
    // Tout fil (atomique) : une ressource protegee gardee (vrai) ou detruite au-dela de Ressources.GarderMaxPiles
    // (faux) au vidage des sacs. Bilan `coa hv:` (ressources_gardees, ressources_detruites). Compte des DECISIONS par
    // passage de visiteur : PlayerbotFactory::Reset enchaine ClearAllItems puis ClearInventory, une pile gardee y
    // est donc comptee deux fois (et le plafond vaut par visiteur ; tous deux ne voient que sacs et equipement).
    void NoterRessourceAuVidage(bool gardee);
}

#endif
