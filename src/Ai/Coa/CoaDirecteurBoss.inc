// ------------------------------------------------------------------ chefs a mecaniques (10/10/2026)
//
// FRAGMENT de CoaDirecteur.cpp : inclus par lui, DANS son espace de noms anonyme, juste apres le lot G
// (CoaDirecteurGardien.inc, dont il reprend EvDose/DoserInvoque, GardienAllume et le patron de DeclencherRenforts) et
// donc apres le lot D (registre evInvoques / evLots, InvoquesVivantsDe, evNiveauForce, SolSec, PointSansPvp). Il n'est
// pas une unite de compilation. Conception : coa-project/docs/EVENEMENTS-boss-mecaniques.md.
//
// Demande du commanditaire (10/10, Maree d'orage v2) : « Toujours un peu decevant, le boss final, qui n'est finalement
// juste qu'un gros sac a points de vie ». Les mecaniques elles-memes (annonces, phases, coups) sont de la DONNEE
// (smart_scripts / creature_text, migrations exploitation/directeur/sql/2026_10_11_0*_evenement_boss_*.sql engendrees
// par outils/generer-boss-meca.py) ; ce fragment porte ce que SmartAI ne sait pas faire :
//
// TOUT EST ETEINT tant que AiPlayerbot.Directeur.EvenementMecaniques = 0 (lue a chaque commande dans la conf CHARGEE,
// sConfigMgr, comme EvenementGardien ; absente : 0) :
//   1. FORME : le 7e morceau de la forme etendue du lot G accepte `meca1` | `meca2` | `meca3` (palier du public : seul,
//      2 a 5 presents, 5 et plus ; calibrage.palier_public cote service). Gardes (RefusMeca, apres RefusGardien, donc
//      EvenementGardien = 1 aussi) : option a 0 -> `meca_eteint` ; chef hors de la table FERMEE RegleMeca ou nombre != 1
//      -> `liste_blanche_ou_bornes`. A la pose (ArmerMeca, fil monde, cartes a l'arret) : marque EvMeca dans le
//      CustomData du chef (lue par le crochet des degats) et SetData(1, k) a son SmartAI : ses lignes DATA_SET posent
//      le bloc de phases du public (1 / 5 / 9) et SET_EVENT_FLAG_RESET 0. Sans SetData, le chef reste en phase 0 : les
//      lignes neuves (phases 1-12) ne jouent pas.
//   2. RENFORTS A PALIERS (DeclencherPaliers, chaque seconde, fil monde, depuis SuivreInvoques a cote du renfort a
//      mi-vie dont il reprend le corps) : pour chaque palier 75 / 50 / 25 franchi (HealthBelowPct), UNE fois (meme
//      refuse), n[k] renforts de la table au NIVEAU du chef (evNiveauForce), a SA dose, inscrits sous SON lot (plafond
//      par joueur et du royaume, levee avec le lot), lances sur sa cible. Le mage du glas (9100303) n'est jamais pose
//      sous le niveau EV_MECA_MAGE_NIVEAU_MIN (Frostbolt 20792 a degats fixes). Journal :
//      `coa directeur evenement: id= sous_action=meca joueur= valeur=<chef> motif=palier palier= public= poses=
//       renfort_entree= niveau= pv_chef= raison=`.
//   3. BOUCLIER LIE (Gardien 71463, Haute-sirene 39872, Dame pleureuse 12843 : immunite a toutes les ecoles, LU) : au
//      palier 50, s'il a pose au moins un renfort, le C++ LANCE le bouclier (declenche) et dit le groupe
//      EV_MECA_TEXTE_BOUCLIER ; chaque seconde (SuivreBoucliers) il le RETIRE quand tous les renforts du palier 50 sont
//      morts (`motif=bouclier_brise`) ou a l'echeance 12 / 18 / 20 s selon le public (`bouclier_echu`), et dit le groupe
//      EV_MECA_TEXTE_BRIS. Le bouclier n'est donc jamais pose sans renfort a tuer, et jamais juge avant d'avoir ete pose
//      (la conception le faisait lancer par SmartAI : « tous morts » aurait ete vrai avant la pose des renforts).
//   4. DEGATS EN % (PctMeca, lu par les crochets de coa_directeur_dose_unitscript, AVANT la dose) : un sort de la table
//      EV_MECA_PCT lance par un chef ARME frappe `pct` % des PV max de la cible (coup direct ou tic), quel que soit le
//      niveau 10-80 (Rate.Creature.*.SpellDamage = 1 : les degats DBC plafonnent au niveau 60 ou sont fixes). Les
//      monstres des vagues qui lancent les memes sorts (Geant 9100211 : Thunderclap ; Ame 9100311 : Cry of the Dead) ne
//      portent pas la marque : inchanges.
//   5. CIBLE DES BOTS : CibleDEvenement et AssaillantsDEvenement ecartent un invoque INSENSIBLE (immunise aux degats
//      physiques ET magiques) tant qu'un autre invoque de l'evenement est valide (CoaDirecteur.cpp).
// Les crochets de degats (fils de carte) ne lisent que l'attaquant (son CustomData, ecrit une fois a la pose) et une
// table constexpr : aucun verrou. Le registre est au FIL MONDE seul (P-137).

constexpr char const EV_MECA_OPTION[] = "AiPlayerbot.Directeur.EvenementMecaniques";
constexpr uint32 EV_MECA_DATA_ID = 1;              // SmartAI : DATA_SET (1, k) -> bloc de phases du public
constexpr uint8 EV_MECA_PUBLICS = 3;               // meca1 seul, meca2 2 a 5 presents, meca3 5 et plus
constexpr uint8 EV_MECA_PALIERS[3] = {75, 50, 25};
constexpr uint8 EV_MECA_PALIER_BOUCLIER = 1;       // indice du palier 50 dans EV_MECA_PALIERS
constexpr uint8 EV_MECA_TEXTE_BRIS = 20;           // creature_text : emote « le bouclier se brise » (migrations)
constexpr uint8 EV_MECA_TEXTE_BOUCLIER = 21;       // creature_text : cri « ... me protege » (migrations)
constexpr uint32 EV_MECA_MAGE_GLAS = 9100303;      // Mage du glas : Frostbolt 20792 a degats fixes (66-75)
constexpr uint8 EV_MECA_MAGE_NIVEAU_MIN = 15;      // calibrage_nuit.MAGE_NIVEAU_MIN

struct RenfortPalier
{
    uint32 entree;           // 0 : aucun renfort a ce palier
    uint8 n[EV_MECA_PUBLICS];
};

struct RegleMecaChef
{
    uint32 chef;
    RenfortPalier palier[3];              // 75, 50, 25
    uint32 bouclier;                      // sort lie aux renforts du palier 50 ; 0 : aucun
    uint8 bouclierS[EV_MECA_PUBLICS];     // duree maximale du bouclier (s), seul / petit groupe / grand groupe
};

// TABLE FERMEE (docs/EVENEMENTS-boss-mecaniques.md § 3 ; la meme dans exploitation/directeur/boss_meca.py, CHEFS,
// relue par outils/test-directeur-boss-meca.py). Toute entree et tout renfort sont des gabarits de la plage, dans la
// liste par defaut EvenementCreatures en service.
constexpr RegleMecaChef EV_MECA_CHEFS[] = {
    // Gardien de la faille : Etincelles, Cyclone furieux ; Aether Shield 71463
    {9100004, {{9100001, {1, 2, 2}}, {9100001, {1, 2, 3}}, {9100002, {0, 1, 1}}}, 71463, {12, 18, 20}},
    // Sonneur de glas : morts releves, squelette du glas, mage du glas (jamais sous le niveau 15)
    {9100304, {{9100301, {2, 3, 4}}, {9100302, {1, 2, 2}}, {9100303, {0, 1, 2}}}, 0, {0, 0, 0}},
    // Matriarche Plume-d'orage : harpies des rafales, sorcieres des vents, hurleuse des falaises
    {9100204, {{9100201, {2, 3, 4}}, {9100202, {1, 2, 2}}, {9100203, {0, 1, 1}}}, 0, {0, 0, 0}},
    // Seigneur des cimes : rien a 75 ; geant des cimes, colosse forge-tempete (elites, a sa dose)
    {9100213, {{0, {0, 0, 0}}, {9100211, {1, 1, 2}}, {9100212, {0, 1, 1}}}, 0, {0, 0, 0}},
    // Drake de la Fleche-tempete : dragonnets d'orage ; rien a 25
    {9100223, {{9100221, {2, 3, 4}}, {9100221, {1, 2, 3}}, {0, {0, 0, 0}}}, 0, {0, 0, 0}},
    // Haute-sirene des tempetes : myrmidons, sirenes (le bouclier), guerriers de la maree ; Tidal Shield 39872
    {9100234, {{9100231, {1, 2, 3}}, {9100232, {2, 2, 3}}, {9100233, {0, 1, 2}}}, 39872, {12, 18, 20}},
    // Dame pleureuse : ames en peine (le bouclier au palier 50), banshee hurlante ; Mordresh's Shield 12843
    {9100314, {{9100311, {2, 3, 4}}, {9100311, {2, 3, 3}}, {9100313, {0, 1, 1}}}, 12843, {12, 18, 20}},
    // Boucher de la fosse (l'etalon, renforts maigres) : mort releve, recousu, abomination crocheteuse
    {9100323, {{9100301, {1, 2, 3}}, {9100321, {0, 1, 1}}, {9100322, {0, 0, 1}}}, 0, {0, 0, 0}},
    // Val'kyr de Naxxramas : geists frenetiques (50 : pendant Dark Shield, lance par SmartAI), gargouilles putrides
    {9100333, {{9100331, {2, 3, 4}}, {9100331, {3, 4, 5}}, {9100332, {0, 1, 2}}}, 0, {0, 0, 0}},
    // Commandant de la Lame d'ebene : inities d'Acherus, chevalier de la mort, inities
    {9100345, {{9100342, {1, 2, 3}}, {9100343, {0, 1, 2}}, {9100342, {1, 2, 2}}}, 0, {0, 0, 0}},
};

// Degats des sorts des mecaniques ET des routines gardees des chefs, en % des PV max de la cible
// (docs/EVENEMENTS-boss-mecaniques.md § 2.5 ; la meme table dans boss_meca.POURCENTAGES). coup : degats directs (SPELL_EFFECT_SCHOOL_DAMAGE) ; tic : aure periodique.
// 0 : non touche.
struct PctMecaSort
{
    uint32 sort;
    uint8 coup;
    uint8 tic;
};
constexpr PctMecaSort EV_MECA_PCT[] = {
    {8293, 12, 6},    // Lightning Cloud : impact, tic de 5 s
    {8147, 15, 0},    // Thunderclap
    {21071, 20, 0},   // Boulder
    {32938, 12, 0},   // Cry of the Dead
    {16340, 15, 0},   // Frost Breath
    {8364, 0, 5},     // Blizzard : tic de 2 s
    {37615, 10, 3},   // Toxic Slime : impact, tic de 3 s
    {9081, 12, 0},    // Shadow Bolt Volley
    {12058, 10, 0},   // Chain Lightning (Haute-sirene ; routine de la Matriarche et du Seigneur)
    {15117, 10, 0},   // Chain Lightning (Drake)
    // Routines gardees (relecture : 23-30 % des PV nus d'un mage aux niveaux 10-37 au DBC, 2-5 % au niveau 80).
    {20627, 8, 0},    // Lightning Breath (Drake)
    {12548, 8, 0},    // Frost Shock (Commandant)
    {16583, 6, 0},    // Shadow Shock (Dame pleureuse, Commandant)
    {32018, 6, 0},    // Call Lightning (Gardien)
};

// Clef de la marque dans Object::CustomData (comme EV_DOSE_CLE).
std::string const EV_MECA_CLE("coa_directeur.meca");

struct EvMeca : DataMap::Base
{
    uint8 public_ = 0;       // 1-3
};

bool MecaAllume()
{
    return sConfigMgr->GetOption<bool>(EV_MECA_OPTION, false, false);
}

RegleMecaChef const* RegleMeca(uint32 chef)
{
    for (RegleMecaChef const& r : EV_MECA_CHEFS)
        if (r.chef == chef)
            return &r;
    return nullptr;
}

// Le 7e morceau `meca1` | `meca2` | `meca3` (casse exacte). Faux : ce n'est pas un `meca`.
bool LireMeca(std::string const& m, EvAppLu& lu)
{
    if (m.size() != 5 || m.compare(0, 4, "meca") != 0 || m[4] < '1' || m[4] > char('0' + EV_MECA_PUBLICS))
        return false;
    lu.meca = uint8(m[4] - '0');
    return true;
}

// Gardes propres a `meca<k>`, AVANT toute pose (apres RefusGardien : la forme etendue exige deja EvenementGardien).
char const* RefusMeca(EvAppLu const& lu, uint32 entree)
{
    if (!lu.meca)
        return nullptr;
    if (!MecaAllume())
        return "meca_eteint";
    if (!RegleMeca(entree) || lu.nombre != 1)
        return "liste_blanche_ou_bornes";
    return nullptr;
}

// Fil monde, a la pose (ExecuterInvoquer, apres DoserInvoque) : la marque des degats en %, puis le palier du public
// au SmartAI (DATA_SET). Un AI qui n'est pas SmartAI ignore SetData (UnitAI::SetData est vide).
void ArmerMeca(Creature* cr, uint8 public_)
{
    if (!cr || public_ < 1 || public_ > EV_MECA_PUBLICS)
        return;
    EvMeca* m = new EvMeca();
    m->public_ = public_;
    cr->CustomData.Set(EV_MECA_CLE, m);
    if (cr->AI())
        cr->AI()->SetData(EV_MECA_DATA_ID, public_);
}

// Crochets de degats (fils de carte) : vrai et `pct` si `sort` est dans la table, avec un pourcentage pour ce genre
// de degats (tic ou coup), et que l'attaquant est un chef ARME (marque EvMeca). Ne lit que l'attaquant.
bool PctMeca(Unit const* attaquant, uint32 sort, bool tic, uint8& pct)
{
    if (!attaquant || attaquant->GetTypeId() != TYPEID_UNIT)
        return false;
    uint32 const entree = attaquant->GetEntry();
    if (entree < EV_ENTREE_MIN || entree > EV_ENTREE_MAX)
        return false;
    for (PctMecaSort const& p : EV_MECA_PCT)
    {
        if (p.sort != sort)
            continue;
        uint8 const v = tic ? p.tic : p.coup;
        if (!v || !attaquant->CustomData.Get<EvMeca>(EV_MECA_CLE))
            return false;
        pct = v;
        return true;
    }
    return false;
}

// Immunise aux degats physiques ET magiques (les boucliers des chefs : aure 39, toutes les ecoles ; Dark Shield :
// deux aures, physique et magique). Lu par le bot sur son fil de carte, creature de la meme carte.
bool Insensible(Unit const* u)
{
    return u && u->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) && u->IsImmunedToDamage(SPELL_SCHOOL_MASK_MAGIC);
}

// Le chef dit un groupe de creature_text s'il existe (migration absente : rien, sans erreur au journal).
void DireMeca(Creature* chef, uint8 groupe)
{
    if (chef && chef->AI() && sCreatureTextMgr->TextExist(chef->GetEntry(), groupe))
        chef->AI()->Talk(groupe);
}

// Pose jusqu'a `voulu` renforts `entree` autour du chef (patron de DeclencherRenforts, lot G). -> poses ; `raison` si
// moins que voulu. `parent` est une COPIE : evInvoques grandit ici.
uint32 PoserRenfortsMeca(EvInvoque const& parent, Creature* chef, uint32 entree, uint32 voulu, uint8 palier,
                         char const*& raison)
{
    auto lot = evLots.find(parent.id);
    Player* p = JoueurConnecte(parent.joueur);
    uint32 n = 0;
    if (!entree || !voulu)
        raison = "aucun";
    else if (entree == EV_MECA_MAGE_GLAS && parent.niveau < EV_MECA_MAGE_NIVEAU_MIN)
        raison = "niveau";
    else if (lot == evLots.end() || lot->second.retraitDemande)
        raison = "lot_leve";
    else if (!p)
        raison = "hors_ligne";
    else if (!sObjectMgr->GetCreatureTemplate(entree))
        raison = "gabarit";
    else
    {
        uint32 const vivants = InvoquesVivantsDe(parent.joueur);
        uint32 const cap = sPlayerbotAIConfig.directeurEvenementInvoquesParJoueur;
        size_t const placeRoyaume = evInvoques.size() < EV_INVOQUES_MAX ? EV_INVOQUES_MAX - evInvoques.size() : 0;
        uint32 const placeJoueur = vivants < cap ? cap - vivants : 0;
        n = std::min<uint32>(voulu, std::min<uint32>(placeJoueur, uint32(placeRoyaume)));
        if (n < voulu)
            raison = "plein";
    }
    uint32 poses = 0;
    if (!n)
        return 0;
    Map* map = chef->GetMap();
    Unit* cible = chef->GetVictim();
    if (!cible || !cible->IsAlive() || !chef->IsValidAttackTarget(cible))
        cible = (p->IsAlive() && p->GetMapId() == chef->GetMapId() && chef->IsValidAttackTarget(p)) ? p : nullptr;
    for (uint32 i = 0; i < n; ++i)
    {
        float const angle = frand(0.0f, 2.0f * float(M_PI));
        float const ecart = frand(EV_RENFORT_ECART_MIN_YD, EV_RENFORT_ECART_MAX_YD);
        float x = chef->GetPositionX() + ecart * std::cos(angle);
        float y = chef->GetPositionY() + ecart * std::sin(angle);
        float z = chef->GetPositionZ();
        float sol = 0.0f;
        if (SolSec(map, x, y, z, sol) && std::fabs(sol - z) <= EV_SOL_ECART_MAX_YD && !PointSansPvp(map, x, y, sol))
            z = sol;
        else
        {
            x = chef->GetPositionX();
            y = chef->GetPositionY();
        }
        evNiveauForce = parent.niveau;
        TempSummon* s = map->SummonCreature(entree, Position(x, y, z, chef->GetOrientation()), nullptr,
                                            EV_RENFORT_VIE_S * IN_MILLISECONDS, nullptr);
        evNiveauForce = 0;
        if (!s)
        {
            raison = "invocation";
            break;
        }
        s->SetTempSummonType(TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT);
        if (s->GetLevel() != parent.niveau)
        {
            s->DespawnOrUnsummon();   // comme ExecuterInvoquer : jamais a un autre niveau que celui du chef
            raison = "echelle";
            break;
        }
        DoserInvoque(s, parent.pv, parent.degats);
        EvInvoque r;
        r.id = parent.id;
        r.joueur = parent.joueur;
        r.guid = s->GetGUID();
        r.carte = parent.carte;
        r.entree = entree;
        r.niveau = parent.niveau;
        r.renfort = true;
        r.pv = parent.pv;
        r.degats = parent.degats;
        r.chefGuid = parent.guid;
        r.palier = palier;
        evInvoques.push_back(r);
        ++lot->second.renforts;
        ++poses;
        if (cible && s->AI() && s->IsValidAttackTarget(cible))
            s->AI()->AttackStart(cible);
    }
    return poses;
}

// Les renforts a paliers (fil monde, chaque seconde, depuis SuivreInvoques apres DeclencherRenforts, AVANT les fins de
// lot). Les chefs arms sont releves d'abord (par guid) : poser pousse dans evInvoques.
void DeclencherPaliers()
{
    std::vector<ObjectGuid> chefs;
    for (EvInvoque const& v : evInvoques)
        if (v.meca && !v.mort && !v.retraitDepuisS && v.paliersFaits != 0x7)
            chefs.push_back(v.guid);
    for (ObjectGuid const& g : chefs)
    {
        for (uint8 k = 0; k < 3; ++k)
        {
            // Relu a chaque palier : evInvoques a pu grandir (iterateurs invalides).
            auto it = std::find_if(evInvoques.begin(), evInvoques.end(), [&g](EvInvoque const& v) { return v.guid == g; });
            if (it == evInvoques.end() || (it->paliersFaits & (1u << k)))
                continue;
            Creature* chef = CreatureInvoquee(*it);
            if (!chef || !chef->IsAlive() || !chef->IsInWorld() || !chef->HealthBelowPct(EV_MECA_PALIERS[k]))
                break;   // paliers dans l'ordre : sous 50 implique sous 75
            it->paliersFaits |= uint8(1u << k);   // UNE fois, meme refuse
            EvInvoque const parent = *it;          // copie : evInvoques grandit plus bas
            RegleMecaChef const* regle = RegleMeca(parent.entree);
            uint8 const pub = std::clamp<uint8>(parent.meca, 1, EV_MECA_PUBLICS);
            char const* raison = nullptr;
            uint32 entree = 0, voulu = 0, poses = 0;
            if (!regle)
                raison = "regle";
            else
            {
                entree = regle->palier[k].entree;
                voulu = entree ? regle->palier[k].n[pub - 1] : 0;
                poses = PoserRenfortsMeca(parent, chef, entree, voulu, EV_MECA_PALIERS[k], raison);
            }
            LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=meca joueur={} valeur={} motif=palier"
                     " palier={} public={} poses={} voulus={} renfort_entree={} niveau={} pv_chef={} raison={}", parent.id,
                     parent.joueur, parent.entree, uint32(EV_MECA_PALIERS[k]), uint32(pub), poses, voulu, entree,
                     uint32(parent.niveau), uint32(chef->GetHealthPct()), raison ? raison : "-");
            if (k != EV_MECA_PALIER_BOUCLIER || !regle || !regle->bouclier)
                continue;
            // Bouclier lie : seulement s'il y a des renforts a tuer (sinon il ne tomberait qu'a l'echeance).
            if (!poses)
            {
                LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=meca joueur={} valeur={}"
                         " motif=bouclier_non_pose sort={} raison=sans_renfort", parent.id, parent.joueur, parent.entree,
                         regle->bouclier);
                continue;
            }
            // Declenche (aucune incantation) : l'aure est posee dans l'appel ; SuivreBoucliers la juge des la passe
            // suivante (absente alors : `bouclier_perdu`).
            bool const pose = chef->CastSpell(chef, regle->bouclier, true) == SPELL_CAST_OK;
            auto it2 = std::find_if(evInvoques.begin(), evInvoques.end(), [&g](EvInvoque const& v) { return v.guid == g; });
            if (it2 != evInvoques.end() && pose)
            {
                it2->bouclierActif = true;
                it2->bouclierDepuisS = std::max<uint32>(Maintenant(), 1);
                DireMeca(chef, EV_MECA_TEXTE_BOUCLIER);
            }
            LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=meca joueur={} valeur={} motif={}"
                     " sort={} renforts={} duree_max_s={}", parent.id, parent.joueur, parent.entree,
                     pose ? "bouclier_pose" : "bouclier_non_pose", regle->bouclier, poses,
                     uint32(regle->bouclierS[pub - 1]));
        }
    }
}

// Le bouclier lie (fil monde, chaque seconde, apres DeclencherPaliers) : retire quand ses renforts du palier 50 sont
// tous morts (ou partis), ou a l'echeance du public. Un bouclier deja disparu (dissipe, chef mort) n'est plus suivi.
void SuivreBoucliers()
{
    uint32 const maintenant = Maintenant();
    for (EvInvoque& v : evInvoques)
    {
        if (!v.bouclierActif)
            continue;
        RegleMecaChef const* regle = RegleMeca(v.entree);
        Creature* chef = CreatureInvoquee(v);
        char const* motif = nullptr;
        if (!regle || !regle->bouclier || v.mort || !chef || !chef->IsAlive() || !chef->HasAura(regle->bouclier))
            motif = "bouclier_perdu";
        else
        {
            ObjectGuid const g = v.guid;
            bool const gardiens = std::any_of(evInvoques.begin(), evInvoques.end(), [&g](EvInvoque const& r)
                                              { return r.chefGuid == g && r.palier == EV_MECA_PALIERS[EV_MECA_PALIER_BOUCLIER] && !r.mort; });
            uint8 const pub = std::clamp<uint8>(v.meca, 1, EV_MECA_PUBLICS);
            if (!gardiens)
                motif = "bouclier_brise";
            else if (maintenant - v.bouclierDepuisS >= regle->bouclierS[pub - 1])
                motif = "bouclier_echu";
        }
        if (!motif)
            continue;
        v.bouclierActif = false;
        if (chef && regle && regle->bouclier && chef->HasAura(regle->bouclier))
        {
            chef->RemoveAurasDueToSpell(regle->bouclier);
            DireMeca(chef, EV_MECA_TEXTE_BRIS);
        }
        LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=meca joueur={} valeur={} motif={} sort={}"
                 " tenu_s={}", v.id, v.joueur, v.entree, motif, regle ? regle->bouclier : 0,
                 maintenant - v.bouclierDepuisS);
    }
}
