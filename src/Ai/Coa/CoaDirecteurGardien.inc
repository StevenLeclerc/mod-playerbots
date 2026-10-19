// ------------------------------------------------------------------ lot G des evenements (08/10/2026) : finir la faille
//
// FRAGMENT de CoaDirecteur.cpp : inclus par lui, DANS son espace de noms anonyme, juste apres le lot P
// (CoaDirecteurPortail.inc, dont il reprend RetirerObjetPortail) et donc apres le lot D (PointApparition, SolSec,
// PointSansPvp, registre evInvoques / evLots, InvoquesVivantsDe, evNiveauForce). Il n'est pas une unite de
// compilation. Fichier a part pour que la fusion avec les lots paralleles ne touche que quelques lignes de
// CoaDirecteur.cpp (docs/EVENEMENTS-lot-G.md § 3, liste des points d'accroche).
//
// Demande du commanditaire (08/10) : « Finir la faille : le Gardien de la faille, aujourd'hui jamais sorti en
// solo ; son renfort a mi-vie ; un vrai butin au coffre ». MESURE avant le lot (faille-orage.log, 05-08/10) : en
// solo (budget 4 a 10), le Gardien n'est jamais sorti (« Gardien : budget 10 < 18 »).
//
// TOUT EST ETEINT tant que AiPlayerbot.Directeur.EvenementGardien = 0 (lue a chaque commande dans la conf
// CHARGEE, sConfigMgr, comme EvenementPortail : `.reload config` suffit ; absente : 0) :
//   1. DOSE (sous-action `invoquer` etendue, grammaire FERMEE, LireApparition) :
//        invoquer:<n>:<d>:<vue>:<arrivee>:<pv>:<degats>:<renfort>
//      pv, degats : pourcentages ENTIERS 25-100 (decimal, sans zero de tete) ; renfort : `renfort` | `sans`.
//      PV : la valeur de base UNIT_MOD_HEALTH (posee par Creature::SelectLevel) multipliee par pv/100, puis
//      SetCreateHealth et vie pleine (DoserInvoque) : le modele de stats la garde (UpdateMaxHealth relit
//      BASE_VALUE). DEGATS : CustomData de la creature (EvDose), lu par trois crochets UnitScript (melee, sort
//      direct, tic periodique non positif) quand l'ATTAQUANT est de la plage 9100000-9109999. La forme a 4
//      morceaux reste celle d'avant (aucune dose, aucun renfort). Etendue et option a 0 : refus
//      `gardien_eteint`. `renfort` exige un chef de la table RegleRenfort (le Gardien EV_GARDIEN_ENTREE ; depuis
//      la nuit des morts, aussi le Sonneur EV_SONNEUR_ENTREE) et nombre = 1, sinon `liste_blanche_ou_bornes`.
//   2. RENFORT A MI-VIE (DeclencherRenforts, chaque seconde, fil monde, depuis SuivreInvoques) : un invoque
//      arme (`renfort`) VIVANT sous EV_RENFORT_SEUIL_PCT % de ses PV fait sortir son renfort (RegleRenfort :
//      EV_RENFORT_NOMBRE Etincelles EV_RENFORT_ENTREE pour le Gardien) a ses cotes, UNE SEULE FOIS
//      (renfortFait, meme si refuse), au NIVEAU du Gardien (evNiveauForce), a SA dose, inscrites sous SON lot
//      (plafond par joueur et du royaume, levee, retrait, cibles des bots : evInvoques), lancees sur la cible du
//      Gardien (ou le joueur). Map::SummonCreature SANS invocateur : le crochet OnCreatureAddWorld ne les prend
//      pas pour des renforts SmartAI. Journal :
//      `coa directeur evenement: id= sous_action=invoquer joueur= valeur= motif=renfort mivie=1 poses= ...`
//      (motif `renfort` : le service l'ignore comme fin de lot ; `poses=0 raison=` quand rien n'est sorti).
//   3. COFFRE (sous-action `coffre`, texte exact `coffre`, valeur 0, duree_s = vie 60-600 s, point carte/x/y/z
//      controle comme `invoquer`) : pose le gameobject EV_COFFRE_ENTREE (9100024, type 3 CHEST, consommable,
//      gameobject_loot_template 9100024 par tranche de niveau du pilleur ; migration
//      exploitation/directeur/sql/2026_10_08_01_evenement_coffre_faille.sql). SEUL le joueur de l'evenement
//      l'ouvre (GlobalScript::OnAllowedToLootContainerCheck : un bot, un membre du groupe ou un autre joueur
//      recoit « LOOT_ERROR_DIDNT_KILL ») et il voit l'objet de SA tranche. Relecture du 08/10 : le coeur remplit
//      le butin AVANT ce refus (Player::SendLoot, FillLoot puis le crochet) ; si un autre l'a ouvert d'abord, le
//      butin a ete tire et COMPTE pour lui -- PlayerScript::OnPlayerBeforeSendLoot le retire alors pour le joueur
//      de l'evenement (clear + FillLoot personnel), sans quoi le coffre ne se viderait jamais. Le groupe est
//      ecarte : un butin non « pour tous » ne se partage pas entre deux pilleurs de la meme tranche (le premier
//      prend l'objet), et un coffre consommable s'efface des que le premier a tout pris (LootHandler.cpp,
//      DoLootRelease -> GO_JUST_DEACTIVATED). La part des autres humains reste celle du portail (lot P). Les
//      bots ne le visent plus (LootObject::IsLootPossible, LootObjectStack.cpp). Vide, il s'efface seul (coeur :
//      coffre consommable invoque -> Delete) : accuse `leve` raison `pille`. Sinon retire a l'echeance (`echu`),
//      au depart du joueur (`hors_ligne`, `carte`), a `lever`, `.directeur stop` et bus eteint
//      (LeverEvenementsSi). Un par joueur.
// Les crochets de fil de carte (degats, butin) ne lisent que la creature, le coffre et le joueur en cause, et une
// petite table sous evCoffresVerrou (le SEUL verrou du lot) ; le registre evCoffres est au FIL MONDE seul (P-137).
//
// NUIT DES MORTS (08/10/2026, docs/EVENEMENTS-nuit-des-morts.md) : le renfort a mi-vie n'est plus propre au Gardien.
// Une TABLE FERMEE (RegleRenfort) dit, pour chaque chef d'evenement, quel renfort il appelle et combien :
//   9100004 Gardien de la faille  -> 2 x 9100001 Etincelle d'orage (inchange)
//   9100304 Sonneur de glas       -> 2 x 9100302 Squelette du glas (nuit des morts ; 2 et non 3 depuis la relecture
//                                    du 08/10 : 2 x 0,8 PV reste sous les 2 Etincelles a 1 du Gardien)
// Une entree hors de la table refuse `renfort` (`liste_blanche_ou_bornes`), comme avant pour tout autre que le
// Gardien. Le Sonneur et ses squelettes ne sortent que si EvenementCreatures les permet (absents par defaut : inerte).
// Le COFFRE aussi a deux formes, fermees : `coffre` -> 9100024 « Coffre de la faille » (inchange) ; `coffre:9100026`
// -> 9100026 « Coffre du Sonneur » (nuit des morts : meme modele, meme butin -- son lootId est 9100024 --, son nom).
// Gabarit absent (migration 2026_10_08_02 non appliquee) : refus `gabarit`.

constexpr char const EV_GARDIEN_OPTION[] = "AiPlayerbot.Directeur.EvenementGardien";
constexpr uint32 EV_GARDIEN_ENTREE = 9100004;          // Gardien de la faille (migration 2026_10_05_01)
constexpr uint32 EV_RENFORT_ENTREE = 9100001;          // Etincelle d'orage
constexpr uint32 EV_RENFORT_NOMBRE = 2;
constexpr uint32 EV_SONNEUR_ENTREE = 9100304;          // Sonneur de glas (nuit des morts, migration 2026_10_08_02)
constexpr uint32 EV_SONNEUR_RENFORT_ENTREE = 9100302;  // Squelette du glas
constexpr uint32 EV_SONNEUR_RENFORT_NOMBRE = 2;
constexpr int32 EV_RENFORT_SEUIL_PCT = 50;
constexpr uint32 EV_RENFORT_VIE_S = 300;               // TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT, comme invoquer
constexpr float EV_RENFORT_ECART_MIN_YD = 3.0f;
constexpr float EV_RENFORT_ECART_MAX_YD = 5.0f;
constexpr uint32 EV_DOSE_MIN_PCT = 25;
constexpr uint32 EV_DOSE_MAX_PCT = 100;
constexpr uint32 EV_COFFRE_ENTREE = 9100024;           // gameobject_template (grimoire § 5.5 : 9100020-9100029)
constexpr uint32 EV_COFFRE_NUIT_ENTREE = 9100026;      // nuit des morts : « Coffre du Sonneur » (meme plage)
constexpr uint32 EV_COFFRE_VIE_MIN_S = 60;
constexpr uint32 EV_COFFRE_VIE_MAX_S = 600;
constexpr uint32 EV_COFFRE_FILET_S = 30;               // l'objet s'efface seul 30 s apres l'echeance (registre perdu)
constexpr size_t EV_COFFRE_MAX = 16;                   // tout le royaume ; un seul par joueur

// Clef de la dose de degats dans Object::CustomData. Une std::string (DataMap::Get prend std::string const&) :
// un litteral serait converti a chaque coup porte.
std::string const EV_DOSE_CLE("coa_directeur.dose");

struct EvDose : DataMap::Base
{
    uint32 degats = 100;     // pourcentage des degats infliges, 25-99 (100 : aucune entree n'est posee)
};

bool GardienAllume()
{
    return sConfigMgr->GetOption<bool>(EV_GARDIEN_OPTION, false, false);
}

// Un pourcentage de dose : 2 ou 3 chiffres decimaux, sans zero de tete, EV_DOSE_MIN_PCT a EV_DOSE_MAX_PCT.
bool LireDose(std::string const& m, uint8& pct)
{
    if (m.size() < 2 || m.size() > 3 || m[0] == '0' ||
        !std::all_of(m.begin(), m.end(), [](char x) { return x >= '0' && x <= '9'; }))
        return false;
    uint32 const v = uint32(std::stoul(m));
    if (v < EV_DOSE_MIN_PCT || v > EV_DOSE_MAX_PCT)
        return false;
    pct = uint8(v);
    return true;
}

// Les trois morceaux de la forme etendue (5e a 7e). Faux : refus `texte`.
bool LireMorceauxGardien(std::string const& pv, std::string const& degats, std::string const& renfort, EvAppLu& lu)
{
    if (!LireDose(pv, lu.pv) || !LireDose(degats, lu.degats))
        return false;
    if (renfort == "renfort")
        lu.renfort = true;
    else if (renfort != "sans" && !LireMeca(renfort, lu))   // 10/10 : `meca1-3` (CoaDirecteurBoss.inc)
        return false;
    lu.etendue = true;
    return true;
}

// Le renfort a mi-vie d'un chef d'evenement (table FERMEE, voir l'en-tete). -> vrai et (entree, nombre) si `chef`
// en appelle un ; faux sinon.
bool RegleRenfort(uint32 chef, uint32& entree, uint32& nombre)
{
    switch (chef)
    {
        case EV_GARDIEN_ENTREE: entree = EV_RENFORT_ENTREE; nombre = EV_RENFORT_NOMBRE; return true;
        case EV_SONNEUR_ENTREE: entree = EV_SONNEUR_RENFORT_ENTREE; nombre = EV_SONNEUR_RENFORT_NOMBRE; return true;
        default: return false;
    }
}

// Gardes propres a la forme etendue, AVANT toute pose. -> raison du refus ou nullptr.
char const* RefusGardien(EvAppLu const& lu, uint32 entree)
{
    if (!lu.etendue)
        return nullptr;
    if (!GardienAllume())
        return "gardien_eteint";
    uint32 renfortEntree = 0, renfortNombre = 0;
    if (lu.renfort && (!RegleRenfort(entree, renfortEntree, renfortNombre) || lu.nombre != 1))
        return "liste_blanche_ou_bornes";
    return nullptr;
}

// La dose sur une creature d'evenement fraichement posee (fil monde, cartes a l'arret : P-137). pv, degats :
// 25-100 ; 0 ou 100 : rien. Les PV gardent le modele de stats (BASE_VALUE de UNIT_MOD_HEALTH, que
// Creature::UpdateMaxHealth relit) ; la creature repart pleine.
void DoserInvoque(Creature* cr, uint8 pv, uint8 degats)
{
    if (!cr)
        return;
    if (pv >= EV_DOSE_MIN_PCT && pv < EV_DOSE_MAX_PCT)
    {
        float const base = cr->GetFlatModifierValue(UNIT_MOD_HEALTH, BASE_VALUE);
        float const dosee = std::max(1.0f, std::round(base * float(pv) / 100.0f));
        cr->SetCreateHealth(uint32(dosee));
        cr->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, dosee);   // UpdateUnitMod -> UpdateMaxHealth
        cr->SetHealth(cr->GetMaxHealth());
    }
    if (degats >= EV_DOSE_MIN_PCT && degats < EV_DOSE_MAX_PCT)
    {
        EvDose* d = new EvDose();
        d->degats = degats;
        cr->CustomData.Set(EV_DOSE_CLE, d);
    }
}

// Pourcentage des degats de l'attaquant (crochets de fil de carte) : 100 hors plage ou sans dose. Ne lit que
// l'attaquant : son CustomData n'est ecrit qu'une fois, a la pose, au fil monde (cartes a l'arret).
uint32 DegatsDoses(Unit const* attaquant)
{
    if (!attaquant || attaquant->GetTypeId() != TYPEID_UNIT)
        return 100;
    uint32 const entree = attaquant->GetEntry();
    if (entree < EV_ENTREE_MIN || entree > EV_ENTREE_MAX)
        return 100;
    EvDose const* d = attaquant->CustomData.Get<EvDose>(EV_DOSE_CLE);
    return d ? d->degats : 100;
}

// Les renforts a mi-vie (fil monde, chaque seconde, depuis SuivreInvoques APRES le releve des morts et des
// retraits, AVANT les fins de lot : un lot dont le Gardien vient de faire sortir ses Etincelles reste ouvert).
// Les invoques a renforcer sont releves d'abord (par guid) : poser pousse dans evInvoques.
void DeclencherRenforts()
{
    std::vector<ObjectGuid> aRenforcer;
    for (EvInvoque const& v : evInvoques)
        if (v.renfortArme && !v.renfortFait && !v.mort && !v.retraitDepuisS)
            aRenforcer.push_back(v.guid);
    for (ObjectGuid const& g : aRenforcer)
    {
        auto it = std::find_if(evInvoques.begin(), evInvoques.end(), [&g](EvInvoque const& v) { return v.guid == g; });
        if (it == evInvoques.end())
            continue;
        Creature* gardien = CreatureInvoquee(*it);
        if (!gardien || !gardien->IsAlive() || !gardien->IsInWorld() || !gardien->HealthBelowPct(EV_RENFORT_SEUIL_PCT))
            continue;
        EvInvoque const parent = *it;     // copie : evInvoques grandit plus bas
        it->renfortFait = true;           // UNE seule fois, meme refuse
        uint32 const pvGardien = uint32(gardien->GetHealthPct());
        auto lot = evLots.find(parent.id);
        Player* p = JoueurConnecte(parent.joueur);
        char const* raison = nullptr;
        uint32 n = 0;
        // Nuit des morts : le renfort du chef (table fermee) ; un arme hors table n'existe pas (RefusGardien).
        uint32 renfortEntree = 0, renfortNombre = 0;
        if (!RegleRenfort(parent.entree, renfortEntree, renfortNombre))
            raison = "regle";
        else if (lot == evLots.end() || lot->second.retraitDemande)
            raison = "lot_leve";
        else if (!p)
            raison = "hors_ligne";
        else
        {
            uint32 const vivants = InvoquesVivantsDe(parent.joueur);
            uint32 const cap = sPlayerbotAIConfig.directeurEvenementInvoquesParJoueur;
            size_t const placeRoyaume = evInvoques.size() < EV_INVOQUES_MAX ? EV_INVOQUES_MAX - evInvoques.size() : 0;
            uint32 const placeJoueur = vivants < cap ? cap - vivants : 0;
            n = std::min<uint32>(renfortNombre, std::min<uint32>(placeJoueur, uint32(placeRoyaume)));
            if (!n)
                raison = "plein";
        }
        uint32 poses = 0;
        if (n)
        {
            Map* map = gardien->GetMap();
            Unit* cible = gardien->GetVictim();
            if (!cible || !cible->IsAlive() || !gardien->IsValidAttackTarget(cible))
                cible = (p->IsAlive() && p->GetMapId() == gardien->GetMapId() && gardien->IsValidAttackTarget(p)) ? p
                                                                                                                 : nullptr;
            for (uint32 i = 0; i < n; ++i)
            {
                float const angle = frand(0.0f, 2.0f * float(M_PI));
                float const ecart = frand(EV_RENFORT_ECART_MIN_YD, EV_RENFORT_ECART_MAX_YD);
                float x = gardien->GetPositionX() + ecart * std::cos(angle);
                float y = gardien->GetPositionY() + ecart * std::sin(angle);
                float z = gardien->GetPositionZ();
                float sol = 0.0f;
                // Comme un point disperse d'invoquer : sol sec proche, hors lieu sans JcJ ; sinon au pied du Gardien.
                if (SolSec(map, x, y, z, sol) && std::fabs(sol - z) <= EV_SOL_ECART_MAX_YD && !PointSansPvp(map, x, y, sol))
                    z = sol;
                else
                {
                    x = gardien->GetPositionX();
                    y = gardien->GetPositionY();
                }
                evNiveauForce = parent.niveau;
                TempSummon* s = map->SummonCreature(renfortEntree, Position(x, y, z, gardien->GetOrientation()),
                                                    nullptr, EV_RENFORT_VIE_S * IN_MILLISECONDS, nullptr);
                evNiveauForce = 0;
                if (!s)
                {
                    raison = "invocation";
                    break;
                }
                s->SetTempSummonType(TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT);
                if (s->GetLevel() != parent.niveau)
                {
                    // Comme ExecuterInvoquer : jamais laisse a un autre niveau que celui du Gardien.
                    s->DespawnOrUnsummon();
                    raison = "echelle";
                    break;
                }
                DoserInvoque(s, parent.pv, parent.degats);
                EvInvoque r;
                r.id = parent.id;
                r.joueur = parent.joueur;
                r.guid = s->GetGUID();
                r.carte = parent.carte;
                r.entree = renfortEntree;
                r.niveau = parent.niveau;
                r.renfort = true;
                r.pv = parent.pv;
                r.degats = parent.degats;
                evInvoques.push_back(r);
                ++lot->second.renforts;
                ++poses;
                if (cible && s->AI() && s->IsValidAttackTarget(cible))
                    s->AI()->AttackStart(cible);
            }
        }
        LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=invoquer joueur={} valeur={} motif=renfort"
                 " mivie=1 poses={} renfort_entree={} niveau={} pv_gardien={} pv={} degats={} raison={}", parent.id,
                 parent.joueur, parent.entree, poses, renfortEntree, parent.niveau, pvGardien, parent.pv,
                 parent.degats, raison ? raison : "-");
    }
}

// ------------------------------------------------------------------ le coffre

struct EvCoffre
{
    uint32 id;               // commande `evenement` coffre
    uint32 entree = EV_COFFRE_ENTREE;   // gameobject pose (la faille, ou la nuit des morts)
    uint32 joueur;           // le joueur de l'evenement
    ObjectGuid guid;
    uint32 carte;
    uint32 finS;             // Maintenant() de l'echeance
};
std::vector<EvCoffre> evCoffres;                       // fil monde seul

// Ce que les crochets de butin (fils de carte) ont le droit de lire : le coffre et son joueur, et s'il a ete
// ouvert (pour dire `pille` plutot que `disparu`). Ecrit au fil monde (pose, fin), lu et marque par les crochets.
struct EvCoffreAcces
{
    ObjectGuid guid;
    uint32 joueur;
    bool ouvert = false;
};
std::mutex evCoffresVerrou;                            // le SEUL verrou du lot G : la table ci-dessous
std::vector<EvCoffreAcces> evCoffresAcces;             // sous evCoffresVerrou

bool EstCoffre(std::string const& t)
{
    return t == "coffre" || t.rfind("coffre:", 0) == 0;
}

void RefuserCoffre(CoaDirCommande const& c, char const* raison)
{
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=coffre joueur={} valeur={} motif={}", c.id,
             c.cible, c.valeur, raison);
    Refuser(c, raison);
}

GameObject* ObjetCoffre(EvCoffre const& co)
{
    Map* map = sMapMgr->FindBaseNonInstanceMap(co.carte);
    return map ? map->GetGameObject(co.guid) : nullptr;
}

void ExecuterCoffre(CoaDirCommande const& c)
{
    // Deux formes fermees (voir l'en-tete) : `coffre` (la faille), `coffre:9100026` (la nuit des morts).
    uint32 const entree =
        c.texte == "coffre" ? EV_COFFRE_ENTREE : c.texte == "coffre:9100026" ? EV_COFFRE_NUIT_ENTREE : 0;
    if (!TexteBusValide(c.texte) || !entree)
    {
        RefuserCoffre(c, "texte");
        return;
    }
    if (!CibleJoueurValide(c) || c.valeur != 0 || c.dureeS < EV_COFFRE_VIE_MIN_S || c.dureeS > EV_COFFRE_VIE_MAX_S)
    {
        RefuserCoffre(c, "bornes");
        return;
    }
    if (!GardienAllume())
    {
        RefuserCoffre(c, "gardien_eteint");
        return;
    }
    Player* p = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!p)
    {
        RefuserCoffre(c, "hors_ligne");
        return;
    }
    if (!Humain(p))
    {
        RefuserCoffre(c, "pas_humain");
        return;
    }
    if (!p->IsInWorld() || p->IsBeingTeleported() || !CarteDeSuspension(p->GetMapId()))
    {
        RefuserCoffre(c, "carte");
        return;
    }
    GameObjectTemplate const* gabarit = sObjectMgr->GetGameObjectTemplate(entree);
    if (!gabarit || gabarit->type != GAMEOBJECT_TYPE_CHEST)
    {
        RefuserCoffre(c, "gabarit");   // migration du lot G absente (ou appliquee apres le demarrage)
        return;
    }
    float sol = 0.0f;
    char const* raison = nullptr;
    if (!PointApparition(c, p, sol, raison))
    {
        RefuserCoffre(c, raison);
        return;
    }
    if (std::any_of(evCoffres.begin(), evCoffres.end(), [&c](EvCoffre const& x) { return x.joueur == c.cible; }))
    {
        RefuserCoffre(c, "deja_ouvert");
        return;
    }
    if (evCoffres.size() >= EV_COFFRE_MAX)
    {
        RefuserCoffre(c, "plein");
        return;
    }
    float const face = std::atan2(p->GetPositionY() - c.y, p->GetPositionX() - c.x);
    GameObject* go = p->GetMap()->SummonGameObject(entree, c.x, c.y, sol, face, 0.0f, 0.0f,
                                                   std::sin(face / 2.0f), std::cos(face / 2.0f),
                                                   c.dureeS + EV_COFFRE_FILET_S);
    if (!go)
    {
        RefuserCoffre(c, "invocation");
        return;
    }
    EvCoffre co;
    co.id = c.id;
    co.entree = entree;
    co.joueur = c.cible;
    co.guid = go->GetGUID();
    co.carte = p->GetMapId();
    co.finS = Maintenant() + c.dureeS;
    evCoffres.push_back(co);
    {
        std::lock_guard<std::mutex> garde(evCoffresVerrou);
        evCoffresAcces.push_back(EvCoffreAcces{co.guid, co.joueur, false});
    }
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=coffre joueur={} valeur=0 motif=pose vie_s={}"
             " x={:.1f} y={:.1f} z={:.1f} objet={}", c.id, c.cible, c.dureeS, c.x, c.y, sol, entree);
    Accuser(c, "execute", nullptr);
}

// ------------------------------------------------------------------ le gisement (19/10/2026)
//
// Decision du commanditaire (19/10, coa-project/docs/CONCEPTION-artisanat-gisements-evenement.md) : apres chaque
// evenement reussi, des noeuds de recolte du theme apparaissent autour du lieu et restent 30 minutes.
//   gisement:orage   Faille d'orage : Veines foudroyees (9101000) et Fleurs des sommets orageux (9101006), en alternance
//   gisement:nuit    Nuit des morts : Ossuaires mineralises (9101001) et Fleurs de tombe (9101007), en alternance
// Forme FERMEE. valeur = nombre de noeuds, 1 a EV_GISEMENT_MAX (dose par le service selon les humains presents) ;
// duree_s = vie des noeuds, EV_GISEMENT_VIE_MIN_S a EV_GISEMENT_VIE_MAX_S ; carte/x/y/z = le point de l'evenement
// (PointApparition, comme le coffre) ; jamais de rayon. Cible : l'ANCRE de l'evenement (AncreDEvenement : un humain, ou un
// bot si EvenementAncreBot ; les humains du public recoltent). Option AiPlayerbot.Directeur.EvenementGisement, lue a
// chaque commande dans la conf CHARGEE (defaut 0 : refus `gisement_eteint`).
// Les noeuds sont des gameobjects 9101000-9101099 : les bots ne les visent jamais (LootObject::IsLootPossible,
// LootObjectStack.cpp) ; leur butin exige l'outil et la competence de metier (donnees de l'artisanat). Ils s'effacent
// seuls une fois vides (coffre consommable invoque). Revue du 19/10 : un noeud ouvert puis relache sans tout prendre reste
// en GO_ACTIVATED (DoLootRelease), etat ou l'echeance du coeur (GO_READY) ne joue pas : un REGISTRE (evGisements, fil
// monde seul, P-137) le retire a l'echeance (SuivreGisements, chaque seconde, RetirerObjetPortail) ; le respawnTime du
// coeur (vie + EV_COFFRE_FILET_S) reste le filet si le registre est perdu. Un gisement n'est pas une menace : `lever` et
// `.directeur stop` le laissent vivre ses 30 minutes ; un redemarrage l'efface (objet non sauve).
constexpr char const EV_GISEMENT_OPTION[] = "AiPlayerbot.Directeur.EvenementGisement";
constexpr uint32 EV_GISEMENT_MAX = 8;
constexpr uint32 EV_GISEMENT_VIE_MIN_S = 300;
constexpr uint32 EV_GISEMENT_VIE_MAX_S = 3600;
constexpr float EV_GISEMENT_RAYON_MIN_YD = 8.0f;     // hors du pied du chef et du coffre (5 yd)
constexpr float EV_GISEMENT_RAYON_MAX_YD = 28.0f;    // visible depuis le point, a portee de course
constexpr int EV_GISEMENT_ESSAIS = 5;                // tirages par noeud (sol sec, sans toit, dans la fenetre de hauteur)
constexpr std::array<uint32, 2> EV_GISEMENT_ORAGE = {9101000, 9101006};
constexpr std::array<uint32, 2> EV_GISEMENT_NUIT = {9101001, 9101007};
constexpr size_t EV_GISEMENT_NOEUDS_MAX = 96;        // noeuds vivants dans tout le royaume (12 gisements pleins)

struct EvGisementNoeud
{
    ObjectGuid guid;
    uint32 carte;
    uint32 finS;             // Maintenant() de l'echeance
};
std::vector<EvGisementNoeud> evGisements;            // fil monde seul

bool EstGisement(std::string const& t)
{
    return t == "gisement" || t.rfind("gisement:", 0) == 0;
}

void RefuserGisement(CoaDirCommande const& c, char const* raison)
{
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=gisement joueur={} valeur={} motif={}", c.id,
             c.cible, c.valeur, raison);
    Refuser(c, raison);
}

void ExecuterGisement(CoaDirCommande const& c)
{
    std::array<uint32, 2> const* noeuds =
        c.texte == "gisement:orage" ? &EV_GISEMENT_ORAGE : c.texte == "gisement:nuit" ? &EV_GISEMENT_NUIT : nullptr;
    if (!TexteBusValide(c.texte) || !noeuds)
    {
        RefuserGisement(c, "texte");
        return;
    }
    if (!CibleJoueurValide(c) || c.valeur < 1 || c.valeur > int32(EV_GISEMENT_MAX) ||
        c.dureeS < EV_GISEMENT_VIE_MIN_S || c.dureeS > EV_GISEMENT_VIE_MAX_S)
    {
        RefuserGisement(c, "bornes");
        return;
    }
    if (!sConfigMgr->GetOption<bool>(EV_GISEMENT_OPTION, false, false))
    {
        RefuserGisement(c, "gisement_eteint");
        return;
    }
    Player* p = ObjectAccessor::FindPlayerByLowGUID(c.cible);
    if (!p)
    {
        RefuserGisement(c, "hors_ligne");
        return;
    }
    if (!AncreDEvenement(p))
    {
        RefuserGisement(c, "pas_humain");
        return;
    }
    if (!p->IsInWorld() || p->IsBeingTeleported() || !CarteDeSuspension(p->GetMapId()))
    {
        RefuserGisement(c, "carte");
        return;
    }
    for (uint32 e : *noeuds)
    {
        GameObjectTemplate const* g = sObjectMgr->GetGameObjectTemplate(e);
        if (!g || g->type != GAMEOBJECT_TYPE_CHEST)
        {
            RefuserGisement(c, "gabarit");   // donnees de l'artisanat absentes
            return;
        }
    }
    float sol = 0.0f;
    char const* raison = nullptr;
    if (!PointApparition(c, p, sol, raison))
    {
        RefuserGisement(c, raison);
        return;
    }
    Map* map = p->GetMap();
    uint32 const n = uint32(c.valeur);
    if (evGisements.size() + n > EV_GISEMENT_NOEUDS_MAX)
    {
        RefuserGisement(c, "plein");
        return;
    }
    float const depart = frand(0.0f, 2.0f * float(M_PI));
    uint32 poses = 0, rates = 0;
    for (uint32 k = 0; k < n; ++k)
    {
        uint32 const entree = (*noeuds)[k % noeuds->size()];
        bool pose = false;
        for (int essai = 0; essai < EV_GISEMENT_ESSAIS && !pose; ++essai)
        {
            // Secteurs reguliers (un noeud par secteur, ecartes les uns des autres), un peu de jeu dans chacun.
            float const angle = depart + 2.0f * float(M_PI) * (float(k) + frand(0.15f, 0.85f)) / float(n);
            float const r = frand(EV_GISEMENT_RAYON_MIN_YD, EV_GISEMENT_RAYON_MAX_YD);
            float const px = c.x + r * std::cos(angle);
            float const py = c.y + r * std::sin(angle);
            float h = 0.0f;
            if (!SolSec(map, px, py, sol + Z_OFFSET_FIND_HEIGHT, h) || std::fabs(h - sol) > TIRAGE_ECART_Z ||
                ToitAuDessus(map, px, py, h, sol) || PointSansPvp(map, px, py, h))
                continue;
            float const o = frand(0.0f, 2.0f * float(M_PI));
            if (GameObject* go = map->SummonGameObject(entree, px, py, h, o, 0.0f, 0.0f, std::sin(o / 2.0f),
                                                       std::cos(o / 2.0f), c.dureeS + EV_COFFRE_FILET_S))
            {
                evGisements.push_back(EvGisementNoeud{go->GetGUID(), map->GetId(), Maintenant() + c.dureeS});
                pose = true;
                ++poses;
            }
        }
        if (!pose)
            ++rates;
    }
    if (!poses)
    {
        RefuserGisement(c, "invocation");
        return;
    }
    CoaDirecteur::Compter(CoaDirecteur::COA_DIR_EXECUTEES);
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=gisement joueur={} valeur={} motif=pose poses={}"
             " rates={} vie_s={} x={:.1f} y={:.1f} z={:.1f} theme={}", c.id, c.cible, c.valeur, poses, rates, c.dureeS,
             c.x, c.y, sol, c.texte.substr(9));
    Accuser(c, "execute", nullptr);
}

// Chaque seconde (Entretien, fil monde) : un noeud de gisement disparu (recolte, grille dechargee) quitte le registre ; a
// l'echeance, il est retire meme entame (GO_ACTIVATED). Une ligne de journal par gisement echu n'est pas utile : compte.
void SuivreGisements()
{
    if (evGisements.empty())
        return;
    uint32 const maintenant = Maintenant();
    uint32 retires = 0;
    for (auto it = evGisements.begin(); it != evGisements.end();)
    {
        Map* map = sMapMgr->FindBaseNonInstanceMap(it->carte);
        GameObject* go = map ? map->GetGameObject(it->guid) : nullptr;
        if (go && it->finS > maintenant)
        {
            ++it;
            continue;
        }
        if (go)
        {
            RetirerObjetPortail(go);
            ++retires;
        }
        it = evGisements.erase(it);
    }
    if (retires)
        LOG_INFO("playerbots", "coa directeur evenement: sous_action=gisement motif=echu retires={} restants={}", retires,
                 evGisements.size());
}

// Fin d'un coffre (le registre l'a deja quitte) : objet retire s'il existe encore, table d'acces videe, accuse.
void FermerCoffre(EvCoffre const& co, char const* motif)
{
    bool ouvert = false;
    {
        std::lock_guard<std::mutex> garde(evCoffresVerrou);
        auto it = std::find_if(evCoffresAcces.begin(), evCoffresAcces.end(),
                               [&co](EvCoffreAcces const& a) { return a.guid == co.guid; });
        if (it != evCoffresAcces.end())
        {
            ouvert = it->ouvert;
            evCoffresAcces.erase(it);
        }
    }
    RetirerObjetPortail(ObjetCoffre(co));   // meme retrait vrai que le portail (SetRespawnTime(0) puis Despawn)
    LOG_INFO("playerbots", "coa directeur evenement: id={} sous_action=coffre joueur={} valeur=0 motif={} ouvert={}",
             co.id, co.joueur, motif, ouvert ? 1 : 0);
    CloreEvenement(co.id, co.joueur, "leve", motif);
}

bool CoffreOuvert(ObjectGuid const& g)
{
    std::lock_guard<std::mutex> garde(evCoffresVerrou);
    auto it = std::find_if(evCoffresAcces.begin(), evCoffresAcces.end(),
                           [&g](EvCoffreAcces const& a) { return a.guid == g; });
    return it != evCoffresAcces.end() && it->ouvert;
}

// Chaque seconde (Entretien, bus allume ou eteint) : objet disparu (vide : le coeur l'efface), echeance, joueur
// parti.
void SuivreCoffres()
{
    if (evCoffres.empty())
        return;
    uint32 const maintenant = Maintenant();
    std::vector<std::pair<EvCoffre, char const*>> fins;
    for (auto it = evCoffres.begin(); it != evCoffres.end();)
    {
        Player* p = JoueurConnecte(it->joueur);
        char const* motif = !ObjetCoffre(*it)                                                        ? nullptr
                            : !p                                                                     ? "hors_ligne"
                            : (p->IsInWorld() && !p->IsBeingTeleported() && p->GetMapId() != it->carte) ? "carte"
                            : it->finS <= maintenant                                                 ? "echu"
                                                                                                     : "";
        if (!motif)
            motif = CoffreOuvert(it->guid) ? "pille" : "disparu";
        if (*motif)
        {
            fins.emplace_back(*it, motif);
            it = evCoffres.erase(it);
        }
        else
            ++it;
    }
    for (auto const& [co, motif] : fins)
        FermerCoffre(co, motif);
}

// Leve les coffres pour lesquels `lever(id, joueur)` est vrai : lever, stop, bus eteint.
template <typename Predicat>
size_t LeverCoffresSi(Predicat lever, char const* raison)
{
    std::vector<EvCoffre> partis;
    for (auto it = evCoffres.begin(); it != evCoffres.end();)
    {
        if (lever(it->id, it->joueur))
        {
            partis.push_back(*it);
            it = evCoffres.erase(it);
        }
        else
            ++it;
    }
    for (EvCoffre const& co : partis)
    {
        CoaDirecteur::Compter(CoaDirecteur::COA_DIR_LEVEES);
        FermerCoffre(co, raison);
    }
    return partis.size();
}

// Le joueur `q` peut-il piller le coffre `g` ? Fil de carte (SendLoot). Le joueur HUMAIN de l'evenement, lui seul
// (relecture du 08/10 : le groupe est ecarte, voir l'en-tete) ; un coffre de l'entree absent de la table (registre
// perdu) : personne. Marque `ouvert`.
bool CoffrePermis(Player* q, ObjectGuid const& g)
{
    if (!q || !Humain(q))
        return false;
    std::lock_guard<std::mutex> garde(evCoffresVerrou);
    auto it = std::find_if(evCoffresAcces.begin(), evCoffresAcces.end(),
                           [&g](EvCoffreAcces const& a) { return a.guid == g; });
    if (it == evCoffresAcces.end() || q->GetGUID().GetCounter() != it->joueur)
        return false;
    it->ouvert = true;
    return true;
}

bool EstNotreCoffre(ObjectGuid const& g)
{
    return g.IsGameObject() && (g.GetEntry() == EV_COFFRE_ENTREE || g.GetEntry() == EV_COFFRE_NUIT_ENTREE);
}

// Degats doses (fils de carte). Seuls les attaquants de la plage portant une dose sont touches.
class coa_directeur_dose_unitscript : public UnitScript
{
public:
    coa_directeur_dose_unitscript()
        : UnitScript("coa_directeur_dose_unitscript", true,
                     {UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
                      UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK})
    {
    }

    void ModifyMeleeDamage(Unit* /*target*/, Unit* attacker, uint32& damage) override
    {
        uint32 const pct = DegatsDoses(attacker);
        if (pct < 100)
            damage = uint32(uint64(damage) * pct / 100);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        // Chefs a mecaniques (10/10, CoaDirecteurBoss.inc) : un sort de la table lance par un chef arme frappe en % des
        // PV max de la cible, AVANT la dose (qui l'abaisse ensuite, seul). Relecture : SpellDamageBonusTaken a deja ete
        // applique a l'ancienne valeur (SpellEffects.cpp:970) ; le % le repasse, sans quoi Mur protecteur, Suppression
        // de la douleur et les reductions Ascension seraient ignores (cette fonction n'appelle aucun crochet).
        uint8 mp = 0;
        if (target && spellInfo && damage > 0 && PctMeca(attacker, spellInfo->Id, false, mp))
            damage = int32(target->SpellDamageBonusTaken(attacker, spellInfo, target->CountPctFromMaxHealth(mp),
                                                         SPELL_DIRECT_DAMAGE));
        uint32 const pct = DegatsDoses(attacker);
        if (pct < 100 && damage > 0)
            damage = int32(int64(damage) * pct / 100);
    }

    // Ce crochet voit aussi les tics de SOIN (SpellAuraEffects.cpp, HandlePeriodicHealAurasTick) : un sort
    // positif n'est jamais touche.
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (spellInfo && spellInfo->IsPositive())
            return;
        uint8 mp = 0;   // 10/10 : tic d'un sort de la table lance par un chef arme -> % des PV max (CoaDirecteurBoss.inc)
        if (target && spellInfo && PctMeca(attacker, spellInfo->Id, true, mp))
            damage = target->CountPctFromMaxHealth(mp);
        uint32 const pct = DegatsDoses(attacker);
        if (pct < 100)
            damage = uint32(uint64(damage) * pct / 100);
    }
};

// BUTIN SELON LA CLASSE (09/10/2026, commanditaire : « faut quand meme que tu fasses des recompenses en fonction de
// la classe du personnage qui ramasse »). Le butin 9100024 (celui des DEUX coffres : 9100026 a le meme lootId) porte,
// par objet, une condition de CLASSE (CONDITION_CLASS, masque) en plus de sa tranche de niveau (CONDITION_LEVEL) :
// migration exploitation/directeur/sql/2026_10_09_01_evenement_butin_classe.sql, engendree par
// outils/generer-coffre-faille.py. LU dans le coeur (LootMgr.cpp) : les conditions ne sont evaluees qu'a
// l'AFFICHAGE (LootItem::AllowedForPlayer, depuis FillNonQuestNonFFAConditionalLoot pour un butin personnel) ; le
// TIRAGE d'un groupe les ignore (LootTemplate::LootGroup::Roll : LootGroupInvalidSelector ne lit que le mode de butin
// et les doublons). Un groupe de tranche qui mele les classes tirerait donc le plus souvent l'objet d'une AUTRE
// classe, invisible au pilleur : coffre vide. Et chaque groupe pose un objet (14 tranches) sous le plafond de
// MAX_NR_LOOT_ITEMS (18, LootMgr.h) : un groupe par tranche ET par classe le depasserait (les objets en trop sont
// perdus sans un mot, Loot::AddItem).
// Ces deux crochets restreignent le TIRAGE aux objets dont les conditions passent pour le pilleur, et seulement pour
// les objets de ce butin (une condition de source 4, GAMEOBJECT_LOOT_TEMPLATE, de groupe EV_COFFRE_ENTREE) :
//   - OnItemRoll : un objet a chance EXPLICITE (les bleus) non permis voit sa chance mise a 0 (le groupe passe au
//     suivant ; rendre faux viderait tout le groupe, LootGroup::Roll) ;
//   - OnBeforeLootEqualChanced : parmi les objets « a chance egale » (les verts), un tirage parmi les PERMIS seulement,
//     pose par Loot::AddItem ; aucun permis : rien (le groupe d'une autre tranche ne pose plus d'objet cache). Rend
//     faux pour CE butin (LootGroup::Roll rend alors nullptr, l'objet est deja pose) ; vrai pour tout autre.
// Sans la migration (butin du 08/10 : conditions de niveau seules), le tirage reste juste : seule la tranche du
// pilleur pose son objet. Fils de carte (SendLoot) : lecture seule des conditions chargees.
bool EstButinCoffre(LootStoreItem const* item)
{
    if (!item)
        return false;
    for (Condition const* c : item->conditions)
        if (c && c->SourceType == CONDITION_SOURCE_TYPE_GAMEOBJECT_LOOT_TEMPLATE && c->SourceGroup == EV_COFFRE_ENTREE)
            return true;
    return false;
}

bool ButinPermis(Player const* player, LootStoreItem const* item)
{
    return sConditionMgr->IsObjectMeetToConditions(const_cast<Player*>(player), item->conditions);
}

// Le droit d'ouvrir le coffre (fil de carte, Player::SendLoot). Vrai = REFUS (CALL_ENABLED_BOOLEAN_HOOKS : un
// script qui rend vrai fait rendre faux a ScriptMgr, et SendLoot repond LOOT_ERROR_DIDNT_KILL). Et le tirage du
// butin selon la classe (voir plus haut).
class coa_directeur_coffre_globalscript : public GlobalScript
{
public:
    coa_directeur_coffre_globalscript()
        : GlobalScript("coa_directeur_coffre_globalscript",
                       {GLOBALHOOK_ON_ALLOWED_TO_LOOT_CONTAINER_CHECK, GLOBALHOOK_ON_ITEM_ROLL,
                        GLOBALHOOK_ON_BEFORE_LOOT_EQUAL_CHANCED})
    {
    }

    bool OnAllowedToLootContainerCheck(Player const* player, ObjectGuid source) override
    {
        if (!EstNotreCoffre(source))
            return false;
        return !CoffrePermis(const_cast<Player*>(player), source);
    }

    bool OnItemRoll(Player const* player, LootStoreItem const* item, float& chance, Loot& /*loot*/,
                    LootStore const& store) override
    {
        if (&store != &LootTemplates_Gameobject || !player || !EstButinCoffre(item))
            return true;
        if (!ButinPermis(player, item))
            chance = 0.0f;
        return true;
    }

    bool OnBeforeLootEqualChanced(Player const* player, std::list<LootStoreItem*> entries, Loot& loot,
                                  LootStore const& store) override
    {
        if (&store != &LootTemplates_Gameobject || !player ||
            std::none_of(entries.begin(), entries.end(), [](LootStoreItem const* e) { return EstButinCoffre(e); }))
            return true;
        // Relecture du 09/10 : le tirage direct saute ce que LootGroup::Roll fait apres ce crochet. On en refait ici
        // ce qui est lisible : les DOUBLONS (LootGroupInvalidSelector : un objet equipable deja pose pour ce groupe
        // ne l'est pas deux fois) et le MODE DE BUTIN, rapporte au mode par defaut -- le crochet ne recoit pas le mode
        // du coffre ; nos coffres ne le changent jamais (GameObject::m_LootMode reste LOOT_MODE_DEFAULT, aucun
        // SetLootMode/AddLootMode dans le module) et la migration ecrit LootMode = 1 partout. Rend faux : Roll rend
        // nullptr, donc ni OnBeforeDropAddItem ni la repetition Rate.Drop.Item.GroupAmount (1 en service) -- ce
        // butin pose UN objet par tranche, quel que soit ce reglage.
        std::vector<LootStoreItem*> permis;
        for (LootStoreItem* e : entries)
        {
            if (!EstButinCoffre(e) || !(e->lootmode & LOOT_MODE_DEFAULT) || !ButinPermis(player, e))
                continue;
            if (std::any_of(loot.items.begin(), loot.items.end(), [e](LootItem const& li)
                            { return li.itemid == e->itemid && li.groupid == e->groupid; }))
                continue;
            permis.push_back(e);
        }
        if (!permis.empty())
        {
            LootStoreItem const* elu = permis[urand(0, uint32(permis.size() - 1))];
            loot.AddItem(*elu);
            // Trace propre a ce correctif (preuve d'installation lisible par strings, puis en jeu au premier coffre) :
            // une ligne par objet pose, donc une par coffre (seule la tranche du pilleur a des permis).
            LOG_INFO("playerbots", "coa directeur evenement: sous_action=coffre joueur={} valeur={} "
                     "motif=tirage_classe classe={} permis={}", player->GetGUID().GetCounter(), elu->itemid,
                     uint32(player->getClass()), uint32(permis.size()));
        }
        return false;
    }
};

// Le butin du coffre est celui du joueur de l'evenement (fil de carte, juste avant que SendLoot le montre ; le
// crochet n'est atteint que par un pilleur PERMIS, le refus du GlobalScript tombe avant). Player::SendLoot tire le
// butin a la PREMIERE ouverture, quel que soit l'ouvreur, et le compte pour lui (FillLoot personnel ->
// FillNotNormalLootFor(ouvreur) : unlootedCount compte l'objet de SA tranche) ; le refus n'arrive qu'apres. Si le
// butin n'a pas ete tire pour ce joueur (un bot ou un autre l'a ouvert avant), on le retire pour lui : rien n'a
// pu en etre pris (l'autre a ete refuse), et sans cela l'objet de l'autre tranche resterait compte, le coffre ne
// se viderait jamais (`echu ouvert=1` au lieu de `pille`). Apres ce tirage, lootOwnerGUID est le joueur : ses
// reouvertures ne retirent rien (l'objet reste en GO_ACTIVATED, DoLootRelease).
class coa_directeur_coffre_playerscript : public PlayerScript
{
public:
    coa_directeur_coffre_playerscript()
        : PlayerScript("coa_directeur_coffre_playerscript", {PLAYERHOOK_ON_BEFORE_SEND_LOOT})
    {
    }

    void OnPlayerBeforeSendLoot(Player* player, ObjectGuid lootGuid, Loot* loot) override
    {
        if (!player || !loot || !EstNotreCoffre(lootGuid) || loot->lootOwnerGUID == player->GetGUID())
            return;
        GameObject* go = player->GetMap() ? player->GetMap()->GetGameObject(lootGuid) : nullptr;
        if (!go || &go->loot != loot)
            return;
        uint32 const lootId = go->GetGOInfo()->GetLootId();
        LootType const type = loot->loot_type;   // clear() le remet a LOOT_NONE ; SendLoot l'a deja pose
        loot->clear();
        if (lootId)
            loot->FillLoot(lootId, LootTemplates_Gameobject, player, true, false, go->GetLootMode(), go);
        loot->loot_type = type;
        LOG_INFO("playerbots", "coa directeur evenement: sous_action=coffre joueur={} valeur=0 motif=retire_pour_joueur",
                 player->GetGUID().GetCounter());
    }
};
