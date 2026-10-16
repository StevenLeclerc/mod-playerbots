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

// ---- VERSION 2 (13/10/2026 au soir, retour du commanditaire apres le premier banc : « c'est trop mou ») -------------
// Trois ajouts, tous derriere une SECONDE option, AiPlayerbot.Directeur.EvenementConvoiV2 (lue a chaud par sConfigMgr ;
// defaut 0 : refus `convoi_v2_eteint`, et le lot V d'avant joue a l'identique) :
//   ordre:escorter:<vitesse>  le protege COURT (FORCED_MOVEMENT_RUN) vers le point, a <vitesse> % de la course de base
//             (7 yd/s : 100 -> 7 yd/s, 140 -> 9,8 yd/s), EV_CONVOI_VITESSE_MIN a _MAX, trois chiffres, sans zero de tete.
//             `ordre:escorter` (sans vitesse) reste la MARCHE du lot V. Le protege qui court publie son etat toutes les
//             EV_CONVOI_ETAT_RAPIDE_S secondes (le service anticipe le point suivant sur cette position) ;
//   barricade  un gameobject EV_BARRICADE_ENTREE (GOOBER, migration coa-project
//             exploitation/directeur/sql/2026_10_13_23_evenement_convoi_v2.sql) pose EN TRAVERS de la route : cible = le
//             joueur, valeur = la commande `invoquer` du PROTEGE (vivant, de ce joueur : sinon `pas_protege` ; la
//             barricade lui fait face), duree_s = sa vie (EV_BARRICADE_VIE_MIN_S a _MAX_S), carte/x/y/z = le point
//             (PointApparition : portee, sol, lieu sans JcJ). Elle ne BLOQUE rien d'elle-meme : c'est le service qui
//             n'envoie pas le protege au-dela tant qu'elle tient. Accuse `execute` a la pose ; l'accuse FINAL `leve` dit
//             comment elle est tombee : `detruite_humain`, `detruite_bot`, ou sans destruction `echu`, `hors_ligne`,
//             `carte`, `disparu`, et les raisons de LeverEvenementsSi (`lever`, `arret`, `bus_eteint`...) ;
//   DEMOLIR   un HUMAIN vivant clique dessus (crochet CanGameObjectGossipHello, comme le portail : file sous verrou, tout
//             le reste au fil monde) a EV_BARRICADE_CLIC_YD au plus : il la demolit en EV_BARRICADE_DEMOLITION_S secondes
//             (il frappe : emote d'attaque chaque seconde) s'il reste vivant a portee ; sinon la demolition s'interrompt
//             (un nouveau clic la reprend). Un BOT la demolit aussi (un convoi entre bots ne reste pas bloque) : un bot
//             vivant, HORS COMBAT, a EV_BARRICADE_BOT_YD, quand plus AUCUN invoque vivant du joueur (la garde) n'est a
//             EV_BARRICADE_GARDE_YD d'elle, pendant EV_BARRICADE_BOT_S secondes d'affilee.
// Journal : `coa directeur evenement: id= sous_action=barricade joueur= valeur= motif=<pose|demolition|interrompue|
// detruite|refus|...>`.
constexpr char const EV_CONVOI_V2_OPTION[] = "AiPlayerbot.Directeur.EvenementConvoiV2";
constexpr char const EV_CONVOI_ESCORTER_RAPIDE[] = "ordre:escorter:";
constexpr uint32 EV_CONVOI_VITESSE_MIN = 100;
constexpr uint32 EV_CONVOI_VITESSE_MAX = 200;
constexpr uint32 EV_CONVOI_ETAT_RAPIDE_S = 2;
constexpr char const EV_BARRICADE_TEXTE[] = "barricade";
constexpr uint32 EV_BARRICADE_ENTREE = 9100027;        // gameobject_template (plage 9100020-9100029, grimoire § 5.5)
constexpr uint32 EV_BARRICADE_VIE_MIN_S = 60;
constexpr uint32 EV_BARRICADE_VIE_MAX_S = 1800;
constexpr uint32 EV_BARRICADE_FILET_S = 30;            // l'objet s'efface seul 30 s apres l'echeance (registre perdu)
constexpr size_t EV_BARRICADES_MAX = 16;               // tout le royaume
constexpr size_t EV_BARRICADES_PAR_JOUEUR = 2;
constexpr float EV_BARRICADE_CLIC_YD = 10.0f;          // le paquet est deja borne a GetInteractionDistance
constexpr uint32 EV_BARRICADE_DEMOLITION_S = 3;
constexpr float EV_BARRICADE_BOT_YD = 25.0f;
constexpr uint32 EV_BARRICADE_BOT_S = 8;
constexpr float EV_BARRICADE_GARDE_YD = 35.0f;            // les caches sont a 28 yd au plus (+ 4 de dispersion)
constexpr size_t EV_BARRICADE_CLICS_FILE_MAX = 64;

bool ConvoiV2Allume()
{
    return sConfigMgr->GetOption<bool>(EV_CONVOI_V2_OPTION, false, false);
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
    // V2 : `ordre:escorter:<vitesse>`, trois chiffres sans zero de tete, EV_CONVOI_VITESSE_MIN a _MAX.
    size_t const nr = sizeof(EV_CONVOI_ESCORTER_RAPIDE) - 1;
    if (t.size() > nr && t.compare(0, nr, EV_CONVOI_ESCORTER_RAPIDE) == 0)
    {
        if (t.size() != nr + 3 || t[nr] == '0')
            return false;
        uint32 v = 0;
        for (size_t i = nr; i < t.size(); ++i)
        {
            if (t[i] < '0' || t[i] > '9')
                return false;
            v = v * 10 + uint32(t[i] - '0');
        }
        if (v < EV_CONVOI_VITESSE_MIN || v > EV_CONVOI_VITESSE_MAX)
            return false;
        lu.sous = EvApp::Ordre;
        lu.ordre = EvOrdre::Escorter;
        lu.vitesse = static_cast<uint16>(v);
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

// Le protege vivant du lot `id` du joueur `joueur`, ou nullptr ; `entree` (V2) : son entree au registre.
Creature* ProtegeDuLot(uint32 id, uint32 joueur, EvInvoque** entree = nullptr)
{
    for (EvInvoque& v : evInvoques)
        if (v.id == id && v.joueur == joueur && v.protege && !v.mort)
            if (Creature* cr = CreatureInvoquee(v))
                if (cr->IsAlive())
                {
                    if (entree)
                        *entree = &v;
                    return cr;
                }
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
        // V2 : la course exige la seconde option (jugee AVANT le point : ce refus ne depend pas du lieu).
        if (lu.vitesse && !ConvoiV2Allume())
        {
            RefuserApparition(c, lu, "convoi_v2_eteint");
            return;
        }
        if (!PointApparition(c, p, sol, raison))
        {
            RefuserApparition(c, lu, raison);
            return;
        }
        EvInvoque* entree = nullptr;
        Creature* cr = ProtegeDuLot(vise, c.cible, &entree);
        if (!cr)
        {
            RefuserApparition(c, lu, "pas_protege");
            return;
        }
        cr->SetReactState(REACT_PASSIVE);
        if (lu.vitesse)
        {
            // V2 : il COURT, a lu.vitesse % de la course de base (Unit::GetSpeed : taux x 7 yd/s), envoye au client.
            // La vitesse « normale » du moment d'abord (Unit::UpdateSpeed : gabarit, ralentissements comme le Hamstring
            // des bandits, PV sous 30 %), rapportee au gabarit : sans aure ni blessure, le taux vaut lu.vitesse / 100 ;
            // ralenti ou blesse, il court d'autant moins. Un UpdateSpeed ulterieur (aure posee ou retiree) revient a la
            // vitesse du gabarit jusqu'au `escorter` suivant (un point toutes les 4 s environ).
            cr->SetWalk(false);
            cr->UpdateSpeed(MOVE_RUN, false);
            CreatureTemplate const* gabaritP = cr->GetCreatureTemplate();
            float const base = gabaritP && gabaritP->speed_run > 0.0f ? gabaritP->speed_run : 1.0f;
            cr->SetSpeed(MOVE_RUN, cr->GetSpeedRate(MOVE_RUN) / base * float(lu.vitesse) / 100.0f, true);
        }
        else
            cr->SetWalk(true);
        if (entree)
            entree->rapide = lu.vitesse != 0;
        cr->SetHomePosition(c.x, c.y, sol, cr->GetOrientation());
        MotionMaster* mm = cr->GetMotionMaster();
        mm->Clear();
        mm->MovePoint(0, c.x, c.y, sol, lu.vitesse ? FORCED_MOVEMENT_RUN : FORCED_MOVEMENT_WALK);
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
        LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=ordre ordre=escorter joueur={} valeur={}"
                 " motif=pose combat={} x={:.1f} y={:.1f} z={:.1f} vitesse={}", c.id, c.cible, vise,
                 cr->IsInCombat() ? 1 : 0, c.x, c.y, sol, lu.vitesse);
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
        // L'immunite aux PNJ doit tomber AVANT le test (Unit::_IsValidAttackTarget la juge) ; relecture : un assaillant
        // qui ne peut pas frapper la reprend et n'est pas arme (sinon agressif et frappable par les gardes du monde).
        cr->SetImmuneToNPC(false);
        if (!cr->IsValidAttackTarget(cible))
        {
            cr->SetImmuneToNPC(true);
            continue;
        }
        v.attaqueS = 0;                  // garde des egares desarmee (comme charger_vers)
        v.egareDepuisS = 0;
        v.assailli = cible->GetGUID();   // revue 16/10 : l'immunite sera rendue quand ce protege ne sera plus
        cr->SetReactState(REACT_AGGRESSIVE);
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
    if (v.etatConvoiS && maintenant - v.etatConvoiS < (v.rapide ? EV_CONVOI_ETAT_RAPIDE_S : EV_CONVOI_ETAT_S))
        return;
    v.etatConvoiS = std::max<uint32>(maintenant, 1);
    JournaliserProtege(v, cr, "etat");
}

// Revue 16/10 : SuivreInvoques, chaque seconde. `assaillir` retire IMMUNE_TO_NPC a ses bandits (sinon ils ne peuvent
// pas frapper le protege) ; rien ne le rendait : apres la mort du convoi (ou sa victoire), ils restaient frappables et
// agressifs envers les PNJ du monde (gardes, donneurs de quete -- faction 14, la raison de la migration 2026_10_10_01)
// jusqu'au retrait hors de vue (300 s au plus) ou sans fin si le service ne clot pas. Protege mort, absent du registre,
// ou retrait demande : l'immunite est rendue. Unit::SetImmuneToNPC(true) clot les combats contre des PNJ et GARDE ceux
// contre les joueurs (refs PvE dont l'autre n'est pas PLAYER_CONTROLLED) : un bandit aux prises avec le joueur continue.
void SuivreAssaillant(EvInvoque& v, Creature* cr)
{
    if (v.assailli.IsEmpty() || v.mort || !cr || !cr->IsAlive())
        return;
    bool const protegeVivant = std::any_of(evInvoques.begin(), evInvoques.end(), [&v](EvInvoque const& p)
                                           { return p.protege && !p.mort && p.joueur == v.joueur && p.guid == v.assailli; });
    if (protegeVivant && !v.retraitDepuisS)
        return;
    v.assailli = ObjectGuid::Empty;
    cr->SetImmuneToNPC(true);
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=ordre ordre=assaillir joueur={} valeur={}"
             " motif=immunite_rendue raison={} combat={}", v.id, v.joueur, v.entree, protegeVivant ? "retrait" : "protege_fini",
             cr->IsInCombat() ? 1 : 0);
}

// ------------------------------------------------------------------ V2 : la BARRICADE (voir l'en-tete du fichier)

struct EvBarricade
{
    uint32 id;                   // commande `evenement` barricade
    uint32 joueur;               // le joueur de l'evenement
    uint32 protege;              // la commande `invoquer` du protege (valeur)
    ObjectGuid guid;             // l'objet pose
    uint32 carte;
    uint32 finS;                 // Maintenant() de l'echeance
    uint32 demolisseur = 0;      // guid bas de l'humain qui la demolit ; 0 : personne
    uint32 demolitionS = 0;      // Maintenant() du debut de sa demolition
    uint32 botDepuisS = 0;       // Maintenant() du debut de la demolition par un bot ; 0 : aucune
    uint32 clicsRefuses = 0;     // clics refuses (pas humain, mort, trop loin...), dits a la fermeture
};
std::vector<EvBarricade> evBarricades;                 // fil monde seul

std::mutex evClicsBarricadeVerrou;                     // le SEUL verrou de la barricade : la file ci-dessous
std::vector<EvClic> evClicsBarricade;                  // sous evClicsBarricadeVerrou (EvClic : CoaDirecteurPortail.inc)
uint32 evClicsBarricadePerdus = 0;                     // sous evClicsBarricadeVerrou : file pleine

bool EstBarricade(std::string const& t)
{
    return t == EV_BARRICADE_TEXTE;
}

GameObject* ObjetBarricade(EvBarricade const& b)
{
    Map* map = sMapMgr->FindBaseNonInstanceMap(b.carte);
    return map ? map->GetGameObject(b.guid) : nullptr;
}

void RefuserBarricade(CoaDirCommande const& c, char const* raison)
{
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=barricade joueur={} valeur={} motif=refus"
             " raison={}", c.id, c.cible, c.valeur, raison);
    Refuser(c, raison);
}

void ExecuterBarricade(CoaDirCommande const& c)
{
    if (!TexteBusValide(c.texte) || !EstBarricade(c.texte))
    {
        RefuserBarricade(c, "texte");
        return;
    }
    if (!CibleJoueurValide(c) || c.valeur <= 0 || c.dureeS < EV_BARRICADE_VIE_MIN_S || c.dureeS > EV_BARRICADE_VIE_MAX_S)
    {
        RefuserBarricade(c, "bornes");
        return;
    }
    if (!ConvoiAllume() || !ConvoiV2Allume())
    {
        RefuserBarricade(c, "convoi_v2_eteint");
        return;
    }
    Player* p = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!p)
    {
        RefuserBarricade(c, "hors_ligne");
        return;
    }
    if (!Humain(p))                      // le convoi n'a qu'une ancre humaine (le service refuse --ancre-bot)
    {
        RefuserBarricade(c, "pas_humain");
        return;
    }
    if (!p->IsInWorld() || p->IsBeingTeleported() || !CarteDeSuspension(p->GetMapId()))
    {
        RefuserBarricade(c, "carte");
        return;
    }
    GameObjectTemplate const* gabarit = sObjectMgr->GetGameObjectTemplate(EV_BARRICADE_ENTREE);
    if (!gabarit || gabarit->type != GAMEOBJECT_TYPE_GOOBER)
    {
        RefuserBarricade(c, "gabarit");   // migration 2026_10_13_23 absente, ou appliquee apres le demarrage
        return;
    }
    Creature* protege = ProtegeDuLot(static_cast<uint32>(c.valeur), c.cible);
    if (!protege)
    {
        RefuserBarricade(c, "pas_protege");
        return;
    }
    float sol = 0.0f;
    char const* raison = nullptr;
    if (!PointApparition(c, p, sol, raison))
    {
        RefuserBarricade(c, raison);
        return;
    }
    size_t const siennes = static_cast<size_t>(std::count_if(evBarricades.begin(), evBarricades.end(),
                                                             [&c](EvBarricade const& x) { return x.joueur == c.cible; }));
    if (siennes >= EV_BARRICADES_PAR_JOUEUR || evBarricades.size() >= EV_BARRICADES_MAX)
    {
        RefuserBarricade(c, "plein");
        return;
    }
    // En travers de la route : face au protege (qui arrive par la route). Quaternion d'une rotation autour de z.
    float const face = std::atan2(protege->GetPositionY() - c.y, protege->GetPositionX() - c.x);
    GameObject* go = p->GetMap()->SummonGameObject(EV_BARRICADE_ENTREE, c.x, c.y, sol, face, 0.0f, 0.0f,
                                                   std::sin(face / 2.0f), std::cos(face / 2.0f),
                                                   c.dureeS + EV_BARRICADE_FILET_S);
    if (!go)
    {
        RefuserBarricade(c, "invocation");
        return;
    }
    EvBarricade b;
    b.id = c.id;
    b.joueur = c.cible;
    b.protege = static_cast<uint32>(c.valeur);
    b.guid = go->GetGUID();
    b.carte = p->GetMapId();
    b.finS = Maintenant() + c.dureeS;
    evBarricades.push_back(b);
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=barricade joueur={} valeur={} motif=pose"
             " duree_s={} x={:.1f} y={:.1f} z={:.1f}", c.id, c.cible, c.valeur, c.dureeS, c.x, c.y, sol);
    Accuser(c, "execute", nullptr);
}

// La barricade tombe (ou part sans tomber) : objet retire tout de suite (RetirerObjetPortail, CoaDirecteurPortail.inc :
// SetRespawnTime(0) puis DespawnOrUnsummon), accuse final `leve` avec la raison.
void FermerBarricade(EvBarricade const& b, char const* motif, uint32 par)
{
    RetirerObjetPortail(ObjetBarricade(b));
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=barricade joueur={} valeur={} motif={} par={}"
             " clics_refuses={}", b.id, b.joueur, b.protege, motif, par, b.clicsRefuses);
    CloreEvenement(b.id, b.joueur, "leve", motif);
}

// La garde est-elle tombee ? Aucun invoque VIVANT du joueur (protege excepte) a EV_BARRICADE_GARDE_YD de l'objet.
bool GardeTombee(EvBarricade const& b, GameObject* go)
{
    for (EvInvoque const& v : evInvoques)
    {
        if (v.joueur != b.joueur || v.mort || v.protege)
            continue;
        Creature* cr = CreatureInvoquee(v);
        if (cr && cr->IsAlive() && cr->GetMapId() == go->GetMapId() && cr->GetExactDist(go) <= EV_BARRICADE_GARDE_YD)
            return false;
    }
    return true;
}

// Un BOT capable de la demolir : vivant, en monde, hors combat, a EV_BARRICADE_BOT_YD. Le plus proche, ou nullptr.
Player* BotDemolisseur(GameObject* go)
{
    Player* meilleur = nullptr;
    float meilleurD = EV_BARRICADE_BOT_YD;
    for (auto const& ref : go->GetMap()->GetPlayers())
    {
        Player* q = ref.GetSource();
        if (!q || Humain(q) || !GET_PLAYERBOT_AI(q) || !q->IsInWorld() || q->IsBeingTeleported() || !q->IsAlive() ||
            q->IsInCombat())
            continue;
        float const d = q->GetExactDist(go);
        if (d <= meilleurD)
        {
            meilleur = q;
            meilleurD = d;
        }
    }
    return meilleur;
}

// Chaque seconde (Entretien, bus allume ou eteint) : les clics, puis chaque barricade (demolition humaine ou par un
// bot, echeance, joueur parti, objet disparu).
void SuivreBarricades()
{
    std::vector<EvClic> clics;
    uint32 perdus = 0;
    {
        std::lock_guard<std::mutex> garde(evClicsBarricadeVerrou);
        clics.swap(evClicsBarricade);
        std::swap(perdus, evClicsBarricadePerdus);
    }
    if (perdus)
        LOG_INFO("playerbots", "coa directeur evenement: sous_action=barricade motif=clics_perdus perdus={}", perdus);
    if (evBarricades.empty())
        return;
    uint32 const maintenant = std::max<uint32>(Maintenant(), 1);
    // 1. Les clics : un humain vivant a portee COMMENCE la demolition (un autre deja en cours : il la reprend).
    for (EvClic const& k : clics)
    {
        auto it = std::find_if(evBarricades.begin(), evBarricades.end(),
                               [&k](EvBarricade const& x) { return x.guid == k.portail; });
        if (it == evBarricades.end())
            continue;
        Player* q = JoueurConnecte(k.joueur);
        GameObject* go = ObjetBarricade(*it);
        char const* refus = !q || !Humain(q)                                                       ? "pas_humain"
                            : !q->IsInWorld() || q->IsBeingTeleported() || q->GetMapId() != it->carte ? "carte"
                            : !q->IsAlive()                                                        ? "mort"
                            : !go                                                                  ? "disparu"
                            : q->GetExactDist(go) > EV_BARRICADE_CLIC_YD                           ? "distance"
                                                                                                   : nullptr;
        if (refus)
        {
            ++it->clicsRefuses;
            continue;
        }
        if (it->demolisseur == k.joueur)
            continue;                    // deja a l'ouvrage
        it->demolisseur = k.joueur;
        it->demolitionS = maintenant;
        q->HandleEmoteCommand(EMOTE_ONESHOT_ATTACK1H);
        LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=barricade joueur={} valeur={}"
                 " motif=demolition par={} humain=1", it->id, it->joueur, it->protege, k.joueur);
    }
    // 2. Chaque barricade.
    struct Fin
    {
        EvBarricade b;
        char const* motif;
        uint32 par;
    };
    std::vector<Fin> fins;
    for (auto it = evBarricades.begin(); it != evBarricades.end();)
    {
        EvBarricade& b = *it;
        Player* p = JoueurConnecte(b.joueur);
        GameObject* go = ObjetBarricade(b);
        char const* motif = !p                                                                           ? "hors_ligne"
                            : (p->IsInWorld() && !p->IsBeingTeleported() && p->GetMapId() != b.carte) ? "carte"
                            : b.finS <= maintenant                                                     ? "echu"
                            : !go                                                                      ? "disparu"
                                                                                                       : nullptr;
        uint32 par = 0;
        if (!motif && b.demolisseur)
        {
            Player* q = JoueurConnecte(b.demolisseur);
            if (!q || !q->IsInWorld() || q->IsBeingTeleported() || !q->IsAlive() || q->GetMapId() != b.carte ||
                q->GetExactDist(go) > EV_BARRICADE_CLIC_YD)
            {
                LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=barricade joueur={} valeur={}"
                         " motif=interrompue par={}", b.id, b.joueur, b.protege, b.demolisseur);
                b.demolisseur = 0;
                b.demolitionS = 0;
            }
            else if (maintenant - b.demolitionS >= EV_BARRICADE_DEMOLITION_S)
            {
                motif = "detruite_humain";
                par = b.demolisseur;
            }
            else
                q->HandleEmoteCommand(EMOTE_ONESHOT_ATTACK1H);
        }
        if (!motif && !b.demolisseur)
        {
            Player* bot = GardeTombee(b, go) ? BotDemolisseur(go) : nullptr;
            if (!bot)
                b.botDepuisS = 0;
            else
            {
                if (!b.botDepuisS)
                {
                    b.botDepuisS = maintenant;
                    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=barricade joueur={} valeur={}"
                             " motif=demolition par={} humain=0", b.id, b.joueur, b.protege,
                             bot->GetGUID().GetCounter());
                }
                bot->SetFacingToObject(go);
                bot->HandleEmoteCommand(EMOTE_ONESHOT_ATTACK1H);
                if (maintenant - b.botDepuisS >= EV_BARRICADE_BOT_S)
                {
                    motif = "detruite_bot";
                    par = bot->GetGUID().GetCounter();
                }
            }
        }
        if (motif)
        {
            fins.push_back(Fin{b, motif, par});
            it = evBarricades.erase(it);
        }
        else
            ++it;
    }
    for (Fin const& f : fins)
        FermerBarricade(f.b, f.motif, f.par);
}

// Leve (sans destruction) les barricades pour lesquelles `lever(id, joueur)` est vrai : lever, stop, bus eteint.
template <typename Predicat>
size_t LeverBarricadesSi(Predicat lever, char const* raison)
{
    std::vector<EvBarricade> parties;
    for (auto it = evBarricades.begin(); it != evBarricades.end();)
    {
        if (lever(it->id, it->joueur))
        {
            parties.push_back(*it);
            it = evBarricades.erase(it);
        }
        else
            ++it;
    }
    for (EvBarricade const& b : parties)
        FermerBarricade(b, raison, 0);
    return parties.size();
}

// Le crochet de CLIC (fil quelconque, comme celui du portail) : n'ecrit que la file sous verrou ; vrai pour NOTRE objet
// seulement (GameObject::Use s'arrete la), faux pour tout autre.
class coa_directeur_barricade_goscript : public AllGameObjectScript
{
public:
    coa_directeur_barricade_goscript() : AllGameObjectScript("coa_directeur_barricade_goscript") {}

    bool CanGameObjectGossipHello(Player* player, GameObject* go) override
    {
        if (!player || !go || go->GetEntry() != EV_BARRICADE_ENTREE)
            return false;
        std::lock_guard<std::mutex> garde(evClicsBarricadeVerrou);
        if (evClicsBarricade.size() >= EV_BARRICADE_CLICS_FILE_MAX)
            ++evClicsBarricadePerdus;
        else
            evClicsBarricade.push_back(EvClic{go->GetGUID(), player->GetGUID().GetCounter()});
        return true;
    }
};
