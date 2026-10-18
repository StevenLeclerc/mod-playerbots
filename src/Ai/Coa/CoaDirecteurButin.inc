// ------------------------------------------------------------------ butin des bots aux evenements (12/10/2026)
//
// FRAGMENT de CoaDirecteur.cpp : inclus par lui, A LA FIN, HORS de son espace de noms anonyme (il definit des
// fonctions publiques de CoaDirecteur.h). Il n'est pas une unite de compilation. Conception :
// coa-project/docs/CONCEPTION-ressources-rares-evenements.md § 4.1 (lot 3), decisions D2 et D6 du 12/10.
//
// LE VERROU (LU) : LootAction::isUseful rend faux pour un bot d'un groupe en butin LIBRE quand
// AiPlayerbot.FreeMethodLoot = 0 (en service) et que le bot n'est pas un « self bot ». Or le groupe d'evenement cree
// par le Directeur est en FREE_FOR_ALL (CoaDirecteurGroupe.inc) : ses bots ne ramassent rien.
//
// D2, voie (a) retenue : une exception CIBLEE dans LootAction, et rien d'autre. Ni l'option globale (c : tous les
// groupes FFA du royaume), ni le mode de butin du groupe (b : tirage Besoin/Cupidite chez l'humain a chaque rare).
// L'exception ne s'ouvre QUE dans le cas ou l'amont refusait (RamassageRestreint), et elle y est restreinte :
//   - un corps de creature d'evenement seulement (plage EV_ENTREE_MIN-EV_ENTREE_MAX), jamais un coffre ni un
//     autre corps ; LootAction::Execute retire de la pile tout autre candidat ;
//   - les objets selon D6 (PeutPrendreEnButinLibre) ; jamais l'or (StoreLootAction n'envoie pas CMSG_LOOT_MONEY).
// Le critere est le CORPS, pas l'inscription : un bot inscrit (affectation Evenement) ET un bot present dans le
// groupe d'evenement sans inscription (inscrit mort puis releve : son inscription est levee `mort`, il reste au
// groupe) ramassent de meme. Un groupe FFA d'un humain sans rapport avec un evenement n'est touche que pres d'un
// corps d'evenement, et n'y prend que la copie MULTI_DROP de ses bots (ou la verte, s'il n'y a aucun humain).
//
// D6 (LU, LootMgr.cpp:400, 565-579, FillFFALoot / FillNonQuestNonFFAConditionalLoot) : un objet MULTI_DROP (0x800 :
// rare et epique, ressources_evenements.py) est COPIE pour chaque ayant droit ; un objet ordinaire (la verte) est un
// exemplaire unique, au premier qui le prend. Un bot ramasse a 15 yd (LootDistance) en 1 s (LootDelay) : sans D6 il
// prendrait la verte sous les yeux de l'humain. Donc : sa copie MULTI_DROP toujours ; la verte seulement si aucun
// membre du groupe n'est humain (les evenements entre bots seuls, frequents depuis l'ancre bot).
//
// FILS : tout est lu depuis le fil de carte du bot (LootAction, StoreLootAction, OnPlayerLootItem). La conf est ecrite
// par PlayerbotAIConfig::Initialize, fils de carte a l'arret. AffectationDe est « tout fil ». Les membres du groupe
// se lisent par leurs guids (GetMemberSlots) et ObjectAccessor::FindConnectedPlayer (verrou propre de l'accesseur),
// sans toucher leur Player au-dela de sa session.

bool CoaDirecteur::EstRessourceDEvenement(uint32 itemId)
{
    if (!itemId)
        return false;
    std::vector<uint32> const& objets = sPlayerbotAIConfig.directeurButinObjets;
    return std::find(objets.begin(), objets.end(), itemId) != objets.end();
}

bool CoaDirecteur::EstCreatureDEvenement(uint32 entree)
{
    return entree >= EV_ENTREE_MIN && entree <= EV_ENTREE_MAX;
}

bool CoaDirecteur::RamassageRestreint(Player* bot)
{
    if (!sPlayerbotAIConfig.directeurButinBots || !bot)
        return false;
    // Exactement la negation de LootAction::isUseful d'origine : on ne s'ouvre que la ou l'amont refusait.
    if (sPlayerbotAIConfig.freeMethodLoot || IsSelfBot(bot))
        return false;
    Group* groupe = bot->GetGroup();
    return groupe && groupe->GetLootMethod() == FREE_FOR_ALL;
}

namespace
{
    // Un membre du groupe est-il (peut-etre) humain ? Un membre hors ligne ou introuvable compte comme humain.
    bool GroupeAvecHumain(Player* bot)
    {
        Group* groupe = bot->GetGroup();
        if (!groupe)
            return false;
        for (Group::MemberSlot const& m : groupe->GetMemberSlots())
        {
            if (m.guid == bot->GetGUID())
                continue;
            Player* membre = ObjectAccessor::FindConnectedPlayer(m.guid);
            if (!membre || !membre->GetSession() || !membre->GetSession()->IsBot())
                return true;
        }
        return false;
    }
}

bool CoaDirecteur::PeutPrendreEnButinLibre(Player* bot, uint32 itemId)
{
    if (!bot || !EstRessourceDEvenement(itemId))
        return false;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return false;
    if (proto->HasFlag(ITEM_FLAG_MULTI_DROP))
        return true;
    return !GroupeAvecHumain(bot);
}

bool CoaDirecteur::ResteAPrendre(Player* bot, Creature* corps)
{
    if (!bot || !corps)
        return false;
    Loot const& loot = corps->loot;
    // Copies par ayant droit (MULTI_DROP) : la liste du bot, remplie au tirage (Loot::FillFFALoot), comme
    // Loot::hasItemFor (LootMgr.cpp).
    QuestItemMap const& ffa = loot.GetPlayerFFAItems();
    auto mien = ffa.find(bot->GetGUID());
    if (mien != ffa.end() && mien->second)
        for (QuestItem const& q : *mien->second)
            if (q.index < loot.items.size() && !q.is_looted && !loot.items[q.index].is_looted &&
                PeutPrendreEnButinLibre(bot, loot.items[q.index].itemid))
                return true;
    // Exemplaires uniques (la verte) : non pris, permis a ce bot par le coeur et par D6.
    for (LootItem const& li : loot.items)
        if (!li.freeforall && !li.is_looted && li.AllowedForPlayer(bot, corps->GetGUID()) &&
            PeutPrendreEnButinLibre(bot, li.itemid))
            return true;
    return false;
}

void CoaDirecteur::TracerButin(Player* pilleur, uint32 itemId, uint32 quantite, ObjectGuid const& source)
{
    if (!sPlayerbotAIConfig.directeurButinTrace || !pilleur || !EstRessourceDEvenement(itemId))
        return;

    WorldSession* session = pilleur->GetSession();
    bool const bot = session && session->IsBot();
    uint32 const low = pilleur->GetGUID().GetCounter();

    Affectation a;
    bool const inscrit = bot && AffectationDe(low, &a) && a.mission == CoaDirMission::Evenement;

    char const* groupe = "aucun";
    if (Group* g = pilleur->GetGroup())
        groupe = g->GetLootMethod() == FREE_FOR_ALL ? "libre" : "autre";

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    uint32 const qualite = proto ? uint32(proto->Quality) : 0;
    bool const copie = proto && proto->HasFlag(ITEM_FLAG_MULTI_DROP);

    // source : le corps (creature) ou l'objet pille ; 0 si ce n'est pas une creature.
    uint32 const sourceEntree = source.IsCreatureOrVehicle() ? source.GetEntry() : 0;

    LOG_INFO("playerbots",
             "coa directeur butin: pilleur={} guid={} bot={} inscrit={} groupe={} objet={} qualite={} copie={} n={} "
             "source={} evenement={} carte={} zone={}",
             pilleur->GetName(), low, bot ? 1 : 0, inscrit ? 1 : 0, groupe, itemId, qualite, copie ? 1 : 0, quantite,
             sourceEntree, EstCreatureDEvenement(sourceEntree) ? 1 : 0, pilleur->GetMapId(), pilleur->GetZoneId());
}
