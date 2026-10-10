/*
 * Conquest of Azeroth — les bots aleatoires et l'hotel des ventes (lots 1 a 3).
 * Voir l'en-tete pour le pourquoi et la contrainte de fil.
 *
 * Lot 1 (Simulation = 1, defaut) : on LIT les sacs et l'HV, on n'ecrit RIEN.
 * Seule sortie : le journal `playerbots.coa` (lignes `coa hv simu:`).
 *
 * Lot 2 (Simulation = 0) : depot reel, calque sur
 * AuctionHouseHandler.cpp, HandleAuctionSellItem (branche « la pile entiere
 * part ») : AddAItem, AddAuction, MoveItemFromInventory, puis dans UNE
 * transaction par bot : DeleteFromInventoryDB, Item::SaveToDB,
 * AuctionEntry::SaveToDB, SaveInventoryAndGoldToDB. Pas de caution.
 * Journal : `coa hv depot:`.
 *
 * Lot 3 (Courrier = 1) : ReleverCourrier. Calque sur HandleMailTakeMoney
 * (ModifyMoney sans depasser MAX_MONEY_AMOUNT, SaveGoldToDB) et sur la
 * branche MAIL_STATE_DELETED de Player::_SaveMail (DELETE mail, mail_items,
 * item_instance) -- _SaveMail est protegee, on emet les memes requetes.
 *
 * Bilan cumule : ligne `coa hv:` toutes les BilanSecondes.
 */

#include "CoaHotelDesVentes.h"

#include "CoaDirecteur.h"

#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Mail.h"
#include "MailMgr.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "World.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Vrai seulement sur le fil qui a pose un PasseMonde, c'est-a-dire le fil
    // monde dans UpdateAIInternal. thread_local : un fil de carte qui entre
    // dans ProcessBot(Player*) par RandomBotUpdateAction lit toujours faux.
    thread_local bool tlFilMonde = false;

    // Le seul compteur touche hors du fil monde : atomique.
    std::atomic<uint64> horsFilMonde{ 0 };

    // Tout le reste n'est lu et ecrit que sous tlFilMonde, donc par le fil
    // monde seul : pas de verrou.
    struct Compteurs
    {
        uint64 passes = 0;
        uint64 appels = 0;
        uint64 ecarteBot = 0;        // hors monde, pas aleatoire, maitre humain, groupe
        uint64 ecarteLieu = 0;       // BG, file de BG, instance
        uint64 ecarteDirecteur = 0;  // affectation ou cible de banc
        uint64 ecarteCamp = 0;       // ni Alliance ni Horde
        uint64 ecarteMaison = 0;     // AuctionHouse.dbc ou HV introuvable
        uint64 sacsSansCandidat = 0;
        uint64 objetsVus = 0;
        uint64 horsCategorie = 0;
        uint64 exclusLiaison = 0;    // !CanBeTraded, lie, BoP, quete, compte
        uint64 exclusConjure = 0;
        uint64 exclusDuree = 0;
        uint64 exclusSac = 0;
        uint64 exclusSansPrix = 0;   // SellPrice 0
        uint64 exclusQuete = 0;      // RandomQuestItems, StartQuest
        uint64 exclusAutre = 0;      // deja a l'HV, en echange, remboursable
        uint64 rejetPlafondMarchand = 0; // BuyPrice x n - 1 sous SellPrice x n
        uint64 rejetTropCher = 0;    // au-dela de MAX_MONEY_AMOUNT
        uint64 rejetParBot = 0;      // au-dela de MaxParBotParPassage
        uint64 rejetActivesBot = 0;  // au-dela de MaxActivesParBot
        uint64 rejetMaison = 0;      // au-dela de MaxParMaison
        uint64 rejetModele = 0;      // au-dela de MaxParModele
        uint64 rejetBudget = 0;      // budget de la passe epuise
        uint64 rejetMinute = 0;      // budget de la minute epuise (MaxParMinute)
        uint64 postes = 0;           // simules (Simulation = 1)
        uint64 deposes = 0;          // reels (Simulation = 0)
        uint64 echecs = 0;           // depot reel refuse au dernier controle
        uint64 unites = 0;
        uint64 sousEnchere = 0;
        uint64 plafonne = 0;
        uint64 plancher = 0;
        uint64 valeurRachat = 0;     // cuivre, somme des prix d'achat immediat
        uint64 valeurMarchand = 0;   // cuivre, SellPrice x n : ce que le marchand en donnerait
        // Lot 3. Ignores et reportes comptent des OCCURRENCES : un meme
        // courrier laisse en place est recompte a chaque passage du bot.
        uint64 courrierReleves = 0;  // courriers effaces
        uint64 courrierOr = 0;       // cuivre credite
        uint64 courrierObjets = 0;   // objets joints detruits
        uint64 courrierRefusOr = 0;  // ModifyMoney refuse (MAX_MONEY) : courrier laisse
        uint64 courrierIgnores = 0;  // contre-remboursement, sujet illisible, AUCTION_WON
        uint64 courrierReportes = 0; // au-dela de CourrierMaxParPasse
        // Lot 4 (TeleportVisible).
        uint64 visibleMarques = 0;         // bots marques « vient de deposer » (depot reel)
        uint64 visibleProposes = 0;        // commissaires proposes a RandomTeleport (pas une arrivee prouvee)
        uint64 visibleTirageNon = 0;       // marque consommee, tirage TeleportVisiblePct perdu
        uint64 visibleSansCommissaire = 0; // marque consommee, aucun commissaire pour ce camp / niveau
        uint64 visibleNonConsommes = 0;    // marques effacees en fin de passe sans branche ville
    };

    Compteurs c;
    uint32 budgetPasse = 0;
    uint32 budgetCourrier = 0;
    time_t fenetreMinute = 0;        // debut de la fenetre de MaxParMinute
    uint32 depotsFenetre = 0;
    time_t dernierBilan = 0;

    // Index d'une maison, bati au plus une fois par passe et seulement si un
    // bot de la passe a un candidat. Chaque depot de la passe (simule ou reel)
    // s'y ajoute aussitot : les plafonds et la sous-enchere mordent dans la
    // passe. D'une passe a l'autre il est rebati depuis l'HV REELLE (les depots
    // simules y sont donc oublies, les reels y sont).
    struct Bas
    {
        uint64 prix = 0;     // cuivre, unitaire ; 0 = vide
        uint32 proprio = 0;  // guid bas du vendeur
    };

    struct Modele
    {
        uint32 encheresBots = 0;
        // Les deux plus bas prix unitaires de bots, de DEUX vendeurs distincts :
        // b1 est le plus bas, b2 le plus bas parmi les autres vendeurs que
        // celui de b1. Suffit pour « le plus bas d'un AUTRE vendeur » (D2).
        Bas b1;
        Bas b2;

        void Noter(uint64 prix, uint32 proprio)
        {
            if (!prix)
                return;
            if (b1.prix && proprio == b1.proprio)
            {
                if (prix < b1.prix)
                    b1.prix = prix;
                return;
            }
            if (!b1.prix || prix < b1.prix)
            {
                if (b1.prix)
                    b2 = b1;
                b1.prix = prix;
                b1.proprio = proprio;
                return;
            }
            if (!b2.prix || prix < b2.prix)
            {
                b2.prix = prix;
                b2.proprio = proprio;
            }
        }

        // Le plus bas prix unitaire d'un vendeur autre que `moi` ; 0 = aucun.
        uint64 PlusBasAutre(uint32 moi) const
        {
            if (b1.prix && b1.proprio != moi)
                return b1.prix;
            return b2.prix;
        }
    };

    struct IndexMaison
    {
        bool bati = false;
        uint32 encheresBots = 0;
        std::unordered_map<uint32, Modele> parModele;       // item_template -> modele
        std::unordered_map<uint32, uint32> parProprietaire; // guid bas -> encheres
    };

    // Deux maisons possibles ici : 2 (Alliance) et 6 (Horde), ou 7 seule si
    // AllowTwoSide.Interaction.Auction ; indexe par l'id EFFECTIF de la maison.
    std::unordered_map<uint32, IndexMaison> indexPasse;

    // Lot 4. Guid bas des bots qui ont fait un depot REEL dans la passe en
    // cours. Pose par AvantDestruction, consomme par DestinationsCommissaire,
    // vide a la fermeture de la passe la plus externe : aucune marque ne
    // survit a sa passe. Fil monde seul (sous tlFilMonde).
    std::unordered_set<uint32> deposeCettePasse;

    // Lot 4. Commissaires-priseurs par camp (0 Alliance, 1 Horde), sur le
    // modele du cache des banquiers (TravelMgr::PrepareDestinationCache).
    // Bati paresseusement au premier besoin, sur le fil monde, depuis
    // ObjectMgr (fixe apres le chargement) ; rebati si
    // AllowTwoSide.Interaction.Auction change, puisque la maison d'un
    // commissaire en depend.
    struct CacheCommissaires
    {
        bool bati = false;
        bool deuxCotes = false;
        std::vector<WorldLocation> parCamp[2];
    };
    CacheCommissaires commissaires;

    void BatirCommissaires()
    {
        bool const deuxCotes = sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION);
        if (commissaires.bati && commissaires.deuxCotes == deuxCotes)
            return;

        commissaires = CacheCommissaires();
        commissaires.bati = true;
        commissaires.deuxCotes = deuxCotes;

        // La maison ou le bot depose (AvantDestruction) : un commissaire n'est
        // retenu que s'il ouvre CETTE maison, donc pas Baie-du-Butin, Gadgetzan
        // ni Long-guet (maison neutre) tant que AllowTwoSide vaut 0.
        AuctionHouseEntry const* maisonCamp[2] = {
            AuctionHouseMgr::GetAuctionHouseEntryFromHouse(AuctionHouseId::Alliance),
            AuctionHouseMgr::GetAuctionHouseEntryFromHouse(AuctionHouseId::Horde) };
        uint32 const masqueCamp[2] = { FACTION_MASK_ALLIANCE, FACTION_MASK_HORDE };

        for (auto const& [spawnId, d] : sObjectMgr->GetAllCreatureData())
        {
            if (!(d.phaseMask & PHASEMASK_NORMAL))
                continue;
            CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(d.id);
            if (!t || !(t->npcflag & UNIT_NPC_FLAG_AUCTIONEER))
                continue;
            // Carte continentale seulement : une WorldLocation ne porte pas
            // d'instance.
            MapEntry const* carte = sMapStore.LookupEntry(d.mapid);
            if (!carte || !carte->IsContinent())
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(t->faction);
            AuctionHouseEntry const* maison = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(t->faction);
            if (!faction || !maison)
                continue;

            // Comme les banquiers : 6 yd devant le PNJ, tourne vers lui. Le
            // sol est recalcule par RandomTeleport.
            float const o = d.orientation;
            WorldLocation const loc(d.mapid, d.posX + std::cos(o) * 6.0f, d.posY + std::sin(o) * 6.0f,
                                    d.posZ + 2.0f, o + float(M_PI));
            for (uint8 i = 0; i < 2; ++i)
                if (maisonCamp[i] && maison->houseId == maisonCamp[i]->houseId &&
                    !(faction->hostileMask & masqueCamp[i]))
                    commissaires.parCamp[i].push_back(loc);
        }

        LOG_INFO("playerbots.coa", "coa hv visible: commissaires alliance={} horde={} deux_cotes={}",
                 commissaires.parCamp[0].size(), commissaires.parCamp[1].size(), deuxCotes ? 1 : 0);
    }

    void BatirIndex(IndexMaison& idx, AuctionHouseObject* ah)
    {
        idx = IndexMaison();
        idx.bati = true;

        // Un proprietaire est-il un bot aleatoire ? Memorise dans la passe :
        // IsInRandomAccountList est une recherche lineaire. Un vendeur supprime
        // (compte 0) compte comme humain : hors plafonds, hors reference.
        std::unordered_map<uint32, bool> estBot;

        for (auto const& [id, a] : ah->GetAuctions())
        {
            if (!a)
                continue;

            uint32 const proprio = a->owner.GetCounter();
            auto it = estBot.find(proprio);
            if (it == estBot.end())
            {
                uint32 const compte = sCharacterCache->GetCharacterAccountIdByGuid(a->owner);
                bool const bot = compte && sPlayerbotAIConfig.IsInRandomAccountList(compte);
                it = estBot.emplace(proprio, bot).first;
            }

            // Les encheres humaines ne comptent dans aucun plafond (§ 7 :
            // « comptees a part ») et ne servent pas de reference de prix.
            if (!it->second)
                continue;

            ++idx.encheresBots;
            ++idx.parProprietaire[proprio];

            Modele& m = idx.parModele[a->item_template];
            ++m.encheresBots;
            if (a->buyout && a->itemCount)
                m.Noter(std::max<uint64>(1, uint64(a->buyout) / a->itemCount), proprio);
        }
    }

    // Tirage borne qui ne peut pas faire tomber l'ASSERT(max >= min) de frand.
    float Tirer(float a, float b)
    {
        if (a > b)
            std::swap(a, b);
        return frand(a, b);
    }

    // Pas de l'arrondi « joli », selon l'ordre de grandeur.
    uint64 PasJoli(uint64 p)
    {
        if (p < 100)            // < 1 pa : au cuivre
            return 1;
        if (p < 1000)           // < 10 pa : 5 pc
            return 5;
        if (p < 10000)          // < 1 po : 25 pc
            return 25;
        if (p < 100000)         // < 10 po : 1 pa
            return 100;
        if (p < 1000000)        // < 100 po : 5 pa
            return 500;
        return 10000;           // au-dela : 1 po
    }

    // Arrondi « joli » au plus proche. Jamais 0.
    uint64 ArrondiJoli(uint64 p)
    {
        uint64 const pas = PasJoli(p);
        uint64 const r = ((p + pas / 2) / pas) * pas;
        return r ? r : 1;
    }

    // Arrondi « joli » par defaut (jamais au-dessus de p). 0 si p < pas.
    uint64 ArrondiJoliBas(uint64 p)
    {
        uint64 const pas = PasJoli(p);
        return (p / pas) * pas;
    }

    enum class Categorie : uint8
    {
        Aucune,
        ArtisanatBlanc,     // class 7, qualite 1
        ArtisanatVertBleu,  // class 7, qualite 2-3
        EquipementVert,     // class 2/4, qualite 2
        EquipementBleu      // class 2/4, qualite 3
    };

    Categorie Classer(ItemTemplate const* t)
    {
        if (t->Class == ITEM_CLASS_TRADE_GOODS)
        {
            if (t->Quality == ITEM_QUALITY_NORMAL)
                return Categorie::ArtisanatBlanc;
            if (t->Quality == ITEM_QUALITY_UNCOMMON || t->Quality == ITEM_QUALITY_RARE)
                return Categorie::ArtisanatVertBleu;
            return Categorie::Aucune;
        }
        if (t->Class == ITEM_CLASS_WEAPON || t->Class == ITEM_CLASS_ARMOR)
        {
            if (t->Quality == ITEM_QUALITY_UNCOMMON)
                return Categorie::EquipementVert;
            if (t->Quality == ITEM_QUALITY_RARE)
                return Categorie::EquipementBleu;
        }
        return Categorie::Aucune;
    }

    void Coefficients(Categorie cat, float& mini, float& maxi)
    {
        PlayerbotAIConfig const& k = sPlayerbotAIConfig;
        switch (cat)
        {
            case Categorie::ArtisanatBlanc:    mini = k.coaHvCoefArtisanatQ1Min;  maxi = k.coaHvCoefArtisanatQ1Max;  break;
            case Categorie::ArtisanatVertBleu: mini = k.coaHvCoefArtisanatQ23Min; maxi = k.coaHvCoefArtisanatQ23Max; break;
            case Categorie::EquipementVert:    mini = k.coaHvCoefEquipVertMin;    maxi = k.coaHvCoefEquipVertMax;    break;
            case Categorie::EquipementBleu:    mini = k.coaHvCoefEquipBleuMin;    maxi = k.coaHvCoefEquipBleuMax;    break;
            default:                           mini = 0.0f; maxi = 0.0f; break;
        }
    }

    // Les exclusions de la § 7, plus celles du gestionnaire du coeur
    // (AuctionHouseHandler.cpp, HandleAuctionSellItem) : la simulation
    // n'annonce pas un depot que le lot 2 ne pourrait pas faire. Vrai = eligible.
    bool Eligible(Item* item, ItemTemplate const* t)
    {
        // Lie, BoP lie, enchantement liant, objet equipe : le test du coeur.
        if (!item->CanBeTraded() || item->IsSoulBound() || item->IsBoundAccountWide() ||
            t->Bonding == BIND_WHEN_PICKED_UP || t->Bonding == BIND_QUEST_ITEM || t->Bonding == BIND_QUEST_ITEM1)
        {
            ++c.exclusLiaison;
            return false;
        }
        if (t->HasFlag(ITEM_FLAG_CONJURED))
        {
            ++c.exclusConjure;
            return false;
        }
        if (item->GetUInt32Value(ITEM_FIELD_DURATION))
        {
            ++c.exclusDuree;
            return false;
        }
        if (item->IsNotEmptyBag())
        {
            ++c.exclusSac;
            return false;
        }
        if (!t->SellPrice)
        {
            ++c.exclusSansPrix;
            return false;
        }
        if (t->StartQuest || sPlayerbotAIConfig.IsInRandomQuestItemList(t->ItemId))
        {
            ++c.exclusQuete;
            return false;
        }
        if (sAuctionMgr->GetAItem(item->GetGUID()) || item->IsInTrade() || item->IsRefundable())
        {
            ++c.exclusAutre;
            return false;
        }
        return true;
    }

    // Ce qui a fixe le prix final, pour le journal et le bilan.
    enum class Regle : uint8
    {
        Grille,       // coefficient de la categorie, bruit
        SousEnchere,  // le plus bas d'un AUTRE vendeur bot du meme modele, moins 1 a 5 %
        Plancher,     // remonte a SellPrice x PlancherCoef
        Plafond       // rabattu a BuyPrice x n - 1
    };

    char const* NomRegle(Regle r)
    {
        switch (r)
        {
            case Regle::SousEnchere: return "sousenchere";
            case Regle::Plancher:    return "plancher";
            case Regle::Plafond:     return "plafond";
            default:                 return "grille";
        }
    }

    struct Candidat
    {
        Item* item;
        ItemTemplate const* tpl;
        Categorie cat;
        uint32 n;
        uint64 rachat = 0;
        uint64 mise = 0;
        Regle regle = Regle::Grille;
    };

    void VisiterObjet(Item* item, std::vector<Candidat>& sortie)
    {
        if (!item)
            return;

        ++c.objetsVus;
        ItemTemplate const* t = item->GetTemplate();
        if (!t)
        {
            ++c.horsCategorie;
            return;
        }

        Categorie const cat = Classer(t);
        if (cat == Categorie::Aucune)
        {
            ++c.horsCategorie;
            return;
        }
        if (!Eligible(item, t))
            return;

        uint32 const n = item->GetCount();
        if (!n)
        {
            ++c.exclusAutre;
            return;
        }

        sortie.push_back({ item, t, cat, n });
    }

    // Le prix selon la grille validee (§ 7). Faux = objet ecarte (compte).
    //
    // Ordre (correctif D1 de la revue du lot 1) :
    //   1. grille + bruit, arrondi joli (le plancher n'est applique qu'a
    //      l'etape 3 : un prix remonte au plancher n'est donc pas « joli ») ;
    //   2. sous-enchere APRES l'arrondi, seulement si le plus bas d'un AUTRE
    //      vendeur bot existe et que le prix de grille ne le bat pas deja
    //      (D2 : on ne se sous-enchere jamais soi-meme) ; le resultat est
    //      strictement sous ce plus bas, arrondi joli vers le bas si possible ;
    //   3. plancher re-applique (il peut empecher de battre le plus bas : c'est
    //      voulu, on ne descend pas sous SellPrice x PlancherCoef) ;
    //   4. plafond marchand, qui prime sur tout.
    bool Chiffrer(Candidat& k, Modele const* modele, uint32 botLow)
    {
        PlayerbotAIConfig const& cfg = sPlayerbotAIConfig;
        // Tout en double puis en uint64 : SellPrice (uint32) x coefficient x
        // pile depasse uint32 bien avant MAX_MONEY_AMOUNT.
        double const vente = double(k.tpl->SellPrice);
        double const n = double(k.n);

        float coefMin, coefMax;
        Coefficients(k.cat, coefMin, coefMax);
        float const bruit = float(cfg.coaHvBruitPct) / 100.0f;
        double unitaire = vente * Tirer(coefMin, coefMax) * (1.0 + Tirer(-bruit, bruit));
        k.regle = Regle::Grille;

        double const plancherD = std::ceil(vente * cfg.coaHvPlancherCoef * n);
        if (!(plancherD >= 0.0) || plancherD > double(MAX_MONEY_AMOUNT))
        {
            ++c.rejetTropCher;
            return false;
        }
        uint64 const plancher = uint64(plancherD);

        double const total = unitaire * n;
        if (!(total >= 0.0) || total > double(MAX_MONEY_AMOUNT))
        {
            ++c.rejetTropCher;
            return false;
        }

        uint64 rachat = ArrondiJoli(uint64(total));

        // 2. Sous-enchere, apres l'arrondi.
        uint64 const refUnitaire = modele ? modele->PlusBasAutre(botLow) : 0;
        if (refUnitaire)
        {
            uint64 const refTotal = refUnitaire * k.n; // <= MAX_MONEY x 1000 : tient en uint64
            if (rachat >= refTotal)
            {
                float const s = Tirer(float(cfg.coaHvSousEnchereMinPct), float(cfg.coaHvSousEnchereMaxPct)) / 100.0f;
                uint64 cible = uint64(double(refTotal) * (1.0 - double(s)));
                if (cible >= refTotal)
                    cible = refTotal - 1; // refTotal >= 1 : pas de debordement
                uint64 const joli = ArrondiJoliBas(cible);
                rachat = (joli && joli >= plancher) ? joli : cible;
                k.regle = Regle::SousEnchere;
            }
        }

        // 3. Plancher.
        if (rachat < plancher)
        {
            rachat = plancher;
            k.regle = Regle::Plancher;
        }

        // 4. Plafond : le joueur fait toujours une affaire face au marchand.
        if (k.tpl->BuyPrice > 0)
        {
            uint64 const plafond = uint64(k.tpl->BuyPrice) * k.n - 1;
            // BuyPrice <= SellPrice : vendre en dessous du marchand n'a pas de sens.
            if (plafond < uint64(k.tpl->SellPrice) * k.n)
            {
                ++c.rejetPlafondMarchand;
                return false;
            }
            if (rachat > plafond)
            {
                rachat = plafond;
                k.regle = Regle::Plafond;
            }
        }

        if (!rachat || rachat > uint64(MAX_MONEY_AMOUNT))
        {
            ++c.rejetTropCher;
            return false;
        }

        float const pct = Tirer(float(cfg.coaHvMiseMinPct), float(cfg.coaHvMiseMaxPct)) / 100.0f;
        uint64 mise = ArrondiJoli(uint64(double(rachat) * pct));
        if (mise >= rachat)
            mise = rachat > 1 ? rachat - 1 : 1;
        if (!mise)
            mise = 1;

        k.rachat = rachat;
        k.mise = mise;
        return true;
    }

    // Lot 2 : le depot reel d'UNE pile, sur le patron de
    // AuctionHouseHandler.cpp (HandleAuctionSellItem, branche itemsCount == 1
    // et pile entiere). Tous les controles AVANT la premiere ecriture : un
    // refus ne laisse aucune trace. Les ecritures en base vont dans `trans`,
    // que l'appelant valide une fois pour le bot.
    bool Deposer(Player* bot, Candidat const& k, AuctionHouseEntry const* entree, AuctionHouseObject* ah,
                 CharacterDatabaseTransaction trans, uint32& idEnchere, uint32& duree)
    {
        Item* item = k.item;
        if (!item)
            return false;

        ObjectGuid const guid = item->GetGUID();
        // L'objet est-il toujours celui qu'on a chiffre, dans les sacs du bot ?
        if (bot->GetItemByGuid(guid) != item || item->GetEntry() != k.tpl->ItemId || item->GetCount() != k.n ||
            item->GetOwnerGUID() != bot->GetGUID())
            return false;
        // AddAItem fait ASSERT sur un doublon : ne jamais l'atteindre.
        if (sAuctionMgr->GetAItem(guid) || item->IsInTrade() || !item->CanBeTraded())
            return false;
        if (!k.mise || k.mise > uint64(MAX_MONEY_AMOUNT) || !k.rachat || k.rachat > uint64(MAX_MONEY_AMOUNT) ||
            k.mise > k.rachat)
            return false;

        // Duree : tirage 12, 24 ou 48 h, les trois valeurs que le coeur accepte
        // (1, 2, 4 x MIN_AUCTION_TIME), puis Rate.Auction.Time comme lui.
        static uint32 const facteurs[3] = { 1, 2, 4 };
        uint32 const etime = facteurs[urand(0, 2)] * MIN_AUCTION_TIME;
        uint32 const auctionTime = uint32(etime * sWorld->getRate(RATE_AUCTION_TIME));

        uint8 const sac = item->GetBagSlot();
        uint8 const case_ = item->GetSlot();

        AuctionEntry* AH = new AuctionEntry;
        AH->Id = sObjectMgr->GenerateAuctionID();
        AH->houseId = AuctionHouseId(entree->houseId);
        AH->item_guid = guid;
        AH->item_template = item->GetEntry();
        AH->itemCount = item->GetCount();
        AH->owner = bot->GetGUID();
        AH->startbid = uint32(k.mise);
        AH->bidder = ObjectGuid::Empty;
        AH->bid = 0;
        AH->buyout = uint32(k.rachat);
        AH->expire_time = GameTime::GetGameTime().count() + auctionTime;
        AH->deposit = 0;
        AH->auctionHouseEntry = entree;

        sAuctionMgr->AddAItem(item);
        ah->AddAuction(AH);

        bot->MoveItemFromInventory(sac, case_, true);

        // Garde-fou : l'objet ne peut pas etre a la fois dans le sac et a l'HV.
        // MoveItemFromInventory ne fait rien si la case ne porte plus cet
        // objet ; on defait alors l'enchere (RemoveAuction libere AH).
        if (bot->GetItemByGuid(guid))
        {
            sAuctionMgr->RemoveAItem(guid);
            ah->RemoveAuction(AH);
            LOG_ERROR("playerbots.coa", "coa hv: depot defait, objet {} toujours dans les sacs de {}",
                      guid.ToString(), bot->GetName());
            return false;
        }

        item->DeleteFromInventoryDB(trans);
        item->SaveToDB(trans);
        AH->SaveToDB(trans);

        idEnchere = AH->Id;
        duree = auctionTime;
        return true;
    }

    // Sujet d'un courrier d'HV : "item_template:0:reponse:Id:nombre"
    // (AuctionEntry::BuildAuctionMailSubject). Rend la reponse, -1 si illisible.
    int ReponseDuSujet(std::string const& s)
    {
        std::size_t const a = s.find(':');
        if (a == std::string::npos)
            return -1;
        std::size_t const b = s.find(':', a + 1);
        if (b == std::string::npos)
            return -1;
        std::size_t const e = s.find(':', b + 1);
        std::string const r = s.substr(b + 1, e == std::string::npos ? std::string::npos : e - b - 1);
        if (r.empty() || r.size() > 3)
            return -1;
        int v = 0;
        for (char ch : r)
        {
            if (ch < '0' || ch > '9')
                return -1;
            v = v * 10 + (ch - '0');
        }
        return v;
    }

    void PublierBilan()
    {
        uint64 const hors = horsFilMonde.load(std::memory_order_relaxed);
        LOG_INFO("playerbots.coa",
                 "coa hv: mode={} passes={} appels={} hors_fil_monde={} postes={} deposes={} echecs={} unites={} "
                 "rachat_total={} marchand_total={} regle_sousenchere={} regle_plafond={} regle_plancher={} "
                 "ecarte_bot={} ecarte_lieu={} ecarte_directeur={} ecarte_camp={} ecarte_maison={} sans_candidat={} "
                 "objets_vus={} hors_categorie={} exclu_liaison={} exclu_conjure={} exclu_duree={} exclu_sac={} "
                 "exclu_sans_prix={} exclu_quete={} exclu_autre={} "
                 "refus_plafond_marchand={} refus_trop_cher={} refus_par_bot={} refus_actives_bot={} "
                 "refus_par_maison={} refus_par_modele={} refus_budget={} refus_par_minute={} "
                 "courrier_releves={} courrier_or={} courrier_objets={} courrier_refus_or={} courrier_ignores={} "
                 "courrier_reportes={} visible_marques={} visible_proposes={} visible_tirage_non={} "
                 "visible_sans_commissaire={} visible_non_consommes={}",
                 sPlayerbotAIConfig.coaHvSimulation ? "simulation" : "reel",
                 c.passes, c.appels, hors, c.postes, c.deposes, c.echecs, c.unites,
                 c.valeurRachat, c.valeurMarchand, c.sousEnchere, c.plafonne, c.plancher,
                 c.ecarteBot, c.ecarteLieu, c.ecarteDirecteur, c.ecarteCamp, c.ecarteMaison, c.sacsSansCandidat,
                 c.objetsVus, c.horsCategorie, c.exclusLiaison, c.exclusConjure, c.exclusDuree, c.exclusSac,
                 c.exclusSansPrix, c.exclusQuete, c.exclusAutre,
                 c.rejetPlafondMarchand, c.rejetTropCher, c.rejetParBot, c.rejetActivesBot, c.rejetMaison,
                 c.rejetModele, c.rejetBudget, c.rejetMinute,
                 c.courrierReleves, c.courrierOr, c.courrierObjets, c.courrierRefusOr, c.courrierIgnores,
                 c.courrierReportes, c.visibleMarques, c.visibleProposes, c.visibleTirageNon,
                 c.visibleSansCommissaire, c.visibleNonConsommes);
    }
}

namespace CoaHotelDesVentes
{
    PasseMonde::PasseMonde() : _precedent(tlFilMonde)
    {
        tlFilMonde = true;

        // Une passe imbriquee (la commande `.playerbots rndbot update` appelle
        // UpdateAIInternal depuis le fil monde) ne rouvre pas le budget.
        if (_precedent)
            return;

        indexPasse.clear();
        deposeCettePasse.clear(); // deja vide : le destructeur de la passe precedente l'a vide
        budgetPasse = sPlayerbotAIConfig.coaHvMaxParPasse;
        budgetCourrier = sPlayerbotAIConfig.coaHvCourrierMaxParPasse;

        if (!sPlayerbotAIConfig.coaHvEnable)
            return;

        ++c.passes;
        time_t const maintenant = GameTime::GetGameTime().count();
        if (!dernierBilan)
            dernierBilan = maintenant;
        else if (sPlayerbotAIConfig.coaHvBilanSecondes &&
                 maintenant - dernierBilan >= time_t(sPlayerbotAIConfig.coaHvBilanSecondes))
        {
            dernierBilan = maintenant;
            PublierBilan();
        }
    }

    PasseMonde::~PasseMonde()
    {
        // Lot 4 : une marque « vient de deposer » ne survit pas a sa passe
        // (Revive enchaine sur RandomTeleportGrindForLevel, qui ne la lit pas).
        if (!_precedent && !deposeCettePasse.empty())
        {
            c.visibleNonConsommes += deposeCettePasse.size();
            deposeCettePasse.clear();
        }
        tlFilMonde = _precedent;
    }

    void AvantDestruction(Player* bot, char const* motif)
    {
        PlayerbotAIConfig const& cfg = sPlayerbotAIConfig;
        if (!cfg.coaHvEnable)
            return;

        // GARDE DE FIL. Hors du fil monde, on ne lit meme pas l'HV.
        if (!tlFilMonde)
        {
            horsFilMonde.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        bool const reel = !cfg.coaHvSimulation;

        ++c.appels;
        if (!motif)
            motif = "?";

        // QUI : bot aleatoire, en monde, sans maitre humain, sans groupe.
        PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!bot || !ai || !bot->IsInWorld() || !sRandomPlayerbotMgr.IsRandomBot(bot) || bot->GetGroup())
        {
            ++c.ecarteBot;
            return;
        }
        if (Player* maitre = ai->GetMaster())
        {
            if (IsRealPlayer(maitre))
            {
                ++c.ecarteBot;
                return;
            }
        }

        // OU : hors BG, file de BG, instance.
        Map* carte = bot->GetMap();
        if (!carte || bot->InBattleground() || bot->InBattlegroundQueue() || carte->Instanceable())
        {
            ++c.ecarteLieu;
            return;
        }

        uint32 const botLow = bot->GetGUID().GetCounter();
        if (CoaDirecteur::AffectationDe(botLow, nullptr) || CoaDirecteur::CibleDeBanc(botLow))
        {
            ++c.ecarteDirecteur;
            return;
        }

        // LA MAISON : celle du camp d'origine (race), pas celui du moment.
        AuctionHouseId maison;
        switch (bot->GetTeamId(true))
        {
            case TEAM_ALLIANCE: maison = AuctionHouseId::Alliance; break;
            case TEAM_HORDE:    maison = AuctionHouseId::Horde;    break;
            default:
                ++c.ecarteCamp;
                return;
        }
        // Les deux suivent AllowTwoSide.Interaction.Auction : a 1, la maison
        // effective est la neutre (7), et c'est elle qu'on ecrit dans houseId,
        // comme HandleAuctionSellItem.
        AuctionHouseEntry const* entree = AuctionHouseMgr::GetAuctionHouseEntryFromHouse(maison);
        AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMapByHouseId(maison);
        if (!entree || !ah)
        {
            ++c.ecarteMaison;
            return;
        }

        // QUOI : les sacs seulement. Sac a dos, puis les quatre sacs equipes
        // (leur contenu ; le sac lui-meme est porte, donc ignore).
        std::vector<Candidat> candidats;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            VisiterObjet(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), candidats);
        for (uint8 sac = INVENTORY_SLOT_BAG_START; sac < INVENTORY_SLOT_BAG_END; ++sac)
        {
            Bag* b = bot->GetBagByPos(sac);
            if (!b)
                continue;
            for (uint32 j = 0; j < b->GetBagSize(); ++j)
                VisiterObjet(b->GetItemByPos(uint8(j)), candidats);
        }

        if (candidats.empty())
        {
            ++c.sacsSansCandidat;
            return;
        }

        IndexMaison& idx = indexPasse[entree->houseId];
        if (!idx.bati)
            BatirIndex(idx, ah);

        std::vector<Candidat> chiffres;
        chiffres.reserve(candidats.size());
        for (Candidat& k : candidats)
        {
            auto it = idx.parModele.find(k.tpl->ItemId);
            if (Chiffrer(k, it != idx.parModele.end() ? &it->second : nullptr, botLow))
                chiffres.push_back(k);
        }

        // Les plus chers d'abord : ce que les plafonds laissent passer, autant
        // que ce soit ce qui vaut la peine.
        std::sort(chiffres.begin(), chiffres.end(),
                  [](Candidat const& a, Candidat const& b) { return a.rachat > b.rachat; });

        // Fenetre glissante d'une minute pour MaxParMinute (D3) : bornee en
        // duree, pas seulement par passe (la cadence des passes varie).
        time_t const maintenant = GameTime::GetGameTime().count();
        if (!fenetreMinute || maintenant - fenetreMinute >= 60)
        {
            fenetreMinute = maintenant;
            depotsFenetre = 0;
        }

        // Une transaction pour tous les depots reels de ce bot, ouverte au
        // premier depot seulement.
        CharacterDatabaseTransaction trans;

        uint32 postesBot = 0;
        for (Candidat const& k : chiffres)
        {
            if (postesBot >= cfg.coaHvMaxParBotParPassage)
            {
                ++c.rejetParBot;
                continue;
            }
            if (!budgetPasse)
            {
                ++c.rejetBudget;
                continue;
            }
            if (depotsFenetre >= cfg.coaHvMaxParMinute)
            {
                ++c.rejetMinute;
                continue;
            }
            if (idx.parProprietaire[botLow] >= cfg.coaHvMaxActivesParBot)
            {
                ++c.rejetActivesBot;
                continue;
            }
            if (idx.encheresBots >= cfg.coaHvMaxParMaison)
            {
                ++c.rejetMaison;
                continue;
            }
            Modele& m = idx.parModele[k.tpl->ItemId];
            if (m.encheresBots >= cfg.coaHvMaxParModele)
            {
                ++c.rejetModele;
                continue;
            }

            uint32 idEnchere = 0;
            uint32 duree = 0;
            if (reel)
            {
                if (!trans)
                    trans = CharacterDatabase.BeginTransaction();
                if (!Deposer(bot, k, entree, ah, trans, idEnchere, duree))
                {
                    ++c.echecs;
                    continue;
                }
            }

            // Retenu (et, en reel, depose) : l'index de la passe le voit
            // aussitot, pour les plafonds et la sous-enchere des suivants.
            ++postesBot;
            --budgetPasse;
            ++depotsFenetre;
            ++idx.parProprietaire[botLow];
            ++idx.encheresBots;
            ++m.encheresBots;
            m.Noter(std::max<uint64>(1, k.rachat / k.n), botLow);

            if (reel)
                ++c.deposes;
            else
                ++c.postes;
            c.unites += k.n;
            c.valeurRachat += k.rachat;
            c.valeurMarchand += uint64(k.tpl->SellPrice) * k.n;
            switch (k.regle)
            {
                case Regle::SousEnchere: ++c.sousEnchere; break;
                case Regle::Plafond:     ++c.plafonne;    break;
                case Regle::Plancher:    ++c.plancher;    break;
                default:                 break;
            }

            if (reel)
                LOG_INFO("playerbots.coa",
                         "coa hv depot: bot={} objet={} ({}) n={} mise={} rachat={} maison={} motif={} regle={} "
                         "enchere={} duree={}",
                         bot->GetName(), k.tpl->Name1, k.tpl->ItemId, k.n, k.mise, k.rachat, entree->houseId, motif,
                         NomRegle(k.regle), idEnchere, duree);
            else
                LOG_INFO("playerbots.coa",
                         "coa hv simu: bot={} objet={} ({}) n={} mise={} rachat={} maison={} motif={} regle={}",
                         bot->GetName(), k.tpl->Name1, k.tpl->ItemId, k.n, k.mise, k.rachat, entree->houseId, motif,
                         NomRegle(k.regle));
        }

        if (trans && postesBot)
        {
            // Comme le gestionnaire du coeur : inventaire et or du vendeur dans
            // la meme transaction que l'objet et l'enchere.
            bot->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            // Lot 4 : depot REEL fait (trans n'est ouverte qu'en reel). Le
            // teleport qui suit dans ProcessBot (RandomTeleportForLevel) lira
            // la marque ; sinon elle meurt avec la passe.
            if (cfg.coaHvTeleportVisible && deposeCettePasse.insert(botLow).second)
                ++c.visibleMarques;
        }
    }

    void ReleverCourrier(Player* bot)
    {
        PlayerbotAIConfig const& cfg = sPlayerbotAIConfig;
        if (!cfg.coaHvEnable || !cfg.coaHvCourrier)
            return;

        // GARDE DE FIL : m_mail est aussi ecrit par AuctionHouseMgr::Update
        // (fil monde) et lu par la sauvegarde du joueur (fil de carte).
        if (!tlFilMonde)
        {
            horsFilMonde.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (!bot || !bot->IsInWorld() || !bot->GetSession() || !sRandomPlayerbotMgr.IsRandomBot(bot))
            return;

        // Les courriers sont charges a la connexion (Player::_LoadMail, appele
        // sans condition par LoadFromDB) : ce coeur n'a pas de m_mailsLoaded.
        // Un bot en ligne a donc sa boite en memoire.
        time_t const maintenant = GameTime::GetGameTime().count();

        // 1. Choisir, sans rien modifier (RemoveMail touche la deque).
        std::vector<Mail*> choisis;
        for (Mail* m : bot->GetMails())
        {
            if (!m || m->state == MAIL_STATE_DELETED)
                continue;
            if (m->messageType != MAIL_AUCTION)
                continue;
            if (m->deliver_time > maintenant)
                continue;

            // Contre-remboursement : un courrier d'HV n'en porte jamais.
            // AUCTION_WON porte un objet ACHETE : un bot n'encherit pas, mais
            // si cela arrivait on ne detruirait pas un achat. Sujet illisible :
            // on ne devine pas.
            int const reponse = ReponseDuSujet(m->subject);
            if (m->COD || reponse < 0 || reponse == AUCTION_WON)
            {
                ++c.courrierIgnores;
                continue;
            }

            if (choisis.size() >= budgetCourrier)
            {
                ++c.courrierReportes;
                continue;
            }
            choisis.push_back(m);
        }

        if (choisis.empty())
            return;

        // 2. Une transaction pour tout le bot.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        uint32 const botLow = bot->GetGUID().GetCounter();
        uint32 traites = 0;
        bool orModifie = false;

        for (Mail* m : choisis)
        {
            // L'or d'abord : s'il ne passe pas (MAX_MONEY_AMOUNT), le courrier
            // reste INTACT, objets compris, comme HandleMailTakeMoney.
            if (m->money)
            {
                if (m->money > uint32(MAX_MONEY_AMOUNT) || !bot->ModifyMoney(int32(m->money), false))
                {
                    ++c.courrierRefusOr;
                    continue;
                }
                c.courrierOr += m->money;
                m->money = 0;
                orModifie = true;
            }

            // Objets joints : rangee item_instance effacee, objet en memoire
            // retire de mMitems et libere. Un objet absent de mMitems (echec
            // de chargement) voit quand meme sa rangee effacee.
            for (MailItemInfo const& mi : m->items)
            {
                Item::DeleteFromDB(trans, mi.item_guid);
                if (Item* it = bot->GetMItem(mi.item_guid))
                {
                    bot->RemoveMItem(mi.item_guid);
                    delete it;
                }
                ++c.courrierObjets;
            }
            m->items.clear();

            // Le courrier : les deux DELETE de la branche MAIL_STATE_DELETED
            // de Player::_SaveMail, puis retrait de la boite et liberation.
            uint32 const id = m->messageID;
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
            stmt->SetData(0, id);
            trans->Append(stmt);
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM_BY_ID);
            stmt->SetData(0, id);
            trans->Append(stmt);

            m->state = MAIL_STATE_DELETED;
            bot->RemoveMail(id);
            delete m;
            sMailMgr->OnMailDeleted(botLow);

            ++traites;
            ++c.courrierReleves;
        }

        if (!traites)
            return; // rien d'ecrit : la transaction vide est abandonnee

        budgetCourrier -= std::min(budgetCourrier, traites);
        if (orModifie)
            bot->SaveGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        bot->UpdateNextMailTimeAndUnreads();
    }

    bool DestinationsCommissaire(Player* bot, std::vector<WorldLocation>& locs)
    {
        PlayerbotAIConfig const& cfg = sPlayerbotAIConfig;
        if (!cfg.coaHvEnable || !cfg.coaHvTeleportVisible || !bot)
            return false;

        // GARDE DE FIL : la marque et le cache ne vivent que sur le fil monde.
        // Un appel depuis un fil de carte (montee de niveau) ne lit rien.
        if (!tlFilMonde)
            return false;

        uint32 const botLow = bot->GetGUID().GetCounter();
        auto marque = deposeCettePasse.find(botLow);
        if (marque == deposeCettePasse.end())
            return false;
        deposeCettePasse.erase(marque); // consommee, quoi qu'il arrive ensuite

        if (urand(1, 100) > cfg.coaHvTeleportVisiblePct)
        {
            ++c.visibleTirageNon;
            return false;
        }

        // Le camp ou il a depose (race) ; et RandomTeleport ecarte les zones
        // ennemies selon le camp DU MOMENT : s'ils different, banquier.
        TeamId const camp = bot->GetTeamId(true);
        if ((camp != TEAM_ALLIANCE && camp != TEAM_HORDE) || bot->GetTeamId() != camp)
        {
            ++c.visibleSansCommissaire;
            return false;
        }

        BatirCommissaires();

        // NIVEAU. La branche ville ne filtre pas par niveau de zone mais par
        // palier de banquier (TravelMgr::PrepareDestinationCache : 1-60 monde
        // d'origine, 61-70 Shattrath, 71+ Dalaran). Shattrath n'a pas d'HV :
        // on garde les capitales pour tous, et le Norfendre (Dalaran) a
        // partir de 71 seulement, comme ses banquiers. Sous 17, le teleport
        // exige aussi la carte et la distance du point de depart
        // (StarterLevelDistanceCheck, repris de RandomTeleport) : on le
        // prefiltre ici pour pouvoir retomber sur le banquier, puisque
        // RandomTeleport, s'il ecarte tout, ne teleporte pas du tout.
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        uint32 const niveau = bot->GetLevel();
        locs.clear();
        for (WorldLocation const& loc : commissaires.parCamp[camp == TEAM_ALLIANCE ? 0 : 1])
        {
            if (loc.GetMapId() == MAP_NORTHREND && niveau < 71)
                continue;
            if (std::find(cfg.randomBotMaps.begin(), cfg.randomBotMaps.end(), loc.GetMapId()) ==
                cfg.randomBotMaps.end())
                continue;
            if (ai && !ai->StarterLevelDistanceCheck(bot, loc, true))
                continue;
            locs.push_back(loc);
        }

        if (locs.empty())
        {
            ++c.visibleSansCommissaire;
            return false;
        }

        ++c.visibleProposes;
        LOG_INFO("playerbots.coa", "coa hv visible: bot={} niveau={} camp={} commissaires={}", bot->GetName(), niveau,
                 camp == TEAM_ALLIANCE ? "alliance" : "horde", locs.size());
        return true;
    }
}
