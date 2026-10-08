// ------------------------------------------------------------------ lot P des evenements (06/10/2026) : le portail a refermer
//
// FRAGMENT de CoaDirecteur.cpp : inclus par lui, DANS son espace de noms anonyme, juste apres le bloc du lot D
// (il en reprend PointApparition, et du lot R3 LireObjetsRecompense / ObjetRecompensePermis). Il n'est pas une
// unite de compilation (aucun .cpp : le module ne le compile pas seul). Fichier a part pour que la fusion avec
// les autres lots des evenements ne touche que six lignes de CoaDirecteur.cpp (docs/EVENEMENTS-lot-P.md § 2).
//
// Demande du commanditaire (06/10, apres la Faille d'orage) : « Oui ajoute une recompense. Peut-etre qu'il
// faudrait qu'on ferme le portail pour avoir la recompense, qu'on le rende cliquable ».
//
// Sous-action `portail` du verbe `evenement` (ETEINTE tant que AiPlayerbot.Directeur.EvenementPortail = 0) :
//   portail | portail:<id:q[,id:q]>
//     cible = le joueur humain de l'evenement (`joueur` 0 ou le meme) ; valeur = cuivres de la recompense,
//     CALCULES PAR LE SERVICE (recompenses.faille_recompense), bornes par le jeu comme `recompenser` (R3) :
//     0 a RecompenseMaxCuivre ; objets apres `portail:` : liste blanche RecompenseObjets, 4 au plus, meme
//     grammaire que le `texte` de `recompenser` ; duree_s = delai de clic (EV_PORTAIL_DELAI_MIN_S a _MAX_S) ;
//     carte/x/y/z = le point, controle comme celui d'`invoquer` (PointApparition : portee, sol, lieu sans JcJ).
//   Pose le gameobject EV_PORTAIL_ENTREE (9100023, GOOBER, migration
//   exploitation/directeur/sql/2026_10_06_01_evenement_portail_faille.sql) par Map::SummonGameObject, vu de tous
//   (aucune phase). Accuse `execute` a la pose.
//   CLIC : le joueur, ou un membre HUMAIN de son groupe, a EV_PORTAIL_CLIC_YD au plus, vivant -> le portail est
//   referme (retire tout de suite : animation de disparition puis suppression, RetirerObjetPortail) et la
//   recompense est versee au joueur de l'evenement (08/10 : dans ses sacs et sa bourse s'il est dans le monde, le
//   reste et tout hors du monde par COURRIER ; VerserRecompense de CoaDirecteur.cpp), dans la MEME transaction que
//   l'accuse final de la commande (statut `leve`, raison `referme`) : apres un plantage, soit tout est en base, soit
//   rien. Un clic n'est traite qu'une fois (le portail quitte le registre avant la transaction).
//   SANS CLIC : a l'echeance, au depart du joueur (hors ligne, autre carte), a `lever` (id ou tout), a
//   `.directeur stop` et au bus eteint (LeverEvenementsSi), le portail est retire SANS recompense, accuse `leve`
//   avec la raison (`echu`, `hors_ligne`, `carte`, `disparu`, `lever`, `arret`, `bus_eteint`).
// Le CROCHET de clic (AllGameObjectScript::CanGameObjectGossipHello, appele par GameObject::Use, GameObject.cpp)
// tourne sur le fil qui traite le paquet CMSG_GAMEOBJ_USE : PROCESS_INPLACE (Opcodes.cpp), donc un fil de carte
// aussi bien que le fil monde ; une IA de bot peut aussi appeler Use sur son fil de carte. Il ne lit que l'entree
// et le guid de l'objet et le guid du joueur, et ne touche que la file evClics sous evClicsVerrou ; tout le reste
// (registre, joueur, groupe, courrier) se fait au FIL MONDE, chaque seconde (SuivrePortails, Entretien), P-137.
// Journal : `coa directeur evenement: id= sous_action=portail joueur= valeur= motif=<pose|referme|refus|...>`.
//
// NUIT DES MORTS (08/10/2026, docs/EVENEMENTS-nuit-des-morts.md) : la meme sous-action pose aussi l'OBJET A SCELLER
// de la nuit des morts, par une seconde forme du texte, au MEME comportement (clic, versement, levees) :
//   sceau | sceau:<id:q[,id:q]>   -> gameobject EV_SCEAU_ENTREE (9100025 « Cercle du Sonneur », GOOBER, migration
//   exploitation/directeur/sql/2026_10_08_02_evenement_nuit_des_morts.sql). Meme interrupteur (EvenementPortail),
//   memes bornes, un seul portail OU sceau par joueur. Gabarit absent : refus `gabarit` (inerte sans la migration).

constexpr uint32 EV_PORTAIL_ENTREE = 9100023;          // gameobject_template (grimoire § 5.5 : 9100020-9100029)
constexpr uint32 EV_SCEAU_ENTREE = 9100025;            // nuit des morts : le cercle a sceller (meme plage)
constexpr uint32 EV_PORTAIL_DELAI_MIN_S = 30;
constexpr uint32 EV_PORTAIL_DELAI_MAX_S = 600;
constexpr uint32 EV_PORTAIL_FILET_S = 30;              // l'objet s'efface seul 30 s apres l'echeance (registre perdu)
constexpr size_t EV_PORTAIL_MAX = 16;                  // tout le royaume ; un seul par joueur
constexpr float EV_PORTAIL_CLIC_YD = 10.0f;            // le paquet est deja borne a GetInteractionDistance (5 yd)
constexpr size_t EV_PORTAIL_CLICS_FILE_MAX = 64;
constexpr char const EV_PORTAIL_OPTION[] = "AiPlayerbot.Directeur.EvenementPortail";

struct EvPortail
{
    uint32 id;               // commande `evenement` portail
    uint32 entree = EV_PORTAIL_ENTREE;   // gameobject pose : le portail de la faille, ou le sceau de la nuit
    uint32 joueur;           // le joueur de l'evenement, beneficiaire du courrier
    ObjectGuid guid;         // l'objet pose
    uint32 carte;
    uint32 finS;             // Maintenant() de l'echeance du clic
    uint32 cuivre;
    std::vector<std::pair<uint32, uint32>> objets;
    uint32 clicsRefuses = 0; // clics refuses (pas humain, hors groupe, mort, trop loin), dits a la fermeture
};
std::vector<EvPortail> evPortails;                     // fil monde seul

struct EvClic
{
    ObjectGuid portail;
    uint32 joueur;           // guid bas de celui qui a clique
};
std::mutex evClicsVerrou;                              // le SEUL verrou du lot P : la file ci-dessous
std::vector<EvClic> evClics;                           // sous evClicsVerrou
uint32 evClicsPerdus = 0;                              // sous evClicsVerrou : file pleine

// Interrupteur lu dans la conf CHARGEE (sConfigMgr), a chaque pose : `.reload config` suffit, sans
// `playerbots rndbot reload` (il n'est pas dans PlayerbotAIConfig, pour ne pas toucher les fichiers communs
// aux lots paralleles). Absent : faux, sans ligne d'erreur au journal (showLogs = false).
bool PortailAllume()
{
    return sConfigMgr->GetOption<bool>(EV_PORTAIL_OPTION, false, false);
}

bool EstPortail(std::string const& t)
{
    return t == "portail" || t.rfind("portail:", 0) == 0 || t == "sceau" || t.rfind("sceau:", 0) == 0;
}

// `portail` -> aucun objet ; `portail:<id:q[,id:q]>` -> les objets (grammaire de `recompenser`, R3 : non vide,
// 4 au plus, quantites 1-20). Nuit des morts : `sceau` et `sceau:<id:q[,id:q]>`, memes objets, entree
// EV_SCEAU_ENTREE. Tout le reste : faux (refus `texte`). `entree` : le gameobject a poser.
bool LirePortail(std::string const& t, std::vector<std::pair<uint32, uint32>>& objets, uint32& entree)
{
    objets.clear();
    entree = EV_PORTAIL_ENTREE;
    if (t == "portail")
        return true;
    if (t == "sceau")
    {
        entree = EV_SCEAU_ENTREE;
        return true;
    }
    static constexpr char const PREFIXE[] = "portail:";
    static constexpr char const PREFIXE_SCEAU[] = "sceau:";
    size_t const n = sizeof(PREFIXE) - 1;
    size_t const ns = sizeof(PREFIXE_SCEAU) - 1;
    if (t.size() > n && t.compare(0, n, PREFIXE) == 0)
        return LireObjetsRecompense(t.substr(n), objets);
    if (t.size() > ns && t.compare(0, ns, PREFIXE_SCEAU) == 0)
    {
        entree = EV_SCEAU_ENTREE;
        return LireObjetsRecompense(t.substr(ns), objets);
    }
    return false;
}

// Les mots du courrier et des messages, selon l'objet (la faille d'orage, ou le cercle de la nuit des morts).
bool EstSceau(EvPortail const& po)
{
    return po.entree == EV_SCEAU_ENTREE;
}

std::string TexteObjets(std::vector<std::pair<uint32, uint32>> const& objets)
{
    std::string s;
    for (auto const& [id, n] : objets)
        s += (s.empty() ? "" : ",") + std::to_string(id) + ":" + std::to_string(n);
    return s.empty() ? std::string("-") : s;
}

GameObject* ObjetPortail(EvPortail const& po)
{
    Map* map = sMapMgr->FindBaseNonInstanceMap(po.carte);
    return map ? map->GetGameObject(po.guid) : nullptr;
}

// Retire VRAIMENT l'objet, tout de suite (lu dans le coeur, GameObject.cpp) : Map::SummonGameObject a pose
// m_respawnTime = pose + delai + filet et SetSpellId(1). DespawnOrUnsummon() seul ne touche pas m_respawnTime
// (aucun m_goData) ; au GO_JUST_DEACTIVATED suivant, isSummonedAndExpired est faux, et un GOOBER non
// consommable prend la branche « Do not delete chests or goobers » : il redevient GO_READY, VISIBLE, jusqu'au
// filet. SetRespawnTime(0) d'abord : la mise a jour suivante prend la branche GetSpellId() (SetRespawnTime(0),
// Delete()), apres l'animation de disparition envoyee par DespawnOrUnsummon. Ne PAS passer consumable a 1 en
// base : le chemin IsDespawnAtAction cache l'objet sans le supprimer.
void RetirerObjetPortail(GameObject* go)
{
    if (!go)
        return;
    go->SetRespawnTime(0);
    go->DespawnOrUnsummon();
}

void RefuserPortail(CoaDirCommande const& c, char const* raison)
{
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=portail joueur={} valeur={} motif={}", c.id,
             c.cible, c.valeur, raison);
    Refuser(c, raison);
}

void ExecuterPortail(CoaDirCommande const& c)
{
    std::vector<std::pair<uint32, uint32>> objets;
    uint32 entree = EV_PORTAIL_ENTREE;
    if (!TexteBusValide(c.texte) || !LirePortail(c.texte, objets, entree))
    {
        RefuserPortail(c, "texte");
        return;
    }
    if (!CibleJoueurValide(c) || c.valeur < 0 || c.dureeS < EV_PORTAIL_DELAI_MIN_S || c.dureeS > EV_PORTAIL_DELAI_MAX_S)
    {
        RefuserPortail(c, "bornes");
        return;
    }
    if (!PortailAllume())
    {
        RefuserPortail(c, "portail_eteint");
        return;
    }
    Player* p = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!p)
    {
        RefuserPortail(c, "hors_ligne");
        return;
    }
    if (!Humain(p))
    {
        RefuserPortail(c, "pas_humain");
        return;
    }
    if (!p->IsInWorld() || p->IsBeingTeleported() || !CarteDeSuspension(p->GetMapId()))
    {
        RefuserPortail(c, "carte");
        return;
    }
    GameObjectTemplate const* gabarit = sObjectMgr->GetGameObjectTemplate(entree);
    if (!gabarit || gabarit->type != GAMEOBJECT_TYPE_GOOBER)
    {
        RefuserPortail(c, "gabarit");   // migration du lot P (ou de la nuit) absente, ou appliquee apres le demarrage
        return;
    }
    // La recompense est bornee ICI, a la pose, comme `recompenser` (R3) : un portail pose ne peut plus rien
    // verser d'autre que ce qui a ete accepte.
    if (static_cast<uint32>(c.valeur) > sPlayerbotAIConfig.directeurRecompenseMaxCuivre)
    {
        RefuserPortail(c, "plafond");
        return;
    }
    for (auto const& [id, n] : objets)
    {
        ItemTemplate const* it = sObjectMgr->GetItemTemplate(id);
        if (!ObjetRecompensePermis(id) || !it || n > it->GetMaxStackSize() ||
            (it->MaxCount > 0 && n > static_cast<uint32>(it->MaxCount)))
        {
            RefuserPortail(c, "objet");
            return;
        }
    }
    float sol = 0.0f;
    char const* raison = nullptr;
    if (!PointApparition(c, p, sol, raison))
    {
        RefuserPortail(c, raison);
        return;
    }
    if (std::any_of(evPortails.begin(), evPortails.end(), [&c](EvPortail const& x) { return x.joueur == c.cible; }))
    {
        RefuserPortail(c, "deja_ouvert");
        return;
    }
    if (evPortails.size() >= EV_PORTAIL_MAX)
    {
        RefuserPortail(c, "plein");
        return;
    }
    // Face au joueur. Rotation : le quaternion d'une rotation de `face` autour de z (comme le coeur pour un
    // objet pose sans rotation propre).
    float const face = std::atan2(p->GetPositionY() - c.y, p->GetPositionX() - c.x);
    GameObject* go = p->GetMap()->SummonGameObject(entree, c.x, c.y, sol, face, 0.0f, 0.0f,
                                                   std::sin(face / 2.0f), std::cos(face / 2.0f),
                                                   c.dureeS + EV_PORTAIL_FILET_S);
    if (!go)
    {
        RefuserPortail(c, "invocation");
        return;
    }
    EvPortail po;
    po.id = c.id;
    po.entree = entree;
    po.joueur = c.cible;
    po.guid = go->GetGUID();
    po.carte = p->GetMapId();
    po.finS = Maintenant() + c.dureeS;
    po.cuivre = static_cast<uint32>(c.valeur);
    po.objets = objets;
    evPortails.push_back(po);
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots",
             "coa directeur evenement: id={} sous_action=portail joueur={} valeur={} motif=pose objets={} delai_s={}"
             " x={:.1f} y={:.1f} z={:.1f} objet={}",
             c.id, c.cible, po.cuivre, TexteObjets(objets), c.dureeS, c.x, c.y, sol, entree);
    Accuser(c, "execute", nullptr);
}

// Fermeture SANS recompense (echeance, joueur parti, objet disparu, lever, stop, bus eteint).
void FermerPortail(EvPortail const& po, char const* motif)
{
    RetirerObjetPortail(ObjetPortail(po));
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=portail joueur={} valeur={} motif={}"
             " clics_refuses={}", po.id, po.joueur, po.cuivre, motif, po.clicsRefuses);
    CloreEvenement(po.id, po.joueur, "leve", motif);
}

// Le clic est-il recevable ? -> raison du refus, ou nullptr.
char const* RefusClic(EvPortail const& po, Player* q, GameObject* go)
{
    if (!q || !Humain(q))
        return "pas_humain";
    if (!q->IsInWorld() || q->IsBeingTeleported() || q->GetMapId() != po.carte)
        return "carte";
    if (!q->IsAlive())
        return "mort";
    if (!go)
        return "disparu";
    if (q->GetExactDist(go) > EV_PORTAIL_CLIC_YD)
        return "distance";
    if (q->GetGUID().GetCounter() != po.joueur)
    {
        Group* g = q->GetGroup();
        if (!g || !g->IsMember(ObjectGuid::Create<HighGuid::Player>(po.joueur)))
            return "hors_groupe";
    }
    return nullptr;
}

// Le portail REFERME par un clic recevable : la recompense au joueur de l'evenement (sacs et bourse s'il est dans
// le monde, courrier pour le reste : VerserRecompense), et l'accuse final de la commande, dans UNE transaction
// (forme d'ExecuterRecompense, R3). Le portail a deja quitte le registre : ce clic ne peut pas etre traite deux fois.
void RefermerPortail(EvPortail const& po, Player* q, GameObject* go)
{
    char const* raison = "referme";
    bool const aPayer = po.cuivre || !po.objets.empty();
    Player* j = JoueurConnecte(po.joueur);
    std::vector<Item*> crees;
    if (aPayer && !sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(po.joueur)))
        raison = "referme_sans_recompense";   // personnage absent du cache : inatteignable (il etait en jeu)
    if (aPayer && raison == std::string_view("referme"))
        for (auto const& [id, n] : po.objets)
        {
            Item* objet = Item::CreateItem(id, n, j);
            if (!objet)
            {
                for (Item* x : crees)
                    delete x;
                crees.clear();
                raison = "referme_sans_recompense";
                break;
            }
            crees.push_back(objet);
        }
    bool const paye = aPayer && raison == std::string_view("referme");
    RemiseRecompense r;
    // Rien a ecrire (rien a verser, bus inerte) : aucune transaction vide.
    if (paye || source)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        // 08/10 : sacs et bourse s'il est dans le monde, le reste par courrier (VerserRecompense).
        if (paye)
            r = VerserRecompense(trans, j, po.joueur, po.cuivre, po.objets, crees,
                                 EstSceau(po) ? "La nuit des morts" : "La faille d'orage",
                                 EstSceau(po) ? "Vous avez scellé le cercle du Sonneur. Voici votre part. Le Directeur."
                                              : "Vous avez refermé la faille. Voici votre part. Le Directeur.");
        // L'accuse final dans la MEME transaction ; meme garde de statut que CoaDirSourceMysql::Clore.
        if (source)
            trans->Append(Acore::StringFormat(
                "UPDATE coa_dir_commande SET statut = 'leve', raison = '{}', vu_le = NOW(3) WHERE id = {}"
                " AND statut IN ('en_attente', 'execute')",
                raison, po.id));
        CharacterDatabase.CommitTransaction(trans);
    }
    if (paye)
        DireRemise(j, r, EstSceau(po) ? "Le cercle est scellé, votre récompense"
                                      : "La faille est refermée, votre récompense");
    RetirerObjetPortail(go);
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
    LOG_INFO("playerbots", "coa directeur cmd: id={} verbe=evenement cible={} joueur={} statut=leve raison={}", po.id,
             po.joueur, po.joueur, raison);
    LOG_INFO("playerbots",
             "coa directeur evenement: id={} sous_action=portail joueur={} valeur={} motif={} par={} cuivre={}"
             " objets={} paye={} en_ligne={} clics_refuses={} direct={} direct_cuivre={} direct_objets={} courrier={}"
             " courrier_cuivre={} courrier_objets={}",
             po.id, po.joueur, po.cuivre, raison, q ? q->GetGUID().GetCounter() : 0, paye ? po.cuivre : 0,
             paye ? TexteObjets(po.objets) : std::string("-"), paye ? 1 : 0, j ? 1 : 0, po.clicsRefuses,
             r.direct ? 1 : 0, r.cuivreDirect, TexteObjetsRemise(r.objetsDirect), r.courrier ? 1 : 0,
             r.cuivreCourrier, TexteObjetsRemise(r.objetsCourrier));
}

// Chaque seconde (Entretien, bus allume ou eteint) : les clics de la file, puis echeances, joueur parti et
// objet disparu.
void SuivrePortails()
{
    std::vector<EvClic> clics;
    uint32 perdus = 0;
    {
        std::lock_guard<std::mutex> garde(evClicsVerrou);
        clics.swap(evClics);
        std::swap(perdus, evClicsPerdus);
    }
    if (perdus)
        LOG_INFO("playerbots", "coa directeur evenement: sous_action=portail motif=clics_perdus perdus={}", perdus);
    if (evPortails.empty())
        return;   // clics sur un portail inconnu (deja ferme) : rien a faire
    // 1. Les clics, dans l'ordre d'arrivee ; le premier recevable referme.
    for (EvClic const& k : clics)
    {
        auto it = std::find_if(evPortails.begin(), evPortails.end(),
                               [&k](EvPortail const& x) { return x.guid == k.portail; });
        if (it == evPortails.end())
            continue;
        Player* q = JoueurConnecte(k.joueur);
        GameObject* go = ObjetPortail(*it);
        if (char const* refus = RefusClic(*it, q, go))
        {
            ++it->clicsRefuses;
            // Un bot qui « utilise » l'objet n'est pas journalise a chaque fois (compte a la fermeture).
            if (std::string_view(refus) != "pas_humain")
            {
                LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=portail joueur={} valeur={}"
                         " motif=clic_refuse raison={} par={}", it->id, it->joueur, it->cuivre, refus, k.joueur);
                if (q && std::string_view(refus) == "hors_groupe")
                    ChatHandler(q->GetSession()).SendSysMessage(std::string(PREFIXE_DIRECTEUR) +
                                                                (EstSceau(*it) ? "Ce cercle n'est pas le vôtre."
                                                                               : "Cette faille n'est pas la vôtre."));
            }
            continue;
        }
        EvPortail const po = *it;
        evPortails.erase(it);
        RefermerPortail(po, q, go);
    }
    // 2. Echeance, joueur parti (hors ligne, autre carte), objet disparu.
    uint32 const maintenant = Maintenant();
    std::vector<std::pair<EvPortail, char const*>> fins;
    for (auto it = evPortails.begin(); it != evPortails.end();)
    {
        Player* p = JoueurConnecte(it->joueur);
        char const* motif = !p                                                                        ? "hors_ligne"
                            : (p->IsInWorld() && !p->IsBeingTeleported() && p->GetMapId() != it->carte) ? "carte"
                            : it->finS <= maintenant                                                  ? "echu"
                            : !ObjetPortail(*it)                                                      ? "disparu"
                                                                                                      : nullptr;
        if (motif)
        {
            fins.emplace_back(*it, motif);
            it = evPortails.erase(it);
        }
        else
            ++it;
    }
    for (auto const& [po, motif] : fins)
        FermerPortail(po, motif);
}

// Leve (sans recompense) les portails pour lesquels `lever(id, joueur)` est vrai : lever, stop, bus eteint.
template <typename Predicat>
size_t LeverPortailsSi(Predicat lever, char const* raison)
{
    std::vector<EvPortail> partis;
    for (auto it = evPortails.begin(); it != evPortails.end();)
    {
        if (lever(it->id, it->joueur))
        {
            partis.push_back(*it);
            it = evPortails.erase(it);
        }
        else
            ++it;
    }
    for (EvPortail const& po : partis)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        FermerPortail(po, raison);
    }
    return partis.size();
}

// Le crochet de CLIC. Fil quelconque (voir l'en-tete) : n'ecrit que la file sous verrou. Rend vrai pour NOTRE
// objet seulement : GameObject::Use s'arrete la (aucune action par defaut du goober) ; faux pour tout autre.
class coa_directeur_portail_goscript : public AllGameObjectScript
{
public:
    coa_directeur_portail_goscript() : AllGameObjectScript("coa_directeur_portail_goscript") {}

    bool CanGameObjectGossipHello(Player* player, GameObject* go) override
    {
        if (!player || !go || (go->GetEntry() != EV_PORTAIL_ENTREE && go->GetEntry() != EV_SCEAU_ENTREE))
            return false;
        std::lock_guard<std::mutex> garde(evClicsVerrou);
        if (evClics.size() >= EV_PORTAIL_CLICS_FILE_MAX)
            ++evClicsPerdus;
        else
            evClics.push_back(EvClic{go->GetGUID(), player->GetGUID().GetCounter()});
        return true;
    }
};
