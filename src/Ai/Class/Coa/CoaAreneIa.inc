/*
 * CoaArene IA -- lots L0 et L1 (10/10/2026). Conception : coa-project/docs/CONCEPTION-arene-1v1.md § 3 a 5.
 *
 * INCLUS DANS L'ESPACE DE NOMS ANONYME DE CoaAiObjectContext.cpp, juste avant CoaAttackAction : ce fichier se
 * sert de Usable, KnownAbilities, StrictCheck, CastFirst, DropUncastableHere, EffectiveSpell, IsAttack et des
 * KIND_* de ce fichier, qui n'ont pas d'en-tete. Il ne se compile pas seul.
 *
 * TOUT EST INERTE PAR DEFAUT. Les quatre options AiPlayerbot.CoaArene.* valent 0/faux/100 ; a 0, la strategie
 * « coa arene » n'est pas composee (AiFactory.cpp), CastFirst et CoaAttackAction sortent sur un test de booleen.
 *
 * CE QUI EST MESURE ET CE QUI NE L'EST PAS. Lus dans Spell.dbc le 10/10 (coa-project/outils/sonde-kit-sorts.py <ids>) :
 *   Quick Shot 500074/572727    portee 5-30 (SpellRange 54, drapeau DEFAULT), instantane, +1 Advantage
 *   Toxic Dart 807237/807324    5-30, instantane, recharge 6 s, poison + silence 2,5 s (807821), +1 Advantage
 *   Serrated Shot 500073 (16)   5-30, saignement, CONSOMME l'Advantage (CasterAuraSpell 804329)
 *   Skullpiercer 802036/501715  0-30, instantane, CONSOMME toute la pile d'Advantage (804329, 5 au plus),
 *                               degats accrus par charge (AscensionClassMechanics.cpp:1412-1437)
 *   Wild Strike 800083          melee 5 yd, +1 Advantage
 *   Elude 801345 (4)            furtivite, hors combat seulement, recharge 10 s, rompue par un sort lance
 *   Flank 804940 (4)            melee 5 yd, SPELL_ATTR0_ONLY_STEALTHED : une ouverture, pas une rupture
 *   Blackjack 520568 (12)       8 yd, MOD_STUN (« sap », 8 s contre un joueur), cible hors combat seulement
 *   Dust Toss 807820 (14)       15 yd, recharge 8 s, -100 % de toucher + Dazed 1604 (-50 % de vitesse, 4 s),
 *                               applique par spell_ascension_ranger_dust_toss (AscensionRangerTalents.cpp:612)
 * Aucun sort du Ranger 1-20 n'a de temps d'incantation (index 18 = -1 000 000 ms, ramene a 0 par CalcCastTime).
 * Le SEUL sort du kit 10-13 qui cree de la distance en combat est... aucun : Dust Toss arrive au niveau 14.
 * D'ou la regle de L1 : on ne recule que si la cible NE PEUT PAS suivre ; sinon on reste et on frappe
 * (Skullpiercer et Wild Strike portent au contact). Fuir une cible a pleine vitesse, c'est lui tourner le dos
 * (tous les tirs exigent d'etre face a la cible, FacingCasterFlags = 1) et prendre ses coups sans rien rendre.
 *
 * AUCUN ID DE SORT N'EST ECRIT ICI. Rupture, ouverture, consommateurs et pile se reconnaissent a leurs champs
 * DBC (effets, aura appliquee, CasterAuraSpell, StackAmount, ONLY_STEALTHED). Une autre classe ou un autre rang
 * qui porte les memes champs en profite ; la classe Ranger n'est testee que pour borner L1.
 *
 * FIL. Tout ce qui suit tourne dans la mise a jour d'IA du bot (fil monde ou fil de carte du bot), et ne lit
 * que le bot, sa cible et son adversaire, qui sont sur la meme carte (P-137). Aucun etat partage entre bots.
 */

enum CoaAreneCadre : uint8
{
    COA_ARENE_AUCUN = 0,
    COA_ARENE_ARENE = 1,
    COA_ARENE_DUEL = 2,
    COA_ARENE_MONDE = 3
};

char const* CoaAreneCadreNom(uint8 cadre)
{
    switch (cadre)
    {
        case COA_ARENE_ARENE: return "arene";
        case COA_ARENE_DUEL:  return "duel";
        case COA_ARENE_MONDE: return "monde";
        default:              return "aucun";
    }
}

// Apres la derniere trace d'engagement, combien de temps il reste ouvert (L0 et bilan). Couvre le cas mesure le
// 02/10 : « drop target », moteur non-combat, et « food » x23 pendant que le joueur frappe encore.
constexpr uint32 CoaAreneGraceMs = 10 * IN_MILLISECONDS;
// Une mise a jour d'etat par tick : la multiplication, les declencheurs et CastFirst la demandent plusieurs fois
// dans le meme tour. Un tick d'IA dure au moins reactDelay (100 ms et plus).
constexpr uint32 CoaAreneMajMs = 50;
constexpr uint32 CoaAreneTenirJournalMs = 5 * IN_MILLISECONDS;
// Entre deux essais de recul rates : sans ce delai, un bot adosse a un mur referait les sept calculs de chemin
// a chaque tick.
constexpr uint32 CoaAreneReculReessaiMs = 1500;
constexpr uint32 CoaAreneOuvertureReessaiMs = 3 * IN_MILLISECONDS;
constexpr uint32 CoaAreneOuvertureApprocheMs = IN_MILLISECONDS;
constexpr uint32 CoaAreneOuvertureApresSuccesMs = 10 * IN_MILLISECONDS;
constexpr uint32 CoaArenePreparationReessaiMs = 5 * IN_MILLISECONDS;
// La cible ne peut pas suivre si elle court a 70 % de la vitesse du bot ou moins (Dazed : 50 %).
constexpr float CoaAreneVitesseRelative = 0.7f;

CoaAreneEtat& CoaAreneEtatDe(PlayerbotAI* botAI)
{
    return static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->arene;
}

bool CoaAreneArmee()
{
    return sPlayerbotAIConfig.coaAreneIaCommune || sPlayerbotAIConfig.coaAreneIaRanger;
}

// L1 ne concerne que le Ranger de degats a distance (specialisations 28 et 29, CoaSpecialization.cpp:138).
bool CoaAreneEstRangerDistance(Player* bot)
{
    return sPlayerbotAIConfig.coaAreneIaRanger && bot->getClass() == CLASS_RANGER &&
           GetCoaRole(bot) == CoaRole::Dps && GetCoaStyle(bot) != CoaStyle::Melee;
}

// Distance « de combat » : celle que le coeur compare aux portees des sorts (IsWithinCombatRange), rayons de
// combat deduits. Le journal, lui, ecrit la distance de centre a centre, celle de sonde-combat-pvp.py.
float CoaAreneDistanceCombat(Unit const* a, Unit const* b)
{
    return std::max(0.0f, a->GetExactDist2d(b) - a->GetCombatReach() - b->GetCombatReach());
}

// La plage de tir du bot, lue sur ses propres sorts de DEGATS a distance (25 yd et plus) : [plus grande portee
// minimale, plus petite portee maximale]. Repli 5-30 (Quick Shot) si le kit n'en dit rien.
// Revue M1 : un utilitaire court (Dust Toss, 0-15, sans effet de degats) ramenait `haut` a 15 et la distance
// visee a 8 yd au lieu de 23. Seuls comptent les sorts qui portent la rotation a distance.
void CoaArenePlage(PlayerbotAI* botAI, Player* bot, float& bas, float& haut)
{
    bas = 0.0f;
    haut = 0.0f;
    for (Usable const& spell : KnownAbilities(botAI, bot,
             [](uint16 kind) { return IsAttack(kind, false) && (kind & KIND_DAMAGE); }))
    {
        SpellInfo const* info = spell.info;
        if (!info->RangeEntry || info->RangeEntry->ID == 1 || info->RangeEntry->Flags == SPELL_RANGE_MELEE)
            continue;
        float const max = info->GetMaxRange(false, bot);
        if (max < 25.0f)
            continue;
        bas = std::max(bas, info->GetMinRange(false));
        haut = haut > 0.0f ? std::min(haut, max) : max;
    }
    if (haut <= 0.0f)
    {
        bas = 5.0f;
        haut = 30.0f;
    }
}

// Ou reculer, en distance de combat : 7 yd sous la portee maximale (23 pour 5-30), jamais a moins de 8 yd de la
// portee minimale ni a moins de 3 yd de la maximale.
float CoaAreneDistanceVisee(float bas, float haut)
{
    return std::clamp(haut - 7.0f, std::min(bas + 8.0f, haut - 3.0f), haut - 3.0f);
}

// La cible ne peut pas suivre : immobilisee, neutralisee, ou ralentie sous 70 % de la vitesse du bot.
bool CoaAreneFenetreDeRecul(Player* bot, Unit* cible)
{
    if (cible->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING) ||
        cible->isFrozen() || cible->HasRootAura())
        return true;
    float const vBot = bot->GetSpeed(MOVE_RUN);
    return vBot > 0.0f && cible->GetSpeed(MOVE_RUN) <= vBot * CoaAreneVitesseRelative;
}

// Un sort qui CREE de la distance : il ralentit, enracine ou neutralise la cible, par ses propres effets ou par
// un sort qu'il declenche (Dust Toss -> Dazed 1604). Les auras de proc sont ecartees, comme dans Classify.
bool CoaAreneAuraDeRupture(uint32 aura)
{
    switch (aura)
    {
        case SPELL_AURA_MOD_DECREASE_SPEED: case SPELL_AURA_MOD_ROOT: case SPELL_AURA_MOD_STUN:
        case SPELL_AURA_MOD_FEAR: case SPELL_AURA_MOD_CONFUSE:
            return true;
        default:
            return false;
    }
}

bool CoaAreneCreeDistance(SpellInfo const* info, uint8 profondeur = 0)
{
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (!effect.IsEffect())
            continue;
        bool const ennemi = IsEnemyTarget(effect.TargetA.GetTarget()) || IsEnemyTarget(effect.TargetB.GetTarget());
        if (ennemi && IsAuraEffect(effect) && CoaAreneAuraDeRupture(effect.ApplyAuraName))
            return true;
        bool const proc = IsAuraEffect(effect) &&
            (effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL || effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE ||
             effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_DAMAGE || effect.ApplyAuraName == SPELL_AURA_ADD_TARGET_TRIGGER);
        if (ennemi && effect.TriggerSpell && !proc && !profondeur)
            if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                if (CoaAreneCreeDistance(declenche, 1))
                    return true;
    }
    return false;
}

// L'adversaire d'arene : le joueur vivant de l'autre equipe le plus proche.
Player* CoaAreneAdversaireArene(Player* bot, Battleground* bg)
{
    Player* meilleur = nullptr;
    float meilleure = 0.0f;
    for (auto const& [guid, joueur] : bg->GetPlayers())
    {
        if (!joueur || joueur == bot || !joueur->IsInWorld() || !joueur->IsAlive() ||
            joueur->GetMapId() != bot->GetMapId() || joueur->GetBgTeamId() == bot->GetBgTeamId())
            continue;
        float const d = bot->GetExactDist2d(joueur);
        if (!meilleur || d < meilleure)
        {
            meilleur = joueur;
            meilleure = d;
        }
    }
    return meilleur;
}

// Quel engagement, s'il y en a un. `preparation` : arene portes fermees, duel en compte a rebours.
uint8 CoaAreneDetecter(PlayerbotAI* botAI, Player* bot, bool& preparation, Unit*& adversaire)
{
    preparation = false;
    adversaire = nullptr;
    if (!bot->IsAlive())
        return COA_ARENE_AUCUN;

    if (bot->InArena())
    {
        Battleground* bg = bot->GetBattleground();
        if (!bg)
            return COA_ARENE_AUCUN;
        if (bg->GetStatus() != STATUS_IN_PROGRESS && bg->GetStatus() != STATUS_WAIT_JOIN)
            return COA_ARENE_AUCUN;
        preparation = bg->GetStatus() == STATUS_WAIT_JOIN;
        adversaire = CoaAreneAdversaireArene(bot, bg);
        return COA_ARENE_ARENE;
    }

    if (bot->duel && bot->duel->Opponent)
    {
        if (bot->duel->State != DUEL_STATE_IN_PROGRESS && bot->duel->State != DUEL_STATE_COUNTDOWN)
            return COA_ARENE_AUCUN;
        preparation = bot->duel->State == DUEL_STATE_COUNTDOWN;
        adversaire = bot->duel->Opponent;
        return COA_ARENE_DUEL;
    }

    // Monde ouvert : un contre un, seul, hors instance, en combat contre un joueur. La cible courante d'abord ;
    // a defaut (moteur non-combat : PlayerbotAI l'efface quand le bot est en combat, PlayerbotAI.cpp:1719) les
    // assaillants.
    if (!sPlayerbotAIConfig.coaAreneMondeOuvert || bot->GetGroup() || !bot->IsInCombat() ||
        !bot->GetMap() || bot->GetMap()->Instanceable())
        return COA_ARENE_AUCUN;

    Unit* cible = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (cible && cible->IsPlayer() && cible->IsAlive() && cible->IsInWorld())
        adversaire = cible;
    else
        for (Unit* assaillant : bot->getAttackers())
            if (assaillant && assaillant->IsPlayer() && assaillant->IsAlive())
            {
                adversaire = assaillant;
                break;
            }
    return adversaire ? COA_ARENE_MONDE : COA_ARENE_AUCUN;
}

void CoaAreneJournal(Player* bot, CoaAreneEtat const& etat, char const* action, float avant, float apres,
                     std::string const& suite = "")
{
    LOG_INFO("playerbots.coa", "coa arene ia: guid={} action={} cadre={} bras={} dist_avant={:.1f} dist_apres={:.1f}{}",
             bot->GetGUID().GetCounter(), action, CoaAreneCadreNom(etat.cadre), etat.bras ? 1 : 0, avant, apres, suite);
}

// Fin d'un recul : la distance atteinte se lit a l'arrivee, pas a la decision.
void CoaAreneCloreRecul(Player* bot, CoaAreneEtat& etat, Unit* adversaire)
{
    float const apres = adversaire ? bot->GetExactDist2d(adversaire) : -1.0f;
    bool const reussi = adversaire && CoaAreneDistanceCombat(bot, adversaire) >= etat.reculVise - 5.0f;
    etat.reculsReussis += reussi ? 1 : 0;
    CoaAreneJournal(bot, etat, "recul", etat.reculDistAvant, apres,
                    Acore::StringFormat(" vise={:.1f} reussi={} duree_ms={}", etat.reculVise, reussi ? 1 : 0,
                                        getMSTimeDiff(etat.reculDebutMs, getMSTime())));
    etat.reculFinMs = 0;
}

/*
 * L'etat du tick : engagement ouvert, tire, suivi, clos. Une seule mise a jour par tick (CoaAreneMajMs).
 *
 * Le bras est tire UNE FOIS par engagement et par bot (mesure-experimentale.md § 2 : le sujet compare a
 * lui-meme). Le bilan est ecrit pour les DEUX bras : c'est la comparaison.
 */
CoaAreneEtat& CoaAreneMaj(PlayerbotAI* botAI, Player* bot)
{
    CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
    uint32 const maintenant = getMSTime();
    if (etat.majMs && getMSTimeDiff(etat.majMs, maintenant) < CoaAreneMajMs)
        return etat;
    etat.majMs = maintenant ? maintenant : 1;

    bool preparation = false;
    Unit* adversaire = nullptr;
    uint8 const cadre = CoaAreneDetecter(botAI, bot, preparation, adversaire);

    if (cadre != COA_ARENE_AUCUN && !etat.actif)
    {
        uint32 const tirage = sPlayerbotAIConfig.coaAreneTirage;
        etat = CoaAreneEtat();
        etat.majMs = maintenant ? maintenant : 1;
        etat.actif = true;
        etat.cadre = cadre;
        etat.bras = tirage >= 100 || (tirage > 0 && urand(1, 100) <= tirage);
        etat.debutMs = maintenant;
        etat.dernierContactMs = maintenant;
        CoaAreneJournal(bot, etat, "engagement", adversaire ? bot->GetExactDist2d(adversaire) : -1.0f, -1.0f,
                        Acore::StringFormat(" adversaire={} classe={} niveau={} preparation={}",
                                            adversaire ? adversaire->GetGUID().GetCounter() : 0, bot->getClass(),
                                            bot->GetLevel(), preparation ? 1 : 0));
    }

    if (!etat.actif)
        return etat;

    etat.vuCeTick = cadre != COA_ARENE_AUCUN && !preparation;
    if (cadre != COA_ARENE_AUCUN)
    {
        etat.cadre = cadre;
        etat.preparation = preparation;
        if (adversaire)
            etat.adversaire = adversaire->GetGUID();
        if (!preparation)
            etat.dernierContactMs = maintenant;
    }
    else
        etat.preparation = false;

    Unit* adv = etat.adversaire ? ObjectAccessor::GetUnit(*bot, etat.adversaire) : nullptr;
    if (adv && !adv->IsInWorld())
        adv = nullptr;

    // Recul en cours : clos a l'echeance, ou quand le bot s'est arrete (arrive, ou retenu).
    if (etat.reculFinMs &&
        (getMSTimeDiff(etat.reculDebutMs, maintenant) >= getMSTimeDiff(etat.reculDebutMs, etat.reculFinMs) ||
         (!bot->isMoving() && getMSTimeDiff(etat.reculDebutMs, maintenant) > 300)))
        CoaAreneCloreRecul(bot, etat, adv);

    if (cadre == COA_ARENE_AUCUN && getMSTimeDiff(etat.dernierContactMs, maintenant) > CoaAreneGraceMs)
    {
        if (etat.reculFinMs)
            CoaAreneCloreRecul(bot, etat, adv);
        uint32 const n = std::max<uint32>(etat.echantillons, 1);
        CoaAreneJournal(bot, etat, "bilan", -1.0f, -1.0f,
                        Acore::StringFormat(" duree_s={} echantillons={} contact_pct={} plage_pct={} reculs={} "
                                            "reculs_reussis={} ruptures={} bursts={} ouvertures={} pv_min={:.0f}",
                                            getMSTimeDiff(etat.debutMs, maintenant) / IN_MILLISECONDS, etat.echantillons,
                                            etat.auContact * 100 / n, etat.dansLaPlage * 100 / n, etat.reculs,
                                            etat.reculsReussis, etat.ruptures, etat.bursts, etat.ouvertures, etat.pvMin));
        etat.actif = false;
        etat.reculFinMs = 0;
        return etat;
    }

    // Echantillon de distance, pour les deux bras, combat seulement.
    if (!etat.preparation && adv && adv->IsAlive() && bot->IsAlive())
    {
        etat.pvMin = std::min(etat.pvMin, bot->GetHealthPct());
        if (bot->getClass() == CLASS_RANGER)
        {
            float bas = 0.0f, haut = 0.0f;
            CoaArenePlage(botAI, bot, bas, haut);
            float const d = CoaAreneDistanceCombat(bot, adv);
            ++etat.echantillons;
            bool const contact = d < bas + 1.0f;
            etat.auContact += contact ? 1 : 0;
            etat.dansLaPlage += (!contact && d <= haut) ? 1 : 0;

            // « tenir » : au contact, la cible peut suivre, pas de recul en cours -- le bot reste et frappe
            // (Skullpiercer, Wild Strike) au lieu de lui tourner le dos. Echantillonne.
            if (etat.bras && etat.vuCeTick && CoaAreneEstRangerDistance(bot) && contact && !etat.reculFinMs &&
                !CoaAreneFenetreDeRecul(bot, adv) &&
                (!etat.dernierTenirMs || getMSTimeDiff(etat.dernierTenirMs, maintenant) >= CoaAreneTenirJournalMs))
            {
                etat.dernierTenirMs = maintenant ? maintenant : 1;
                float const c = bot->GetExactDist2d(adv);
                CoaAreneJournal(bot, etat, "tenir", c, c);
            }
        }
    }

    return etat;
}

// L'IA s'applique a ce tick : engagement ouvert, bras tire, et pas en preparation.
bool CoaAreneEnCombat(CoaAreneEtat const& etat)
{
    return etat.actif && etat.bras && !etat.preparation;
}

bool CoaAreneReculEnCours(CoaAreneEtat const& etat)
{
    return etat.reculFinMs != 0;
}

// L1 (revue M2) : le delai de grace ne vaut que pour L0. Rupture, recul, burst et ouverture exigent que
// l'engagement soit VU a ce tick (arene ou duel en cours, ou un contre un du monde ouvert), sans quoi ils
// joueraient pendant 10 s contre la cible courante -- un monstre, ou l'ancien adversaire redevenu amical.
bool CoaAreneL1Actif(CoaAreneEtat const& etat)
{
    return CoaAreneEnCombat(etat) && etat.vuCeTick;
}

// La cible courante est bien l'adversaire de l'engagement.
bool CoaAreneEstAdversaire(CoaAreneEtat const& etat, Unit const* cible)
{
    return cible && etat.adversaire && cible->GetGUID() == etat.adversaire;
}

// ---------------------------------------------------------------------------------------------------------------
// L0, second volet : ne pas tenter ce que le coeur refusera a cause de l'etat du lanceur.
//
// Recopie CONSERVATRICE de Spell::CheckCasterAuras (Spell.cpp:6956-7100) : on n'ecarte un sort que dans les cas
// ou le coeur le refuse a coup sur. Tout cas ou le coeur regarde plus loin (SPELL_ATTR6_NOT_AN_ATTACK, sorts qui
// accordent une immunite, sorts utilisables etourdi dont il trie les mecanismes) est laisse au coeur. Le filtre
// ne peut donc jamais etre plus strict que lui.
// ---------------------------------------------------------------------------------------------------------------
bool CoaAreneFiltreEtatActif(PlayerbotAI* botAI, Player* bot)
{
    if (!sPlayerbotAIConfig.coaAreneIaCommune || !bot)
        return false;
    // Un bot libre de ses mouvements n'a rien a filtrer : on ne paie pas la mise a jour d'etat.
    if (!bot->HasUnitFlag(UNIT_FLAG_STUNNED) && !bot->HasUnitFlag(UNIT_FLAG_CONFUSED) &&
        !bot->HasUnitFlag(UNIT_FLAG_FLEEING) && !bot->HasUnitFlag(UNIT_FLAG_SILENCED) &&
        !bot->HasUnitFlag(UNIT_FLAG_PACIFIED))
        return false;
    return CoaAreneEnCombat(CoaAreneMaj(botAI, bot));
}

bool CoaAreneBloqueParEtat(Player* bot, SpellInfo const* info)
{
    if (info->HasAttribute(SPELL_ATTR6_NOT_AN_ATTACK) || info->HasAttribute(SPELL_ATTR1_IMMUNITY_PURGES_EFFECT))
        return false;

    uint32 const drapeaux = bot->GetUnitFlags();
    if (drapeaux & UNIT_FLAG_STUNNED)
    {
        if (!info->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_STUNNED))
            return true;
    }
    else if (drapeaux & UNIT_FLAG_CONFUSED)
    {
        if (!info->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_CONFUSED))
            return true;
    }
    else if (drapeaux & UNIT_FLAG_FLEEING)
    {
        if (!info->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_FLEEING))
            return true;
    }

    if ((drapeaux & UNIT_FLAG_SILENCED) && info->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
        return true;
    if ((drapeaux & UNIT_FLAG_PACIFIED) && info->PreventionType == SPELL_PREVENTION_TYPE_PACIFY)
        return true;
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// L1, burst : les sorts qui CONSOMMENT une pile (CasterAuraSpell = une aura du bot a plusieurs charges, ici
// Advantage 804329, 5 au plus) sont gardes pour une fenetre.
//
// Pourquoi c'est sans perte : le consommateur prend TOUTE la pile (AscensionClassMechanics.cpp:1430-1436) et ses
// degats montent par charge ; au plafond, chaque generateur de plus est perdu. Le lancer a 1 charge, comme le
// tourniquet le fait aujourd'hui, revient a brader la pile. Et a 0 charge le coeur le refuse
// (CASTER_AURASTATE), ce qui le met au banc 20 s (IsLastingFailure) : on l'ecarte alors dans les deux cas.
//
// Fenetre : pile pleine, cible sous 30 %, cible neutralisee (etourdie, desorientee, en fuite), ou bot sous 35 %
// (on depense avant de mourir). Hors fenetre, les consommateurs sont retires -- sauf si la liste en serait
// videe : un bot qui n'a rien d'autre a lancer lance quand meme.
// ---------------------------------------------------------------------------------------------------------------
// Recopie de SpellInfo::CheckTarget (SpellInfo.cpp:1853) : SPELL_ATTR1_ONLY_PEACEFUL_TARGETS refuse une cible en
// combat (Blackjack 520568 : AttributesEx 0x08040520, lu dans Spell.dbc). Le coeur rend
// SPELL_FAILED_TARGET_AFFECTING_COMBAT, qui ne met pas au banc : le tourniquet le retentait a chaque passage.
bool CoaAreneCiblePaisibleExigee(SpellInfo const* info, Unit* cible)
{
    return cible && (info->AttributesEx & SPELL_ATTR1_ONLY_PEACEFUL_TARGETS) &&
           (cible->IsInCombat() || cible->IsPetInCombat());
}

struct CoaAreneBurst
{
    bool fenetre = false;
    uint8 pile = 0;
    char const* raison = "";
    uint32 aura = 0;
};

uint32 CoaAreneAuraConsommee(Player* bot, SpellInfo const* info)
{
    SpellInfo const* effectif = EffectiveSpell(bot, info);
    if (!effectif->CasterAuraSpell)
        return 0;
    SpellInfo const* aura = sSpellMgr->GetSpellInfo(effectif->CasterAuraSpell);
    return aura && aura->StackAmount > 1 ? effectif->CasterAuraSpell : 0;
}

bool CoaAreneDegatsDirects(SpellInfo const* info)
{
    for (SpellEffectInfo const& effect : info->Effects)
        if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
            return true;
    return false;
}

// Pret a ce tick : ni en recharge, ni au banc, et la ressource suffit (meme test que StrictCheck). Le reste
// (portee, orientation) a deja ete filtre par DropUncastableHere chez l'appelant.
bool CoaAreneSortPret(PlayerbotAI* botAI, Player* bot, Usable const& spell)
{
    auto const& banc = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->benchedSpells;
    time_t const maintenant = time(nullptr);
    SpellInfo const* info = EffectiveSpell(bot, spell.info);
    if (bot->HasSpellCooldown(info->Id) || IsBenched(banc, spell.info->Id, maintenant) || IsBenched(banc, info->Id, maintenant))
        return false;
    if (info->PowerType < MAX_POWERS && info->PowerType != POWER_HEALTH)
    {
        int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
        if (cout > 0 && bot->GetPower(Powers(info->PowerType)) < uint32(cout))
            return false;
    }
    return true;
}

CoaAreneBurst CoaAreneOrdonnerBurst(PlayerbotAI* botAI, Player* bot, Unit* cible, std::vector<Usable>& usable)
{
    CoaAreneBurst burst;
    if (!CoaAreneEstRangerDistance(bot) || !cible || usable.empty())
        return burst;
    CoaAreneEtat& etat = CoaAreneMaj(botAI, bot);
    if (!CoaAreneL1Actif(etat) || !CoaAreneEstAdversaire(etat, cible))
        return burst;

    // Un sort reserve aux cibles hors combat ne partira pas contre celle-ci (voir CoaAreneCiblePaisibleExigee).
    if (std::any_of(usable.begin(), usable.end(), [&](Usable const& spell)
            { return !CoaAreneCiblePaisibleExigee(EffectiveSpell(bot, spell.info), cible); }))
        usable.erase(std::remove_if(usable.begin(), usable.end(), [&](Usable const& spell)
            { return CoaAreneCiblePaisibleExigee(EffectiveSpell(bot, spell.info), cible); }), usable.end());

    for (Usable const& spell : usable)
        if ((burst.aura = CoaAreneAuraConsommee(bot, spell.info)))
            break;
    if (!burst.aura)
        return burst;

    Aura const* pile = bot->GetAura(burst.aura);
    burst.pile = pile ? pile->GetStackAmount() : 0;
    SpellInfo const* auraInfo = sSpellMgr->GetSpellInfo(burst.aura);
    uint8 const plafond = auraInfo ? uint8(auraInfo->StackAmount) : 0;

    if (burst.pile && plafond && burst.pile >= plafond)
        burst.raison = "pile";
    else if (burst.pile && cible->GetHealthPct() <= 30.0f)
        burst.raison = "vie_cible";
    else if (burst.pile && cible->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
        burst.raison = "controle";
    else if (burst.pile && bot->GetHealthPct() <= 35.0f)
        burst.raison = "vie_bot";
    burst.fenetre = *burst.raison != '\0';

    auto consommateur = [bot](Usable const& spell) { return CoaAreneAuraConsommee(bot, spell.info) != 0; };
    if (burst.fenetre)
    {
        // Consommateurs devant, degats directs (Skullpiercer) avant le saignement (Serrated Shot).
        std::stable_partition(usable.begin(), usable.end(), [&](Usable const& spell)
            { return consommateur(spell) && CoaAreneDegatsDirects(EffectiveSpell(bot, spell.info)); });
        std::stable_partition(usable.begin(), usable.end(), consommateur);
        return burst;
    }

    // Revue M3 : ne compter que ce qui PEUT partir maintenant. Un Wild Strike sans Focus ou un Dust Toss en
    // recharge comptaient, Skullpiercer etait retire, et le Ranger ne faisait plus que des coups blancs.
    size_t const autres = size_t(std::count_if(usable.begin(), usable.end(),
        [&](Usable const& spell) { return !consommateur(spell) && CoaAreneSortPret(botAI, bot, spell); }));
    if (autres || !burst.pile)
        usable.erase(std::remove_if(usable.begin(), usable.end(), consommateur), usable.end());
    return burst;
}

void CoaAreneNoterBurst(PlayerbotAI* botAI, Player* bot, Unit* cible, CoaAreneBurst const& burst, SpellInfo const* cast)
{
    if (!burst.fenetre || !cast || !CoaAreneAuraConsommee(bot, cast))
        return;
    CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
    ++etat.bursts;
    float const d = cible ? bot->GetExactDist2d(cible) : -1.0f;
    CoaAreneJournal(bot, etat, "burst", d, d,
                    Acore::StringFormat(" sort={} pile={} raison={} pv_cible={:.0f}", cast->Id, burst.pile, burst.raison,
                                        cible ? cible->GetHealthPct() : -1.0f));
}

// Les sorts d'attaque du bot qui creent de la distance, prets a partir sur cette cible maintenant : ni en
// recharge, ni au banc, ni reserves aux cibles hors combat (Blackjack), ni refuses ici (DropUncastableHere).
// Sans ce tri, un Dust Toss en recharge gardait le declencheur actif et l'action echouait a chaque tick.
std::vector<Usable> CoaAreneRuptures(PlayerbotAI* botAI, Player* bot, Unit* cible)
{
    auto const& banc = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->benchedSpells;
    time_t const maintenant = time(nullptr);
    std::vector<Usable> ruptures = KnownAbilities(botAI, bot, [](uint16 kind) { return IsAttack(kind, false); });
    ruptures.erase(std::remove_if(ruptures.begin(), ruptures.end(), [&](Usable const& spell)
        {
            SpellInfo const* info = EffectiveSpell(bot, spell.info);
            return !CoaAreneCreeDistance(info) || bot->HasSpellCooldown(info->Id) ||
                   IsBenched(banc, spell.info->Id, maintenant) || IsBenched(banc, info->Id, maintenant) ||
                   CoaAreneCiblePaisibleExigee(info, cible);
        }), ruptures.end());
    if (!ruptures.empty())
        DropUncastableHere(bot, cible, ruptures);
    return ruptures;
}

// ---------------------------------------------------------------------------------------------------------------
// Multiplicateur : L0 (repas, boisson, suggestions, monture) et, pour le Ranger, la fuite amont et ce qui
// casserait un recul en cours.
// ---------------------------------------------------------------------------------------------------------------
class CoaAreneMultiplier : public Multiplier
{
public:
    CoaAreneMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "coa arene") {}

    float GetValue(Action* action) override
    {
        if (!action || !CoaAreneArmee())
            return 1.0f;
        CoaAreneEtat const& etat = CoaAreneMaj(botAI, bot);
        if (!CoaAreneEnCombat(etat))
            return 1.0f;

        std::string const nom = action->getName();
        if (sPlayerbotAIConfig.coaAreneIaCommune)
        {
            if (nom == "food" || nom == "drink" || nom == "suggest what to do" || nom == "mount")
                return 0.0f;
            // « check mount state » monte ET descend : on ne lui laisse que la descente.
            if (nom == "check mount state" && !bot->IsMounted())
                return 0.0f;
        }

        if (CoaAreneEstRangerDistance(bot))
        {
            // La fuite amont (« enemy too close for spell » -> « flee », 5 yd a reculons, RangedCombatStrategy)
            // est remplacee par le recul de L1 : les deux se disputeraient le bot.
            if (nom == "flee" && CoaAreneL1Actif(etat))
                return 0.0f;
            // Un recul en cours ne se laisse pas defaire : se retourner vers la cible (set facing, 37) ou
            // lancer un sort a incantation (CastFirst arrete le bot) le figeraient au contact.
            if (CoaAreneReculEnCours(etat) && (nom == "set facing" || nom == "coa attack" || nom == "coa aoe"))
                return 0.0f;
        }
        return 1.0f;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Declencheurs.
// ---------------------------------------------------------------------------------------------------------------

// Au contact d'une cible qui peut suivre, et un sort de rupture est pret.
class CoaAreneRuptureTrigger : public Trigger
{
public:
    CoaAreneRuptureTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene rupture") {}

    bool IsActive() override
    {
        if (!CoaAreneEstRangerDistance(bot))
            return false;
        CoaAreneEtat const& etat = CoaAreneMaj(botAI, bot);
        if (!CoaAreneL1Actif(etat) || CoaAreneReculEnCours(etat))
            return false;
        Unit* cible = AI_VALUE(Unit*, "current target");
        if (!cible || !cible->IsAlive() || !CoaAreneEstAdversaire(etat, cible) || CoaAreneFenetreDeRecul(bot, cible))
            return false;
        float bas = 0.0f, haut = 0.0f;
        CoaArenePlage(botAI, bot, bas, haut);
        if (CoaAreneDistanceCombat(bot, cible) >= bas + 1.0f)
            return false;
        return !CoaAreneRuptures(botAI, bot, cible).empty();
    }
};

// Au contact d'une cible qui NE PEUT PAS suivre, bot libre de ses mouvements.
class CoaAreneReculTrigger : public Trigger
{
public:
    CoaAreneReculTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene recul") {}

    bool IsActive() override
    {
        if (!CoaAreneEstRangerDistance(bot))
            return false;
        CoaAreneEtat const& etat = CoaAreneMaj(botAI, bot);
        if (!CoaAreneL1Actif(etat) || CoaAreneReculEnCours(etat))
            return false;
        if (etat.dernierEssaiReculMs && getMSTimeDiff(etat.dernierEssaiReculMs, getMSTime()) < CoaAreneReculReessaiMs)
            return false;
        if (bot->IsRooted() || bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_LOST_CONTROL) || !botAI->CanMove())
            return false;
        Unit* cible = AI_VALUE(Unit*, "current target");
        if (!cible || !cible->IsAlive() || !CoaAreneEstAdversaire(etat, cible) || !CoaAreneFenetreDeRecul(bot, cible))
            return false;
        float bas = 0.0f, haut = 0.0f;
        CoaArenePlage(botAI, bot, bas, haut);
        return CoaAreneDistanceCombat(bot, cible) < bas + 1.0f;
    }
};

// Le bot est furtif pendant l'engagement (Elude avant le combat, ou Dust Toss en combat) : ouvrir.
class CoaAreneOuvertureTrigger : public Trigger
{
public:
    CoaAreneOuvertureTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene ouverture") {}

    bool IsActive() override
    {
        if (!CoaAreneEstRangerDistance(bot) || !bot->HasStealthAura())
            return false;
        CoaAreneEtat const& etat = CoaAreneMaj(botAI, bot);
        if (!CoaAreneL1Actif(etat) || !etat.adversaire || CoaAreneReculEnCours(etat))
            return false;
        if (etat.prochaineOuvertureMs && getMSTimeDiff(etat.prochaineOuvertureMs, getMSTime()) >= 0x80000000u)
            return false;
        // Blackjack et Dust Toss ne rompent pas la furtivite (SPELL_ATTR1_ALLOW_WHILE_STEALTHED, lu dans Spell.dbc) :
        // apres un sap, le bot est encore furtif, au contact d'une cible etourdie. C'est au recul de jouer d'abord ;
        // ouvrir la tout de suite romprait le sap au contact.
        Unit* adv = ObjectAccessor::GetUnit(*bot, etat.adversaire);
        if (adv && adv->IsAlive() && CoaAreneFenetreDeRecul(bot, adv))
        {
            float bas = 0.0f, haut = 0.0f;
            CoaArenePlage(botAI, bot, bas, haut);
            if (CoaAreneDistanceCombat(bot, adv) < CoaAreneDistanceVisee(bas, haut) - 5.0f)
                return false;
        }
        return true;
    }
};

// Arene portes fermees, ou duel en compte a rebours : prendre la furtivite (Elude) pour ouvrir depuis elle.
class CoaArenePreparationTrigger : public Trigger
{
public:
    CoaArenePreparationTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene preparation") {}

    bool IsActive() override
    {
        if (!CoaAreneEstRangerDistance(bot) || bot->HasStealthAura() || bot->IsInCombat() || !ClassHas(bot, KIND_STEALTH))
            return false;
        CoaAreneEtat const& etat = CoaAreneMaj(botAI, bot);
        if (!etat.actif || !etat.bras || !etat.preparation)
            return false;
        return !etat.prochainePreparationMs || getMSTimeDiff(etat.prochainePreparationMs, getMSTime()) < 0x80000000u;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Actions.
// ---------------------------------------------------------------------------------------------------------------

class CoaAreneRuptureAction : public Action
{
public:
    CoaAreneRuptureAction(PlayerbotAI* botAI) : Action(botAI, "coa arene rupture") {}

    bool Execute(Event /*event*/) override
    {
        Unit* cible = AI_VALUE(Unit*, "current target");
        if (!cible || !cible->IsAlive())
            return false;
        std::vector<Usable> const ruptures = CoaAreneRuptures(botAI, bot, cible);
        SpellInfo const* cast = RecordUsage(USAGE_ATTACK, CastFirst(botAI, bot, ruptures, cible, USAGE_ATTACK));
        if (!cast)
            return false;
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        ++etat.ruptures;
        float const d = bot->GetExactDist2d(cible);
        CoaAreneJournal(bot, etat, "rupture", d, d, Acore::StringFormat(" sort={}", cast->Id));
        return true;
    }
};

/*
 * Le recul. Un point a la distance visee DE LA CIBLE, dans la direction qui l'eloigne, puis a +/-30, 60 et
 * 90 degres si la premiere est fermee. Un point n'est retenu que :
 *   - s'il a un sol et un chemin (Map::CheckCollisionAndGetValidCoords, sans echec sur collision : il ramene le
 *     point contre le mur, d'ou le controle de distance qui suit -- c'est ce qui ecarte le mur et le vide) ;
 *   - sans denivele de plus de 8 yd (ni falaise ni corniche) ;
 *   - a au moins (visee - 6) yd de la cible et sans depasser la portee maximale des tirs ;
 *   - en ligne de vue de la cible (pas derriere un pilier : on recule pour tirer) ;
 *   - et si MovementAction::MoveTo accepte d'y aller (chemin calcule, priorite de combat).
 * Le bot COURT (pas a reculons, 4,5 yd/s contre 7) : il se retournera a l'arrivee (« set facing »).
 */
class CoaAreneReculAction : public MovementAction
{
public:
    CoaAreneReculAction(PlayerbotAI* botAI) : MovementAction(botAI, "coa arene recul") {}

    bool Execute(Event /*event*/) override
    {
        Unit* cible = AI_VALUE(Unit*, "current target");
        if (!cible || !cible->IsAlive() || !bot->GetMap())
            return false;

        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        uint32 const maintenant = getMSTime();
        etat.dernierEssaiReculMs = maintenant ? maintenant : 1;

        // Revue m1 : MoveTo refuserait tout point tant qu'un mouvement de priorite au moins egale n'a pas fini
        // son delai. Le dire avant de calculer sept points, et sans journaliser un faux « recul_impossible ».
        if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
            return false;

        float bas = 0.0f, haut = 0.0f;
        CoaArenePlage(botAI, bot, bas, haut);
        float const visee = CoaAreneDistanceVisee(bas, haut);
        float const rayons = bot->GetCombatReach() + cible->GetCombatReach();
        float const rayon = visee + rayons;
        float const avant = bot->GetExactDist2d(cible);
        float const angle0 = cible->GetAngle(bot);

        bool doublon = false;
        static constexpr float Ecarts[] = { 0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f };  // x 30 degres
        for (float const ecart : Ecarts)
        {
            float const angle = angle0 + ecart * float(M_PI) / 6.0f;
            float x = cible->GetPositionX() + std::cos(angle) * rayon;
            float y = cible->GetPositionY() + std::sin(angle) * rayon;
            float z = bot->GetPositionZ();
            if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                                bot->GetPositionZ(), x, y, z, false))
                continue;  // ni chemin ni sol
            if (std::fabs(z - bot->GetPositionZ()) > 8.0f)
                continue;
            float const atteinte = cible->GetExactDist2d(x, y) - rayons;
            if (atteinte < visee - 6.0f || atteinte > haut - 1.0f)
                continue;  // ramene contre un mur, ou hors de portee
            if (!cible->IsWithinLOS(x, y, z + 2.0f))
                continue;
            // Le meme point que le dernier mouvement : MoveTo le refuserait sans que la geometrie soit en cause.
            if (IsDuplicateMove(x, y, z))
            {
                doublon = true;
                continue;
            }
            if (!MoveTo(bot->GetMapId(), x, y, z, false, false, true, false, MovementPriority::MOVEMENT_COMBAT, true,
                        false))
                continue;

            float const vitesse = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
            uint32 const trajetMs = uint32(bot->GetExactDist2d(x, y) / vitesse * IN_MILLISECONDS) + 500;
            etat.reculDebutMs = maintenant;
            etat.reculFinMs = (maintenant + trajetMs) ? maintenant + trajetMs : 1;
            etat.reculDistAvant = avant;
            etat.reculVise = visee;
            ++etat.reculs;
            return true;
        }

        if (!doublon &&
            (!etat.dernierImpossibleMs || getMSTimeDiff(etat.dernierImpossibleMs, maintenant) >= CoaAreneTenirJournalMs))
        {
            etat.dernierImpossibleMs = maintenant ? maintenant : 1;
            CoaAreneJournal(bot, etat, "recul_impossible", avant, avant, Acore::StringFormat(" vise={:.1f}", visee));
        }
        return false;
    }

    bool isUseful() override { return IsMovingAllowed(); }
};

/*
 * L'ouverture depuis la furtivite, SANS PERDRE LA CIBLE (P-167). Un bot furtif ne « voit » plus sa proie par
 * « enemy player target » (EnemyPlayerValue.cpp:248 teste enemy->CanSeeOrDetect(bot)) : l'adversaire est donc
 * pris dans l'etat de l'engagement (adversaire de duel, joueur de l'autre equipe d'arene), jamais dans cette
 * valeur. La cible courante est posee, puis, dans l'ordre :
 *   1. un controle sur une cible hors combat (Blackjack, sap 8 s) : le recul suit, la cible etant etourdie ;
 *   2. un sort « seulement furtif » (Flank, au contact) ;
 *   3. le reste des attaques (Toxic Dart et son silence, Quick Shot), consommateurs filtres comme en burst.
 * Rien ne part : on s'approche (a 2 yd sous la portee du controle s'il est pret et la cible hors combat, sinon
 * a 5 yd sous la portee maximale), et on reessaie dans 3 s au plus tard.
 *
 * CHOIX (revue m3) : l'approche ne va JAMAIS au contact pour Flank. Le Ranger pilote est un tireur : aller a 5 yd
 * d'un joueur de melee pour un coup d'ouverture le met dans la zone ou ses tirs ne partent plus (portee minimale
 * 5) et ou la cible le frappe. Flank n'est lance que si le bot est DEJA au contact en etant furtif (apres un
 * Blackjack, ou si la cible est venue a lui). Le Ranger de melee (specialisation 30) n'est pas concerne par L1.
 */
class CoaAreneOuvertureAction : public MovementAction
{
public:
    CoaAreneOuvertureAction(PlayerbotAI* botAI) : MovementAction(botAI, "coa arene ouverture") {}

    bool Execute(Event /*event*/) override
    {
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        uint32 const maintenant = getMSTime();
        etat.prochaineOuvertureMs = maintenant + CoaAreneOuvertureReessaiMs;

        Unit* adv = etat.adversaire ? ObjectAccessor::GetUnit(*bot, etat.adversaire) : nullptr;
        if (!adv || !adv->IsInWorld() || !adv->IsAlive() || !bot->IsValidAttackTarget(adv))
            return false;

        context->GetValue<Unit*>("current target")->Set(adv);
        bot->SetSelection(adv->GetGUID());

        bool const horsCombat = !adv->IsInCombat();
        // Le kit entier sert a choisir la distance d'approche ; la liste de lancement, elle, est filtree d'abord.
        std::vector<Usable> const ouvertures = KnownAbilities(botAI, bot, [](uint16 kind) { return IsAttack(kind, false); });

        float const avant = bot->GetExactDist2d(adv);
        if (avant <= 40.0f && bot->IsWithinLOSInMap(adv))
        {
            if (!bot->HasInArc(static_cast<float>(M_PI), adv))
            {
                if (bot->isMoving())
                    bot->StopMoving();
                ServerFacade::instance().SetFacingTo(bot, adv);
            }
            // Revue M3 : DropUncastableHere AVANT le tri du burst (qui compte ce qui peut partir), puis l'ordre de
            // l'ouverture par-dessus : controle (cible hors combat), puis « seulement furtif », puis le reste, dont
            // les consommateurs dans l'ordre que le burst leur a donne (tris stables).
            std::vector<Usable> pretes = ouvertures;
            DropUncastableHere(bot, adv, pretes);
            CoaAreneBurst const burst = CoaAreneOrdonnerBurst(botAI, bot, adv, pretes);
            std::stable_partition(pretes.begin(), pretes.end(),
                [](Usable const& spell) { return spell.info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED); });
            if (horsCombat)
                std::stable_partition(pretes.begin(), pretes.end(),
                    [](Usable const& spell) { return (spell.kind & KIND_CONTROL) != 0; });
            if (SpellInfo const* cast = RecordUsage(USAGE_ATTACK, CastFirst(botAI, bot, pretes, adv, USAGE_ATTACK)))
            {
                ++etat.ouvertures;
                CoaAreneNoterBurst(botAI, bot, adv, burst, cast);
                bool const controle = std::any_of(pretes.begin(), pretes.end(),
                    [cast](Usable const& s) { return s.info == cast && (s.kind & KIND_CONTROL); });
                CoaAreneJournal(bot, etat, "ouverture", avant, bot->GetExactDist2d(adv),
                                Acore::StringFormat(" sort={} controle={} furtif_seul={}", cast->Id, controle ? 1 : 0,
                                                    cast->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) ? 1 : 0));
                if (botAI->GetState() != BOT_STATE_COMBAT)
                    botAI->ChangeEngine(BOT_STATE_COMBAT);
                // Toxic Dart et Dust Toss ne rompent pas non plus la furtivite : sans ce delai, l'ouverture
                // redeviendrait la rotation tant que dure Elude. Une ouverture par furtivite (recharge d'Elude).
                etat.prochaineOuvertureMs = maintenant + CoaAreneOuvertureApresSuccesMs;
                return true;
            }
        }

        // Rien n'est parti : s'approcher, sans entrer au contact.
        float bas = 0.0f, haut = 0.0f;
        CoaArenePlage(botAI, bot, bas, haut);
        float approche = haut - 5.0f;
        if (horsCombat)
            for (Usable const& spell : ouvertures)
            {
                if (!(spell.kind & KIND_CONTROL) || bot->HasSpellCooldown(spell.info->Id))
                    continue;
                approche = std::max(1.0f, spell.info->GetMaxRange(false, bot) - 2.0f);
                break;
            }
        if (CoaAreneDistanceCombat(bot, adv) <= approche)
            return false;
        float const rayon = approche + bot->GetCombatReach() + adv->GetCombatReach();
        float const angle = adv->GetAngle(bot);
        float x = adv->GetPositionX() + std::cos(angle) * rayon;
        float y = adv->GetPositionY() + std::sin(angle) * rayon;
        float z = bot->GetPositionZ();
        if (!bot->GetMap() || !bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                                              bot->GetPositionZ(), x, y, z, false))
            return false;
        if (!MoveTo(bot->GetMapId(), x, y, z, false, false, true, false, MovementPriority::MOVEMENT_COMBAT, true, false))
            return false;
        etat.prochaineOuvertureMs = maintenant + CoaAreneOuvertureApprocheMs;
        return true;
    }
};

class CoaArenePreparationAction : public Action
{
public:
    CoaArenePreparationAction(PlayerbotAI* botAI) : Action(botAI, "coa arene preparation") {}

    bool Execute(Event /*event*/) override
    {
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        etat.prochainePreparationMs = getMSTime() + CoaArenePreparationReessaiMs;
        // La meme porte d'entree que la traque d'une prime (CoaChasse.h) : une furtivite, jamais canalisee sous
        // Directeur.Embuscade, distance 0 donc aucune exigence de duree.
        return CoaChasseEntrerEnFurtivite(botAI, bot, 0.0f);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// La strategie, ses fabriques. Fabrique SANS freres (P-130) : sa pose ne retire rien du moteur.
// ---------------------------------------------------------------------------------------------------------------

/*
 * Priorites, contre celles qui existent :
 *   ouverture 57 = AmbushPriority (56) + 1 : passe devant « attack enemy player » (55) et « coa stealth » (56),
 *     faute de quoi l'attaque degainerait a decouvert ; reste sous « drop target » (99).
 *   preparation 57 : moteur non-combat, en preparation seulement (rien d'autre n'y joue a ce niveau).
 *   recul 38 = ACTION_MOVE + 8 : devant « set facing » (37), la fuite amont (34) et « coa defensive » (28) ;
 *     sous les interruptions (40) et le controle de monture (54).
 *   rupture 36 = ACTION_MOVE + 6 : sous « set facing » (37), car Dust Toss exige d'etre face a la cible ; devant
 *     la rotation (10), « reach spell » (20) et « coa defensive » (28) -- creer la distance protege mieux qu'un
 *     defensif ne soigne, et le defensif reste disponible au tick suivant.
 */
constexpr float CoaAreneOuverturePriority = AmbushPriority + 1.0f;
constexpr float CoaAreneReculPriority = ACTION_MOVE + 8.0f;
constexpr float CoaAreneRupturePriority = ACTION_MOVE + 6.0f;

class CoaAreneStrategy : public Strategy
{
public:
    CoaAreneStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "coa arene"; }

    void InitMultipliers(std::vector<Multiplier*>& multipliers) override
    {
        multipliers.push_back(new CoaAreneMultiplier(botAI));
    }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override
    {
        triggers.push_back(new TriggerNode("coa arene ouverture", { NextAction("coa arene ouverture", CoaAreneOuverturePriority) }));
        triggers.push_back(new TriggerNode("coa arene preparation", { NextAction("coa arene preparation", CoaAreneOuverturePriority) }));
        triggers.push_back(new TriggerNode("coa arene recul", { NextAction("coa arene recul", CoaAreneReculPriority) }));
        triggers.push_back(new TriggerNode("coa arene rupture", { NextAction("coa arene rupture", CoaAreneRupturePriority) }));
    }
};

class CoaAreneStrategyFactoryInternal : public NamedObjectContext<Strategy>
{
public:
    CoaAreneStrategyFactoryInternal() { creators["coa arene"] = &CoaAreneStrategyFactoryInternal::coa_arene; }

private:
    static Strategy* coa_arene(PlayerbotAI* botAI) { return new CoaAreneStrategy(botAI); }
};

class CoaAreneActionFactoryInternal : public NamedObjectContext<Action>
{
public:
    CoaAreneActionFactoryInternal()
    {
        creators["coa arene rupture"] = &CoaAreneActionFactoryInternal::rupture;
        creators["coa arene recul"] = &CoaAreneActionFactoryInternal::recul;
        creators["coa arene ouverture"] = &CoaAreneActionFactoryInternal::ouverture;
        creators["coa arene preparation"] = &CoaAreneActionFactoryInternal::preparation;
    }

private:
    static Action* rupture(PlayerbotAI* botAI) { return new CoaAreneRuptureAction(botAI); }
    static Action* recul(PlayerbotAI* botAI) { return new CoaAreneReculAction(botAI); }
    static Action* ouverture(PlayerbotAI* botAI) { return new CoaAreneOuvertureAction(botAI); }
    static Action* preparation(PlayerbotAI* botAI) { return new CoaArenePreparationAction(botAI); }
};

class CoaAreneTriggerFactoryInternal : public NamedObjectContext<Trigger>
{
public:
    CoaAreneTriggerFactoryInternal()
    {
        creators["coa arene rupture"] = &CoaAreneTriggerFactoryInternal::rupture;
        creators["coa arene recul"] = &CoaAreneTriggerFactoryInternal::recul;
        creators["coa arene ouverture"] = &CoaAreneTriggerFactoryInternal::ouverture;
        creators["coa arene preparation"] = &CoaAreneTriggerFactoryInternal::preparation;
    }

private:
    static Trigger* rupture(PlayerbotAI* botAI) { return new CoaAreneRuptureTrigger(botAI); }
    static Trigger* recul(PlayerbotAI* botAI) { return new CoaAreneReculTrigger(botAI); }
    static Trigger* ouverture(PlayerbotAI* botAI) { return new CoaAreneOuvertureTrigger(botAI); }
    static Trigger* preparation(PlayerbotAI* botAI) { return new CoaArenePreparationTrigger(botAI); }
};
