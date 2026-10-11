// ------------------------------------------------------------------ ressources rares des evenements (11/10/2026)
//
// FRAGMENT de CoaDirecteur.cpp : inclus par lui, DANS son espace de noms anonyme, apres CoaDirecteurBoss.inc. Il
// n'est pas une unite de compilation. Conception : coa-project/docs/CONCEPTION-ressources-rares-evenements.md § 3.1,
// decision D4a du commanditaire (11/10) : LA TRANCHE DE RESSOURCE SUIT LE NIVEAU DU MONSTRE, pas celui du pilleur
// (un 80 qui aide un 10 recoit la ressource de tranche basse).
//
// FORME DES DONNEES (coa-project/exploitation/directeur/ressources_evenements.py, migration 2026_10_11_11) : une
// table de butin par famille et par role porte, en lignes INDEPENDANTES (GroupId 0) et SANS condition, l'objet de
// CHACUNE des quatre tranches. Ce crochet ne garde au tirage que l'objet de la tranche du NIVEAU DE LA CREATURE
// pillee ; les trois autres prennent une chance nulle.
//
// L'objet dit lui-meme sa tranche (plage RESERVEE, conception § 2.2) : 9100100 + 10 x famille + rang, rang 0-3 les
// fragments des tranches 1-4, rang 4-7 les essences des tranches 1-4, rangs 8-9 reserves (non touches) ; cinq
// familles au plus (9100100-9100149 : Orage, Glas, trois futures). Aucune autre table du royaume ne porte ces ids
// (controle de l'epreuve coa-project/outils/test-directeur-ressources.py).
//
// LU dans le coeur :
//   - LootStoreItem::Roll (LootMgr.cpp:311-330) appelle OnItemRoll pour CHAQUE ligne independante, avec une COPIE
//     de la chance (_chance) : la mettre a 0 ecarte la ligne de CE tirage seulement (roll_chance_f(0) est faux).
//     On ne rend jamais faux : dans un groupe (LootGroup::Roll, LootMgr.cpp:1276) cela viderait le groupe entier.
//   - Remplissages de LootTemplates_Creature : Unit::Kill (Unit.cpp:14810, FillLoot(..., looter, ..., creature)) ;
//     quelques scripts d'instance (boss_priestess_delrissa.cpp:120, instance_utgarde_keep.cpp:229/244,
//     boss_high_king_maulgar.cpp:115) dont les tables ne portent pas nos ids : la decision y est Neutre AVANT toute
//     lecture de carte ; la simulation `.debug loot creature` (cs_debug.cpp), Loot neuf sans source : creature
//     introuvable, ressources ecartees (sans journal).
//     Dans Unit::Kill, le pilleur passe est le marqueur s'il est sur la carte de la creature
//     (IsInMap), sinon un membre IsAtLootRewardDistance, ou le pilleur a tour de role choisi par
//     Group::UpdateLooterGuid parmi les memes. IsAtLootRewardDistance mesure depuis le CORPS d'un joueur mort
//     (Player::IsAtGroupRewardDistance), mais ce corps est cherche sur la carte du JOUEUR (Player::GetCorpse ->
//     GetMap()->GetCorpseByPlayer ; Map::AddCorpse l'inscrit sur SA carte, Map.cpp:3080) : un corps trouve est donc
//     sur la carte du joueur, et IsInMap(creature) impose la meme carte. Le pilleur est toujours sur la carte de la
//     creature : Map::GetCreature sur SA carte ne lit que la carte du fil courant (P-137).
//   - Loot::sourceWorldObjectGUID est pose par Creature::AddToWorld (Creature.cpp:331) et Loot::clear ne le remet
//     pas a zero (LootMgr.h:343-365) : c'est le guid de la creature pillee. On verifie en plus que son `loot` EST le
//     butin en cours de tirage.
//   - Le niveau d'une creature d'evenement est celui de l'ancre a +-1 pres (CoaDirecteur.cpp, evNiveauForce), ses
//     renforts celui du chef (CoaDirecteurBoss.inc).
//
// AiPlayerbot.Directeur.RessourcesNiveauMonstre = 0 (defaut) : le crochet rend la main sans rien toucher, AVANT toute
// lecture : comportement IDENTIQUE a celui d'avant. Le lot 2 (butin sans condition) ne s'applique qu'option a 1 : sans
// elle, chaque corps tirerait les quatre tranches ; et tant que ce lot est en base, ne jamais remettre en service un
// binaire SANS ce fragment. La valeur chargee se lit dans CoaBots.log : `coa directeur ressources: sous_action=config
// niveau_monstre=0|1` (PlayerbotAIConfig::Initialize). Creature introuvable (ne devrait pas arriver, voir plus haut) :
// aucune ressource sur ce corps (chance 0) et une ligne de journal par objet.

constexpr uint32 EV_RESSOURCE_PREMIER = 9100100;       // conception § 2.2 : 9100100-9100149, cinq familles
constexpr uint32 EV_RESSOURCE_DERNIER = 9100149;
constexpr uint32 EV_RESSOURCE_PAR_FAMILLE = 10;        // x0-x3 fragments, x4-x7 essences, x8-x9 reserve
constexpr uint32 EV_RESSOURCE_RANGS = 8;
constexpr uint32 EV_RESSOURCE_TRANCHES = 4;
// Borne HAUTE de chaque tranche de niveau (1-34, 35-54, 55-67, 68-80 ; au-dela de 80 : la derniere).
constexpr uint8 EV_RESSOURCE_TRANCHE_HAUT[EV_RESSOURCE_TRANCHES] = {34, 54, 67, 80};

// -> tranche 0-3 d'une ressource d'evenement, -1 pour tout autre objet (reserve x8-x9 compris).
int32 TrancheDeRessource(uint32 itemid)
{
    if (itemid < EV_RESSOURCE_PREMIER || itemid > EV_RESSOURCE_DERNIER)
        return -1;
    uint32 const rang = (itemid - EV_RESSOURCE_PREMIER) % EV_RESSOURCE_PAR_FAMILLE;
    if (rang >= EV_RESSOURCE_RANGS)
        return -1;
    return int32(rang % EV_RESSOURCE_TRANCHES);
}

// -> tranche 0-3 d'un niveau de creature (0 compte en tranche 0, au-dela de 80 en tranche 3).
int32 TrancheDuNiveau(uint32 niveau)
{
    for (uint32 t = 0; t < EV_RESSOURCE_TRANCHES; ++t)
        if (niveau <= EV_RESSOURCE_TRANCHE_HAUT[t])
            return int32(t);
    return int32(EV_RESSOURCE_TRANCHES - 1);
}

enum class EvRessourceTirage : uint8
{
    Neutre,     // pas notre affaire (option eteinte, autre butin, autre objet) : rien n'est touche
    Garde,      // ressource de la tranche du monstre : sa chance reste
    Ecarte,     // ressource d'une autre tranche : chance 0
    Inconnue    // creature pillee introuvable : chance 0, journal
};

// Decision PURE (epreuve par doublure : coa-project/outils/test-directeur-ressources.py). niveauCreature < 0 :
// creature introuvable. Le niveau n'est lu que si les trois premieres gardes passent (lecture de carte evitee).
EvRessourceTirage DeciderRessource(bool allumee, bool butinDeCreature, uint32 itemid, int32 niveauCreature)
{
    if (!allumee || !butinDeCreature)
        return EvRessourceTirage::Neutre;
    int32 const tranche = TrancheDeRessource(itemid);
    if (tranche < 0)
        return EvRessourceTirage::Neutre;
    if (niveauCreature < 0)
        return EvRessourceTirage::Inconnue;
    return TrancheDuNiveau(uint32(niveauCreature)) == tranche ? EvRessourceTirage::Garde : EvRessourceTirage::Ecarte;
}

// La creature dont on tire le butin, sur la carte du pilleur (voir l'en-tete), ou nullptr.
Creature const* CreaturePillee(Player const* player, Loot const& loot)
{
    if (!player || !player->IsInWorld())
        return nullptr;
    ObjectGuid const source = loot.sourceWorldObjectGUID;
    if (!source.IsCreatureOrVehicle())
        return nullptr;
    Map* map = player->GetMap();
    Creature* cr = map ? map->GetCreature(source) : nullptr;
    return (cr && &cr->loot == &loot) ? cr : nullptr;
}

class coa_directeur_ressources_globalscript : public GlobalScript
{
public:
    coa_directeur_ressources_globalscript()
        : GlobalScript("coa_directeur_ressources_globalscript", {GLOBALHOOK_ON_ITEM_ROLL})
    {
    }

    bool OnItemRoll(Player const* player, LootStoreItem const* item, float& chance, Loot& loot,
                    LootStore const& store) override
    {
        bool const allumee = sPlayerbotAIConfig.directeurRessourcesNiveauMonstre;
        bool const creature = &store == &LootTemplates_Creature;
        if (!item || DeciderRessource(allumee, creature, item->itemid, 0) == EvRessourceTirage::Neutre)
            return true;
        Creature const* cr = CreaturePillee(player, loot);
        EvRessourceTirage const d = DeciderRessource(allumee, creature, item->itemid, cr ? int32(cr->GetLevel()) : -1);
        if (d == EvRessourceTirage::Garde)
            return true;
        chance = 0.0f;
        // Butin sans source (`.debug loot creature <id>`, cs_debug.cpp:1704 : un Loot neuf) : ressource ecartee, en
        // silence ; un vrai corps introuvable : une ligne.
        if (d == EvRessourceTirage::Inconnue && !loot.sourceWorldObjectGUID.IsEmpty())
            LOG_INFO("playerbots", "coa directeur ressources: sous_action=tirage joueur={} valeur={} "
                     "motif=creature_introuvable", player ? player->GetGUID().GetCounter() : 0, item->itemid);
        return true;
    }
};
