// ------------------------------------------------------------------ vrai groupe (09/10/2026) : les inscrits rejoignent le joueur
//
// FRAGMENT de CoaDirecteur.cpp : inclus par lui, DANS son espace de noms anonyme, juste apres le lot S. Il n'est pas
// une unite de compilation. Compte rendu : coa-project/docs/EVENEMENTS-groupe.md.
//
// Demande du commanditaire, apres la premiere Nuit des morts (09/10, 10:05-10:13 Paris) : « Les bots se sont battus
// entre eux. [...] Je prefere vraiment qu'ils se mettent en groupe, un vrai groupe, quand ils arrivent sur place. »
//
// LU (coeur, branche coa-service-19092026) :
//  - Unit::GetReactionTo : deux joueurs du meme groupe ou raid (IsInRaidWith) sont REP_FRIENDLY, et ce test passe
//    AVANT celui du FFA ; Unit::_IsValidAttackTarget refuse alors toute attaque (« can't attack friendly targets »),
//    sorts de zone compris (meme fonction, bySpell). Seul le duel en cours passe avant.
//  - Group::AddMember ne teste PAS si un groupe ordinaire est plein (seul un raid cherche un sous-groupe libre) :
//    IsFull() est teste ici. Group::RemoveMember dissout un groupe ordinaire de deux membres (Disband, qui detruit
//    l'objet) : on ne relit jamais le groupe apres.
// LU (mod-playerbots) :
//  - PlayerbotAI::UpdateAIGroupMaster, a chaque UpdateAI du bot (son fil de carte, son proprietaire) : un bot groupe
//    sans maitre prend le chef humain pour maitre (FindNewMaster), ResetStrategies, « +follow », et le dit
//    (« Hello, I follow you! ») ; un bot ALEATOIRE sans groupe perd son maitre, Reset, ResetStrategies : il reprend sa
//    vie. Le fil monde n'ecrit donc NI le maitre NI les strategies (P-134) : il groupe et degroupe, l'IA suit.
//  - AiFactory::AddDefaultNonCombatStrategies (relecture du 09/10) : ResetStrategies d'un bot ALEATOIRE groupe sans en
//    etre le chef ne pose NI « grind » NI « new rpg ». Groupe, l'inscrit n'a donc plus CibleDEvenement
//    (GrindTargetValue), ni la laisse ni le ralliement (NewRpgAction) : il SUIT le joueur. Il engage par
//    « dps assist » / « tank assist » (poses pour toute classe AVANT la branche du bot aleatoire), qui lisent
//    « attackers », ou AssaillantsDEvenement ajoute la defense ET (a) tout invoque a EVENEMENT_ENGAGEMENT_YD dans la
//    laisse : les memes bornes que CibleDEvenement. L'engagement de lui-meme tient par cette voie (LU ; non vu en jeu).
//  - PlayerbotAI::UpdateAI, meriteLeDrapeauFFA : `AllBotsAreTargets && !GetMaster()` ; un bot a maitre perd le FFA
//    au tick suivant (branche de retrait, UpdateFFAPvPState(true)).
//  - AttackersValue::Calculate ajoute les assaillants du GROUPE (AddAttackersOf(group)) ; IsPossibleTarget admet une
//    creature dont le maitre du bot a de la menace : plus d'anti vol de kill entre le joueur et ses bots.
//  - SuivreAffectations leve `maitre_humain` toute affectation d'un bot qui a un maitre humain : l'inscription en est
//    exemptee quand ce maitre est le joueur servi et que le bot est dans le groupe pose ici (GroupeDEvenementPour).
//
// ETAT (fil monde SEUL : ecrit et lu dans Entretien, SuivreAffectations et OnShutdown, jamais depuis un fil de carte) :
//  - groupesEvenement : bot -> joueur servi, commande (inscription) qui l'a groupe, groupe (guid bas), cree ou non ;
//  - groupesTentes : inscriptions (id de commande) deja essayees, pour qu'un refus ne soit dit et tente qu'une fois.
// ENTREE : inscription (presence + mission Evenement) en SEJOUR, non echue, option Directeur.EvenementGroupe allumee.
// SORTIE : plus AUCUN bot inscrit a l'evenement de ce joueur (fin, `lever`, stop, bus eteint, deconnexion du joueur
// -- qui leve l'inscription `joueur_hors_ligne`), option eteinte, joueur ou bot hors ligne, arret du serveur ; et
// `sans_evenement` (relecture du 09/10) : aucun invoque de l'evenement publie pour ce joueur (evfCibles) depuis
// GROUPE_SANS_EVENEMENT_S -- le service mort sans emettre ses levees (plantage, kill -9) laissait sinon le bot dans le
// groupe, a suivre le joueur, jusqu'a l'echeance de l'inscription (duree d'ambiance, jusqu'a 3600 s). Un
// inscrit mort (inscription levee `mort`) RESTE dans le groupe tant que d'autres servent le joueur : le joueur peut le
// relever, et il ne quitte pas le groupe sous ses yeux au milieu d'une vague.

struct GroupeDEvenement
{
    uint32 joueur = 0;                  // humain servi (chef du groupe a l'entree)
    uint32 id = 0;                      // inscription qui l'a groupe
    ObjectGuid::LowType groupe = 0;     // Group::GetGUID().GetCounter()
    bool cree = false;                  // le groupe a ete cree pour l'evenement
    uint32 vuS = 0;                     // Maintenant() a l'entree, puis au dernier invoque publie pour le joueur
};
// Sans invoque publie pour le joueur depuis ce temps, le groupe d'evenement est quitte (`sans_evenement`). Bien
// au-dela des attentes du service (attente des bots 150 s, entre deux vagues 75 s par defaut, nuit_des_morts.py) :
// c'est un filet pour un service mort, pas une regle de jeu.
constexpr uint32 GROUPE_SANS_EVENEMENT_S = 600;
// Defini apres evfCibles (lot F, plus bas dans CoaDirecteur.cpp) : un invoque vivant est-il publie pour ce joueur ?
// Fil monde (verrou partage evfVerrou).
bool EvenementPubliePour(uint32 joueurLow);
std::unordered_map<uint32, GroupeDEvenement> groupesEvenement;   // cle : guid bas du bot
std::unordered_set<uint32> groupesTentes;                        // ids d'inscription deja essayes

bool GroupeDEvenementPour(uint32 botLow, uint32 joueurLow)
{
    if (groupesEvenement.empty())
        return false;
    auto const it = groupesEvenement.find(botLow);
    return it != groupesEvenement.end() && it->second.joueur == joueurLow;
}

// Le bot quitte le groupe pose pour l'evenement. Fil monde. `raison` : mot du journal.
void QuitterGroupeDEvenement(uint32 botLow, GroupeDEvenement const& e, char const* raison)
{
    ObjectGuid const botGuid = ObjectGuid::Create<HighGuid::Player>(botLow);
    Group* g = e.groupe ? sGroupMgr->GetGroupByGUID(e.groupe) : nullptr;
    uint32 membres = 0;
    bool retire = false;
    bool dissous = false;
    if (g && g->IsMember(botGuid))
    {
        membres = g->GetMembersCount();
        // Groupe ordinaire de deux membres : RemoveMember le dissout (Disband detruit l'objet ; `g` n'est plus relu).
        dissous = membres <= 2;
        g->RemoveMember(botGuid, GROUP_REMOVEMETHOD_DEFAULT);
        retire = true;
    }
    LOG_INFO("playerbots",
             "coa directeur groupe: id={} guid={} joueur={} motif=quitte raison={} retire={} dissous={} membres_avant={}"
             " cree={}",
             e.id, botLow, e.joueur, raison, retire ? 1 : 0, dissous ? 1 : 0, membres, e.cree ? 1 : 0);
}

// Pourquoi ce bot ne peut pas rejoindre le joueur (nullptr : il le peut). `groupe` : le groupe du joueur, ou nul.
char const* RefusGroupeDEvenement(Player* bot, Player* joueur, Group* groupe)
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
        return "bot_absent";
    if (!joueur || !joueur->IsInWorld() || !Humain(joueur))
        return "joueur_absent";
    if (bot->GetMapId() != joueur->GetMapId())
        return "carte";
    if (bot->GetTeamId() != joueur->GetTeamId())
        return "faction";
    if (bot->InBattleground() || bot->InArena() || joueur->InBattleground() || joueur->InArena())
        return "champ_de_bataille";
    if (bot->GetGroup() || bot->GetOriginalGroup())
        return "bot_groupe";
    if (bot->GetGroupInvite() || joueur->GetGroupInvite())
        return "invitation_en_cours";
    // Un groupe cree ou change sous un joueur en file du Donjon (outil de recherche) le sortirait de la file.
    if (sLFGMgr->GetState(joueur->GetGUID()) != lfg::LFG_STATE_NONE)
        return "file_donjon";
    if (!groupe)
        return nullptr;
    if (groupe->isBGGroup() || groupe->isBFGroup() || groupe->isLFGGroup())
        return "groupe_special";
    if (groupe->isRaidGroup())
        return "raid";
    if (!groupe->IsLeader(joueur->GetGUID()))
        return "pas_chef";
    if (groupe->IsFull())
        return "plein";
    return nullptr;
}

// L'inscrit `botLow` (commande `id`) rejoint le groupe du joueur `joueurLow`. Fil monde, cartes a l'arret.
void RejoindreGroupeDEvenement(uint32 botLow, uint32 joueurLow, uint32 id)
{
    Player* bot = JoueurConnecte(botLow);
    Player* joueur = JoueurConnecte(joueurLow);
    Group* groupe = joueur ? joueur->GetGroup() : nullptr;
    if (char const* refus = RefusGroupeDEvenement(bot, joueur, groupe))
    {
        LOG_INFO("playerbots",
                 "coa directeur groupe: id={} guid={} joueur={} motif=refus raison={} membres={}", id, botLow, joueurLow,
                 refus, groupe ? groupe->GetMembersCount() : 0);
        return;
    }
    bool cree = false;
    if (!groupe)
    {
        // Enregistre aupres du gestionnaire, sinon le groupe n'a pas d'identifiant de stockage (meme patron que
        // RecruitCoaBot et `.coa groupe`, CoaDuelCommandScript.cpp).
        groupe = new Group();
        if (!groupe->Create(joueur))
        {
            delete groupe;
            LOG_INFO("playerbots", "coa directeur groupe: id={} guid={} joueur={} motif=refus raison=creation membres=0",
                     id, botLow, joueurLow);
            return;
        }
        sGroupMgr->AddGroup(groupe);
        cree = true;
    }
    if (!groupe->AddMember(bot))
    {
        // Un groupe cree a l'instant pour ce bot et reste a un seul membre : rien ne le retient, il est dissous.
        uint32 const membres = groupe->GetMembersCount();
        if (cree && membres <= 1)
            groupe->Disband();
        LOG_INFO("playerbots", "coa directeur groupe: id={} guid={} joueur={} motif=refus raison=ajout membres={}", id,
                 botLow, joueurLow, membres);
        return;
    }
    groupe->BroadcastGroupUpdate();
    GroupeDEvenement e;
    e.joueur = joueurLow;
    e.id = id;
    e.groupe = groupe->GetGUID().GetCounter();
    e.cree = cree;
    e.vuS = Maintenant();
    groupesEvenement[botLow] = e;
    LOG_INFO("playerbots", "coa directeur groupe: id={} guid={} joueur={} motif={} membres={}", id, botLow, joueurLow,
             cree ? "cree" : "ajoute", groupe->GetMembersCount());
}

// Chaque seconde, fil monde (Entretien, apres SuivreAffectations : une arrivee constatee a cette seconde est en
// SEJOUR), cartes a l'arret (P-137). Sans entree ni option : une lecture de booleen et un test de conteneur vide.
void SuivreGroupesEvenement()
{
    bool const option = sPlayerbotAIConfig.directeurEvenementGroupe && sPlayerbotAIConfig.directeurEvenementBots;
    if (!option && groupesEvenement.empty())
    {
        groupesTentes.clear();
        return;
    }
    uint32 const maintenant = Maintenant();
    struct Candidat
    {
        uint32 bot, joueur, id;
    };
    std::vector<Candidat> candidats;
    std::unordered_set<uint32> joueursServis;   // joueurs dont au moins un bot est encore inscrit (toute phase)
    std::unordered_set<uint32> idsActifs;
    if (nbAffectations.load(std::memory_order_relaxed))
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto const& [guid, f] : affectations)
        {
            if (!f.a.presence || f.a.mission != CoaDirecteur::CoaDirMission::Evenement || !f.a.missionParam ||
                f.a.finS <= maintenant)
                continue;
            joueursServis.insert(f.a.missionParam);
            idsActifs.insert(f.a.id);
            if (f.a.phase == CoaDirecteur::CoaDirPhase::Sejour)
                candidats.push_back(Candidat{guid, f.a.missionParam, f.a.id});
        }
    }

    // 1. Sorties (d'abord : un bot qui sort ne rentre pas a la meme seconde, son inscription etant deja essayee).
    std::vector<std::pair<uint32, char const*>> sorties;
    std::unordered_map<uint32, bool> publies;   // joueur -> invoque publie (une lecture sous verrou par joueur)
    for (auto& [botLow, e] : groupesEvenement)
    {
        auto pub = publies.find(e.joueur);
        if (pub == publies.end())
            pub = publies.emplace(e.joueur, EvenementPubliePour(e.joueur)).first;
        if (pub->second)
            e.vuS = maintenant;
        char const* raison = nullptr;
        Player* bot = JoueurConnecte(botLow);
        // Ordre : la cause la plus precise d'abord (un joueur deconnecte voit aussi ses inscriptions levees a la
        // meme seconde, `joueur_hors_ligne` : sans cet ordre, la ligne dirait `fin`).
        if (!option)
            raison = "option_eteinte";
        else if (!JoueurConnecte(e.joueur))
            raison = "joueur_hors_ligne";
        else if (!bot)
            raison = "bot_hors_ligne";
        else if (!bot->GetGroup() || bot->GetGroup()->GetGUID().GetCounter() != e.groupe)
            raison = "plus_dans_le_groupe";   // renvoye par le joueur, groupe dissous par lui : rien a retirer
        else if (!joueursServis.count(e.joueur))
            raison = "fin";
        else if (maintenant > e.vuS + GROUPE_SANS_EVENEMENT_S)
            raison = "sans_evenement";   // service mort sans ses levees : l'inscription tient, le groupe non
        if (raison)
            sorties.emplace_back(botLow, raison);
    }
    for (auto const& [botLow, raison] : sorties)
    {
        auto const it = groupesEvenement.find(botLow);
        if (it == groupesEvenement.end())
            continue;
        GroupeDEvenement const e = it->second;
        groupesEvenement.erase(it);
        // Plus dans le groupe : on ne retire personne (QuitterGroupeDEvenement ne retire qu'un MEMBRE, et le groupe
        // a pu etre dissous : GetGroupByGUID rend alors nul).
        QuitterGroupeDEvenement(botLow, e, raison);
    }

    // 2. Oubli des essais dont l'inscription n'est plus active (borne la memoire, une ligne par inscription).
    for (auto it = groupesTentes.begin(); it != groupesTentes.end();)
    {
        if (!idsActifs.count(*it))
            it = groupesTentes.erase(it);
        else
            ++it;
    }
    if (!option)
        return;

    // 3. Entrees : une tentative par inscription arrivee.
    for (Candidat const& c : candidats)
    {
        if (groupesEvenement.count(c.bot) || !groupesTentes.insert(c.id).second)
            continue;
        RejoindreGroupeDEvenement(c.bot, c.joueur, c.id);
    }
}

// Arret du serveur : les groupes sont gardes en base ; sans cela, un bot aleatoire resterait au groupe du joueur a la
// reconnexion, maitre humain compris, sans que rien ne l'en sorte. Fil monde (OnShutdown).
void PurgerGroupesEvenement(char const* raison)
{
    std::unordered_map<uint32, GroupeDEvenement> copie;
    copie.swap(groupesEvenement);
    groupesTentes.clear();
    for (auto const& [botLow, e] : copie)
        QuitterGroupeDEvenement(botLow, e, raison);
}
