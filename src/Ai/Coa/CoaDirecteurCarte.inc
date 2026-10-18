// ================================================================== carte des evenements, prefixe COAEV (17/10/2026)
//
// FRAGMENT de CoaDirecteur.cpp : inclus par lui DANS l'espace de noms anonyme de l'onglet « Bounty Hunting », juste
// avant sa fermeture (apres ChampOnglet, NomZoneTraqueur, le lot D -- evInvoques --, les objectifs -- evObjectifs --, le
// convoi -- evBarricades, EntreeProtege -- et les primes). Il n'est pas une unite de compilation.
// Conception : coa-project apercu-carte-evenements/ (proposition C v2, revue adverse revue-c2.js) ; addon client :
// coa-project client/CoAMonde ; epreuve hors jeu : coa-project outils/test-coamonde.lua.
//
// L'addon CoAMonde chuchote a soi-meme (meme voie que COABH) :
//   `COAEV\tCARTE`   a l'ouverture de la carte du monde, puis toutes les 15 s carte ouverte ;
//   `COAEV\tCOMPTE`  carte fermee, toutes les 30 s (le bouton de la mini-carte).
// Reponse a CARTE : chuchotements d'addon `COAEV\t<i>/<n>|<morceau>`, morceaux de CARTE_MORCEAU octets au plus, jamais
// coupes dans un caractere UTF-8 ; lignes separees par `^`, champs par `|` :
//   V|1                                        version du protocole
//   C|<continent>                              continent du joueur : 1 Kalimdor, 2 Royaumes de l'Est, 3 Outreterre,
//                                              4 Norfendre ; 0 hors continent (donjon, champ de bataille...)
//   E|id|type|var|zone|cx|cy|zx|zy|etat|n|m|reste|ici|ou|cap|fini_s      un evenement du continent du joueur
//     id : numero OPAQUE tire au hasard, stable le temps de l'evenement (jamais un guid) ; type : faille | nuit | convoi | autre ;
//     var : cle de variante (nuee_harpies, procession_ames, caravane... ; vide : inconnue) ; zone : nom de la zone ;
//     cx, cy : position sur la carte du continent, 0-1000 ; zx, zy : sur la carte de la zone ; vides si inconnues.
//       Type `autre` (titre 11 sans protege vu : repli de toute cle inconnue) : JAMAIS de position, sa zone seule.
//       CONVOI : JAMAIS sa position -- cx..zy sont le CENTRE DU RECTANGLE DE LA ZONE (WorldMapArea), le meme pour tout
//       convoi de cette zone ; `cap` dit seulement vers ou il va (0-7 : nord, nord-ouest, ouest... sens trigonometrique,
//       comme les SECTEURS de CoABounty) ;
//     etat : presage renforts ouverture vague champion chrono chef chef_approche coffre portail barricade attaque
//       route victoire defaite fin ; n, m : vague n/m (vague, champion), phase du chef (chef) ; reste : secondes a
//       tenir (chrono) ; ici : 1 si l'evenement est dans la zone du joueur ; ou : direction du joueur vers l'evenement
//       (0-7, zone du joueur seulement, jamais pour un convoi) ; fini_s : secondes depuis l'issue (vide : en cours),
//       une issue reste CARTE_FIN_S secondes.
//   P|id|force|zone|cx|cy|cr|zx|zy|zr|age_s   une tete mise a prix du continent du joueur
//     id OPAQUE ; force 1 Faible, 2 Moyenne, 3 Forte (le plafond de chasseurs, les mots des boutons de CoABounty) ;
//     cx, cy, cr : centre et rayon du CERCLE sur la carte du continent (0-1000, rayon en millieme de la LARGEUR) ;
//     zx, zy, zr : idem sur la carte de la zone ; age_s : age du cercle, ARRONDI A LA MINUTE. Le cercle CONTIENT la
//     cible a sa pose mais n'est pas centre sur elle (decalage tire au hasard, 0 a 70 % du rayon), il est le MEME pour
//     tous les joueurs et ne bouge pas tant que la cible y reste (ou pendant CARTE_CERCLE_MIN_S au moins, meme si elle
//     meurt, se teleporte ou entre en sanctuaire) : impossible d'en faire la moyenne. Chaque repose tire un nouvel id. JAMAIS le nom, le montant, la classe, le niveau, ni « joueur » ou « bot ». Aucune tete en lieu sans JcJ
//     (sanctuaire, capitale), hors continent, en teleportation, ni AFK (une proie de personne).
//   A|continent|evenements|primes|type|zone    resume d'un AUTRE continent (type, zone : le premier evenement)
//   N|evenements|primes|ailleurs               les TOTAUX, sans le plafond des lignes E et P (meme ligne que COMPTE)
//   F                                          fin
// Continent : celui que le CLIENT dessine (GetVirtualMapForMapAndZone : Eversong, l'Exodar... ne sont pas l'Outreterre).
// Reponse a COMPTE : UN chuchotement `COAEV\tN|evenements|primes|ailleurs` (evenements EN COURS et primes du
// continent du joueur, evenements en cours des autres continents).
//
// SOURCES -- uniquement les registres du jeu, au FIL MONDE (CMSG_MESSAGECHAT est PROCESS_THREADUNSAFE : traite par
// World::UpdateSessions, cartes a l'arret, P-137), aucune requete MySQL :
//   evObjectifs (CoaDirecteurObjectifs.inc) : un evenement = un objectif de zone (titre 1-5 faille, 6-10 nuit, 11
//     « autre » : convoi si son joueur a un protege). Le service ne pose d'objectif que pour un HUMAIN.
//   evInvoques SANS objectif (17/10, DecrireSansObjectif) : les evenements entre bots (failles, nuits, convois ancres
//     sur un bot) -- un evenement = les invoques VIVANTS d'une meme ancre sur une meme carte, ancre sans objectif ;
//     type deduit des gabarits (CARTE_FAILLE_*, CARTE_NUIT_*, CARTE_CONVOI_* : creature_template lue le 17/10),
//     etat `vague` (faille, nuit) ou route/attaque/barricade (convoi, protege vivant exige), jamais d'issue.
//     18/10 : une faille ou une nuit sans vivant (entre deux vagues) reste CARTE_SANS_OBJ_GRACE_MS (90 s) avec sa derniere
//     description et son id ; jamais un convoi.
//   Garde des primes (17/10) : l'ancre d'une tete mise a prix n'a JAMAIS de position, sa zone seule.
//   evInvoques (lot D) : position = centre des invoques vivants de l'evenement, hors renforts, hors protege, hors
//     gabarits des RARES (CARTE_RARE_MIN-MAX : rares.py, plage mesuree le 16/10) -- jamais un rare n'est lu ni envoye ;
//     protege du convoi (EntreeProtege) : sa zone, son cap, son combat.
//   evBarricades (convoi v2) : une barricade debout -> `barricade`.
//   primes (D4c, sous verrouConsignes partage) : la cible, son plafond.
// COUT : une demande CARTE toutes les CARTE_INTERVALLE_MS au plus par joueur, COMPTE toutes les CARTE_COMPTE_MS ; le
// reste est consomme sans reponse. Les registres sont bornes (64 objectifs, 64 invoques par lot...) : quelques
// centaines d'iterations par reponse. Option AiPlayerbot.Directeur.CarteEvenements (lue a chaud, sConfigMgr ; 0 par
// defaut) : 0 -> la demande est consommee, rien n'est envoye, l'addon reste muet (bouton cache).
// Journal : aucune ligne par demande ; `coa directeur carte:` a chaque cercle de prime (re)pose, et
// `coa directeur carte: sans_objectif` a chaque evenement sans objectif vu pour la premiere fois.

constexpr char CARTE_PREFIXE[] = "COAEV\t";
constexpr char CARTE_OPTION[] = "AiPlayerbot.Directeur.CarteEvenements";
constexpr size_t CARTE_MORCEAU = 180;
constexpr uint32 CARTE_INTERVALLE_MS = 4000;
constexpr uint32 CARTE_COMPTE_MS = 10000;
constexpr size_t CARTE_EVENEMENTS_MAX = 6;          // lignes E envoyees (en cours d'abord, puis issues recentes)
constexpr size_t CARTE_PRIMES_MAX = 8;              // lignes P envoyees (l'addon en montre 3)
constexpr uint32 CARTE_FIN_S = 120;                 // une issue reste 2 min sur la carte
constexpr size_t CARTE_FINS_MAX = 8;
constexpr float CARTE_PRIME_RAYON_YD = 300.0f;
constexpr float CARTE_DECALAGE_MIN = 0.0f;          // decalage du centre, en part du rayon (revue M3 : 0 a 0,7)
constexpr float CARTE_DECALAGE_MAX = 0.7f;
constexpr uint32 CARTE_CERCLE_MIN_S = 120;          // un cercle vit au moins 2 min, meme si la cible en sort
constexpr uint32 CARTE_RARE_MIN = 9100400;          // rares.py : PLAGE 9100400-9100499, R7 9100490-9100599, R8 9100600-9100673 (reserve jusqu a 9100699)
constexpr uint32 CARTE_RARE_MAX = 9100699;
constexpr size_t CARTE_SUIVIS_MAX = 128;
constexpr uint32 CARTE_CACHE_MS = 1000;             // l'etat du monde est relu au plus une fois par seconde, tous joueurs

// Rectangles des continents, LUS dans WorldMapArea.dbc du serveur (/opt/coa/server/data/dbc, 17/10/2026) : lignes
// area_id 0 (le coeur ne les charge pas : « continent 0 areas ignored », DBCStructure.h). Ids de ligne 13, 14, 466,
// 485 ; index = GetCurrentMapContinent() du client.
struct CarteContinent
{
    uint32 carte;
    uint8 index;
    float y1, y2, x1, x2;
};
constexpr CarteContinent CARTE_CONTINENTS[] = {
    {1, 1, 17066.0f, -19733.0f, 12799.0f, -11733.0f},           // Kalimdor
    {0, 2, 18171.0f, -22569.0f, 11176.0f, -15973.0f},           // Azeroth (Royaumes de l'Est)
    {530, 3, 12996.0f, -4468.0f, 5821.0f, -5821.0f},            // Expansion01 (Outreterre)
    {571, 4, 9762.5f, -9077.08f, 11106.2f, -1454.17f}};         // Northrend

CarteContinent const* ContinentDe(uint32 carte)
{
    for (CarteContinent const& c : CARTE_CONTINENTS)
        if (c.carte == carte)
            return &c;
    return nullptr;
}

// Continent AFFICHE par le client : Eversong, les Terres fantomes, Azuremyst, Bloodmyst, Lune-d'argent et l'Exodar sont
// sur la carte 530 mais dessines sur les Royaumes de l'Est ou Kalimdor (virtual_map_id de WorldMapArea, revue M1).
uint8 IndexContinent(uint32 carte, uint32 zone)
{
    CarteContinent const* c = ContinentDe(GetVirtualMapForMapAndZone(carte, zone));
    return c ? c->index : 0;
}

// Coordonnees monde -> carte du continent, en milliemes. Faux hors du rectangle (Eversong et l'Exodar sont sur la carte
// 530 mais dessines sur un autre continent : pas de repere de continent, la carte de leur zone reste juste).
bool VersContinent(uint32 carte, float x, float y, int32& cx, int32& cy)
{
    CarteContinent const* c = ContinentDe(carte);
    if (!c)
        return false;
    float const fx = (y - c->y1) / (c->y2 - c->y1);
    float const fy = (x - c->x1) / (c->x2 - c->x1);
    if (!(fx >= 0.0f && fx <= 1.0f && fy >= 0.0f && fy <= 1.0f))
        return false;
    cx = int32(std::lround(fx * 1000.0f));
    cy = int32(std::lround(fy * 1000.0f));
    return true;
}

// Rayon en milliemes de la LARGEUR de la carte du continent (largeur = axe y du monde).
int32 RayonContinent(uint32 carte, float yd)
{
    CarteContinent const* c = ContinentDe(carte);
    return c ? int32(std::lround(yd / std::fabs(c->y2 - c->y1) * 1000.0f)) : 0;
}

// Coordonnees monde -> carte de la zone (Map2ZoneCoordinates, pourcentages), en milliemes. Une zone absente de
// WorldMapArea laisse x, y INCHANGES (DBCStores.cpp) : c'est ainsi qu'on la reconnait.
bool VersZone(uint32 zone, float x, float y, int32& zx, int32& zy)
{
    float a = x, b = y;
    Map2ZoneCoordinates(a, b, zone);
    if (a == x && b == y)
        return false;
    // Revue m6 : un point juste hors du rectangle (moyenne, cercle decale) est ramene au bord ; tres loin : refuse.
    if (!(a >= -10.0f && a <= 110.0f && b >= -10.0f && b <= 110.0f))
        return false;
    zx = int32(std::lround(std::clamp(a, 0.0f, 100.0f) * 10.0f));
    zy = int32(std::lround(std::clamp(b, 0.0f, 100.0f) * 10.0f));
    return true;
}

// Rayon en milliemes de la largeur de la carte de la zone : un pas de `yd` sur l'axe y du monde.
int32 RayonZone(uint32 zone, float x, float y, float yd)
{
    float a = x, b = y, c = x, d = y + yd;
    Map2ZoneCoordinates(a, b, zone);
    Map2ZoneCoordinates(c, d, zone);
    if (a == x && b == y)
        return 0;
    return int32(std::lround(std::fabs(c - a) * 10.0f));
}

// Centre du rectangle de la zone, en coordonnees monde (Zone2MapCoordinates de 50 %, 50 %). Faux si la zone n'est
// pas dans WorldMapArea (x, y laisses a 50).
bool CentreDeZone(uint32 zone, float& x, float& y)
{
    x = 50.0f;
    y = 50.0f;
    Zone2MapCoordinates(x, y, zone);
    return !(x == 50.0f && y == 50.0f);
}

// Secteur 0-7 d'un angle (0 = nord, sens trigonometrique : 1 nord-ouest, 2 ouest... comme SECTEURS de CoABounty).
int32 Secteur(float angle)
{
    float a = std::fmod(angle, 2.0f * float(M_PI));
    if (a < 0.0f)
        a += 2.0f * float(M_PI);
    return int32(std::floor((a + float(M_PI) / 8.0f) / (float(M_PI) / 4.0f))) % 8;
}

// Le jeu : x monde vers le nord, y vers l'ouest ; l'angle de (dx, dy) est donc deja « 0 = nord, sens trigo ».
int32 SecteurVers(float dx, float dy)
{
    return Secteur(std::atan2(dy, dx));
}

char const* const CARTE_VARIANTES[] = {"",
    "classique", "nuee_harpies", "pas_des_geants", "couvee_orage", "maree_orage",              // 1-5 : la faille
    "classique", "procession_ames", "fosse_recousus", "ailes_naxxramas", "chevauchee_acherus",  // 6-10 : la nuit
    ""};                                                                                        // 11 : autre
static_assert(sizeof(CARTE_VARIANTES) / sizeof(CARTE_VARIANTES[0]) == OBJ_TITRES + 1, "un nom par titre");

// Variante d'un convoi, par le gabarit de son protege (convoi.py, VARIANTES[...]["creatures"]["protege"]).
char const* VarianteConvoi(uint32 entree)
{
    switch (entree)
    {
        case 9100016: return "caravane";
        case 9100036: return "prisonniers";
        case 9100046: return "militaire";
        default:      return "";
    }
}

char const* EtatCarte(ObjEtape e, bool chefVu)
{
    switch (e)
    {
        case ObjEtape::Presage:   return "presage";
        case ObjEtape::Renforts:  return "renforts";
        case ObjEtape::Ouverture: return "ouverture";
        case ObjEtape::Vague:     return "vague";
        case ObjEtape::Champion:  return "champion";
        case ObjEtape::Chrono:    return "chrono";
        case ObjEtape::Chef:      return chefVu ? "chef" : "chef_approche";
        case ObjEtape::Coffre:    return "coffre";
        case ObjEtape::Portail:   return "portail";
        case ObjEtape::Victoire:  return "victoire";
        case ObjEtape::Defaite:   return "defaite";
        case ObjEtape::Fin:       return "fin";
    }
    return "presage";
}

// Ce que la carte sait d'un evenement entre deux demandes. Fil monde seul.
struct CarteSuivi
{
    uint32 id = 0;          // numero opaque
    uint32 titre = 0;
    uint32 carte = 0;       // 0 avec aPos faux et zone 0 : rien de connu
    uint32 zone = 0;
    float x = 0.0f, y = 0.0f;
    bool aPos = false;      // x, y : centre des invoques (jamais pour un convoi)
    bool convoi = false;
    bool fini = false;      // une issue a ete vue : un objectif suivant (meme titre) est un AUTRE evenement
    std::string variante;
    int32 cap = -1;
};
std::unordered_map<uint32, CarteSuivi> carteSuivis;   // cle : joueur de l'evenement ; fil monde seul
// Revue m3 : numero OPAQUE tire au hasard (un compteur dirait l'ordre de creation des evenements et des cercles).
uint32 NumeroCarte()
{
    return urand(1, 999999);
}

// Un evenement decrit (en cours ou fini), independant du demandeur.
struct CarteEv
{
    uint32 id = 0;
    char const* type = "autre";
    std::string variante;
    uint32 carte = 0;
    uint32 zone = 0;
    float x = 0.0f, y = 0.0f;
    bool aPos = false;      // position PRECISE (faille, nuit) ; un convoi n'en a jamais
    std::string etat;
    uint32 n = 0, m = 0, reste = 0;
    int32 cap = -1;
    uint32 finS = 0;        // Maintenant() de l'issue ; 0 : en cours
    uint32 ancre = 0;       // guid bas du joueur ou du bot de l'evenement : JAMAIS envoye (garde des primes)
};
std::vector<CarteEv> carteFins;                        // fil monde seul, CARTE_FINS_MAX au plus

// Decrit l'evenement de l'objectif `o` (fil monde : lit evInvoques, les creatures et evBarricades).
CarteEv DecrireEvenement(EvObjectif const& o, uint32 maintenant)
{
    auto [it, neuf] = carteSuivis.try_emplace(o.joueur);
    CarteSuivi& s = it->second;
    if (neuf || s.titre != o.titre || (s.fini && !EtapeDIssue(o.etape)))
    {
        s = CarteSuivi();
        s.id = NumeroCarte();
        s.titre = o.titre;
    }
    // Les invoques de l'evenement : centre des vivants (hors renforts, protege, rares), et le protege d'un convoi.
    uint32 carte = 0, nombre = 0, zone = 0;
    float sx = 0.0f, sy = 0.0f;
    Creature* protege = nullptr;
    bool protegeVu = false;
    for (EvInvoque const& v : evInvoques)
    {
        if (v.joueur != o.joueur || (v.entree >= CARTE_RARE_MIN && v.entree <= CARTE_RARE_MAX))
            continue;
        if (v.protege)
        {
            protegeVu = true;
            if (s.variante.empty())
                s.variante = VarianteConvoi(v.entree);
            if (!v.mort && !protege)
                if (Creature* cr = CreatureInvoquee(v))
                    if (cr->IsInWorld() && cr->IsAlive())
                        protege = cr;
            continue;
        }
        if (v.mort || v.renfort)
            continue;
        Creature* cr = CreatureInvoquee(v);
        if (!cr || !cr->IsInWorld() || !cr->IsAlive())
            continue;
        if (!carte)
        {
            carte = v.carte;
            zone = cr->GetZoneId();
        }
        if (v.carte != carte)
            continue;
        sx += cr->GetPositionX();
        sy += cr->GetPositionY();
        ++nombre;
    }
    if (o.titre == OBJ_TITRES && protegeVu)
        s.convoi = true;
    // Revue M2 : le titre 11 (« autre ») est aussi le repli de toute cle inconnue (objectifs_evenement.titre_de) et un
    // convoi dont le protege n'est pas (ou plus) vu y tomberait avec le centre exact de ses bandits : un evenement
    // « autre » n'a JAMAIS de position, sa zone seulement.
    bool const sansPosition = s.convoi || o.titre == OBJ_TITRES;
    if (s.convoi)
    {
        if (protege)
        {
            s.carte = protege->GetMapId();
            s.zone = protege->GetZoneId();
            if (!protege->IsInCombat() || s.cap < 0)   // revue m7 : en combat, il fait face a l'attaquant
                s.cap = Secteur(protege->GetOrientation());
        }
    }
    if (sansPosition)
    {
        s.aPos = false;     // jamais la position d'un convoi (ni d'un « autre »)
        if (!s.convoi && nombre)
        {
            s.carte = carte;   // « autre » : la zone de ses invoques, sans leur position
            s.zone = zone;
        }
    }
    else if (nombre)
    {
        s.carte = carte;
        s.zone = zone;
        s.x = sx / float(nombre);
        s.y = sy / float(nombre);
        s.aPos = true;
    }
    // Rien de lu encore (presage) : la carte et la ZONE de l'ancre, jamais sa position.
    if (!s.zone)
        if (Player* p = HumainConnecte(o.joueur))
            if (p->IsInWorld() && CarteDeSuspension(p->GetMapId()))
            {
                s.carte = p->GetMapId();
                s.zone = p->GetZoneId();
            }

    CarteEv e;
    e.id = s.id;
    e.ancre = o.joueur;
    e.type = s.convoi ? "convoi" : o.titre >= 1 && o.titre <= 5 ? "faille" : o.titre >= 6 && o.titre <= 10 ? "nuit"
                                                                                                         : "autre";
    e.variante = s.convoi ? s.variante : std::string(o.titre <= OBJ_TITRES ? CARTE_VARIANTES[o.titre] : "");
    e.carte = s.carte;
    e.zone = s.zone;
    e.x = s.x;
    e.y = s.y;
    e.aPos = s.aPos;
    e.etat = EtatCarte(o.etape, o.chefVu);
    if (o.etape == ObjEtape::Vague || o.etape == ObjEtape::Champion)
    {
        e.n = o.n;
        e.m = o.m;
    }
    else if (o.etape == ObjEtape::Chef)
        e.n = o.phase;
    else if (o.etape == ObjEtape::Chrono)
        e.reste = o.chronoFinS > maintenant ? o.chronoFinS - maintenant : 0;
    if (s.convoi)
    {
        e.cap = s.cap;
        if (!EtapeDIssue(o.etape))
        {
            bool barricade = false;
            for (EvBarricade const& b : evBarricades)
                if (b.joueur == o.joueur && ObjetBarricade(b))
                    barricade = true;
            bool attaque = protege && protege->IsInCombat();
            if (barricade)
                e.etat = "barricade";
            else if (attaque)
                e.etat = "attaque";
            else if (protege && (o.etape == ObjEtape::Presage || o.etape == ObjEtape::Ouverture))
                e.etat = "route";
        }
    }
    if (EtapeDIssue(o.etape))
    {
        e.finS = maintenant;
        s.fini = true;
    }
    return e;
}

// Revue m4 : les suivis sans objectif d'abord ; le plafond ensuite (jamais atteint : 64 objectifs au plus).
void PurgerSuivis()
{
    for (auto it = carteSuivis.begin(); it != carteSuivis.end();)
        it = evObjectifs.count(it->first) ? std::next(it) : carteSuivis.erase(it);
    if (carteSuivis.size() > CARTE_SUIVIS_MAX)
        carteSuivis.clear();
}

// Note une ISSUE (victoire, defaite, fin) une seule fois par evenement, pour CARTE_FIN_S secondes.
void NoterFin(CarteEv const& e)
{
    for (CarteEv const& f : carteFins)
        if (f.id == e.id)
            return;
    if (carteFins.size() >= CARTE_FINS_MAX)
        carteFins.erase(carteFins.begin());   // la plus ancienne
    carteFins.push_back(e);
}

// Appele par ExecuterObjectif (CoaDirecteurObjectifs.inc) a chaque objectif pose : une issue est notee tout de suite,
// meme si personne ne regarde la carte pendant les 30 s ou elle reste affichee (ISSUE_AFFICHAGE_S du service).
void CarteObjectifPose(EvObjectif const& o)
{
    if (!EtapeDIssue(o.etape))
        return;
    if (!sConfigMgr->GetOption<bool>(CARTE_OPTION, false, false))
        return;
    NoterFin(DecrireEvenement(o, Maintenant()));
    if (carteSuivis.size() > CARTE_SUIVIS_MAX)
        PurgerSuivis();
}

// ------------------------------------------------------------------ evenements SANS objectif (17/10/2026)
// Plages des gabarits, LUES dans acore_world.creature_template le 17/10/2026 (et CARTE_VARIANTES) :
//   faille 9100001-9100004 (classique, plage 9100001-9100009), 9100201-204 nuee_harpies, 211-213 pas_des_geants, 221-223 couvee_orage,
//          231-234 maree_orage ;
//   nuit   9100301-304 (classique), 311-314 procession_ames, 321-323 fosse_recousus, 331-333 ailes_naxxramas,
//          341-345 chevauchee_acherus ;
//   convoi 9100010-9100016 caravane, 9100030-9100036 prisonniers, 9100040-9100046 militaire, 9100050-9100055 chefs.
// Toute autre entree de 9100000-9100399 : « autre » (zone seule). Au-dela de CARTE_EVENEMENT_MAX (rares, artisanat) :
// jamais lu.
constexpr uint32 CARTE_FAILLE_MIN = 9100001, CARTE_FAILLE_MAX = 9100009;
constexpr uint32 CARTE_FAILLE2_MIN = 9100200, CARTE_FAILLE2_MAX = 9100239;
constexpr uint32 CARTE_NUIT_MIN = 9100300, CARTE_NUIT_MAX = 9100349;
constexpr uint32 CARTE_CONVOI_MIN = 9100010, CARTE_CONVOI_MAX = 9100059;
constexpr uint32 CARTE_EVENEMENT_MAX = 9100399;      // au-dela (rares 9100400-9100699, Sourcier 9105000...) : ignore
constexpr size_t CARTE_SANS_OBJ_MAX = 128;

// Titre (index de CARTE_VARIANTES) d'un gabarit de faille ou de nuit ; 0 : ni l'un ni l'autre.
uint32 TitreDeGabarit(uint32 e)
{
    if (e >= CARTE_FAILLE_MIN && e <= CARTE_FAILLE_MAX)
        return 1;
    if (e >= CARTE_FAILLE2_MIN && e <= CARTE_FAILLE2_MAX)
        return 2 + (e - CARTE_FAILLE2_MIN) / 10;          // 2-5
    if (e >= CARTE_NUIT_MIN && e <= CARTE_NUIT_MAX)
        return 6 + (e - CARTE_NUIT_MIN) / 10;             // 6-10
    return 0;
}

char const* VarianteConvoiParPlage(uint32 e)
{
    switch ((e - CARTE_CONVOI_MIN) / 10)
    {
        case 0: return "caravane";      // 9100010-9100019
        case 2: return "prisonniers";   // 9100030-9100039
        case 3: return "militaire";     // 9100040-9100049
        default: return "";            // 9100050-9100059 : chefs, communs aux variantes
    }
}

constexpr uint32 CARTE_SANS_OBJ_GRACE_MS = 90000;    // 18/10 : un evenement entre bots reste 90 s sur la carte sans vivant

struct CarteSansObj
{
    uint32 id = 0;          // numero opaque
    int32 cap = -1;
    bool vu = false;        // vu a cette passe (purge)
    uint32 vuMs = 0;        // getMSTime() de la derniere passe ou il avait un vivant
    bool decrit = false;    // `dernier` est rempli (faille, nuit : jamais un convoi)
    CarteEv dernier;        // sa derniere description, renvoyee pendant la grace
};
std::unordered_map<uint64, CarteSansObj> carteSansObj;   // cle : (ancre << 32) | carte ; fil monde seul

// Les evenements dont l'ancre n'a PAS d'objectif (fil monde ; pointeurs de creature locaux a l'appel, jamais gardes).
void DecrireSansObjectif(std::vector<CarteEv>& sortie, std::vector<CarteEv>& grace)
{
    struct Groupe
    {
        uint32 joueur = 0, carte = 0;
        uint32 nombre = 0;                  // vivants comptes au centre (hors renforts, protege)
        double sx = 0.0, sy = 0.0;
        Creature* premier = nullptr;        // un vivant quelconque (repli de zone)
        std::vector<Creature*> centre;      // les vivants comptes au centre
        Creature* protege = nullptr;        // protege VIVANT du convoi
        bool convoi = false;
        std::string varConvoi;
        uint32 titres[OBJ_TITRES + 1] = {}; // votes faille/nuit par titre (hors renforts d'abord)
        uint32 titresRenfort[OBJ_TITRES + 1] = {};
    };
    std::vector<Groupe> groupes;
    for (EvInvoque const& v : evInvoques)
    {
        if (v.mort || v.entree > CARTE_EVENEMENT_MAX || (v.entree >= CARTE_RARE_MIN && v.entree <= CARTE_RARE_MAX) ||
            evObjectifs.count(v.joueur))
            continue;
        Creature* cr = CreatureInvoquee(v);
        if (!cr || !cr->IsInWorld() || !cr->IsAlive() || cr->GetMapId() != v.carte)
            continue;
        auto g = std::find_if(groupes.begin(), groupes.end(),
                              [&v](Groupe const& x) { return x.joueur == v.joueur && x.carte == v.carte; });
        if (g == groupes.end())
        {
            if (groupes.size() >= EV_INVOQUES_MAX)
                continue;
            groupes.emplace_back();
            g = std::prev(groupes.end());
            g->joueur = v.joueur;
            g->carte = v.carte;
        }
        if (!g->premier)
            g->premier = cr;
        if (v.entree >= CARTE_CONVOI_MIN && v.entree <= CARTE_CONVOI_MAX)
        {
            g->convoi = true;
            char const* var = v.protege ? VarianteConvoi(v.entree) : VarianteConvoiParPlage(v.entree);
            if (*var && (g->varConvoi.empty() || v.protege))
                g->varConvoi = var;
        }
        if (v.protege)
        {
            g->convoi = true;
            if (!g->protege)
                g->protege = cr;
            continue;
        }
        if (uint32 t = TitreDeGabarit(v.entree))
        {
            if (v.renfort)
                ++g->titresRenfort[t];
            else
                ++g->titres[t];
        }
        if (v.renfort)
            continue;
        g->sx += cr->GetPositionX();
        g->sy += cr->GetPositionY();
        g->centre.push_back(cr);
        ++g->nombre;
    }

    for (auto& [cle, s] : carteSansObj)
        s.vu = false;
    for (Groupe const& g : groupes)
    {
        // Un convoi sans protege vivant n'est plus un convoi qu'on puisse dire « en route » : rien (supposition :
        // le service clot le lot a la perte du protege).
        if (g.convoi && !g.protege)
            continue;
        uint64 const cle = (uint64(g.joueur) << 32) | g.carte;
        auto [it, neuf] = carteSansObj.try_emplace(cle);
        CarteSansObj& s = it->second;
        if (neuf)
            s.id = NumeroCarte();
        s.vu = true;

        CarteEv e;
        e.id = s.id;
        e.ancre = g.joueur;
        e.carte = g.carte;
        if (g.convoi)
        {
            e.type = "convoi";
            e.variante = g.varConvoi;
            e.zone = g.protege->GetZoneId();
            if (!g.protege->IsInCombat() || s.cap < 0)   // en combat, il fait face a l'attaquant (revue m7)
                s.cap = Secteur(g.protege->GetOrientation());
            e.cap = s.cap;
            bool barricade = false;
            for (EvBarricade const& b : evBarricades)
                if (b.joueur == g.joueur && ObjetBarricade(b))
                    barricade = true;
            e.etat = barricade ? "barricade" : g.protege->IsInCombat() ? "attaque" : "route";
        }
        else
        {
            // Titre majoritaire (hors renforts ; a defaut, les renforts) ; egalite : le plus petit.
            auto majoritaire = [](uint32 const* t)
            {
                uint32 titre = 0, voix = 0;
                for (uint32 k = 1; k <= OBJ_TITRES; ++k)
                    if (t[k] > voix)
                    {
                        voix = t[k];
                        titre = k;
                    }
                return titre;
            };
            uint32 titre = majoritaire(g.titres);
            if (!titre)
                titre = majoritaire(g.titresRenfort);
            e.type = titre >= 1 && titre <= 5 ? "faille" : titre >= 6 && titre <= 10 ? "nuit" : "autre";
            e.variante = titre ? CARTE_VARIANTES[titre] : "";
            e.etat = "vague";
            if (g.nombre)
            {
                // Zone du centre = zone du vivant le plus proche du centre (aucune requete de terrain : GetZoneId
                // d'un point creerait sa grille si elle n'est pas chargee).
                float const cx = float(g.sx / g.nombre), cy = float(g.sy / g.nombre);
                Creature const* proche = nullptr;
                float d2min = 0.0f;
                for (Creature const* c : g.centre)
                {
                    float const dx = c->GetPositionX() - cx, dy = c->GetPositionY() - cy;
                    if (!proche || dx * dx + dy * dy < d2min)
                    {
                        proche = c;
                        d2min = dx * dx + dy * dy;
                    }
                }
                e.zone = proche ? proche->GetZoneId() : 0;
                if (titre)   // « autre » : JAMAIS de position, sa zone seule (comme le titre 11)
                {
                    e.x = cx;
                    e.y = cy;
                    e.aPos = true;
                }
            }
            if (!e.zone)
                e.zone = g.premier->GetZoneId();
        }
        if (neuf)   // une ligne par evenement neuf vu par la carte (preuve d'installation : `strings`)
            LOG_INFO("playerbots", "coa directeur carte: sans_objectif id={} type={} carte={} zone={} position={}", e.id,
                     e.type, e.carte, e.zone, e.aPos ? 1 : 0);
        s.vuMs = getMSTime();
        s.decrit = !g.convoi && e.carte && e.zone;
        if (s.decrit)
            s.dernier = e;
        if (e.carte && e.zone)
            sortie.push_back(std::move(e));
    }
    // 18/10 (commanditaire : « je ne vois aucun evenement ») : une faille entre bots pose un monstre a la fois, tue en
    // quelques secondes ; entre deux vagues elle n'a aucun vivant. Elle reste CARTE_SANS_OBJ_GRACE_MS sur la carte avec
    // sa derniere description (meme id), sauf si son ancre a pris un objectif (l'evenement est alors decrit par lui).
    // Un convoi n'en a jamais : sans protege vivant, il n'est plus en route.
    uint32 const maintenantMs = getMSTime();
    for (auto it = carteSansObj.begin(); it != carteSansObj.end();)
    {
        CarteSansObj const& s = it->second;
        if (s.vu)
        {
            ++it;
            continue;
        }
        bool const garde = s.decrit && getMSTimeDiff(s.vuMs, maintenantMs) < CARTE_SANS_OBJ_GRACE_MS &&
                           !evObjectifs.count(uint32(it->first >> 32));
        if (!garde)
        {
            it = carteSansObj.erase(it);
            continue;
        }
        grace.push_back(s.dernier);   // apres les issues recentes (revue m2) : le plafond par continent les garde
        ++it;
    }
    if (carteSansObj.size() > CARTE_SANS_OBJ_MAX)
        carteSansObj.clear();
}

struct CarteCercle
{
    uint32 id = 0;          // numero opaque
    uint32 carte = 0;
    uint32 zone = 0;
    float x = 0.0f, y = 0.0f;
    uint32 poseS = 0;
};
std::unordered_map<uint32, CarteCercle> carteCercles;  // cle : joueur prime ; fil monde seul

struct CartePrime
{
    uint32 id;
    uint8 force;
    CarteCercle cercle;
};

struct CarteEtat
{
    std::vector<CarteEv> evenements;    // en cours puis issues recentes, tous continents
    std::vector<CartePrime> primes;
};

// L'etat du monde pour la carte (fil monde). Tient a jour les suivis, les issues et les cercles.
CarteEtat EtatCarteMonde()
{
    uint32 const maintenant = Maintenant();
    CarteEtat r;
    // 1. Evenements en cours (un objectif de zone par evenement).
    for (auto const& [joueur, o] : evObjectifs)
    {
        if (o.finS <= maintenant)
            continue;
        CarteEv e = DecrireEvenement(o, maintenant);
        if (e.finS)
            NoterFin(e);
        else if (e.carte && e.zone)
            r.evenements.push_back(std::move(e));
    }
    PurgerSuivis();
    // 1 bis. Evenements SANS objectif (entre bots), apres ceux des humains (plafond CARTE_EVENEMENTS_MAX par continent).
    std::vector<CarteEv> grace;
    DecrireSansObjectif(r.evenements, grace);
    // 2. Issues des CARTE_FIN_S dernieres secondes (plus recentes d'abord).
    carteFins.erase(std::remove_if(carteFins.begin(), carteFins.end(),
                                   [maintenant](CarteEv const& f) { return f.finS + CARTE_FIN_S <= maintenant; }),
                    carteFins.end());
    for (auto f = carteFins.rbegin(); f != carteFins.rend(); ++f)
        if (f->carte && f->zone)
            r.evenements.push_back(*f);
    // 2 bis. Evenements entre bots sans vivant, en grace (CARTE_SANS_OBJ_GRACE_MS) : en dernier.
    for (CarteEv& e : grace)
        r.evenements.push_back(std::move(e));
    // 3. Tetes mises a prix : copie du registre sous le verrou partage, puis la cible au fil monde.
    struct Lue
    {
        uint32 joueur, plafond, finS;
        bool afk;
    };
    std::vector<Lue> lues;
    if (nbPrimes.load(std::memory_order_relaxed))
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto const& [j, p] : primes)
            lues.push_back({j, p.plafond, p.finS, p.afk});
    }
    std::unordered_set<uint32> vues;
    for (Lue const& l : lues)
    {
        if (l.finS <= maintenant)
            continue;
        Player* t = !l.afk ? ObjectAccessor::FindPlayerByLowGUID(l.joueur) : nullptr;
        bool const lisible = t && t->IsInWorld() && !t->IsBeingTeleported() && t->IsAlive() &&
                             CarteDeSuspension(t->GetMapId()) &&
                             !sPlayerbotAIConfig.CoaLieuSansPvp(t->GetZoneId(), t->GetAreaId());
        if (!lisible)
        {
            // Revue m1 : un cercle encore jeune reste tel quel (sa disparition dirait la mort, le sanctuaire ou la
            // teleportation de la cible) ; sinon la tete n'est plus montree.
            auto v = carteCercles.find(l.joueur);
            if (v != carteCercles.end() && maintenant < v->second.poseS + CARTE_CERCLE_MIN_S)
            {
                vues.insert(l.joueur);
                uint8 const force = l.plafond >= 4 ? 3 : l.plafond == 3 ? 2 : 1;
                r.primes.push_back({v->second.id, force, v->second});
            }
            continue;
        }
        vues.insert(l.joueur);
        CarteCercle& c = carteCercles[l.joueur];
        float const dx = t->GetPositionX() - c.x, dy = t->GetPositionY() - c.y;
        bool const dedans = c.id && c.carte == t->GetMapId() &&
                            dx * dx + dy * dy <= CARTE_PRIME_RAYON_YD * CARTE_PRIME_RAYON_YD * 0.81f;
        bool const jeune = c.id && c.carte == t->GetMapId() && maintenant < c.poseS + CARTE_CERCLE_MIN_S;
        if (!dedans && !jeune)
        {
            float const angle = frand(0.0f, 2.0f * float(M_PI));
            float const d = CARTE_PRIME_RAYON_YD * frand(CARTE_DECALAGE_MIN, CARTE_DECALAGE_MAX);
            bool const neuf = !c.id;
            c.id = NumeroCarte();   // revue M3 : un nouvel id a chaque repose (rien ne relie deux cercles)
            c.carte = t->GetMapId();
            c.zone = t->GetZoneId();
            c.x = t->GetPositionX() + d * std::cos(angle);
            c.y = t->GetPositionY() + d * std::sin(angle);
            c.poseS = maintenant;
            LOG_INFO("playerbots", "coa directeur carte: cercle={} motif={} carte={} zone={}", c.id,
                     neuf ? "pose" : "sortie", c.carte, c.zone);
        }
        uint8 const force = l.plafond >= 4 ? 3 : l.plafond == 3 ? 2 : 1;
        r.primes.push_back({c.id, force, c});
    }
    for (auto it = carteCercles.begin(); it != carteCercles.end();)
        it = vues.count(it->first) ? std::next(it) : carteCercles.erase(it);
    // 4. Garde des primes (17/10) : un evenement ancre sur une tete mise a prix ne dit jamais sa position (le centre
    // de ses invoques la trahirait) ; sa zone seule, sans direction.
    if (!lues.empty())
        for (CarteEv& e : r.evenements)
            if (e.aPos && std::any_of(lues.begin(), lues.end(),
                                      [&e, maintenant](Lue const& l) { return l.joueur == e.ancre && l.finS > maintenant; }))
                e.aPos = false;
    return r;
}

std::string Millieme(bool ok, int32 v)
{
    return ok ? std::to_string(v) : std::string();
}

std::string LigneEvenement(CarteEv const& e, Player* demandeur, uint32 maintenant)
{
    int32 cx = 0, cy = 0, zx = 0, zy = 0;
    bool okC = false, okZ = false;
    if (e.aPos)
    {
        okC = VersContinent(e.carte, e.x, e.y, cx, cy);
        okZ = VersZone(e.zone, e.x, e.y, zx, zy);
    }
    else if (std::strcmp(e.type, "convoi") == 0)
    {
        // Le centre du RECTANGLE de la zone : le meme pour tout convoi de la zone, jamais sa position.
        float x = 0.0f, y = 0.0f;
        if (CentreDeZone(e.zone, x, y))
        {
            okC = VersContinent(e.carte, x, y, cx, cy);
            okZ = true;
            zx = zy = 500;
        }
    }
    bool const ici = demandeur->GetMapId() == e.carte && demandeur->GetZoneId() == e.zone;
    std::string ou;
    if (ici && e.aPos && !e.finS)
        ou = std::to_string(SecteurVers(e.x - demandeur->GetPositionX(), e.y - demandeur->GetPositionY()));
    return Acore::StringFormat("^E|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", e.id, e.type, e.variante,
                               NomZoneTraqueur(e.zone), Millieme(okC, cx), Millieme(okC, cy), Millieme(okZ, zx),
                               Millieme(okZ, zy), e.etat, e.n ? std::to_string(e.n) : std::string(),
                               e.m ? std::to_string(e.m) : std::string(),
                               e.etat == "chrono" ? std::to_string(e.reste) : std::string(), ici ? 1 : 0, ou,
                               e.cap >= 0 ? std::to_string(e.cap) : std::string(),
                               e.finS ? std::to_string(maintenant - e.finS) : std::string());
}

std::string LignePrime(CartePrime const& p, uint32 maintenant)
{
    CarteCercle const& c = p.cercle;
    int32 cx = 0, cy = 0, zx = 0, zy = 0;
    bool const okC = VersContinent(c.carte, c.x, c.y, cx, cy);
    bool const okZ = VersZone(c.zone, c.x, c.y, zx, zy);
    int32 const cr = okC ? RayonContinent(c.carte, CARTE_PRIME_RAYON_YD) : 0;
    int32 const zr = okZ ? RayonZone(c.zone, c.x, c.y, CARTE_PRIME_RAYON_YD) : 0;
    return Acore::StringFormat("^P|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", p.id, uint32(p.force), NomZoneTraqueur(c.zone),
                               Millieme(okC, cx), Millieme(okC, cy), okC ? std::to_string(cr) : std::string(),
                               Millieme(okZ, zx), Millieme(okZ, zy), okZ ? std::to_string(zr) : std::string(),
                               (maintenant - c.poseS) / 60 * 60);   // revue M3 : a la minute, jamais a la seconde
}

std::string CorpsCompte(Player* demandeur, CarteEtat const& etat);

std::string CorpsCarte(Player* demandeur, CarteEtat const& etat)
{
    uint32 const maintenant = Maintenant();
    uint8 const ici = IndexContinent(demandeur->GetMapId(), demandeur->GetZoneId());
    std::string corps = Acore::StringFormat("V|1^C|{}", uint32(ici));
    size_t n = 0;
    for (CarteEv const& e : etat.evenements)
        if (ici && IndexContinent(e.carte, e.zone) == ici && n < CARTE_EVENEMENTS_MAX)
        {
            corps += LigneEvenement(e, demandeur, maintenant);
            ++n;
        }
    n = 0;
    for (CartePrime const& p : etat.primes)
        if (ici && IndexContinent(p.cercle.carte, p.cercle.zone) == ici && n < CARTE_PRIMES_MAX)
        {
            corps += LignePrime(p, maintenant);
            ++n;
        }
    // Ailleurs : un resume par autre continent (evenements EN COURS seulement, et primes).
    for (CarteContinent const& c : CARTE_CONTINENTS)
    {
        if (c.index == ici)
            continue;
        uint32 nev = 0, npr = 0;
        CarteEv const* premier = nullptr;
        for (CarteEv const& e : etat.evenements)
            if (!e.finS && IndexContinent(e.carte, e.zone) == c.index)
            {
                ++nev;
                if (!premier)
                    premier = &e;
            }
        for (CartePrime const& p : etat.primes)
            if (IndexContinent(p.cercle.carte, p.cercle.zone) == c.index)
                ++npr;
        if (nev || npr)
            corps += Acore::StringFormat("^A|{}|{}|{}|{}|{}", uint32(c.index), nev, npr, premier ? premier->type : "",
                                         premier ? NomZoneTraqueur(premier->zone) : std::string());
    }
    // Revue M4 : les TOTAUX sans plafond (les lignes E et P sont plafonnees), pour le bouton de la mini-carte.
    corps += "^" + CorpsCompte(demandeur, etat);
    corps += "^F";
    return corps;
}

std::string CorpsCompte(Player* demandeur, CarteEtat const& etat)
{
    uint8 const ici = IndexContinent(demandeur->GetMapId(), demandeur->GetZoneId());
    uint32 nev = 0, npr = 0, ailleurs = 0;
    for (CarteEv const& e : etat.evenements)
        if (!e.finS)
            ++(ici && IndexContinent(e.carte, e.zone) == ici ? nev : ailleurs);
    for (CartePrime const& p : etat.primes)
        if (ici && IndexContinent(p.cercle.carte, p.cercle.zone) == ici)
            ++npr;
    return Acore::StringFormat("N|{}|{}|{}", nev, npr, ailleurs);
}

CarteEtat carteCache;                                  // fil monde seul
uint32 carteCacheMs = 0;
bool carteCacheOk = false;

std::mutex verrouCarte;   // ceinture, comme verrouOnglet : seuls les deux registres d'intervalle ci-dessous
std::unordered_map<uint32, uint32> derniereCarte;    // guid -> getMSTime() de la derniere CARTE servie
std::unordered_map<uint32, uint32> dernierCompte;    // guid -> getMSTime() du dernier COMPTE servi

bool CarteTropTot(std::unordered_map<uint32, uint32>& registre, uint32 guid, uint32 intervalleMs)
{
    uint32 const maintenantMs = getMSTime();
    std::lock_guard<std::mutex> garde(verrouCarte);
    auto it = registre.find(guid);
    // Tolerance d'un quart : le minuteur de l'addon arrive parfois quelques ms avant (comme le traqueur).
    if (it != registre.end() && getMSTimeDiff(it->second, maintenantMs) < intervalleMs - intervalleMs / 4)
        return true;
    if (registre.size() > 512)
        registre.clear();
    registre[guid] = maintenantMs;
    return false;
}

void EnvoyerCarte(Player* player, std::string const& texte)
{
    WorldPacket paquet;
    ChatHandler::BuildChatPacket(paquet, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(), texte, 0,
                                 player->GetName(), player->GetName(), 0, false);
    player->SendDirectMessage(&paquet);
}

// Repond a `COAEV\t<demande>` (fil monde). Option eteinte, bot, demande inconnue ou trop rapprochee : rien.
void RepondreCarte(Player* player, std::string const& demande)
{
    if (!sConfigMgr->GetOption<bool>(CARTE_OPTION, false, false) || !Humain(player) || !player->IsInWorld())
        return;
    bool const carte = demande == "CARTE";
    if (!carte && demande != "COMPTE")
        return;
    uint32 const guid = player->GetGUID().GetCounter();
    if (CarteTropTot(carte ? derniereCarte : dernierCompte, guid, carte ? CARTE_INTERVALLE_MS : CARTE_COMPTE_MS))
        return;
    // Garde de cout GLOBALE : quel que soit le nombre de joueurs, les registres sont relus au plus une fois par
    // CARTE_CACHE_MS (l'etat est independant du demandeur ; ici, ou, le continent sont calcules par ligne).
    uint32 const maintenantMs = getMSTime();
    if (!carteCacheOk || getMSTimeDiff(carteCacheMs, maintenantMs) >= CARTE_CACHE_MS)
    {
        carteCache = EtatCarteMonde();
        carteCacheMs = maintenantMs;
        carteCacheOk = true;
    }
    CarteEtat const& etat = carteCache;
    if (!carte)
    {
        EnvoyerCarte(player, std::string(CARTE_PREFIXE) + CorpsCompte(player, etat));
        return;
    }
    std::string const corps = CorpsCarte(player, etat);
    std::vector<std::string> morceaux;
    for (size_t debut = 0; debut < corps.size();)
    {
        size_t fin = std::min(corps.size(), debut + CARTE_MORCEAU);
        while (fin < corps.size() && fin > debut + 1 && (static_cast<unsigned char>(corps[fin]) & 0xC0) == 0x80)
            --fin;
        morceaux.push_back(corps.substr(debut, fin - debut));
        debut = fin;
    }
    size_t const n = morceaux.size();
    for (size_t i = 0; i < n; ++i)
        EnvoyerCarte(player, Acore::StringFormat("{}{}/{}|{}", CARTE_PREFIXE, i + 1, n, morceaux[i]));
}
