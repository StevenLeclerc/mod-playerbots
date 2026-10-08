// ------------------------------------------------------------------ lot S des evenements (08/10/2026) : le soin des bots envers le joueur
//
// FRAGMENT de CoaDirecteur.cpp : inclus par lui, DANS son espace de noms anonyme, juste AVANT le bloc du lot F
// (dont la ligne de bilan `evenement_bots` lit les compteurs d'ici). Il n'est pas une unite de compilation. Il
// ferme l'espace anonyme pour definir ses fonctions publiques, puis le rouvre : le bloc du lot F qui suit reste
// dans l'espace anonyme, comme avant. Fichier a part pour que la fusion avec les autres lots ne touche que
// quatre lignes de CoaDirecteur.cpp (coa-project/docs/EVENEMENTS-lot-S.md § 3).
//
// Demande du commanditaire (08/10) : « Finir la faille : [...] le soin des bots envers toi. »
// LU (mod-playerbots, coa-service-19092026, c329833) : la cible de TOUS les soins de playerbots est la valeur
// « party member to heal » (PartyMemberToHeal::Calculate) -- HealPartyMemberAction et ses triggers pour les
// classes de base, CoaHealAction et `coa heal` pour les classes CoA. Elle ne parcourt que bot->GetGroup() et rend
// `bot` quand il n'y a pas de groupe. Les buffs et dissipations des classes CoA parcourent NearbyGroup(bot),
// groupe seul aussi. Un inscrit n'est pas dans le groupe du joueur servi : il ne le soigne jamais.
// Correctif (option a) : le joueur servi devient un candidat de plus de ces deux parcours (PartyMemberToHeal.cpp,
// CoaAiObjectContext.cpp), par JoueurASoigner ci-dessous. Pas de groupe force (option b ecartee : butin, XP,
// groupe existant du joueur, raid ; EVENEMENTS-lot-S.md § 2.3).
//
// ETATS NEUFS :
//  - evsNbInscrits : nombre d'inscrits (presence, mission Evenement, non echue), recompte CHAQUE SECONDE au fil
//    monde par SuivreSoinsEvenement (Entretien) tant que Directeur.EvenementBots est allumee ; 0 sinon. C'est la
//    premiere garde des deux chemins chauds (valeur de soin des bots, crochet OnHeal de TOUTE unite).
//  - evsZones / evsNbZones (relecture du 08/10) : les lieux d'evenement des inscrits (carte, centre, portee =
//    rayon + laisse + EVS_PORTEE_SOIN_YD), au plus EVS_ZONES_MAX. Seconde garde, SANS verrou : un bot (ou un
//    soigneur) hors de toute zone ne prend jamais verrouConsignes. Sans elle, option allumee, CHAQUE bot du serveur
//    prenait le verrou et copiait une Affectation a chaque evaluation de « party member to heal » et a chaque
//    IsActive de CoaBuff/CoaDispel. Au-dela d'EVS_ZONES_MAX lieux distincts, evsNbZones vaut EVS_ZONES_DEBORDE :
//    pas de prefiltre (comportement d'avant, juste). Ecrit par le fil monde seul, lu par les fils de carte, qui ne
//    tournent pas en meme temps que lui (MapMgr::Update attend ses fils) ; une lecture dechiree ne pourrait que
//    faire manquer ou admettre UNE evaluation, AffectationDe tranchant toujours apres.
//  - evsSoins, evsSoinsPv, evsSoinChoix : compteurs du bilan, lus et remis a zero par la ligne evenement_bots. Hors
//    de cette ligne (relecture du 08/10), ils sont vides dans une ligne a part `evenement_soins_reliquat` :
//    au DEBUT d'une publication (premier invoque publie : ce qui precede n'appartient pas a cet evenement) et quand
//    le dernier inscrit s'en va hors publication (sinon la ligne evenement_bots de cette seconde les prend). Un
//    soin porte apres la ligne de fin n'est donc jamais reporte sur l'evenement suivant.
// Option Directeur.EvenementSoins ETEINTE : JoueurASoigner rend nul (premier test) ; seul le COMPTEUR `soins`
// vit (il doit dire 0, c'est la mesure d'avant).

std::atomic<uint32> evsNbInscrits{0};    // inscrits a un evenement ; fil monde ecrit, fils de carte lisent
std::atomic<uint32> evsSoins{0};         // soins (DealHeal) portes par un inscrit sur le joueur qu'il sert
std::atomic<uint64> evsSoinsPv{0};       // ... PV reellement rendus (gain : surplus exclu)
std::atomic<uint32> evsSoinChoix{0};     // PartyMemberToHeal a retenu le joueur servi : des EVALUATIONS

constexpr uint32 EVS_ZONES_MAX = 8;
constexpr uint32 EVS_ZONES_DEBORDE = EVS_ZONES_MAX + 1;
constexpr float EVS_PORTEE_SOIN_YD = 40.0f;   // portee de soin et de buff (NearbyGroup : 40 yd)

struct EvsZone
{
    std::atomic<uint32> carte{0};
    std::atomic<float> x{0.0f};
    std::atomic<float> y{0.0f};
    std::atomic<float> portee2{0.0f};   // (rayon + laisse + EVS_PORTEE_SOIN_YD)^2
};
EvsZone evsZones[EVS_ZONES_MAX];
std::atomic<uint32> evsNbZones{0};

bool evsPublieAvant = false;   // fil monde seul : un invoque etait publie a la seconde precedente
uint32 evsInscritsAvant = 0;   // fil monde seul

// Seconde garde, sans verrou : `u` est-il pres d'un lieu d'evenement d'inscrit ? Vrai sans prefiltre (debordement).
bool PresDUnEvenement(WorldObject const* u)
{
    uint32 const n = evsNbZones.load(std::memory_order_acquire);
    if (n == EVS_ZONES_DEBORDE)
        return true;
    uint32 const carte = u->GetMapId();
    float const ux = u->GetPositionX();
    float const uy = u->GetPositionY();
    for (uint32 i = 0; i < n && i < EVS_ZONES_MAX; ++i)
    {
        EvsZone const& z = evsZones[i];
        if (z.carte.load(std::memory_order_relaxed) != carte)
            continue;
        float const dx = ux - z.x.load(std::memory_order_relaxed);
        float const dy = uy - z.y.load(std::memory_order_relaxed);
        if (dx * dx + dy * dy <= z.portee2.load(std::memory_order_relaxed))
            return true;
    }
    return false;
}

// Fil monde : vide les compteurs dans une ligne a part, s'il y a quelque chose.
void ViderReliquatSoins(char const* quand)
{
    uint32 const soins = evsSoins.exchange(0, std::memory_order_relaxed);
    uint64 const pv = evsSoinsPv.exchange(0, std::memory_order_relaxed);
    uint32 const choix = evsSoinChoix.exchange(0, std::memory_order_relaxed);
    if (soins || pv || choix)
        LOG_INFO("playerbots", "coa directeur evenement_soins_reliquat: soins={} soins_pv={} soin_choix={} quand={}",
                 soins, pv, choix, quand);
}

// Fil monde, chaque seconde (Entretien), AVANT PublierCiblesEvenement. Un parcours des affectations sous verrou
// partage, seulement quand l'option des inscrits est allumee ; sinon un chargement et une ecriture.
void SuivreSoinsEvenement()
{
    if (!sPlayerbotAIConfig.directeurEvenementBots)
    {
        evsNbInscrits.store(0, std::memory_order_relaxed);
        evsNbZones.store(0, std::memory_order_release);
        evsPublieAvant = false;
        if (evsInscritsAvant)
            ViderReliquatSoins("option");
        evsInscritsAvant = 0;
        return;
    }
    // Debut de publication : meme condition que PublierCiblesEvenement (un invoque vivant, pas en retrait).
    bool publie = false;
    for (EvInvoque const& v : evInvoques)
        if (!v.mort && !v.retraitDepuisS)
        {
            publie = true;
            break;
        }
    bool const publieAvant = evsPublieAvant;
    if (publie && !publieAvant)
        ViderReliquatSoins("debut");
    evsPublieAvant = publie;

    uint32 const maintenant = Maintenant();
    uint32 inscrits = 0;
    uint32 nZones = 0;
    struct Lieu { uint32 carte; float x, y, portee; };
    Lieu lieux[EVS_ZONES_MAX];
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto const& [guid, f] : affectations)
        {
            if (!(f.a.presence && f.a.mission == CoaDirecteur::CoaDirMission::Evenement && f.a.missionParam &&
                  f.a.finS > maintenant))
                continue;
            ++inscrits;
            if (nZones == EVS_ZONES_DEBORDE)
                continue;
            float const portee = f.a.rayon + CoaDirecteur::MISSION_LAISSE_MARGE_YD + EVS_PORTEE_SOIN_YD;
            bool connu = false;
            for (uint32 i = 0; i < nZones; ++i)
                if (lieux[i].carte == f.a.carte && lieux[i].x == f.a.x && lieux[i].y == f.a.y)
                {
                    lieux[i].portee = std::max(lieux[i].portee, portee);
                    connu = true;
                    break;
                }
            if (connu)
                continue;
            if (nZones == EVS_ZONES_MAX)
                nZones = EVS_ZONES_DEBORDE;
            else
                lieux[nZones++] = {f.a.carte, f.a.x, f.a.y, portee};
        }
    }
    if (nZones != EVS_ZONES_DEBORDE)
        for (uint32 i = 0; i < nZones; ++i)
        {
            evsZones[i].carte.store(lieux[i].carte, std::memory_order_relaxed);
            evsZones[i].x.store(lieux[i].x, std::memory_order_relaxed);
            evsZones[i].y.store(lieux[i].y, std::memory_order_relaxed);
            evsZones[i].portee2.store(lieux[i].portee * lieux[i].portee, std::memory_order_relaxed);
        }
    evsNbZones.store(nZones, std::memory_order_release);
    evsNbInscrits.store(inscrits, std::memory_order_relaxed);
    // Dernier inscrit parti : seulement hors publication, cette seconde ET la precedente. Sinon la ligne
    // evenement_bots de PublierCiblesEvenement (qui suit, ou sa ligne de fin a cette seconde) emporte les compteurs.
    if (!inscrits && evsInscritsAvant && !publie && !publieAvant)
        ViderReliquatSoins("fin");
    evsInscritsAvant = inscrits;
}

// Crochet UnitScript::OnHeal (Unit::DealHeal : soins directs ET periodiques, Unit.cpp), fil de carte du receveur.
// Compte un soin dont le soigneur (ou le maitre de son totem, de son familier) est un BOT INSCRIT et le receveur le
// joueur HUMAIN qu'il sert, porte pres d'un lieu d'evenement. Sans inscrit : un chargement atomique ; loin de tout
// lieu : quelques comparaisons, aucun verrou. Ne modifie jamais `gain`.
void NoterSoinRecu(Unit* soigneur, Unit* receveur, uint32 gain)
{
    if (!evsNbInscrits.load(std::memory_order_relaxed) || !soigneur || !receveur || soigneur == receveur)
        return;
    Player* const r = receveur->ToPlayer();
    if (!r || !PresDUnEvenement(r))
        return;
    Player* const s = soigneur->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!s || s == r || !GET_PLAYERBOT_AI(s) || !Humain(r))
        return;
    CoaDirecteur::Affectation aff;
    if (!CoaDirecteur::AffectationDe(s->GetGUID().GetCounter(), &aff) || !aff.presence ||
        aff.mission != CoaDirecteur::CoaDirMission::Evenement || aff.missionParam != r->GetGUID().GetCounter())
        return;
    evsSoins.fetch_add(1, std::memory_order_relaxed);
    evsSoinsPv.fetch_add(gain, std::memory_order_relaxed);
}

class coa_directeur_soin_unitscript : public UnitScript
{
public:
    coa_directeur_soin_unitscript() : UnitScript("coa_directeur_soin_unitscript", true, {UNITHOOK_ON_HEAL}) {}

    void OnHeal(Unit* soigneur, Unit* receveur, uint32& gain) override { NoterSoinRecu(soigneur, receveur, gain); }
};

}  // namespace (lot S : les fonctions publiques, puis retour a l'espace anonyme)

Player* CoaDirecteur::JoueurASoigner(Player* bot)
{
    // LA QUESTION D'ABORD : option eteinte ou aucun inscrit, un test et un chargement atomique.
    if (!sPlayerbotAIConfig.directeurEvenementSoins || !evsNbInscrits.load(std::memory_order_relaxed) || !bot)
        return nullptr;
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive())
        return nullptr;
    // Seconde garde, sans verrou (relecture du 08/10) : un bot loin de tout lieu d'evenement s'arrete ici, avant
    // AffectationDe (verrou partage, recherche, copie).
    if (!PresDUnEvenement(bot))
        return nullptr;
    CoaDirecteur::Affectation aff;
    if (!CoaDirecteur::AffectationDe(bot->GetGUID().GetCounter(), &aff) || !aff.presence ||
        aff.mission != CoaDirecteur::CoaDirMission::Evenement || !aff.missionParam ||
        aff.phase != CoaDirecteur::CoaDirPhase::Sejour || aff.carte != bot->GetMapId())
        return nullptr;
    // Carte du bot seulement (ObjectAccessor::GetPlayer(WorldObject, guid) -> GetPlayer(u.GetMap(), guid)) : jamais un
    // objet d'une autre carte lu depuis ce fil (P-137).
    Player* j = ObjectAccessor::GetPlayer(*bot, ObjectGuid::Create<HighGuid::Player>(aff.missionParam));
    if (!j || j == bot || !j->IsInWorld() || !j->IsAlive() || j->GetMapId() != bot->GetMapId() || !Humain(j))
        return nullptr;
    // Laisse, comme JoueurDEvenement : un joueur qui kite hors de rayon + 30 yd n'entraine pas le soigneur hors du lieu
    // (« reach party member to heal » le suivrait).
    float const laisse = aff.rayon + CoaDirecteur::MISSION_LAISSE_MARGE_YD;
    float const dx = j->GetPositionX() - aff.x;
    float const dy = j->GetPositionY() - aff.y;
    if (dx * dx + dy * dy > laisse * laisse)
        return nullptr;
    // Assistable : sinon le sort serait refuse (BAD_TARGETS) et CastFirst le mettrait au banc 20 s, privant le bot de
    // ce soin pour LUI-MEME aussi. Ecarte : MJ, hostile, joueur en duel, joueur FFA (High Risk) vu d'un bot non FFA
    // hors de son groupe (Unit::_IsValidAssistTarget, exception P-168 reservee au groupe), sanctuaire.
    if (!bot->IsValidAssistTarget(j))
        return nullptr;
    return j;
}

void CoaDirecteur::NoterSoinChoisi()
{
    evsSoinChoix.fetch_add(1, std::memory_order_relaxed);
}

namespace
{
