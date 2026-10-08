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
//    garde atomique des deux chemins chauds (valeur de soin des bots, crochet OnHeal de TOUTE unite).
//  - evsSoins, evsSoinsPv, evsSoinChoix : compteurs du bilan, lus et remis a zero par la ligne evenement_bots.
// Option Directeur.EvenementSoins ETEINTE : JoueurASoigner rend nul (premier test) ; seul le COMPTEUR `soins`
// vit (il doit dire 0, c'est la mesure d'avant).

std::atomic<uint32> evsNbInscrits{0};    // inscrits a un evenement ; fil monde ecrit, fils de carte lisent
std::atomic<uint32> evsSoins{0};         // soins (DealHeal) portes par un inscrit sur le joueur qu'il sert
std::atomic<uint64> evsSoinsPv{0};       // ... PV reellement rendus (gain : surplus exclu)
std::atomic<uint32> evsSoinChoix{0};     // PartyMemberToHeal a retenu le joueur servi : des EVALUATIONS

// Fil monde, chaque seconde (Entretien). Un parcours des affectations sous verrou partage, seulement quand l'option
// des inscrits est allumee ; sinon un chargement et une ecriture.
void SuivreSoinsEvenement()
{
    if (!sPlayerbotAIConfig.directeurEvenementBots)
    {
        evsNbInscrits.store(0, std::memory_order_relaxed);
        return;
    }
    uint32 const maintenant = Maintenant();
    uint32 inscrits = 0;
    {
        std::shared_lock<std::shared_mutex> garde(verrouConsignes);
        for (auto const& [guid, f] : affectations)
            if (f.a.presence && f.a.mission == CoaDirecteur::CoaDirMission::Evenement && f.a.missionParam &&
                f.a.finS > maintenant)
                ++inscrits;
    }
    evsNbInscrits.store(inscrits, std::memory_order_relaxed);
}

// Crochet UnitScript::OnHeal (Unit::DealHeal : soins directs ET periodiques, Unit.cpp), fil de carte du receveur.
// Compte un soin dont le soigneur (ou le maitre de son totem, de son familier) est un BOT INSCRIT et le receveur le
// joueur HUMAIN qu'il sert. Sans inscrit : un chargement atomique. Ne modifie jamais `gain`.
void NoterSoinRecu(Unit* soigneur, Unit* receveur, uint32 gain)
{
    if (!evsNbInscrits.load(std::memory_order_relaxed) || !soigneur || !receveur || soigneur == receveur)
        return;
    Player* const r = receveur->ToPlayer();
    if (!r)
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
