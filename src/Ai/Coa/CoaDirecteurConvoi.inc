// CoA -- le Directeur, LOT V des evenements (13/10/2026) : le CONVOI DE BANDITS (fragment de CoaDirecteur.cpp, inclus
// apres CoaDirecteurBoss.inc, dans le meme espace de noms anonyme). Conception : coa-project
// docs/CONCEPTION-convoi-bandits.md ; service : exploitation/directeur/convoi_bandits.py ; epreuve :
// outils/test-directeur-convoi.py (grammaire, table des proteges et garde de l'option relues ici).
//
// CE QUE LE JEU NE SAVAIT PAS FAIRE (lu le 13/10 dans CoaDirecteur.cpp) : deplacer un invoque le long d'une route au pas
// (seul `charger_vers` existe : en COURSE, hors combat seulement, et il arme l'agressivite), garder un invoque PASSIF,
// lancer des invoques contre une CREATURE (tous portent UNIT_FLAG_IMMUNE_TO_NPC, migration 2026_10_10_01, et `attaquer`
// ne vise que le joueur), ni dire au service ou est un invoque et ce qu'il lui reste de PV. D'ou, ici seulement :
//
//   PROTEGE   un gabarit de la table FERMEE EV_CONVOI_PROTEGES (la mule, le chariot, le kodo du convoi), invoque par
//             `invoquer` comme les autres (nombre 1, option EvenementConvoi) : a la pose, REACT_PASSIVE, au pas, PAS
//             d'IMMUNE_TO_NPC (les bandits peuvent le frapper ; sa faction, 250, est celle du gabarit) ; il n'est
//             JAMAIS une cible publiee aux bots inscrits (PublierCiblesEvenement), jamais touche par `attaquer`,
//             `tenir`, `errer`, `charger_vers` (ExecuterOrdre le saute), jamais egare. Chaque seconde : reaction
//             remise a PASSIVE (un `attaquer` de relance l'aurait rendue agressive) ; toutes les EV_CONVOI_ETAT_S
//             secondes, une ligne `sous_action=convoi motif=etat pv= x= y= z= combat= assaillants=` ; a sa mort, TOUT
//             DE SUITE, `motif=mort` (le lot, lui, ne finit qu'a la passe suivante).
//   ordre:escorter            valeur = la commande `invoquer` du protege ; carte/x/y/z = le point suivant de la route
//             (memes controles de point que `charger_vers` : PointApparition) ; il y MARCHE (FORCED_MOVEMENT_WALK),
//             MEME EN COMBAT (un convoi attaque n'est pas un combat qu'il mene). Refus `pas_protege` si le lot n'en est
//             pas un ;
//   ordre:assaillir:<protege> valeur = la commande `invoquer` des assaillants (0 : tous ceux du joueur) ; chacun, vivant
//             et non protege, perd IMMUNE_TO_NPC (SetImmuneToNPC(false)), passe AGRESSIF et attaque le protege
//             (AttackStart) ; sa garde des egares est desarmee, comme pour charger_vers (le service le relance au besoin).
//             Refus `pas_protege` si <protege> n'est pas un lot de protege vivant de CE joueur.
// Option : AiPlayerbot.Directeur.EvenementConvoi (lue a chaud par sConfigMgr, comme EvenementGardien ; defaut 0 :
// invoquer un protege, escorter et assaillir sont refuses `convoi_eteint`). Recompense : `intensite` = 4 marque le
// courrier « Le convoi des Sept Routes » (ExecuterRecompense).

constexpr char const EV_CONVOI_OPTION[] = "AiPlayerbot.Directeur.EvenementConvoi";
// Table FERMEE des proteges (coa-project exploitation/directeur/convoi.py, role `protege` ; l'epreuve compare).
constexpr uint32 EV_CONVOI_PROTEGES[] = {9100016, 9100036, 9100046};
constexpr uint32 EV_CONVOI_ETAT_S = 5;
constexpr char const EV_CONVOI_ESCORTER[] = "ordre:escorter";
constexpr char const EV_CONVOI_ASSAILLIR[] = "ordre:assaillir:";

bool ConvoiAllume()
{
    return sConfigMgr->GetOption<bool>(EV_CONVOI_OPTION, false, false);
}

bool EntreeProtege(uint32 entree)
{
    for (uint32 e : EV_CONVOI_PROTEGES)
        if (e == entree)
            return true;
    return false;
}

// Grammaire FERMEE du lot V (casse exacte). L'id d'`assaillir` : decimal, sans signe ni zero de tete, 1 a 2^31 - 1.
bool LireOrdreConvoi(std::string const& t, EvAppLu& lu)
{
    if (t == EV_CONVOI_ESCORTER)
    {
        lu.sous = EvApp::Ordre;
        lu.ordre = EvOrdre::Escorter;
        return true;
    }
    size_t const n = sizeof(EV_CONVOI_ASSAILLIR) - 1;
    if (t.size() <= n || t.size() > n + 10 || t.compare(0, n, EV_CONVOI_ASSAILLIR) != 0 || t[n] == '0')
        return false;
    uint64 id = 0;
    for (size_t i = n; i < t.size(); ++i)
    {
        if (t[i] < '0' || t[i] > '9')
            return false;
        id = id * 10 + uint64(t[i] - '0');
    }
    if (id == 0 || id > 0x7FFFFFFFull)
        return false;
    lu.sous = EvApp::Ordre;
    lu.ordre = EvOrdre::Assaillir;
    lu.protege = uint32(id);
    return true;
}

// ExecuterInvoquer, avant la pose : un protege exige l'option et nombre 1.
char const* RefusConvoi(EvAppLu const& lu, uint32 entree)
{
    if (!EntreeProtege(entree))
        return nullptr;
    if (!ConvoiAllume())
        return "convoi_eteint";
    if (lu.nombre != 1 || lu.etendue)
        return "liste_blanche_ou_bornes";
    return nullptr;
}

// ExecuterInvoquer, apres la pose (fil monde).
void ArmerProtege(Creature* cr, EvInvoque& v)
{
    if (!cr || !EntreeProtege(v.entree))
        return;
    v.protege = true;
    cr->SetReactState(REACT_PASSIVE);
    cr->SetImmuneToNPC(false);
    cr->SetWalk(true);
    cr->SetDefaultMovementType(IDLE_MOTION_TYPE);
}

// Le protege vivant du lot `id` du joueur `joueur`, ou nullptr.
Creature* ProtegeDuLot(uint32 id, uint32 joueur)
{
    for (EvInvoque const& v : evInvoques)
        if (v.id == id && v.joueur == joueur && v.protege && !v.mort)
            if (Creature* cr = CreatureInvoquee(v))
                if (cr->IsAlive())
                    return cr;
    return nullptr;
}

// SuivreEgare : un invoque aux prises avec un PROTEGE de son joueur ne combat pas « un PNJ » (il fait ce qu'on lui a dit).
bool EstProtegeDe(Unit const* u, uint32 joueur)
{
    if (!u || u->GetTypeId() != TYPEID_UNIT)
        return false;
    for (EvInvoque const& v : evInvoques)
        if (v.protege && v.joueur == joueur && v.guid == u->GetGUID())
            return true;
    return false;
}

void ExecuterOrdreConvoi(CoaDirCommande const& c, EvAppLu const& lu, Player* p)
{
    if (!ConvoiAllume())
    {
        RefuserApparition(c, lu, "convoi_eteint");
        return;
    }
    uint32 const vise = static_cast<uint32>(c.valeur);
    if (lu.ordre == EvOrdre::Escorter)
    {
        float sol = 0.0f;
        char const* raison = nullptr;
        if (!vise)
        {
            RefuserApparition(c, lu, "liste_blanche_ou_bornes");
            return;
        }
        if (!PointApparition(c, p, sol, raison))
        {
            RefuserApparition(c, lu, raison);
            return;
        }
        Creature* cr = ProtegeDuLot(vise, c.cible);
        if (!cr)
        {
            RefuserApparition(c, lu, "pas_protege");
            return;
        }
        cr->SetReactState(REACT_PASSIVE);
        cr->SetWalk(true);
        cr->SetHomePosition(c.x, c.y, sol, cr->GetOrientation());
        MotionMaster* mm = cr->GetMotionMaster();
        mm->Clear();
        mm->MovePoint(0, c.x, c.y, sol, FORCED_MOVEMENT_WALK);
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
        LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=ordre ordre=escorter joueur={} valeur={}"
                 " motif=pose combat={} x={:.1f} y={:.1f} z={:.1f}", c.id, c.cible, vise, cr->IsInCombat() ? 1 : 0,
                 c.x, c.y, sol);
        Accuser(c, "execute", nullptr);
        return;
    }
    // Assaillir.
    Creature* cible = ProtegeDuLot(lu.protege, c.cible);
    if (!cible)
    {
        RefuserApparition(c, lu, "pas_protege");
        return;
    }
    uint32 vises = 0, appliques = 0;
    for (EvInvoque& v : evInvoques)
    {
        if (v.joueur != c.cible || v.mort || v.protege || (vise && v.id != vise))
            continue;
        ++vises;
        Creature* cr = CreatureInvoquee(v);
        if (!cr || !cr->IsAlive() || !cr->AI() || cr->GetMapId() != cible->GetMapId())
            continue;
        v.attaqueS = 0;                  // garde des egares desarmee (comme charger_vers)
        v.egareDepuisS = 0;
        cr->SetImmuneToNPC(false);
        cr->SetReactState(REACT_AGGRESSIVE);
        if (!cr->IsValidAttackTarget(cible))
            continue;
        cr->AI()->AttackStart(cible);
        ++appliques;
    }
    if (!vises)
    {
        RefuserApparition(c, lu, "aucun_invoque");
        return;
    }
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=ordre ordre=assaillir joueur={} valeur={}"
             " protege={} motif=pose vises={} appliques={}", c.id, c.cible, vise, lu.protege, vises, appliques);
    Accuser(c, "execute", appliques ? nullptr : "aucun_applique");
}

void JournaliserProtege(EvInvoque const& v, Creature* cr, char const* motif)
{
    uint32 assaillants = 0;
    if (cr)
        assaillants = static_cast<uint32>(cr->getAttackers().size());
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=convoi joueur={} valeur={} motif={} pv={}"
             " combat={} assaillants={} x={:.1f} y={:.1f} z={:.1f}", v.id, v.joueur, v.entree, motif,
             cr && cr->IsAlive() ? uint32(cr->GetHealthPct() + 0.5f) : 0u, cr && cr->IsInCombat() ? 1 : 0, assaillants,
             cr ? cr->GetPositionX() : 0.0f, cr ? cr->GetPositionY() : 0.0f, cr ? cr->GetPositionZ() : 0.0f);
}

// SuivreInvoques, chaque seconde, pour un protege VIVANT : reaction remise a PASSIVE, et l'etat toutes les
// EV_CONVOI_ETAT_S secondes.
void SuivreProtege(EvInvoque& v, Creature* cr, uint32 maintenant)
{
    if (!v.protege || v.mort || !cr || !cr->IsAlive())
        return;
    if (!cr->HasReactState(REACT_PASSIVE))
        cr->SetReactState(REACT_PASSIVE);
    if (v.etatConvoiS && maintenant - v.etatConvoiS < EV_CONVOI_ETAT_S)
        return;
    v.etatConvoiS = std::max<uint32>(maintenant, 1);
    JournaliserProtege(v, cr, "etat");
}
