// ------------------------------------------------------------------ lot D5 « l'imprevu », partie jeu (15/10/2026)
//
// FRAGMENT de CoaDirecteur.cpp : inclus par lui, DANS son espace de noms anonyme, juste AVANT CoaDirecteurSoin.inc
// (dont JoueurASoigner et le crochet OnHeal consultent le patient impose d'ici). Il n'est pas une unite de
// compilation. Specification : coa-project/docs/ETUDE-directeur-D5-imprevu.md § 3.1, § 3.2, § 3.5, § 3.7, § 5
// (lots D5b, D5d ; D5a reduit au champ 16 `soin` de `joueur,<guid>`, ecrit dans Joueur()), § 6.
// Decisions arretees : mode `combat` ABANDONNE (D5-0 : mediane des combats faciles 10,4 s < 35 s), seul `patrouille` ;
// creatures EXISTANTES seulement (n° 1) ; 3 au plus (n° 7) ; anti-traine 60 yd (n° 14) ; soigneur jamais pour un
// joueur FFA (High Risk) ni porteur du drapeau JcJ (n° 15).
//
// TOUT SE PASSE AU FIL MONDE (P-137), un seul ecrivain par etat (P-134) : la reception (Recevoir -> Executer*),
// le suivi de chaque seconde (Entretien -> SuivreAttires, SuivreSoigneurs), les levees (LeverImprevuSi : `lever`,
// `.directeur stop`, bus eteint, appelee par LeverEvenementsSi). Les fils de carte ne lisent que la copie publiee des
// soigneurs (atomiques, sans verrou), comme le lot S.
//
// ===== VERBE `attirer` (§ 3.1) =====
// Colonnes : cible 0, joueur = humain vise, valeur 1..3 (creatures), duree_s 30..120 (delai pour engager), rayon
// 120..180 (recherche), texte `patrouille` (le seul mode). Refus propres (apres ceux de Recevoir) : attirer_eteint,
// bornes, hors_ligne, pas_humain, mj, mort, combat, en_vol, champ_de_bataille, carte, defi, lieu_sans_pvp,
// humain_proche, occupe (le joueur a deja des creatures attirees), plein, aucune_creature.
// Choix (a la pose) : creatures VIVANTES, hors combat, rang normal (0), ni bestiole, ni civil, ni PNJ a fonction, ni
// familier/invocation/vehicule, ni creature d'evenement, ni deja active (setActive d'un autre : on ne le defait
// jamais), attaquables et non amicales au joueur, niveau MAXIMAL du gabarit <= niveau du joueur, entre 120 yd du
// joueur et `rayon`, aucun autre humain a 150 yd d'elle, aucun chasseur/inscrit a 60 yd, un CHEMIN (mmap) complet
// de longueur <= 1,5 fois la distance (au plus ATTIRER_CHEMINS_MAX calculs par pose, les plus proches d'abord).
// Cell::VisitObjects ne charge aucune grille (Map::Visit : grille non chargee -> rien).
// Mouvement : setActive(true) a la pose (sans quoi, loin de tout joueur, elle n'est pas mise a jour : Creature::
// IsUpdateNeeded) ; course hors combat, MovePoint sur le joueur RECALE chaque seconde ; a 25 yd, engagement si le
// niveau lu est hisse (>= joueur - 3, ou plus haut qu'a la pose), sinon une seconde de plus, 3 s au plus ;
// AI()->AttackStart(joueur). Une creature qui l'engage d'elle-meme (aggro) est engagee aussi.
// Laisse : Creature::UpdateLeashExtensionTime() chaque seconde tant qu'elle est engagee dans le registre.
// Borne anti-traine, chaque seconde : creature a plus de 60 yd du point d'engagement (`traine`) ; autre humain a
// 150 yd de la creature ou du joueur (`humain`) ; lieu sans JcJ, joueur ou creature (`sans_pvp`) ; autre carte
// (`carte`) ; chasseur de prime/contrat ou inscrit d'evenement a 60 yd de la creature (`chasseur`). Plus : `delai`
// (duree_s sans engagement), `hors_ligne`, `joueur_mort`, `defi`, `arret`, `option`, et les levees (`lever`, `arret`,
// `bus_eteint`). Toutes : EnterEvadeMode (la creature rentre au camp ; sa garde n'est jamais touchee).
// Fins sans evasion : `mort` (creature tuee), `fin_combat` (combat engage puis fini : elle rentre d'elle-meme),
// `detourne` (en combat contre un autre que le joueur pendant la course), `disparu` (plus sur la carte).
// RETOUR : apres une sortie, la creature vivante reste ACTIVE jusqu'a son camp (sinon, loin de tout joueur, elle
// se figerait a mi-chemin), ATTIRER_RETOUR_MAX_S au plus, puis setActive(false). `actifs` du bilan les compte.
// setActive(false) a CHAQUE sortie du registre ; jamais sur une creature introuvable (objet detruit).
// Redemarrage du monde : registre en memoire, perdu ; m_isActive n'est jamais persiste (LU Object.cpp:1061, :1086) :
// les creatures rechargees sont inactives.
//
// ===== VERBE `soigner` (§ 3.2) =====
// Colonnes : cible = bot soigneur, joueur = patient (humain), duree_s 30..420, valeur = seuil de PV 40..70.
// Refus propres : soigner_eteint, bornes, hors_ligne, pas_un_bot, maitre_humain, pas_humain, pas_soigneur
// (PlayerbotAI::IsHeal(bot) : une strategie de soin dans un de ses moteurs), sans_presence (le bot porte une presence
// ARRIVEE -- finale en sejour, non etape -- du meme proprietaire pour ce joueur : la regle des approches C-), ffa
// (High Risk ou drapeau FFA), pvp (drapeau JcJ), defi, mort, bot_mort, carte, faction (!IsValidAssistTarget), plein.
// Le patient n'est rendu qu'a un bot SANS groupe (etude § 4). Un `soigner` sur un bot deja soigneur
// REMPLACE le precedent (clos `remplace`).
// Effet : le registre est publie (copie atomique, SOIGNEURS_MAX places) ; JoueurASoigner (CoaDirecteurSoin.inc) rend
// le patient au bot vise -- c'est le crochet du lot S dans PartyMemberToHeal::Calculate et NearbyGroup -- s'il est
// en monde, vivant, humain, sans drapeau JcJ ni FFA, a SOIGNER_PORTEE_YD du bot, assistable (IsValidAssistTarget),
// et sous le seuil (ou, une fois tous deux en combat, quel que soit son niveau de PV : les seuils de soin d'origine
// decident). Deux prefiltres sans verrou, dans cet ordre : nbSoigneursPublies (un chargement atomique) puis le guid du
// bot dans la copie (au plus SOIGNEURS_MAX comparaisons). Un serveur sans soigneur de passage paie un chargement.
// L'EPREUVE « HORS COMBAT » (§ 3.2 point 1) -- LU : `coa heal` n'est que dans le moteur de COMBAT
// (AiFactory.cpp:428 ; le moteur hors combat des classes CoA porte « dps assist » et « coa buff », AiFactory.cpp
// ~l. 659) et le moteur de combat ne s'ouvre que par AttackAction / PullActions (ChangeEngine(BOT_STATE_COMBAT)).
// Le lot S ne fait donc PAS soigner un bot hors combat : ses inscrits sont deja aux prises avec les invoques. D'ou
// l'ASSISTANCE, chaque seconde au fil monde : patient en combat et sous le seuil, bot vivant, hors combat, moteur hors
// combat, a 40 yd du patient -> le bot engage la creature (jamais un joueur) qui frappe le patient la plus proche de
// lui, comme EngagerChasseur (selection, « current target », ChangeEngine(BOT_STATE_COMBAT), Attack) ; dans le moteur
// de combat, `coa heal` (ACTION_CRITICAL_HEAL et +) passe avant l'attaque. Au plus une assistance toutes les
// SOIGNER_ASSISTANCE_S par soigneur.
// Fins : `echeance` (naturelle, rien en base), `hors_ligne` (bot ou patient), `presence` (sa presence levee ou echue),
// `ffa`, `pvp`, `defi`, `arret`, `option`, `carte` (patient et bot sur deux cartes 30 s de suite), levees.
// Lignes `coa directeur soigner:`.

// ------------------------------------------------------------------ attirer

constexpr uint32 ATTIRER_N_MAX = 3;
constexpr uint32 ATTIRER_DUREE_MIN_S = 30;
constexpr uint32 ATTIRER_DUREE_MAX_S = 120;
constexpr float ATTIRER_RAYON_MIN_YD = 120.0f;
constexpr float ATTIRER_RAYON_MAX_YD = 180.0f;
constexpr float ATTIRER_DISTANCE_MIN_YD = 120.0f;    // 100 yd de visibilite + 20 de marge
constexpr float ATTIRER_ENGAGEMENT_YD = 25.0f;
constexpr uint32 ATTIRER_HISSAGE_MAX_S = 3;
constexpr uint32 ATTIRER_HISSAGE_ECART = 3;          // l'echelle hisse a joueur - 3 (LocalLevelScaling.h)
constexpr float ATTIRER_TRAINE_YD = 60.0f;           // decision n° 14
constexpr float ATTIRER_HUMAIN_YD = 150.0f;
constexpr float ATTIRER_CHASSEUR_YD = 60.0f;
constexpr float ATTIRER_CHEMIN_FACTEUR = 1.5f;
constexpr uint32 ATTIRER_CHEMINS_MAX = 8;
constexpr uint32 ATTIRER_RETOUR_MAX_S = 60;
constexpr float ATTIRER_CAMP_YD = 10.0f;             // « au camp » : a 10 yd de sa garde (GetHomePosition)
constexpr size_t ATTIRES_MAX = 9;                    // trois commandes de trois

enum class PhaseAttire : uint8
{
    Course,     // hors combat, vers le joueur
    Engage,     // aux prises avec le joueur : laisse prolongee, borne anti-traine
    Retour      // sortie du registre decidee : encore active jusqu'a son camp
};

struct Attire
{
    uint32 id = 0;           // commande
    uint32 joueur = 0;
    uint32 carte = 0;
    ObjectGuid guid;
    uint32 entree = 0;
    PhaseAttire phase = PhaseAttire::Course;
    uint32 limiteS = 0;      // Maintenant() au-dela duquel une creature non engagee rentre (`delai`)
    uint32 aPorteeS = 0;     // Maintenant() de la premiere seconde a ATTIRER_ENGAGEMENT_YD (attente du hissage)
    uint32 retourS = 0;      // Maintenant() de l'entree en Retour
    float engX = 0.0f, engY = 0.0f;   // point d'engagement (position du joueur)
    uint8 niveauPose = 0;
    bool banc = false;       // `joueur` est un bot (option Directeur.ImprevuBanc a la pose)
};

std::vector<Attire> attires;   // fil monde seul
uint64 nbAttiresPoses = 0;     // creatures posees (cumul)
uint64 nbAttiresEngages = 0;
uint64 nbAttiresRentres = 0;   // evasions decidees par le registre (cumul)

Creature* CreatureAttiree(Attire const& a)
{
    Map* map = sMapMgr->FindBaseNonInstanceMap(a.carte);
    return map ? map->GetCreature(a.guid) : nullptr;
}

bool CarteDeContinent(uint32 carte)
{
    return carte == 0 || carte == 1 || carte == 530 || carte == 571;
}

// Banc (option Directeur.ImprevuBanc, 15/10/2026) : `banc=1` EN FIN de ligne pour toute ligne qui vise un BOT.
void JournaliserAttirer(uint32 id, uint32 joueur, uint32 n, std::string const& creatures, char const* statut,
                        char const* raison, float dist, int32 niveau, std::string const& suite = std::string(),
                        bool banc = false)
{
    LOG_INFO("playerbots",
             "coa directeur attirer: id={} joueur={} mode=patrouille n={} creatures={} statut={} raison={} dist={} niveau={}{}{}",
             id, joueur, n, creatures.empty() ? std::string("-") : creatures, statut, raison ? raison : "-",
             dist < 0.0f ? std::string("-") : Acore::StringFormat("{:.0f}", dist),
             niveau < 0 ? std::string("-") : std::to_string(niveau), suite, banc ? " banc=1" : "");
}

// ---- banc avec les bots (15/10/2026, option AiPlayerbot.Directeur.ImprevuBanc, demande du commanditaire)
// Option allumee : un BOT ALEATOIRE en jeu (jamais l'altbot d'un compte humain : pas de maitre humain) est admis comme
// `joueur` d'`attirer` et comme patient de `soigner`, la ou un humain etait exige (`pas_humain`). Rien d'autre ne
// change : les autres refus restent, et `humain_proche` / `humain` ne comptent que les VRAIS humains
// (HumainProcheSauf parcourt RandomPlayerbotMgr::GetPlayers). Le bot vise n'est ni groupe, ni deplace, ni recompose :
// seules les creatures (attirer) et le bot soigneur (soigner) agissent. Fil monde seul (IsRandomBot, maitre).
// Option eteinte : Humain(p), a l'identique.
bool BotDeBanc(Player* p)
{
    if (!sPlayerbotAIConfig.directeurImprevuBanc || !p || !p->IsInWorld())
        return false;
    PlayerbotAI* ia = GET_PLAYERBOT_AI(p);
    // Revue de 662e243 (m3) : jamais un mercenaire (meme regle que AncreDEvenement ; FFA, il serait refuse `ffa`
    // de toute facon pour `soigner`, et sa chasse fausserait un banc `attirer`).
    return ia && !ia->HasGameClientMaster() && sRandomPlayerbotMgr.IsRandomBot(p) && !MercenaryRewards::EstMercenaire(p);
}

bool CibleImprevu(Player* p)
{
    return Humain(p) || BotDeBanc(p);
}

std::string NomAttire(Attire const& a)
{
    return Acore::StringFormat("{}:{}", a.entree, a.guid.GetCounter());
}

// Un chasseur de prime ou de contrat (affectation `chasser`) ou un inscrit d'evenement (presence, mission
// Evenement) a `yd` de `o` ? Fil monde (seul ecrivain des affectations : la lecture sous verrou partage est la
// regle commune). Au plus Directeur.MaxAffectations entrees.
bool ChasseurPres(WorldObject const* o, float yd)
{
    if (!nbAffectations.load(std::memory_order_relaxed))
        return false;
    std::vector<uint32> guids;
    {
        uint32 const maintenant = Maintenant();
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto const& [guid, f] : affectations)
            if (f.a.finS > maintenant && f.a.carte == o->GetMapId() &&
                (f.a.chasseur || (f.a.presence && f.a.mission == CoaDirecteur::CoaDirMission::Evenement)))
                guids.push_back(guid);
    }
    for (uint32 g : guids)
    {
        Player* b = JoueurConnecte(g);
        if (b && b->IsInWorld() && b->GetMapId() == o->GetMapId() && b->IsWithinDist2d(o, yd))
            return true;
    }
    return false;
}

bool LieuSansPvp(WorldObject const* o)
{
    return sPlayerbotAIConfig.CoaLieuSansPvp(o->GetZoneId(), o->GetAreaId());
}

// Etat du joueur qui interdit (pose) : la raison, ou nul.
char const* RefusJoueurAttirer(Player* j, uint32 guid)
{
    if (!j || !j->IsInWorld() || j->IsBeingTeleported())
        return "hors_ligne";
    if (!CibleImprevu(j))
        return "pas_humain";
    if (j->IsGameMaster())
        return "mj";
    if (!j->IsAlive())
        return "mort";
    if (j->IsInCombat())
        return "combat";
    if (j->IsInFlight())
        return "en_vol";
    if (j->InBattleground() || j->InArena())
        return "champ_de_bataille";
    if (!CarteDeContinent(j->GetMapId()) || j->GetMap()->Instanceable())
        return "carte";
    if (!defisLus || DefiBloquant(j))
        return "defi";
    if (LieuSansPvp(j))
        return "lieu_sans_pvp";
    if (HumainProcheSauf(WorldPosition(j), j->GetMapId(), ATTIRER_HUMAIN_YD, guid))
        return "humain_proche";
    return nullptr;
}

// Une creature est-elle deja dans le registre ?
bool DejaAttiree(ObjectGuid const& g)
{
    for (Attire const& a : attires)
        if (a.guid == g)
            return true;
    return false;
}

// Candidate a la pose (sans le chemin, calcule a part : c'est le plus cher).
bool CandidateAttiree(Creature* cr, Player* j, float rayon)
{
    if (!cr || !cr->IsInWorld() || !cr->IsAlive() || cr->IsInCombat() || cr->IsInEvadeMode() || cr->isActiveObject())
        return false;
    CreatureTemplate const* modele = cr->GetCreatureTemplate();
    if (!modele || modele->rank != CREATURE_ELITE_NORMAL || cr->isWorldBoss() || cr->IsDungeonBoss())
        return false;
    if (cr->IsCritter() || cr->IsCivilian() || cr->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
        return false;
    if (cr->IsPet() || cr->IsSummon() || cr->IsGuardian() || cr->IsVehicle() || cr->GetVehicle() ||
        cr->GetCharmerOrOwnerGUID() || cr->IsCharmed())
        return false;
    // Membre ou chef d'une formation (CreatureGroup) : un chef attire emmenerait ses suiveurs (plus de 3, et un
    // train) ; un suiveur serait rappele par son chef.
    if (cr->GetFormation())
        return false;
    if (CoaDirecteur::EstCreatureDEvenement(cr->GetEntry()) || DejaAttiree(cr->GetGUID()))
        return false;
    if (!cr->AI() || cr->IsFriendlyTo(j) || !cr->IsValidAttackTarget(j))
        return false;
    if (modele->maxlevel > j->GetLevel())
        return false;
    float const d = cr->GetExactDist2d(j);
    if (d < ATTIRER_DISTANCE_MIN_YD || d > rayon)
        return false;
    if (LieuSansPvp(cr))
        return false;
    if (HumainProcheSauf(WorldPosition(cr), cr->GetMapId(), ATTIRER_HUMAIN_YD, j->GetGUID().GetCounter()))
        return false;
    return true;
}

// Un chemin complet (mmap) de la creature au joueur, <= ATTIRER_CHEMIN_FACTEUR x la distance ?
bool CheminAttire(Creature* cr, Player* j)
{
    PathGenerator chemin(cr);
    chemin.CalculatePath(j->GetPositionX(), j->GetPositionY(), j->GetPositionZ());
    PathType const type = chemin.GetPathType();
    if (!(type & PATHFIND_NORMAL) || (type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORT |
                                              PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT)))
        return false;
    return chemin.getPathLength() <= ATTIRER_CHEMIN_FACTEUR * cr->GetExactDist(j);
}

void CourirVers(Creature* cr, Player* j)
{
    // forceDestination = false (revue de 873cb21) : le point est celui du joueur, pas un point a atteindre a tout prix.
    cr->GetMotionMaster()->MovePoint(0, j->GetPositionX(), j->GetPositionY(), j->GetPositionZ(), FORCED_MOVEMENT_RUN,
                                     0.0f, 0.0f, true, false);
}

void RefuserAttirer(CoaDirCommande const& c, char const* raison, int32 niveau = -1)
{
    JournaliserAttirer(c.id, c.joueur, 0, std::string(), "refuse", raison, -1.0f, niveau, std::string(),
                       c.joueur && BotDeBanc(JoueurConnecte(c.joueur)));
    Refuser(c, raison);
}

void ExecuterAttirer(CoaDirCommande const& c)
{
    if (!sPlayerbotAIConfig.directeurAttirer)
    {
        RefuserAttirer(c, "attirer_eteint");
        return;
    }
    // 6. bornes. `combat` (renfort du combat en cours) est ABANDONNE (D5-0) : refuse `bornes` comme tout autre texte.
    if (c.cible != 0 || !c.joueur || c.valeur < 1 || c.valeur > int32(ATTIRER_N_MAX) || c.dureeS < ATTIRER_DUREE_MIN_S ||
        c.dureeS > ATTIRER_DUREE_MAX_S || !(c.rayon >= ATTIRER_RAYON_MIN_YD && c.rayon <= ATTIRER_RAYON_MAX_YD) ||
        c.texte != "patrouille")
    {
        RefuserAttirer(c, "bornes");
        return;
    }
    Player* j = JoueurConnecte(c.joueur);
    if (char const* refus = RefusJoueurAttirer(j, c.joueur))
    {
        RefuserAttirer(c, refus, j ? int32(j->GetLevel()) : -1);
        return;
    }
    int32 const niveauJ = int32(j->GetLevel());
    for (Attire const& a : attires)
        if (a.joueur == c.joueur && a.phase != PhaseAttire::Retour)
        {
            RefuserAttirer(c, "occupe", niveauJ);
            return;
        }
    size_t vivants = 0;
    for (Attire const& a : attires)
        if (a.phase != PhaseAttire::Retour)
            ++vivants;
    if (vivants + uint32(c.valeur) > ATTIRES_MAX || attires.size() + uint32(c.valeur) > 2 * ATTIRES_MAX)
    {
        RefuserAttirer(c, "plein", niveauJ);
        return;
    }

    // Choix : candidates, les plus proches d'abord, chemin verifie pour au plus ATTIRER_CHEMINS_MAX d'entre elles.
    std::list<Unit*> unites;
    Acore::AnyUnitInObjectRangeCheck controle(j, c.rayon);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> chercheur(j, unites, controle);
    Cell::VisitObjects(j, chercheur, c.rayon);
    std::vector<std::pair<float, Creature*>> candidates;
    for (Unit* u : unites)
    {
        Creature* cr = u ? u->ToCreature() : nullptr;
        if (cr && CandidateAttiree(cr, j, c.rayon))
            candidates.emplace_back(cr->GetExactDist2d(j), cr);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });
    std::vector<Creature*> retenues;
    uint32 chemins = 0;
    for (auto const& [d, cr] : candidates)
    {
        if (retenues.size() >= uint32(c.valeur) || chemins >= ATTIRER_CHEMINS_MAX)
            break;
        ++chemins;   // revue de 873cb21 : le controle des chasseurs compte aussi dans le budget de la pose
        if (ChasseurPres(cr, ATTIRER_CHASSEUR_YD))
            continue;
        if (CheminAttire(cr, j))
            retenues.push_back(cr);
    }
    if (retenues.empty())
    {
        RefuserAttirer(c, "aucune_creature", niveauJ);
        return;
    }

    uint32 const maintenant = Maintenant();
    std::string noms;
    float distMin = -1.0f;
    for (Creature* cr : retenues)
    {
        Attire a;
        a.id = c.id;
        a.joueur = c.joueur;
        a.carte = cr->GetMapId();
        a.guid = cr->GetGUID();
        a.entree = cr->GetEntry();
        a.limiteS = maintenant + c.dureeS;
        a.niveauPose = cr->GetLevel();
        a.banc = !Humain(j);
        cr->setActive(true);
        CourirVers(cr, j);
        float const d = cr->GetExactDist2d(j);
        distMin = distMin < 0.0f ? d : std::min(distMin, d);
        if (!noms.empty())
            noms += ',';
        noms += NomAttire(a);
        attires.push_back(a);
        ++nbAttiresPoses;
    }
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    JournaliserAttirer(c.id, c.joueur, uint32(retenues.size()), noms, "execute", nullptr, distMin, niveauJ,
                       Acore::StringFormat(" candidates={} chemins={} duree_s={} rayon={:.0f} origine={}",
                                           candidates.size(), chemins, c.dureeS, c.rayon, c.origine),
                       !Humain(j));
    Accuser(c, "execute", nullptr);
}

// Revue de 873cb21 (MAJEUR 2) : Creature::CallAssistance (Creature.cpp:869, periodique) donne a ses aides LE MEME
// pointeur de laisse que l'appelante (Creature.cpp:229). Prolonger la laisse de l'attiree prolongerait celle des
// aides, hors registre : train, plafond de 3 depasse. Chaque seconde, avant la prolongation : toute creature qui
// frappe le joueur, hors registre, et partage ce pointeur, recoit le sien (ClearLastLeashExtensionTimePtr : un
// neuf sera cree a sa prochaine lecture). SetNoCallAssistance seul ne suffirait pas (remis a faux toutes les 3 s).
void DelierLaisses(Creature* cr, Player* j)
{
    std::shared_ptr<time_t> const& sienne = cr->GetLastLeashExtensionTimePtr();
    auto delier = [&](Unit* victime)
    {
        for (Unit* u : victime->getAttackers())
        {
            Creature* autre = u ? u->ToCreature() : nullptr;
            if (!autre || autre == cr || DejaAttiree(autre->GetGUID()))
                continue;
            if (autre->GetLastLeashExtensionTimePtr() == sienne)
                autre->ClearLastLeashExtensionTimePtr();
        }
    };
    delier(j);
    if (Pet* familier = j->GetPet())   // une aide peut frapper le familier du joueur
        delier(familier);
}

// Passe en Retour : evasion si `evader` (creature vivante), journal. Elle reste active jusqu'a son camp.
void SortirAttire(Attire& a, Creature* cr, char const* statut, char const* raison, bool evader, float dist)
{
    if (evader && cr && cr->IsAlive() && cr->AI())
    {
        cr->AI()->EnterEvadeMode();
        ++nbAttiresRentres;
    }
    JournaliserAttirer(a.id, a.joueur, 1, NomAttire(a), statut, raison, dist, cr ? int32(cr->GetLevel()) : -1,
                       std::string(), a.banc);
    a.phase = PhaseAttire::Retour;
    a.retourS = Maintenant();
}

// Retire l'entree `i` du registre (setActive(false) si la creature est encore la).
void RetirerAttire(size_t i, Creature* cr, char const* raison)
{
    Attire const a = attires[i];
    if (cr)
        cr->setActive(false);
    LOG_INFO("playerbots", "coa directeur attirer: id={} joueur={} mode=patrouille n=1 creatures={} statut=inactive raison={}{}",
             a.id, a.joueur, NomAttire(a), raison, a.banc ? " banc=1" : "");
    attires[i] = attires.back();
    attires.pop_back();
}

// Chaque seconde (Entretien). Sans registre : un test.
void SuivreAttires()
{
    if (attires.empty())
        return;
    bool const option = sPlayerbotAIConfig.directeurAttirer;
    uint32 const maintenant = Maintenant();
    for (size_t i = 0; i < attires.size();)
    {
        Attire& a = attires[i];
        Creature* cr = CreatureAttiree(a);
        if (!cr || !cr->IsInWorld())
        {
            // Objet detruit ou retire de la carte : rien a desactiver (setActive(false) sur un objet hors monde ne
            // fait que remettre le drapeau ; il est sans objet ici).
            if (cr)
                cr->setActive(false);
            if (a.phase != PhaseAttire::Retour)
                JournaliserAttirer(a.id, a.joueur, 1, NomAttire(a), "fin", "disparu", -1.0f, -1, std::string(), a.banc);
            attires[i] = attires.back();
            attires.pop_back();
            continue;
        }
        if (a.phase == PhaseAttire::Retour)
        {
            // En combat (detournee, ou de nouveau aux prises avec le joueur) : elle est mise a jour de toute facon, et
            // rentrera ensuite ; on la garde active, le delai de retour compte depuis la fin du combat. Ni laisse
            // prolongee, ni borne : ce n'est plus une creature attiree.
            if (cr->IsAlive() && cr->IsInCombat())
            {
                a.retourS = maintenant;
                ++i;
                continue;
            }
            bool const versCamp = cr->IsInEvadeMode() ||
                                  cr->GetMotionMaster()->GetCurrentMovementGeneratorType() == HOME_MOTION_TYPE;
            bool const pres = cr->GetExactDist2d(cr->GetHomePosition().GetPositionX(),
                                                 cr->GetHomePosition().GetPositionY()) <= ATTIRER_CAMP_YD;
            // Revue de 873cb21 (mineur 1) : une IA dont l'evasion est neutralisee (SmartAI mEvadeDisabled /
            // mSuppressEvade, illisibles d'ici) ne rentre pas : on la renvoie a sa garde nous-memes, sans toucher
            // a la garde, tant qu'elle en est loin.
            if (cr->IsAlive() && !versCamp && !pres && maintenant - a.retourS < ATTIRER_RETOUR_MAX_S)
            {
                cr->GetMotionMaster()->MoveTargetedHome();
                ++i;
                continue;
            }
            bool const auCamp = !versCamp && pres;
            if (!cr->IsAlive() || auCamp || maintenant - a.retourS >= ATTIRER_RETOUR_MAX_S)
            {
                RetirerAttire(i, cr, !cr->IsAlive() ? "mort" : auCamp ? "au_camp" : "retour_long");
                continue;
            }
            ++i;
            continue;
        }
        if (!cr->IsAlive())
        {
            JournaliserAttirer(a.id, a.joueur, 1, NomAttire(a), "fin", "mort", -1.0f, int32(cr->GetLevel()), std::string(),
                               a.banc);
            RetirerAttire(i, cr, "mort");
            continue;
        }
        // Conditions communes a la course et a l'engagement.
        Player* j = JoueurConnecte(a.joueur);
        char const* raison = nullptr;
        if (!option)
            raison = "option";
        else if (!j || !j->IsInWorld())
            raison = "hors_ligne";
        else if (a.banc && !BotDeBanc(j))   // option remise a 0, ou bot passe sous un maitre humain
            raison = "banc";
        else if (j->GetMapId() != a.carte || j->GetMapId() != cr->GetMapId())
            raison = "carte";
        else if (!j->IsAlive())
            raison = "joueur_mort";
        else if (CoaDirecteur::EstArrete(a.joueur))
            raison = "arret";
        else if (!defisLus || DefiBloquant(j))
            raison = "defi";
        else if (LieuSansPvp(j) || LieuSansPvp(cr))
            raison = "sans_pvp";
        else if (HumainProcheSauf(WorldPosition(cr), a.carte, ATTIRER_HUMAIN_YD, a.joueur) ||
                 HumainProcheSauf(WorldPosition(j), a.carte, ATTIRER_HUMAIN_YD, a.joueur))
            raison = "humain";
        else if (ChasseurPres(cr, ATTIRER_CHASSEUR_YD))
            raison = "chasseur";
        if (raison)
        {
            // Une creature deja aux prises avec un AUTRE que le joueur (ni lui, ni son familier) garde son combat.
            Unit* victime = cr->GetVictim();
            bool const autreCombat = cr->IsInCombat() && victime && (!j || victime->GetCharmerOrOwnerPlayerOrPlayerItself() != j);
            // Revue 16/10 : deja ENGAGEE et en combat, un humain ou un chasseur qui passe ne la fait plus evader (elle
            // rentrait a pleine vie en plein combat : le joueur perdait son kill, un tiers pouvait le provoquer). Elle
            // passe en Retour sans evasion : plus de laisse prolongee, la laisse naturelle et la fin du combat decident.
            bool const tiersEnCombat = a.phase == PhaseAttire::Engage && cr->IsInCombat() && !autreCombat &&
                                       (std::strcmp(raison, "humain") == 0 || std::strcmp(raison, "chasseur") == 0);
            SortirAttire(a, cr, tiersEnCombat ? "fin" : "rentre", raison, !autreCombat && !tiersEnCombat,
                         j && j->GetMapId() == cr->GetMapId() ? cr->GetExactDist2d(j) : -1.0f);
            ++i;
            continue;
        }
        if (a.phase == PhaseAttire::Course)
        {
            if (cr->IsInCombat())
            {
                Unit* victime = cr->GetVictim();
                if (victime && victime->GetCharmerOrOwnerPlayerOrPlayerItself() == j)
                {
                    // Elle l'a engage d'elle-meme (aggro) : engagement.
                    a.phase = PhaseAttire::Engage;
                    a.engX = j->GetPositionX();
                    a.engY = j->GetPositionY();
                    cr->UpdateLeashExtensionTime();
                    ++nbAttiresEngages;
                    JournaliserAttirer(a.id, a.joueur, 1, NomAttire(a), "engage", "aggro", cr->GetExactDist2d(j),
                                       int32(cr->GetLevel()), Acore::StringFormat(" attente_s={}", a.aPorteeS ? maintenant - a.aPorteeS : 0),
                                       a.banc);
                }
                else
                    SortirAttire(a, cr, "fin", "detourne", false, cr->GetExactDist2d(j));
                ++i;
                continue;
            }
            // Revue de 873cb21 (mineur 2) : deja en evasion (laisse, script) : ne pas la relancer vers le joueur.
            if (cr->IsInEvadeMode())
            {
                SortirAttire(a, cr, "fin", "detourne", false, cr->GetExactDist2d(j));
                ++i;
                continue;
            }
            if (maintenant >= a.limiteS)
            {
                SortirAttire(a, cr, "rentre", "delai", true, cr->GetExactDist2d(j));
                ++i;
                continue;
            }
            float const d = cr->GetExactDist2d(j);
            if (d <= ATTIRER_ENGAGEMENT_YD)
            {
                if (!a.aPorteeS)
                    a.aPorteeS = maintenant;
                uint32 const cible = j->GetLevel() > ATTIRER_HISSAGE_ECART ? j->GetLevel() - ATTIRER_HISSAGE_ECART : 1;
                bool const hisse = cr->GetLevel() >= cible || cr->GetLevel() > a.niveauPose;
                if (hisse || maintenant - a.aPorteeS >= ATTIRER_HISSAGE_MAX_S)
                {
                    if (!cr->AI() || !cr->IsValidAttackTarget(j))
                    {
                        SortirAttire(a, cr, "rentre", "cible_invalide", true, d);
                        ++i;
                        continue;
                    }
                    cr->AI()->AttackStart(j);
                    cr->UpdateLeashExtensionTime();
                    a.phase = PhaseAttire::Engage;
                    a.engX = j->GetPositionX();
                    a.engY = j->GetPositionY();
                    ++nbAttiresEngages;
                    JournaliserAttirer(a.id, a.joueur, 1, NomAttire(a), "engage", hisse ? "hisse" : "attente_max", d,
                                       int32(cr->GetLevel()), Acore::StringFormat(" attente_s={}", maintenant - a.aPorteeS), a.banc);
                    ++i;
                    continue;
                }
            }
            CourirVers(cr, j);   // recale chaque seconde (le joueur bouge)
            ++i;
            continue;
        }
        // Engage.
        if (!cr->IsInCombat())
        {
            // Combat fini (joueur parti, creature rentree d'elle-meme) : elle rentre au camp, encore active.
            SortirAttire(a, cr, "fin", "fin_combat", !cr->IsInEvadeMode(), cr->GetExactDist2d(j));
            ++i;
            continue;
        }
        float const dx = cr->GetPositionX() - a.engX;
        float const dy = cr->GetPositionY() - a.engY;
        if (dx * dx + dy * dy > ATTIRER_TRAINE_YD * ATTIRER_TRAINE_YD)
        {
            SortirAttire(a, cr, "rentre", "traine", true, std::sqrt(dx * dx + dy * dy));
            ++i;
            continue;
        }
        DelierLaisses(cr, j);
        cr->UpdateLeashExtensionTime();
        ++i;
    }
}

// ------------------------------------------------------------------ soigner

constexpr uint32 SOIGNER_DUREE_MIN_S = 30;
constexpr uint32 SOIGNER_DUREE_MAX_S = 420;
constexpr int32 SOIGNER_SEUIL_MIN = 40;
constexpr int32 SOIGNER_SEUIL_MAX = 70;
constexpr uint32 SOIGNEURS_MAX = 16;
constexpr float SOIGNER_PORTEE_YD = 60.0f;       // au-dela, le patient n'est pas candidat (le bot ne court pas)
constexpr float SOIGNER_ASSISTANCE_YD = 40.0f;
constexpr uint32 SOIGNER_ASSISTANCE_S = 5;
constexpr uint32 SOIGNER_CARTE_S = 30;           // patient et bot sur deux cartes plus longtemps : fin `carte`

struct Soigneur
{
    uint32 id = 0;
    uint32 bot = 0;
    uint32 patient = 0;
    uint32 seuil = 0;
    uint32 finS = 0;
    std::string origine;
    bool banc = false;         // patient bot (option Directeur.ImprevuBanc a la pose)
    uint32 assistanceS = 0;    // Maintenant() de la derniere assistance
    uint32 autreCarteS = 0;    // Maintenant() depuis lequel ils sont sur deux cartes, 0 : non
    uint32 assistances = 0;
};

std::vector<Soigneur> soigneurs;   // fil monde seul

// Copie publiee pour les fils de carte. Ecrite par le fil monde seul (PublierSoigneurs), qui ne tourne pas en meme
// temps que les fils de carte (MapMgr::Update les attend) ; meme raisonnement que les zones du lot S.
struct SoigneurPublie
{
    std::atomic<uint32> bot{0};
    std::atomic<uint32> patient{0};
    std::atomic<uint32> seuil{0};
};
SoigneurPublie soigneursPublies[SOIGNEURS_MAX];
std::atomic<uint32> nbSoigneursPublies{0};

std::atomic<uint64> sdSoins{0};     // soins portes par un soigneur sur son patient (crochet OnHeal)
std::atomic<uint64> sdSoinsPv{0};   // PV rendus (gain : surplus exclu)
std::atomic<uint64> sdChoix{0};     // evaluations ou PartyMemberToHeal a retenu le patient
uint64 sdPoses = 0;                 // fil monde seul
uint64 sdAssistances = 0;           // fil monde seul
thread_local bool t_patientImpose = false;   // le dernier JoueurASoigner de CE fil a rendu un patient D5

void PublierSoigneurs()
{
    uint32 const n = static_cast<uint32>(std::min<size_t>(soigneurs.size(), SOIGNEURS_MAX));
    for (uint32 i = 0; i < n; ++i)
    {
        soigneursPublies[i].bot.store(soigneurs[i].bot, std::memory_order_relaxed);
        soigneursPublies[i].patient.store(soigneurs[i].patient, std::memory_order_relaxed);
        soigneursPublies[i].seuil.store(soigneurs[i].seuil, std::memory_order_relaxed);
    }
    nbSoigneursPublies.store(n, std::memory_order_release);
}

// Fil de carte du bot (PartyMemberToHeal, NearbyGroup) : le patient impose a ce bot, ou nul. Premier test : un
// chargement atomique ; puis au plus SOIGNEURS_MAX comparaisons, sans verrou.
Player* PatientImpose(Player* bot)
{
    uint32 const n = nbSoigneursPublies.load(std::memory_order_acquire);
    if (!n || !sPlayerbotAIConfig.directeurSoigneurs)
        return nullptr;
    uint32 const g = bot->GetGUID().GetCounter();
    uint32 patient = 0, seuil = 0;
    for (uint32 i = 0; i < n && i < SOIGNEURS_MAX; ++i)
        if (soigneursPublies[i].bot.load(std::memory_order_relaxed) == g)
        {
            patient = soigneursPublies[i].patient.load(std::memory_order_relaxed);
            seuil = soigneursPublies[i].seuil.load(std::memory_order_relaxed);
            break;
        }
    // Etude § 4 (revue de 873cb21, mineur 3) : patient impose seulement a un bot SANS groupe.
    if (!patient || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive() || bot->GetGroup())
        return nullptr;
    // Carte du bot seulement (GetPlayer(WorldObject, guid) -> GetPlayer(u.GetMap(), guid)) : P-137.
    Player* p = ObjectAccessor::GetPlayer(*bot, ObjectGuid::Create<HighGuid::Player>(patient));
    // Banc (option ImprevuBanc) : un bot patient aussi ; son statut de bot aleatoire a ete verifie a la pose et chaque
    // seconde (fil monde, SuivreSoigneurs : `banc`). Ici, fil de carte : seulement le moteur et l'option.
    if (!p || p == bot || !p->IsInWorld() || !p->IsAlive() || p->GetMapId() != bot->GetMapId())
        return nullptr;
    if (!Humain(p))
    {
        // Revue de 662e243 (m1) : bot patient sans maitre humain (lu sur le fil de carte, meme carte que le bot).
        PlayerbotAI* iaPatient = sPlayerbotAIConfig.directeurImprevuBanc ? GET_PLAYERBOT_AI(p) : nullptr;
        if (!iaPatient || iaPatient->HasGameClientMaster())
            return nullptr;
    }
    // Soigner un porteur du drapeau JcJ le pose au soigneur (Spell.cpp:3005-3008) : jamais (n° 15). FFA : le soin
    // serait refuse hors groupe (P-168) et mettrait le sort au banc.
    if (p->IsPvP() || p->IsFFAPvP())
        return nullptr;
    if (!bot->IsWithinDistInMap(p, SOIGNER_PORTEE_YD))
        return nullptr;
    if (!(p->GetHealthPct() < float(seuil) || (p->IsInCombat() && bot->IsInCombat())))
        return nullptr;
    if (!bot->IsValidAssistTarget(p))
        return nullptr;
    return p;
}

// Crochet OnHeal (CoaDirecteurSoin.inc), fil de carte du receveur. Sans soigneur publie : un chargement atomique.
void NoterSoinPatient(Unit* soigneur, Unit* receveur, uint32 gain)
{
    uint32 const n = nbSoigneursPublies.load(std::memory_order_acquire);
    if (!n || !soigneur || !receveur || soigneur == receveur)
        return;
    Player* const r = receveur->ToPlayer();
    if (!r)
        return;
    Player* const s = soigneur->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!s || s == r)
        return;
    uint32 const sg = s->GetGUID().GetCounter();
    uint32 const rg = r->GetGUID().GetCounter();
    for (uint32 i = 0; i < n && i < SOIGNEURS_MAX; ++i)
        if (soigneursPublies[i].bot.load(std::memory_order_relaxed) == sg &&
            soigneursPublies[i].patient.load(std::memory_order_relaxed) == rg)
        {
            sdSoins.fetch_add(1, std::memory_order_relaxed);
            sdSoinsPv.fetch_add(gain, std::memory_order_relaxed);
            return;
        }
}

void JournaliserSoigner(uint32 id, uint32 bot, uint32 patient, char const* statut, char const* raison,
                        std::string const& suite = std::string(), bool banc = false)
{
    LOG_INFO("playerbots", "coa directeur soigner: id={} bot={} patient={} statut={} raison={}{}{}", id, bot, patient,
             statut, raison ? raison : "-", suite, banc ? " banc=1" : "");
}

void RefuserSoigner(CoaDirCommande const& c, char const* raison)
{
    JournaliserSoigner(c.id, c.cible, c.joueur, "refuse", raison, std::string(),
                       c.joueur && BotDeBanc(JoueurConnecte(c.joueur)));
    Refuser(c, raison);
}

// Le bot porte-t-il une presence non echue du meme proprietaire pour ce joueur ? A la pose (`arrivee`) : ARRIVEE
// (finale en sejour, non etape), la regle des approches `C-` (ExecuterAffectation, `presenceArrivee`). Au suivi : une
// presence quelconque du meme proprietaire (une `C-` posee ensuite repart en TRAJET jusqu'a la bande : elle ne doit
// pas lever le soigneur).
bool PresenceArrivee(uint32 bot, uint32 joueur, CoaDirCommande const* c, std::string const* origine, bool arrivee)
{
    std::shared_lock<std::shared_mutex> garde(verrouConsignes);
    auto it = affectations.find(bot);
    if (it == affectations.end())
        return false;
    AffectationInterne const& f = it->second;
    if (f.a.finS <= Maintenant() || f.joueur != joueur || !f.a.presence)
        return false;
    if (arrivee && (f.a.etape || f.a.phase != CoaDirecteur::CoaDirPhase::Sejour))
        return false;
    if (c)
        return MemeProprietaire(f, *c);
    return origine && f.origine == *origine;
}

void ExecuterSoigner(CoaDirCommande const& c)
{
    if (!sPlayerbotAIConfig.directeurSoigneurs)
    {
        RefuserSoigner(c, "soigner_eteint");
        return;
    }
    if (!c.cible || !c.joueur || c.cible == c.joueur || c.dureeS < SOIGNER_DUREE_MIN_S || c.dureeS > SOIGNER_DUREE_MAX_S ||
        c.valeur < SOIGNER_SEUIL_MIN || c.valeur > SOIGNER_SEUIL_MAX)
    {
        RefuserSoigner(c, "bornes");
        return;
    }
    Player* bot = JoueurConnecte(c.cible);
    if (!bot || !bot->IsInWorld())
    {
        RefuserSoigner(c, "hors_ligne");
        return;
    }
    PlayerbotAI* ia = GET_PLAYERBOT_AI(bot);
    if (!ia)
    {
        RefuserSoigner(c, "pas_un_bot");
        return;
    }
    if (ia->HasGameClientMaster())
    {
        RefuserSoigner(c, "maitre_humain");
        return;
    }
    Player* p = JoueurConnecte(c.joueur);
    if (!p || !p->IsInWorld())
    {
        RefuserSoigner(c, "hors_ligne");
        return;
    }
    if (!CibleImprevu(p))
    {
        RefuserSoigner(c, "pas_humain");
        return;
    }
    bool const banc = !Humain(p);   // patient bot : option ImprevuBanc (seul cas admis ci-dessus)
    if (!PlayerbotAI::IsHeal(bot))
    {
        RefuserSoigner(c, "pas_soigneur");
        return;
    }
    if (!bot->IsAlive())
    {
        RefuserSoigner(c, "bot_mort");
        return;
    }
    // Banc : un bot patient n'a pas de presence autour de lui (peupler exige un humain, sauf ancre d'evenement) ; le
    // soigneur doit seulement etre a SOIGNER_PORTEE_YD de lui pour soigner (PatientImpose) : au service de le choisir.
    if (!banc && !PresenceArrivee(c.cible, c.joueur, &c, nullptr, true))
    {
        RefuserSoigner(c, "sans_presence");
        return;
    }
    if (HighRisk(p) || p->IsFFAPvP())
    {
        RefuserSoigner(c, "ffa");
        return;
    }
    if (p->IsPvP())
    {
        RefuserSoigner(c, "pvp");
        return;
    }
    if (!defisLus || DefiBloquant(p))
    {
        RefuserSoigner(c, "defi");
        return;
    }
    if (!p->IsAlive())
    {
        RefuserSoigner(c, "mort");
        return;
    }
    if (p->GetMapId() != bot->GetMapId())
    {
        RefuserSoigner(c, "carte");
        return;
    }
    // Revue de 662e243 (m2) : en banc, aucune presence n'amene le soigneur ; il doit deja etre a portee.
    if (banc && !bot->IsWithinDistInMap(p, SOIGNER_PORTEE_YD))
    {
        RefuserSoigner(c, "loin");
        return;
    }
    // Revue de 873cb21 (mineur 3) : faction opposee, duel, MJ, sanctuaire... : le soin serait refuse.
    if (!bot->IsValidAssistTarget(p))
    {
        RefuserSoigner(c, "faction");
        return;
    }
    auto it = std::find_if(soigneurs.begin(), soigneurs.end(), [&c](Soigneur const& s) { return s.bot == c.cible; });
    if (it == soigneurs.end() && soigneurs.size() >= SOIGNEURS_MAX)
    {
        RefuserSoigner(c, "plein");
        return;
    }
    Soigneur s;
    s.id = c.id;
    s.bot = c.cible;
    s.patient = c.joueur;
    s.seuil = uint32(c.valeur);
    s.finS = Maintenant() + c.dureeS;
    s.origine = c.origine;
    s.banc = banc;
    if (it != soigneurs.end())
    {
        Soigneur const ancien = *it;
        *it = s;
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_REMPLACEES);
        JournaliserSoigner(ancien.id, ancien.bot, ancien.patient, "remplace", "remplace", std::string(), ancien.banc);
        if (source)
            source->Clore(ancien.id, "remplace", "remplace");
    }
    else
        soigneurs.push_back(s);
    ++sdPoses;
    PublierSoigneurs();
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    JournaliserSoigner(c.id, c.cible, c.joueur, "execute", nullptr,
                       Acore::StringFormat(" seuil={} duree_s={} origine={}", c.valeur, c.dureeS, c.origine), banc);
    Accuser(c, "execute", nullptr);
}

// Assistance : le bot (hors combat) engage la creature qui frappe le patient, la plus proche de lui. Comme
// EngagerChasseur. -> vrai si engage.
bool Assister(Soigneur& s, Player* bot, Player* p, uint32 maintenant)
{
    PlayerbotAI* ia = GET_PLAYERBOT_AI(bot);
    if (!ia || ia->HasGameClientMaster() || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() ||
        ia->GetState() != BOT_STATE_NON_COMBAT || !p->IsInCombat() || p->GetHealthPct() >= float(s.seuil) ||
        !bot->IsWithinDistInMap(p, SOIGNER_ASSISTANCE_YD) || maintenant - s.assistanceS < SOIGNER_ASSISTANCE_S ||
        !bot->IsValidAssistTarget(p) || bot->GetGroup())   // en groupe, PatientImpose ne rend rien : pas d'assistance
        return false;
    Creature* choisie = nullptr;
    float meilleure = 0.0f;
    for (Unit* u : p->getAttackers())
    {
        Creature* cr = u ? u->ToCreature() : nullptr;
        if (!cr || !cr->IsInWorld() || !cr->IsAlive() || cr->GetCharmerOrOwnerPlayerOrPlayerItself() ||
            cr->GetMapId() != bot->GetMapId() || !bot->IsWithinDistInMap(cr, SOIGNER_ASSISTANCE_YD) ||
            !bot->IsValidAttackTarget(cr))
            continue;
        float const d = bot->GetExactDist(cr);
        if (!choisie || d < meilleure)
        {
            choisie = cr;
            meilleure = d;
        }
    }
    if (!choisie)
        return false;
    s.assistanceS = maintenant;
    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    bot->SetSelection(choisie->GetGUID());
    ia->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(choisie);
    ia->ChangeEngine(BOT_STATE_COMBAT);
    bot->Attack(choisie, !PlayerbotAI::IsRanged(bot));
    ++s.assistances;
    ++sdAssistances;
    JournaliserSoigner(s.id, s.bot, s.patient, "assistance", nullptr,
                       Acore::StringFormat(" creature={}:{} pv={:.0f}", choisie->GetEntry(), choisie->GetGUID().GetCounter(),
                                           p->GetHealthPct()), s.banc);
    return true;
}

// Chaque seconde (Entretien). Sans soigneur : un test.
void SuivreSoigneurs()
{
    if (soigneurs.empty())
    {
        if (nbSoigneursPublies.load(std::memory_order_relaxed))
            PublierSoigneurs();
        return;
    }
    bool const option = sPlayerbotAIConfig.directeurSoigneurs;
    uint32 const maintenant = Maintenant();
    for (size_t i = 0; i < soigneurs.size();)
    {
        Soigneur& s = soigneurs[i];
        Player* bot = JoueurConnecte(s.bot);
        Player* p = JoueurConnecte(s.patient);
        char const* raison = nullptr;
        bool naturelle = false;
        if (!option)
            raison = "option";
        else if (s.finS <= maintenant)
        {
            raison = "echeance";
            naturelle = true;
        }
        else if (!bot || !GET_PLAYERBOT_AI(bot) || !p)
            raison = "hors_ligne";
        else if (s.banc && p->IsInWorld() && !BotDeBanc(p))   // option remise a 0, ou patient sous un maitre humain
            raison = "banc";
        else if (CoaDirecteur::EstArrete(s.patient))
            raison = "arret";
        else if (!s.banc && !PresenceArrivee(s.bot, s.patient, nullptr, &s.origine, false))
            raison = "presence";
        else if (HighRisk(p) || p->IsFFAPvP())
            raison = "ffa";
        else if (p->IsPvP())
            raison = "pvp";
        else if (!defisLus || DefiBloquant(p))
            raison = "defi";
        else if (p->GetMapId() != bot->GetMapId())
        {
            if (!s.autreCarteS)
                s.autreCarteS = maintenant;
            else if (maintenant - s.autreCarteS >= SOIGNER_CARTE_S)
                raison = "carte";
        }
        else
            s.autreCarteS = 0;
        if (raison)
        {
            JournaliserSoigner(s.id, s.bot, s.patient, naturelle ? "fin" : "leve", raison,
                               Acore::StringFormat(" assistances={}", s.assistances), s.banc);
            if (!naturelle)
            {
                CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
                if (source)
                    source->Clore(s.id, "leve", raison);
            }
            soigneurs[i] = soigneurs.back();
            soigneurs.pop_back();
            continue;
        }
        if (p->IsInWorld() && bot->IsInWorld() && p->GetMapId() == bot->GetMapId())
            Assister(s, bot, p, maintenant);
        ++i;
    }
    PublierSoigneurs();
}

// ------------------------------------------------------------------ levees communes et bilan

// `lever` (id ou tout), `.directeur stop` (joueur), bus eteint : appele par LeverEvenementsSi. Predicat (id, joueur).
// Rend le nombre de COMMANDES levees.
template <typename Predicat>
size_t LeverImprevuSi(Predicat lever, char const* raison)
{
    if (attires.empty() && soigneurs.empty())
        return 0;
    std::unordered_set<uint32> ids;
    for (Attire& a : attires)
    {
        if (a.phase == PhaseAttire::Retour || !lever(a.id, a.joueur))
            continue;
        Creature* cr = CreatureAttiree(a);
        Player* j = JoueurConnecte(a.joueur);
        Unit* victime = cr ? cr->GetVictim() : nullptr;
        bool const autreCombat = cr && cr->IsInCombat() && victime && (!j || victime->GetCharmerOrOwnerPlayerOrPlayerItself() != j);
        SortirAttire(a, cr, "rentre", raison, !autreCombat,
                     cr && j && j->GetMapId() == cr->GetMapId() ? cr->GetExactDist2d(j) : -1.0f);
        ids.insert(a.id);
    }
    size_t soignes = 0;
    for (size_t i = 0; i < soigneurs.size();)
    {
        Soigneur const s = soigneurs[i];
        if (!lever(s.id, s.patient))
        {
            ++i;
            continue;
        }
        JournaliserSoigner(s.id, s.bot, s.patient, "leve", raison, Acore::StringFormat(" assistances={}", s.assistances),
                           s.banc);
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        if (source)
            source->Clore(s.id, "leve", raison);
        soigneurs[i] = soigneurs.back();
        soigneurs.pop_back();
        ++soignes;
    }
    if (soignes)
        PublierSoigneurs();
    for (uint32 id : ids)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        if (source)
            source->Clore(id, "leve", raison);
    }
    return ids.size() + soignes;
}

// Fin de la ligne `coa directeur bus:` (fil monde). Les champs neufs vont EN FIN (contrat des lecteurs).
std::string BilanImprevu()
{
    return Acore::StringFormat(
        " attires={} engages={} rentres={} attires_actifs={} soigneurs={} soigneurs_actifs={} assistances={}"
        " soins_soigneurs={} soins_soigneurs_pv={} soin_choix_soigneurs={}",
        nbAttiresPoses, nbAttiresEngages, nbAttiresRentres, attires.size(), sdPoses, soigneurs.size(), sdAssistances,
        sdSoins.load(std::memory_order_relaxed), sdSoinsPv.load(std::memory_order_relaxed),
        sdChoix.load(std::memory_order_relaxed));
}
