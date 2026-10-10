// ------------------------------------------------------------------ objectifs d'evenement (10/10/2026)
//
// FRAGMENT de CoaDirecteur.cpp : inclus par lui, DANS son espace de noms anonyme, juste apres le lot G
// (CoaDirecteurGardien.inc), donc apres le lot D (registre evInvoques, CreatureInvoquee). Il n'est pas une unite de
// compilation. Fichier a part pour que la fusion avec les chantiers paralleles ne touche que cinq lignes de
// CoaDirecteur.cpp (coa-project/docs/CONCEPTION-objectifs-evenement.md § 6, liste des points d'accroche).
//
// Demande du commanditaire (10/10, ses mots) : « des qu'il y a un evenement qui commence, avoir un objectif qui
// apparait dans les objectifs de zone (un peu comme on a sur certaines instances ou certains champs de bataille).
// [...] Tu montres juste les etapes de l'evenement et ou on en est dans l'evenement. »
//
// VOIE NATIVE : des ETATS DU MONDE (Player::SendUpdateWorldState -> SMSG_UPDATE_WORLD_STATE, au SEUL destinataire),
// que le client affiche dans son cadre « toujours visible » (WorldStateAlwaysUpFrame, celui des champs de bataille :
// FrameXML WorldStateFrame.lua de patch-B.MPQ, LU) parce que des lignes AJOUTEES a WorldStateUI.dbc (patch client
// patch-ZZ.MPQ, outils/fabriquer-objectifs-evenement.py) les nomment : carte -1 (partout, comme les lignes
// d'invasion d'Ascension 326-333 du meme DBC), zone 0, type 0, une ligne visible quand son etat vaut > 0 (le Lua :
// `if ( state > 0 )`), et le texte porte des valeurs par %<etat>w. Le coeur ne charge PAS WorldStateUI.dbc (structure
// en commentaire, DBCStructure.h) : rien a changer cote serveur hors de ce fichier. SANS LE PATCH CLIENT, les
// paquets partent et le client ne montre rien (aucune ligne ne nomme ces etats) : inoffensif, invisible.
//
// Sous-action `objectif:` du verbe `evenement` (ETEINTE tant que AiPlayerbot.Directeur.EvenementObjectifs = 0, lue
// a chaque commande dans la conf CHARGEE, sConfigMgr, comme EvenementPortail : `.reload config` suffit ; absente : 0) :
//   objectif:<titre>:<etape>[:<n>[:<m>]]     grammaire FERMEE (exploitation/directeur/objectifs_evenement.py, lire)
//     titre 1-11 (la faille d'orage, ses quatre variantes, la nuit des morts, ses quatre variantes, « autre ») ;
//     etape presage | renforts | ouverture | coffre | portail | victoire | defaite | fin  (aucun nombre)
//           vague:<n>:<m> | champion:<n>:<m>   (1 <= n <= m <= 20)
//           chrono:<s>                          (1-3600 s a tenir : le JEU decompte, chaque seconde)
//           chef:<phase>                        (1-9 ; valeur = id du lot `invoquer` du chef, 0 : pas encore pose)
//   cible = le joueur humain de l'evenement (`joueur` 0 ou le meme) ; duree_s = vie de l'affichage (10-3600 s :
//   un service mort ne laisse rien au-dela) ; valeur 0 hors `chef`. Un objectif par joueur : le dernier REMPLACE.
//   Accuse `execute` a la pose (comme `annoncer`) ; jamais clos ensuite.
// CE QUE LE JEU CALCULE LUI-MEME, chaque seconde (Entretien, fil monde), sans commande : les ENNEMIS RESTANTS (les
// invoques vivants du joueur, registre evInvoques, renforts compris), la VIE DU CHEF (le lot `valeur`, creature lue
// au fil monde) et le COMPTE A REBOURS. Le service n'emet qu'aux changements d'etape : le seau du bus
// (MaxCommandesParMinute) est commun au royaume.
// DESTINATAIRES : le joueur, et les HUMAINS de son groupe sur la meme carte (un bot n'a pas de client). Un humain
// sujet de son propre objectif ne recoit que le sien ; membre de deux groupes d'evenement (impossible : un seul
// groupe) -- en cas de doublon, le plus petit guid de joueur gagne (deterministe).
// RENVOI : le client remet ses etats a zero a SMSG_INIT_WORLD_STATES ? (NON LU dans le client : SUPPOSE possible) ;
// Player::UpdateZone l'envoie a chaque changement de zone (PlayerUpdates.cpp), et la connexion aussi
// (CharacterHandler.cpp). D'ou : changement de zone ou de carte constate -> renvoi complet tout de suite ET
// OBJ_RENVOI_APRES_S plus tard (l'INIT peut arriver apres notre constat) ; et un renvoi complet toutes les
// OBJ_RAFRAICHIR_S, filet. Un renvoi complet = tous les etats du bloc, zeros compris.
// FIN : etape d'issue (victoire, defaite, fin : le service lui donne ISSUE_AFFICHAGE_S = 30 s), echeance, `lever`
// (id ou tout), `.directeur stop` du joueur et bus eteint (LeverEvenementsSi) -> tous les etats remis a zero chez
// chaque destinataire en ligne. Un joueur hors ligne garde son objectif jusqu'a l'echeance : il le revoit a sa
// reconnexion (renvoi complet).
// Journal : `coa directeur evenement: id= sous_action=objectif joueur= valeur= etape= motif=<pose|remplace|echu|...>`.

constexpr char const OBJ_OPTION[] = "AiPlayerbot.Directeur.EvenementObjectifs";
constexpr char const OBJ_PREFIXE[] = "objectif:";
constexpr uint32 OBJ_TITRES = 11;
constexpr uint32 OBJ_ETAT_TITRE_BASE = 71000;     // titre n : etat 71000 + n
// Etats qui MONTRENT une ligne (objectifs_evenement.ETAPES_VISIBLES).
constexpr uint32 OBJ_V_PRESAGE = 71020;
constexpr uint32 OBJ_V_RENFORTS = 71021;
constexpr uint32 OBJ_V_VAGUE = 71022;
constexpr uint32 OBJ_V_CHAMPION = 71023;
constexpr uint32 OBJ_V_CHRONO = 71024;
constexpr uint32 OBJ_V_RESTANTS = 71025;
constexpr uint32 OBJ_V_CHEF_APPROCHE = 71026;
constexpr uint32 OBJ_V_CHEF_PV = 71027;
constexpr uint32 OBJ_V_CHEF_PHASE = 71028;
constexpr uint32 OBJ_V_COFFRE = 71029;
constexpr uint32 OBJ_V_PORTAIL = 71030;
constexpr uint32 OBJ_V_VICTOIRE = 71031;
constexpr uint32 OBJ_V_DEFAITE = 71032;
constexpr uint32 OBJ_V_FIN = 71033;
constexpr uint32 OBJ_V_OUVERTURE = 71034;
// Etats qui PORTENT une valeur (objectifs_evenement.VALEURS).
constexpr uint32 OBJ_N_VAGUE = 71040;
constexpr uint32 OBJ_N_VAGUES = 71041;
constexpr uint32 OBJ_N_RESTANTS = 71042;
constexpr uint32 OBJ_N_CHEF_PV = 71043;
constexpr uint32 OBJ_N_CHEF_PHASE = 71044;
constexpr uint32 OBJ_N_CHRONO_MIN = 71045;
constexpr uint32 OBJ_N_CHRONO_DIZ = 71046;
constexpr uint32 OBJ_N_CHRONO_UNITE = 71047;
// Tous les etats du bloc, dans l'ordre croissant (objectifs_evenement.toutes_variables()).
constexpr uint32 OBJ_VARIABLES[] = {
    71001, 71002, 71003, 71004, 71005, 71006, 71007, 71008, 71009, 71010, 71011,
    OBJ_V_PRESAGE, OBJ_V_RENFORTS, OBJ_V_VAGUE, OBJ_V_CHAMPION, OBJ_V_CHRONO, OBJ_V_RESTANTS, OBJ_V_CHEF_APPROCHE,
    OBJ_V_CHEF_PV, OBJ_V_CHEF_PHASE, OBJ_V_COFFRE, OBJ_V_PORTAIL, OBJ_V_VICTOIRE, OBJ_V_DEFAITE, OBJ_V_FIN,
    OBJ_V_OUVERTURE,
    OBJ_N_VAGUE, OBJ_N_VAGUES, OBJ_N_RESTANTS, OBJ_N_CHEF_PV, OBJ_N_CHEF_PHASE, OBJ_N_CHRONO_MIN, OBJ_N_CHRONO_DIZ,
    OBJ_N_CHRONO_UNITE};
constexpr size_t OBJ_N = sizeof(OBJ_VARIABLES) / sizeof(OBJ_VARIABLES[0]);
constexpr uint32 OBJ_DUREE_MIN_S = 10;
constexpr uint32 OBJ_DUREE_MAX_S = 3600;
constexpr uint32 OBJ_VAGUE_MAX = 20;
constexpr uint32 OBJ_CHRONO_MAX_S = 3600;
constexpr uint32 OBJ_PHASE_MAX = 9;
constexpr size_t OBJ_MAX = 64;                     // joueurs suivis, tout le royaume
constexpr uint32 OBJ_RAFRAICHIR_S = 10;            // renvoi complet, filet
constexpr uint32 OBJ_RENVOI_APRES_S = 2;           // second renvoi apres un changement de zone ou de carte

enum class ObjEtape : uint8 { Presage, Renforts, Ouverture, Vague, Champion, Chrono, Chef, Coffre, Portail, Victoire,
                              Defaite, Fin };

struct ObjEtapeNom
{
    char const* nom;
    ObjEtape etape;
    uint8 arite;
};
constexpr ObjEtapeNom OBJ_ETAPES[] = {
    {"presage", ObjEtape::Presage, 0},   {"renforts", ObjEtape::Renforts, 0}, {"ouverture", ObjEtape::Ouverture, 0},
    {"vague", ObjEtape::Vague, 2},       {"champion", ObjEtape::Champion, 2}, {"chrono", ObjEtape::Chrono, 1},
    {"chef", ObjEtape::Chef, 1},         {"coffre", ObjEtape::Coffre, 0},     {"portail", ObjEtape::Portail, 0},
    {"victoire", ObjEtape::Victoire, 0}, {"defaite", ObjEtape::Defaite, 0},   {"fin", ObjEtape::Fin, 0}};

char const* NomObjEtape(ObjEtape e)
{
    for (ObjEtapeNom const& x : OBJ_ETAPES)
        if (x.etape == e)
            return x.nom;
    return "inconnue";
}

struct ObjLu
{
    uint32 titre = 0;
    ObjEtape etape = ObjEtape::Presage;
    uint32 n = 0, m = 0;   // vague, champion : n, m ; chrono : n = secondes ; chef : n = phase
};

struct EvObjectif
{
    uint32 id = 0;          // derniere commande `objectif:` de ce joueur
    uint32 joueur = 0;
    uint32 titre = 0;
    ObjEtape etape = ObjEtape::Presage;
    uint32 n = 0, m = 0;
    uint32 chronoFinS = 0;  // Maintenant() de la fin du compte a rebours (chrono)
    uint32 phase = 1;       // chef
    uint32 chefLot = 0;     // id de la commande `invoquer` du chef (0 : pas encore pose)
    bool chefVu = false;    // le chef a ete vu vivant (sa vie s'affiche des lors, 0 % une fois tombe)
    uint32 chefPv = 0;
    uint32 finS = 0;        // Maintenant() de l'echeance de l'affichage
};
std::unordered_map<uint32, EvObjectif> evObjectifs;   // cle : guid du joueur de l'evenement ; fil monde seul

struct ObjEnvoye
{
    std::array<uint32, OBJ_N> valeurs{};   // ce que le destinataire a recu, etat par etat
    uint32 source = 0;                     // joueur dont l'objectif lui est montre
    uint32 carte = 0;
    uint32 zone = 0;
    uint32 renvoiS = 0;                    // Maintenant() du prochain renvoi complet
    uint32 secondRenvoiS = 0;              // 0 : aucun second renvoi en attente
};
std::unordered_map<uint32, ObjEnvoye> objEnvoyes;     // cle : guid du destinataire ; fil monde seul

bool ObjectifsAllumes()
{
    return sConfigMgr->GetOption<bool>(OBJ_OPTION, false, false);
}

bool EstObjectif(std::string const& t)
{
    return t.rfind(OBJ_PREFIXE, 0) == 0;
}

// Entier decimal sans signe ni zero de tete, 5 chiffres au plus (comme objectifs_evenement.lire).
bool EntierObjectif(std::string const& s, uint32& v)
{
    if (s.empty() || s.size() > 5 || (s.size() > 1 && s[0] == '0'))
        return false;
    v = 0;
    for (char ch : s)
    {
        if (ch < '0' || ch > '9')
            return false;
        v = v * 10 + uint32(ch - '0');
    }
    return true;
}

bool LireObjectif(std::string const& t, ObjLu& lu)
{
    lu = ObjLu();
    if (!EstObjectif(t))
        return false;
    std::vector<std::string> morceaux;
    size_t debut = sizeof(OBJ_PREFIXE) - 1;
    while (true)
    {
        size_t const fin = t.find(':', debut);
        morceaux.push_back(t.substr(debut, fin == std::string::npos ? std::string::npos : fin - debut));
        if (fin == std::string::npos)
            break;
        debut = fin + 1;
    }
    if (morceaux.size() < 2 || !EntierObjectif(morceaux[0], lu.titre) || lu.titre < 1 || lu.titre > OBJ_TITRES)
        return false;
    ObjEtapeNom const* e = nullptr;
    for (ObjEtapeNom const& x : OBJ_ETAPES)
        if (morceaux[1] == x.nom)
            e = &x;
    if (!e || morceaux.size() != size_t(2 + e->arite))
        return false;
    lu.etape = e->etape;
    if (e->arite >= 1 && !EntierObjectif(morceaux[2], lu.n))
        return false;
    if (e->arite >= 2 && !EntierObjectif(morceaux[3], lu.m))
        return false;
    switch (lu.etape)
    {
        case ObjEtape::Vague:
        case ObjEtape::Champion:
            return lu.n >= 1 && lu.n <= lu.m && lu.m <= OBJ_VAGUE_MAX;
        case ObjEtape::Chrono:
            return lu.n >= 1 && lu.n <= OBJ_CHRONO_MAX_S;
        case ObjEtape::Chef:
            return lu.n >= 1 && lu.n <= OBJ_PHASE_MAX;
        default:
            return true;
    }
}

void JournaliserObjectif(uint32 id, uint32 joueur, int32 valeur, char const* etape, char const* motif)
{
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=objectif joueur={} valeur={} etape={} motif={}",
             id, joueur, valeur, etape, motif);
}

void RefuserObjectif(CoaDirCommande const& c, char const* raison)
{
    JournaliserObjectif(c.id, c.cible, c.valeur, "-", raison);
    Refuser(c, raison);
}

void ExecuterObjectif(CoaDirCommande const& c)
{
    if (!ObjectifsAllumes())
    {
        RefuserObjectif(c, "objectifs_eteints");
        return;
    }
    ObjLu lu;
    if (!TexteBusValide(c.texte) || !LireObjectif(c.texte, lu))
    {
        RefuserObjectif(c, "texte");
        return;
    }
    if (!CibleJoueurValide(c) || c.valeur < 0 || (c.valeur != 0 && lu.etape != ObjEtape::Chef) ||
        c.dureeS < OBJ_DUREE_MIN_S || c.dureeS > OBJ_DUREE_MAX_S)
    {
        RefuserObjectif(c, "bornes");
        return;
    }
    Player* p = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!p)
    {
        RefuserObjectif(c, "hors_ligne");
        return;
    }
    if (!Humain(p))
    {
        RefuserObjectif(c, "pas_humain");
        return;
    }
    if (!p->IsInWorld() || !CarteDeSuspension(p->GetMapId()))
    {
        RefuserObjectif(c, "carte");
        return;
    }
    auto it = evObjectifs.find(c.cible);
    if (it == evObjectifs.end() && evObjectifs.size() >= OBJ_MAX)
    {
        RefuserObjectif(c, "plein");
        return;
    }
    uint32 const maintenant = Maintenant();
    EvObjectif o;
    bool const remplace = it != evObjectifs.end();
    if (remplace && it->second.chefLot && it->second.chefLot == uint32(c.valeur))
    {
        o.chefVu = it->second.chefVu;   // meme chef : sa vie connue reste affichee
        o.chefPv = it->second.chefPv;
    }
    o.id = c.id;
    o.joueur = c.cible;
    o.titre = lu.titre;
    o.etape = lu.etape;
    o.finS = maintenant + c.dureeS;
    switch (lu.etape)
    {
        case ObjEtape::Vague:
        case ObjEtape::Champion:
            o.n = lu.n;
            o.m = lu.m;
            break;
        case ObjEtape::Chrono:
            o.chronoFinS = maintenant + lu.n;
            break;
        case ObjEtape::Chef:
            o.phase = lu.n;
            o.chefLot = uint32(c.valeur);
            break;
        default:
            break;
    }
    evObjectifs[c.cible] = o;
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    JournaliserObjectif(c.id, c.cible, c.valeur, NomObjEtape(lu.etape), remplace ? "remplace" : "pose");
    Accuser(c, "execute", nullptr);
}

void PoserValeur(std::array<uint32, OBJ_N>& v, uint32 etat, uint32 valeur)
{
    for (size_t i = 0; i < OBJ_N; ++i)
        if (OBJ_VARIABLES[i] == etat)
        {
            v[i] = valeur;
            return;
        }
}

// Ce que montre l'objectif `o`, a cet instant (fil monde : lit le registre des invoques et la creature du chef).
std::array<uint32, OBJ_N> ValeursObjectif(EvObjectif& o, uint32 maintenant)
{
    std::array<uint32, OBJ_N> v{};
    PoserValeur(v, OBJ_ETAT_TITRE_BASE + o.titre, 1);
    uint32 restants = 0;
    for (EvInvoque const& x : evInvoques)
        if (x.joueur == o.joueur && !x.mort)
            ++restants;
    bool combat = false;
    switch (o.etape)
    {
        case ObjEtape::Presage:   PoserValeur(v, OBJ_V_PRESAGE, 1); break;
        case ObjEtape::Renforts:  PoserValeur(v, OBJ_V_RENFORTS, 1); break;
        case ObjEtape::Ouverture: PoserValeur(v, OBJ_V_OUVERTURE, 1); break;
        case ObjEtape::Coffre:    PoserValeur(v, OBJ_V_COFFRE, 1); break;
        case ObjEtape::Portail:   PoserValeur(v, OBJ_V_PORTAIL, 1); break;
        case ObjEtape::Victoire:  PoserValeur(v, OBJ_V_VICTOIRE, 1); break;
        case ObjEtape::Defaite:   PoserValeur(v, OBJ_V_DEFAITE, 1); break;
        case ObjEtape::Fin:       PoserValeur(v, OBJ_V_FIN, 1); break;
        case ObjEtape::Vague:
        case ObjEtape::Champion:
            combat = true;
            PoserValeur(v, OBJ_V_VAGUE, 1);
            PoserValeur(v, OBJ_N_VAGUE, o.n);
            PoserValeur(v, OBJ_N_VAGUES, o.m);
            if (o.etape == ObjEtape::Champion)
                PoserValeur(v, OBJ_V_CHAMPION, 1);
            break;
        case ObjEtape::Chrono:
        {
            combat = true;
            uint32 const reste = o.chronoFinS > maintenant ? o.chronoFinS - maintenant : 0;
            if (reste)
            {
                PoserValeur(v, OBJ_V_CHRONO, 1);
                PoserValeur(v, OBJ_N_CHRONO_MIN, reste / 60);
                PoserValeur(v, OBJ_N_CHRONO_DIZ, (reste % 60) / 10);
                PoserValeur(v, OBJ_N_CHRONO_UNITE, reste % 10);
            }
            break;
        }
        case ObjEtape::Chef:
        {
            combat = true;
            if (o.chefLot)
            {
                bool vivant = false;
                for (EvInvoque const& x : evInvoques)
                {
                    if (x.id != o.chefLot || x.mort || x.renfort)
                        continue;
                    if (Creature* cr = CreatureInvoquee(x))
                        if (cr->IsAlive())
                        {
                            float const pct = cr->GetHealthPct();
                            o.chefPv = std::min<uint32>(100, std::max<uint32>(1, uint32(std::ceil(pct))));
                            o.chefVu = vivant = true;
                            break;
                        }
                }
                if (!vivant && o.chefVu)
                    o.chefPv = 0;   // tombe (ou retire) : 0 %, jusqu'a l'etape suivante
            }
            if (o.chefVu)
            {
                PoserValeur(v, OBJ_V_CHEF_PV, 1);
                PoserValeur(v, OBJ_N_CHEF_PV, o.chefPv);
                if (o.phase >= 2)
                {
                    PoserValeur(v, OBJ_V_CHEF_PHASE, 1);
                    PoserValeur(v, OBJ_N_CHEF_PHASE, o.phase);
                }
            }
            else
                PoserValeur(v, OBJ_V_CHEF_APPROCHE, 1);
            break;
        }
    }
    if (combat && restants)
    {
        PoserValeur(v, OBJ_V_RESTANTS, 1);
        PoserValeur(v, OBJ_N_RESTANTS, restants);
    }
    return v;
}

// Un destinataire a qui l'on peut ENVOYER maintenant : humain, dans le monde, pas en teleportation (sinon nul : il
// sera servi a son retour -- changement de carte ou de zone constate, renvoi complet).
Player* DestinataireObjectif(uint32 guid)
{
    Player* p = ObjectAccessor::FindPlayerByLowGUID(guid);
    if (!p || !Humain(p) || !p->IsInWorld() || p->IsBeingTeleported())
        return nullptr;
    return p;
}

// Un humain CONNECTE (en teleportation lointaine compris : JoueurConnecte), pour le choix des destinataires.
Player* HumainConnecte(uint32 guid)
{
    Player* p = JoueurConnecte(guid);
    return p && Humain(p) ? p : nullptr;
}

void EnvoyerObjectif(Player* p, ObjEnvoye& e, std::array<uint32, OBJ_N> const& v, bool complet)
{
    for (size_t i = 0; i < OBJ_N; ++i)
        if (complet || e.valeurs[i] != v[i])
            p->SendUpdateWorldState(OBJ_VARIABLES[i], v[i]);
    e.valeurs = v;
}

// Remises a zero en attente : destinataires injoignables (teleportation, hors ligne) quand leur affichage devait
// s'effacer. Servis des qu'ils sont joignables, OBJ_EFFACER_MAX_S au plus. Fil monde seul.
constexpr uint32 OBJ_EFFACER_MAX_S = 600;
std::unordered_map<uint32, uint32> objAEffacer;       // guid -> Maintenant() de l'abandon

void ZerosA(Player* p)
{
    for (size_t i = 0; i < OBJ_N; ++i)
        p->SendUpdateWorldState(OBJ_VARIABLES[i], 0);
}

// Remet a zero chez le destinataire `guid` (tout de suite s'il est joignable, sinon des qu'il le sera) et l'oublie.
void EffacerChez(uint32 guid)
{
    objEnvoyes.erase(guid);
    if (Player* p = DestinataireObjectif(guid))
    {
        ZerosA(p);
        objAEffacer.erase(guid);
    }
    else if (objAEffacer.size() < 4 * OBJ_MAX)
        objAEffacer[guid] = Maintenant() + OBJ_EFFACER_MAX_S;
}

void FinirObjectif(EvObjectif const& o, char const* motif)
{
    JournaliserObjectif(o.id, o.joueur, int32(o.chefLot), NomObjEtape(o.etape), motif);
    std::vector<uint32> chez;
    for (auto const& [dest, e] : objEnvoyes)
        if (e.source == o.joueur)
            chez.push_back(dest);
    if (std::find(chez.begin(), chez.end(), o.joueur) == chez.end())
        chez.push_back(o.joueur);   // le sujet lui-meme, meme s'il n'a encore rien recu (injoignable a la pose)
    for (uint32 dest : chez)
        EffacerChez(dest);
}

// Chaque seconde, fil monde (Entretien, apres SuivreInvoques : les vivants de cette seconde).
void SuivreObjectifs()
{
    if (evObjectifs.empty() && objEnvoyes.empty() && objAEffacer.empty())
        return;
    uint32 const maintenant = Maintenant();
    // 1. Echeances.
    for (auto it = evObjectifs.begin(); it != evObjectifs.end();)
    {
        if (it->second.finS <= maintenant)
        {
            EvObjectif const o = it->second;
            it = evObjectifs.erase(it);
            FinirObjectif(o, "echu");
        }
        else
            ++it;
    }
    // 2. Destinataires (CONNECTES, teleportation comprise) : le joueur d'abord, puis les humains de son groupe sur sa
    //    carte. Un sujet ne recoit que le sien ; deux groupes pour un meme humain : le plus petit guid de joueur.
    std::unordered_map<uint32, uint32> pour;   // destinataire -> joueur source
    for (auto const& [joueur, o] : evObjectifs)
        if (HumainConnecte(joueur))
            pour[joueur] = joueur;
    for (auto const& [joueur, o] : evObjectifs)
    {
        Player* p = HumainConnecte(joueur);
        Group* g = p ? p->GetGroup() : nullptr;
        if (!g)
            continue;
        for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m || m == p || !Humain(m) || m->GetMapId() != p->GetMapId())
                continue;
            uint32 const mg = m->GetGUID().GetCounter();
            if (evObjectifs.count(mg))
                continue;
            auto x = pour.find(mg);
            if (x == pour.end() || joueur < x->second)
                pour[mg] = joueur;
        }
    }
    // 3. Remises a zero en attente : joignables et sans objectif -> zeros ; destinataires a nouveau -> le renvoi
    //    complet du 5 suffit ; trop vieilles -> oubliees.
    for (auto it = objAEffacer.begin(); it != objAEffacer.end();)
    {
        Player* p = pour.count(it->first) ? nullptr : DestinataireObjectif(it->first);
        if (pour.count(it->first) || it->second <= maintenant)
            it = objAEffacer.erase(it);
        else if (p)
        {
            ZerosA(p);
            it = objAEffacer.erase(it);
        }
        else
            ++it;
    }
    // 4. Ceux qui ne sont plus destinataires (ou d'un autre objectif) : remis a zero.
    std::vector<uint32> partis;
    for (auto const& [dest, e] : objEnvoyes)
    {
        auto x = pour.find(dest);
        if (x == pour.end() || x->second != e.source)
            partis.push_back(dest);
    }
    for (uint32 dest : partis)
        EffacerChez(dest);
    // 5. Envois aux destinataires JOIGNABLES : complet a l'entree, au changement de zone ou de carte (et encore
    //    OBJ_RENVOI_APRES_S plus tard), toutes les OBJ_RAFRAICHIR_S ; sinon les seuls etats qui changent.
    std::unordered_map<uint32, std::array<uint32, OBJ_N>> calcules;
    for (auto const& [dest, joueur] : pour)
    {
        Player* p = DestinataireObjectif(dest);
        auto o = evObjectifs.find(joueur);
        if (!p || o == evObjectifs.end())
            continue;
        auto c = calcules.find(joueur);
        if (c == calcules.end())
            c = calcules.emplace(joueur, ValeursObjectif(o->second, maintenant)).first;
        auto [e, nouveau] = objEnvoyes.try_emplace(dest);
        uint32 const carte = p->GetMapId();
        uint32 const zone = p->GetZoneId();
        bool complet = nouveau || maintenant >= e->second.renvoiS;
        if (!nouveau && (carte != e->second.carte || zone != e->second.zone))
        {
            complet = true;
            e->second.secondRenvoiS = maintenant + OBJ_RENVOI_APRES_S;
        }
        else if (e->second.secondRenvoiS && maintenant >= e->second.secondRenvoiS)
        {
            complet = true;
            e->second.secondRenvoiS = 0;
        }
        e->second.source = joueur;
        e->second.carte = carte;
        e->second.zone = zone;
        if (complet)
            e->second.renvoiS = maintenant + OBJ_RAFRAICHIR_S;
        EnvoyerObjectif(p, e->second, c->second, complet);
    }
}

template <typename Predicat>
size_t LeverObjectifsSi(Predicat lever, char const* raison)
{
    std::vector<EvObjectif> leves;
    for (auto it = evObjectifs.begin(); it != evObjectifs.end();)
    {
        if (lever(it->second.id, it->second.joueur))
        {
            leves.push_back(it->second);
            it = evObjectifs.erase(it);
        }
        else
            ++it;
    }
    for (EvObjectif const& o : leves)
        FinirObjectif(o, raison);
    return leves.size();
}
