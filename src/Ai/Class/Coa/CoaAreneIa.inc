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
// L'ouverture ne tente ses sorts qu'a cette distance (centre a centre) ET en vue ; au-dela, ou hors de vue, c'est le
// retour qui mene le bot (banc du 10/10 22:56 Paris, voir CoaAreneOuvertureDansSaFenetre).
constexpr float CoaAreneOuvertureDistanceMax = 40.0f;
constexpr uint32 CoaArenePreparationReessaiMs = 5 * IN_MILLISECONDS;
// La cible ne peut pas suivre si elle court a 70 % de la vitesse du bot ou moins (Dazed : 50 %).
constexpr float CoaAreneVitesseRelative = 0.7f;
// Recul et retour ne visent jamais au-dela de la portee maximale moins cette marge (incident du 10/10 19:21 :
// un recul fini a 32,2 yd, hors de portee et derriere la tombe, puis 106 s sans rien faire).
constexpr float CoaAreneMargePortee = 2.0f;
// Un recul ne vaut la peine que s'il sort le bot de la portee minimale avec cette avance.
constexpr float CoaAreneReculGainMin = 3.0f;
// Duree de fenetre supposee quand l'aura qui ralentit n'a pas de duree lisible.
constexpr uint32 CoaAreneFenetreParDefautMs = 2 * IN_MILLISECONDS;
// Retour a portee et en vue : un essai par seconde au plus, une ligne de journal toutes les 5 s au plus.
constexpr uint32 CoaAreneRetourReessaiMs = IN_MILLISECONDS;
constexpr uint32 CoaAreneRetourJournalMs = 5 * IN_MILLISECONDS;
// Burst a pile pleine (revue du 10/10 soir) : seulement cible ralentie/controlee ou sous 60 % ; au-dela de 4 s
// de pile pleine sans fenetre, chaque generateur est perdu (pas de 6e charge) : on depense quand meme.
// 4 s et non plus 8 (lot 3, demande du commanditaire) : a ~40 de Focus par generateur, 8 s de pile tenue
// valaient deux generateurs perdus, pour une fenetre qui ne vient souvent jamais contre une cible a distance.
constexpr float CoaAreneBurstPvCible = 60.0f;
constexpr uint32 CoaArenePileTenueMaxMs = 4 * IN_MILLISECONDS;
// Defensif d'arene (lot 3) : sous 50 % de vie, ou apres une perte d'au moins 20 points en 2 a 4 s (burst subi).
constexpr float CoaAreneDefensifPv = 50.0f;
constexpr float CoaAreneDefensifBurstPv = 20.0f;
constexpr uint32 CoaAreneDefensifReleveMs = 2 * IN_MILLISECONDS;
// Trace des gladiateurs : lignes par seconde et par bot, au plus (ligne « tick » comprise).
constexpr uint8 CoaAreneTraceParSeconde = 5;
// L2, piliers : seuil de vie, duree max d'une cachette, pause entre deux, cumul par engagement, frequence des
// recherches et debit du journal.
constexpr float CoaArenePilierPvBot = 40.0f;
constexpr uint32 CoaArenePilierMaxMs = 8 * IN_MILLISECONDS;
constexpr uint32 CoaArenePilierPauseMs = 10 * IN_MILLISECONDS;
constexpr uint32 CoaArenePilierCumulMaxMs = 30 * IN_MILLISECONDS;
constexpr uint32 CoaArenePilierEssaiMs = 1500;
constexpr uint32 CoaArenePilierJournalMs = 10 * IN_MILLISECONDS;
constexpr uint8 COA_PILIER_PV = 1;
constexpr uint8 COA_PILIER_RECHARGE = 2;
// Banc d'arene 1v1, mode cacheur : recharge courte entre deux recherches, duree max d'un deplacement vers une
// cachette, pause entre deux tirs a decouvert (un tir, puis a couvert), debit de la ligne « cacheur_aucun ».
constexpr uint32 CoaAreneCacheurEssaiMs = 700;
constexpr uint32 CoaAreneCacheurRouteMaxMs = 4 * IN_MILLISECONDS;
constexpr uint32 CoaAreneCacheurTirPauseMs = 1500;
constexpr uint32 CoaAreneCacheurJournalMs = 10 * IN_MILLISECONDS;

CoaAreneEtat& CoaAreneEtatDe(PlayerbotAI* botAI)
{
    return static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->arene;
}

// Declare avant CastFirst (CoaAiObjectContext.cpp). Lit seulement l'etat du bot, ecrit par ses propres actions d'arene :
// tous ces champs sont nuls hors arene et hors duel, et CoaAreneClore les remet a zero en fin d'engagement (la
// cachette L2, elle, par CoaArenePilierSuivre). Aucune mise a jour d'etat ici : appele a chaque sort.
bool CoaAreneMouvementVoulu(PlayerbotAI* botAI)
{
    CoaAreneEtat const& etat = CoaAreneEtatDe(botAI);
    return etat.actif && (etat.reculFinMs || etat.retourEnCours || etat.pilierDebutMs || etat.cacheurEnRouteMs);
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

// Le retour (10/10/2026 nuit, retour du commanditaire : « des que je suis cache derriere un pilier, que j'ai mis mes
// dots, il ne fait rien ») : ecrit pour le Ranger de L1, il manquait a toutes les autres classes, qui restaient
// plantees apres « invalid target » -> « drop target » (trace : 1422 de 61 a 7 % de vie en 4 s, moteur non-combat,
// cible=0, bouge=0, 20:01:58-20:02:02 UTC). Sous IaCommune : toute classe CoA de degats ou tank. Pas le soigneur, qui
// en arene a plusieurs n'a pas a courir apres l'adversaire.
// Revue M1 (10/10 nuit) : hors Ranger, seulement en DUEL ou en arene 1V1 (type 1, propre a CoaArene1v1) ; dans les
// 2v2/3v3/5v5 des bots, le retour imposait le plus proche ennemi et ecrasait le choix de cible (et l'oracle Laya).
bool CoaAreneRetourArme(Player* bot)
{
    if (CoaAreneEstRangerDistance(bot))
        return true;
    if (!sPlayerbotAIConfig.coaAreneIaCommune || !IsAscensionCustomClassId(bot->getClass()) ||
        GetCoaRole(bot) == CoaRole::Heal)
        return false;
    if (bot->duel && bot->duel->Opponent)
        return true;
    Battleground* bg = bot->InArena() ? bot->GetBattleground() : nullptr;
    return bg && bg->isArena() && bg->GetArenaType() == CoaArene1v1::TYPE_ARENE;
}

// Distance « de combat » : celle que le coeur compare aux portees des sorts (Unit::IsWithinCombatRange, appele par
// Spell::CheckRange), rayons de combat deduits. EN 3D, comme le coeur (dx, dy ET dz) : en 2D, un denivele (tombe,
// rampe, pont) faisait croire le bot a portee alors que le coeur refusait OUT_OF_RANGE. Le journal, lui, ecrit la
// distance de centre a centre en 2D, celle de sonde-combat-pvp.py.
float CoaAreneDistanceCombat(Unit const* a, Unit const* b)
{
    return std::max(0.0f, a->GetExactDist(b) - a->GetCombatReach() - b->GetCombatReach());
}

// Banc du 10/10 (Paris) : le Ranger se plantait au bord (plante jusqu'a haut - 1, 29 yd de combat) et ses premiers
// tirs partaient refuses OUT_OF_RANGE a 33,3 yd de centre a centre (30 + 2 x 1,5 de rayon = 33 yd pour le coeur) :
// l'adversaire n'a qu'a faire un pas. Plante, et retour vers la portee, visent desormais haut - 3.
constexpr float CoaArenePlanteMarge = 3.0f;

// La plage de tir du bot, lue sur ses propres sorts de DEGATS a distance (25 yd et plus) : [plus grande portee
// minimale, plus petite portee maximale]. Repli 5-30 (Quick Shot) si le kit n'en dit rien.
// Revue M1 : un utilitaire court (Dust Toss, 0-15, sans effet de degats) ramenait `haut` a 15 et la distance
// visee a 8 yd au lieu de 23. Seuls comptent les sorts qui portent la rotation a distance.
// Rend vrai si la plage vient de sorts reels, faux si c'est le repli 5-30 (revue D4).
bool CoaArenePlage(PlayerbotAI* botAI, Player* bot, float& bas, float& haut)
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
        return false;
    }
    return true;
}

// Un bot sans plage de tir lue sur de vrais sorts (melee, ou kit sans tir de 25 yd et plus) : son retour s'arrete
// des qu'il voit l'adversaire et le reprend pour cible ; « reach melee » le mene ensuite au contact. Le repli 5-30 de
// CoaArenePlage l'aurait sinon arrete a 26 yd.
bool CoaAreneRetourAuContact(PlayerbotAI* botAI, Player* bot)
{
    if (GetCoaStyle(bot) == CoaStyle::Melee)
        return true;
    float bas = 0.0f, haut = 0.0f;
    return !CoaArenePlage(botAI, bot, bas, haut);
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

// La vitesse a laquelle la cible peut suivre : 0 si elle est immobilisee ou neutralisee.
float CoaAreneVitesseSuivie(Unit* cible)
{
    if (cible->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING) ||
        cible->isFrozen() || cible->HasRootAura())
        return 0.0f;
    return cible->GetSpeed(MOVE_RUN);
}

// Ce qui reste de la fenetre : la plus longue duree restante des auras qui ralentissent, enracinent ou
// neutralisent la cible. Une aura sans duree lisible (permanente, ou duree inconnue) compte pour 2 s.
uint32 CoaAreneDureeFenetreMs(Unit* cible)
{
    int32 reste = 0;
    bool trouve = false;
    for (AuraType const type : { SPELL_AURA_MOD_DECREASE_SPEED, SPELL_AURA_MOD_ROOT, SPELL_AURA_MOD_STUN,
                                 SPELL_AURA_MOD_CONFUSE, SPELL_AURA_MOD_FEAR })
        for (AuraEffect const* effet : cible->GetAuraEffectsByType(type))
        {
            Aura const* aura = effet ? effet->GetBase() : nullptr;
            if (!aura)
                continue;
            trouve = true;
            int32 const d = aura->GetDuration();
            reste = std::max(reste, d > 0 ? d : int32(CoaAreneFenetreParDefautMs));
        }
    return trouve ? uint32(reste) : CoaAreneFenetreParDefautMs;
}

// La distance de combat qu'un recul peut atteindre avant la fin de la fenetre : la cible suit a vCible, le bot
// court a vBot. C'est la PHYSIQUE du recul : a vitesse egale on ne creuse aucun ecart (d'ou la rupture), et un
// Dazed (-50 %) de 4 s ne fait gagner que (7 - 3,5) x 4 = 14 yd. Le 10/10 a 19:15, 8,8 -> 17,5 yd (visee 23).
float CoaAreneReculAtteignable(Player* bot, Unit* cible, float d0)
{
    float const vBot = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
    float const vCible = CoaAreneVitesseSuivie(cible);
    if (vCible >= vBot)
        return d0;
    return d0 + (vBot - vCible) * float(CoaAreneDureeFenetreMs(cible)) / float(IN_MILLISECONDS);
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

/*
 * Banc d'arene 1v1 (`.coa arene banc ... cacheur=A|B`, CoaArene1v1.cpp) : ce bot est-il le CACHEUR d'un match de
 * banc en cours ? Arene 1v1 (type 1) portes ouvertes seulement ; la decision (verrou de CoaArene1v1) est prise une
 * fois par instance et gardee dans l'etat. Independant de CoaArene.IaPiliers et du bras tire : seul le match de banc
 * marque l'active. Fil de l'IA du bot ; ne lit que le bot et son arene (P-137).
 */
bool CoaAreneCacheurActif(Player* bot, CoaAreneEtat& etat)
{
    if (!bot->InArena())
        return false;
    Battleground* bg = bot->GetBattleground();
    if (!bg || !bg->isArena() || bg->GetArenaType() != CoaArene1v1::TYPE_ARENE || bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;
    if (etat.cacheurInstance != bg->GetInstanceID())
    {
        etat.cacheurInstance = bg->GetInstanceID();
        etat.cacheurOui = CoaArene1v1::CacheurDeBanc(bot->GetGUID().GetCounter());
        if (etat.cacheurOui)
            CoaAreneJournal(bot, etat, "cacheur_debut", -1.0f, -1.0f,
                            Acore::StringFormat(" instance={} classe={}", bg->GetInstanceID(), bot->getClass()));
    }
    return etat.cacheurOui;
}

// Fin d'un recul : la distance atteinte se lit a l'arrivee, pas a la decision. `vise` est la distance que la
// fenetre permettait d'atteindre (<= `vise_max`, la distance de confort) ; le succes se juge contre elle.
// `arreter` : le bot court encore (ecart atteint, ou fenetre close) -- il s'arrete pour se retourner et tirer.
void CoaAreneCloreRecul(Player* bot, CoaAreneEtat& etat, Unit* adversaire, char const* raison, bool arreter = false)
{
    if (arreter && bot->isMoving())
        bot->StopMoving();
    float const apres = adversaire ? bot->GetExactDist2d(adversaire) : -1.0f;
    bool const reussi = adversaire && CoaAreneDistanceCombat(bot, adversaire) >= etat.reculVise - 2.0f;
    etat.reculsReussis += reussi ? 1 : 0;
    CoaAreneJournal(bot, etat, "recul", etat.reculDistAvant, apres,
                    Acore::StringFormat(" vise={:.1f} vise_max={:.1f} reussi={} duree_ms={} fin={}", etat.reculVise,
                                        etat.reculViseMax, reussi ? 1 : 0, getMSTimeDiff(etat.reculDebutMs, getMSTime()),
                                        raison));
    etat.reculFinMs = 0;
}

// L2 : suivi d'une cachette en cours (defini plus bas, apres CoaAreneSortPret).
void CoaArenePilierSuivre(PlayerbotAI* botAI, Player* bot, CoaAreneEtat& etat, Unit* adv, bool engage, uint32 maintenant);

// Cloture d'un engagement : recul et cachette en cours clos, bilan ecrit (les deux bras), etat desarme. `fin` dit
// pourquoi : « grace » (10 s sans engagement vu) ou « nouvelle_instance » (une autre arene, ou le passage
// arene <-> duel/monde, avant la fin de la grace). Champ ajoute EN FIN de ligne (outils/banc-arene.py lit cle=valeur).
void CoaAreneClore(PlayerbotAI* botAI, Player* bot, CoaAreneEtat& etat, Unit* adv, uint32 maintenant, char const* fin)
{
    if (etat.reculFinMs)
        CoaAreneCloreRecul(bot, etat, adv, "fin_engagement");
    if (etat.pilierDebutMs)
        CoaArenePilierSuivre(botAI, bot, etat, nullptr, false, maintenant);
    etat.retourEnCours = false;
    uint32 const n = std::max<uint32>(etat.echantillons, 1);
    CoaAreneJournal(bot, etat, "bilan", -1.0f, -1.0f,
                    Acore::StringFormat(" duree_s={} echantillons={} contact_pct={} plage_pct={} reculs={} "
                                        "reculs_reussis={} ruptures={} bursts={} ouvertures={} pv_min={:.0f} "
                                        "retours={} piliers={} pilier_cumul_ms={}",
                                        getMSTimeDiff(etat.debutMs, maintenant) / IN_MILLISECONDS, etat.echantillons,
                                        etat.auContact * 100 / n, etat.dansLaPlage * 100 / n, etat.reculs,
                                        etat.reculsReussis, etat.ruptures, etat.bursts, etat.ouvertures, etat.pvMin,
                                        etat.retours, etat.piliers, etat.pilierCumulMs) +
                    (etat.cacheurOui ? Acore::StringFormat(" cacheur=1 cacheur_caches={} cacheur_tirs={} "
                                                           "cacheur_aucun={} cacheur_vu={}", etat.cacheurCaches,
                                                           etat.cacheurTirs, etat.cacheurAucun, etat.cacheurVu)
                                     : std::string()) +
                    Acore::StringFormat(" instance={} fin={}", etat.instance, fin));
    etat.actif = false;
    etat.cacheurEnRouteMs = 0;
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
    // L'instance d'arene de l'engagement vu a ce tick (0 : duel, monde ouvert, ou aucun). CoaAreneDetecter a deja lu
    // GetBattleground() pour COA_ARENE_ARENE : non nul ici.
    Battleground const* bgVu = cadre == COA_ARENE_ARENE ? bot->GetBattleground() : nullptr;
    uint32 const instanceVue = bgVu ? bgVu->GetInstanceID() : 0;

    // Banc du 10/10/2026 22:55-23:00 (Paris) : 2617 enchaine les matchs 1 et 3 a moins de 10 s (le delai de grace) ;
    // l'engagement restait ouvert et un seul bilan de 246 s couvrait les deux. Un engagement est desormais propre a
    // UNE instance d'arene : une autre instance (ou le passage arene <-> duel/monde) pendant la grace clot l'ancien,
    // bilan ecrit, et le bloc suivant en ouvre un neuf au meme tick.
    if (etat.actif && cadre != COA_ARENE_AUCUN && instanceVue != etat.instance)
    {
        Unit* ancien = etat.adversaire ? ObjectAccessor::GetUnit(*bot, etat.adversaire) : nullptr;
        if (ancien && !ancien->IsInWorld())
            ancien = nullptr;
        CoaAreneClore(botAI, bot, etat, ancien, maintenant, "nouvelle_instance");
    }

    if (cadre != COA_ARENE_AUCUN && !etat.actif)
    {
        uint32 const tirage = sPlayerbotAIConfig.coaAreneTirage;
        etat = CoaAreneEtat();
        // Revue D3 : releve de vie du defensif reparti de zero (deja vrai par les valeurs par defaut ; dit ici pour
        // qu'un champ ajoute a l'etat ne le casse pas en silence) -- pas de faux « burst subi » au match suivant.
        etat.pvRefMs = 0;
        etat.pvRef = etat.pvPrec = bot->GetHealthPct();
        etat.majMs = maintenant ? maintenant : 1;
        etat.actif = true;
        etat.cadre = cadre;
        etat.instance = instanceVue;
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
    // Releve de vie pour le defensif (burst subi) : deux releves espaces de 2 s.
    if (!etat.pvRefMs || getMSTimeDiff(etat.pvRefMs, maintenant) >= CoaAreneDefensifReleveMs)
    {
        etat.pvPrec = etat.pvRefMs ? etat.pvRef : bot->GetHealthPct();
        etat.pvRef = bot->GetHealthPct();
        etat.pvRefMs = maintenant ? maintenant : 1;
    }
    // La pile tenue ne se compte que pendant l'engagement vu (sinon une pile pleine d'avant ferait partir le burst).
    if (!etat.vuCeTick)
        etat.pilePleineDepuisMs = 0;
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

    // Recul en cours : clos quand l'ecart vise est atteint (le bot s'arrete : courir plus loin le sortirait de
    // portee si la cible cesse de suivre), quand la cible peut de nouveau suivre (a vitesse egale, courir ne
    // creuse plus rien et lui tourne le dos), a l'echeance, ou quand le bot s'est arrete (arrive, ou retenu).
    if (etat.reculFinMs)
    {
        uint32 const depuis = getMSTimeDiff(etat.reculDebutMs, maintenant);
        bool const vivante = adv && adv->IsAlive() && bot->IsAlive();
        if (vivante && depuis > 300 && CoaAreneDistanceCombat(bot, adv) >= etat.reculVise)
            CoaAreneCloreRecul(bot, etat, adv, "atteint", true);
        else if (vivante && depuis > 300 && !CoaAreneFenetreDeRecul(bot, adv))
            CoaAreneCloreRecul(bot, etat, adv, "fenetre_close", true);
        else if (depuis >= getMSTimeDiff(etat.reculDebutMs, etat.reculFinMs))
            CoaAreneCloreRecul(bot, etat, adv, "echeance");
        else if (!bot->isMoving() && depuis > 300)
            CoaAreneCloreRecul(bot, etat, adv, "arret");
    }

    // Retour en cours (L1) : le bot court vers la cible ; il s'arrete des qu'il la voit et qu'elle est a portee
    // confortable, au lieu d'aller au contact (un tireur au contact ne tire plus).
    if (etat.retourEnCours)
    {
        float bas = 0.0f, haut = 0.0f;
        if (!adv || !adv->IsAlive() || !bot->IsAlive())
            etat.retourEnCours = false;
        else if (CoaAreneRetourAuContact(botAI, bot))
        {
            // Melee : fini des que l'adversaire est en vue, sans s'arreter (le retour reprend la cible au tick
            // suivant, « reach melee » prend la suite dans le meme elan).
            if (bot->IsWithinLOSInMap(adv))
                etat.retourEnCours = false;
            else if (!bot->isMoving() && etat.dernierRetourMs && getMSTimeDiff(etat.dernierRetourMs, maintenant) > 500)
                etat.retourEnCours = false;
        }
        else
        {
            CoaArenePlage(botAI, bot, bas, haut);
            if (bot->IsWithinLOSInMap(adv) && CoaAreneDistanceCombat(bot, adv) <= haut - CoaAreneMargePortee - 2.0f)
            {
                if (bot->isMoving())
                    bot->StopMoving();
                etat.retourEnCours = false;
            }
            else if (!bot->isMoving() && etat.dernierRetourMs && getMSTimeDiff(etat.dernierRetourMs, maintenant) > 500)
                etat.retourEnCours = false;  // arrive, ou mouvement repris par autre chose : le declencheur jugera
        }
    }

    // L2, cachette en cours : sortie (voir CoaArenePilierSortie, plus bas).
    if (etat.pilierDebutMs)
        CoaArenePilierSuivre(botAI, bot, etat, adv, cadre != COA_ARENE_AUCUN && !preparation, maintenant);

    // Banc, cacheur : deplacement vers la cachette clos a l'arrivee (arret), a l'echeance (4 s : jamais coince en
    // chemin) ou a la mort. `vu` : l'adversaire le voit encore a l'arrivee (cachette ratee ; il en cherchera une autre
    // a la prochaine recherche, 700 ms plus tard).
    if (etat.cacheurEnRouteMs)
    {
        uint32 const depuis = getMSTimeDiff(etat.cacheurEnRouteMs, maintenant);
        if (!bot->IsAlive() || depuis > CoaAreneCacheurRouteMaxMs || (depuis > 300 && !bot->isMoving()))
        {
            bool const vu = adv && adv->IsAlive() && adv->IsWithinLOSInMap(bot);
            etat.cacheurVu += vu ? 1 : 0;
            float const d = adv ? bot->GetExactDist2d(adv) : -1.0f;
            CoaAreneJournal(bot, etat, "cacheur_arrive", d, d,
                            Acore::StringFormat(" vu={} duree_ms={} fin={}", vu ? 1 : 0, depuis,
                                                !bot->IsAlive() ? "mort" : depuis > CoaAreneCacheurRouteMaxMs ? "echeance" : "arret"));
            etat.cacheurEnRouteMs = 0;
        }
    }

    if (cadre == COA_ARENE_AUCUN && getMSTimeDiff(etat.dernierContactMs, maintenant) > CoaAreneGraceMs)
    {
        CoaAreneClore(botAI, bot, etat, adv, maintenant, "grace");
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

// Le sort accorde-t-il une immunite qui peut lever l'entrave ? Recopie de la premiere boucle de
// Spell::CheckCasterAuras (Spell.cpp:6968-6984) : SPELL_ATTR1_IMMUNITY_PURGES_EFFECT n'exempte de rien par lui-meme ;
// le coeur ne regarde plus loin que si un effet accorde une immunite d'ecole, de mecanisme ou de dissipation (ou l'un
// des sept bijoux JcJ nommes). Banc de reference du 10/10 23:38-23:42 (Paris) : Eye of Archimonde 804052, Stoicism
// 804802, Vigil of the Moon 802797 portent l'attribut (AttributesEx 0x8000) SANS aucun effet d'immunite -- 32 refus
// SPELL_FAILED_STUNNED que le filtre laissait passer, l'attribut seul suffisant a les exempter.
bool CoaAreneAccordeImmunite(SpellInfo const* info)
{
    if (!info->HasAttribute(SPELL_ATTR1_IMMUNITY_PURGES_EFFECT))
        return false;
    switch (info->Id)
    {
        case 42292: case 59752: case 65547: case 53490: case 19574: case 34471: case 46227:
            return true;
        default:
            break;
    }
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (info->Effects[i].ApplyAuraName == SPELL_AURA_SCHOOL_IMMUNITY ||
            info->Effects[i].ApplyAuraName == SPELL_AURA_MECHANIC_IMMUNITY ||
            info->Effects[i].ApplyAuraName == SPELL_AURA_DISPEL_IMMUNITY)
            return true;
    return false;
}

bool CoaAreneBloqueParEtat(Player* bot, SpellInfo const* info)
{
    // Un sort qui accorde une immunite : le coeur compare alors chaque aura du lanceur a l'immunite ; on le lui laisse.
    if (info->HasAttribute(SPELL_ATTR6_NOT_AN_ATTACK) || CoaAreneAccordeImmunite(info))
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
// (portee) a deja ete filtre par DropUncastableHere chez l'appelant ; l'orientation, CastFirst la corrige (demi-tour).
bool CoaAreneSortPret(PlayerbotAI* botAI, Player* bot, Usable const& spell)
{
    auto const& banc = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->benchedSpells;
    time_t const maintenant = time(nullptr);
    SpellInfo const* info = EffectiveSpell(bot, spell.info);
    if (bot->HasSpellCooldown(info->Id) || IsBenched(banc, spell.info->Id, maintenant) || IsBenched(banc, info->Id, maintenant))
        return false;
    // Memes tests que CastFirst (ressource, forme, arme : CoaPeutPayer, CoaRefuseParFormeOuArme) : un sort qu'il
    // ecarte ne compte pas comme pret.
    return CoaPeutPayer(bot, info) && !CoaRefuseParFormeOuArme(bot, info);
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

    // Revue du 10/10 soir : la pile pleine seule n'ouvre plus la fenetre (Skullpiercer a pile 5 sur une cible a
    // 100 %, en ouverture, a 19:15). A pile pleine, il faut aussi une cible ralentie, enracinee ou controlee
    // (le recul et les tirs suivants la punissent), ou sous 60 %. Mais au plafond chaque generateur de plus est
    // perdu (pas de 6e charge) : apres 8 s de pile pleine sans fenetre, on depense quand meme (« pile_tenue »).
    bool const pleine = burst.pile && plafond && burst.pile >= plafond;
    uint32 const maintenant = getMSTime();
    if (!pleine)
        etat.pilePleineDepuisMs = 0;
    else if (!etat.pilePleineDepuisMs)
        etat.pilePleineDepuisMs = maintenant ? maintenant : 1;

    if (burst.pile && cible->GetHealthPct() <= 30.0f)
        burst.raison = "vie_cible";
    else if (burst.pile && cible->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
        burst.raison = "controle";
    else if (burst.pile && bot->GetHealthPct() <= 35.0f)
        burst.raison = "vie_bot";
    else if (pleine && CoaAreneFenetreDeRecul(bot, cible))
        burst.raison = "pile_ralentie";
    else if (pleine && cible->GetHealthPct() < CoaAreneBurstPvCible)
        burst.raison = "pile_pv60";
    else if (pleine && getMSTimeDiff(etat.pilePleineDepuisMs, maintenant) >= CoaArenePileTenueMaxMs)
        burst.raison = "pile_tenue";
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

// ---------------------------------------------------------------------------------------------------------------
// Trace des gladiateurs d'arene 1v1 (lot 3, 10/10 soir ; AiPlayerbot.Arene1v1.Trace, defaut 1).
//
// Pourquoi : le combat du 10/10 a 21:12 (Paris) n'a laisse que deux lignes (engagement, bilan) ; on ne savait ni
// quels sorts le gladiateur tentait, ni pourquoi ils ne partaient pas. Ici : une ligne par tentative de sort de
// CastFirst (OK, ou refus avec le SpellCastResult du coeur ou le motif du moteur), et une par seconde avec la
// derniere action reussie du moteur et la cible courante.
//
// Cout : hors arene, un test InArena() par appel de CastFirst. En arene, la decision « gladiateur engage dans une
// arene 1v1 » est prise UNE fois par instance (GladiateurEngage prend un verrou) et gardee dans l'etat du bot.
// Debit : CoaAreneTraceParSeconde lignes par seconde et par bot au plus ; le surplus est compte (`omises=`).
// Fil : celui de l'IA du bot ; ne lit que le bot, sa cible et l'etat du bot (P-137).
// ---------------------------------------------------------------------------------------------------------------
bool CoaAreneTraceActive(PlayerbotAI* botAI, Player* bot)
{
    if (!sPlayerbotAIConfig.arene1v1Trace || !bot || !bot->InArena())
        return false;
    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;
    CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
    if (etat.traceInstance != bg->GetInstanceID())
    {
        etat.traceInstance = bg->GetInstanceID();
        etat.traceOui = bg->isArena() && bg->GetArenaType() == CoaArene1v1::TYPE_ARENE &&
                        CoaArene1v1::GladiateurEngage(bot->GetGUID().GetCounter());
        etat.traceAuraPile = 0;
        if (etat.traceOui)
            for (Usable const& spell : KnownAbilities(botAI, bot, [](uint16) { return true; }))
                if ((etat.traceAuraPile = CoaAreneAuraConsommee(bot, spell.info)))
                    break;
    }
    return etat.traceOui;
}

// Debit : vrai si une ligne peut s'ecrire dans cette seconde ; `omises` rend le nombre tu depuis la derniere.
bool CoaAreneTraceDebit(CoaAreneEtat& etat, uint32& omises)
{
    uint32 const seconde = getMSTime() / IN_MILLISECONDS;
    if (seconde != etat.traceSeconde)
    {
        etat.traceSeconde = seconde;
        etat.traceLignes = 0;
    }
    if (etat.traceLignes >= CoaAreneTraceParSeconde)
    {
        ++etat.traceOmises;
        return false;
    }
    ++etat.traceLignes;
    omises = etat.traceOmises;
    etat.traceOmises = 0;
    return true;
}

uint32 CoaAreneTracePile(Player* bot, CoaAreneEtat const& etat)
{
    Aura const* pile = etat.traceAuraPile ? bot->GetAura(etat.traceAuraPile) : nullptr;
    return pile ? pile->GetStackAmount() : 0;
}

void CoaAreneTracerSort(PlayerbotAI* botAI, Player* bot, SpellInfo const* info, Unit* target, SpellCastResult resultat,
                        char const* motif)
{
    if (!info)
        return;
    CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
    uint32 omises = 0;
    if (!CoaAreneTraceDebit(etat, omises))
        return;
    std::string raison = "-";
    bool const ok = resultat == SPELL_CAST_OK && !motif;
    if (motif)
        raison = motif;
    else if (!ok)
    {
        // EnumUtils leve une exception sur une valeur hors de l'enumeration (enuminfo_SharedDefines.cpp) : le
        // nombre brut alors.
        try { raison = EnumUtils::ToConstant(resultat); }
        catch (...) { raison = std::to_string(uint32(resultat)); }
    }
    std::string nom = (info->SpellName[0] && *info->SpellName[0]) ? info->SpellName[0] : "?";
    std::replace(nom.begin(), nom.end(), ' ', '_');
    LOG_INFO("playerbots.coa", "coa arene trace: guid={} sort={} nom={} resultat={} raison={} dist={:.1f} pv_bot={:.0f}"
             " pv_cible={:.0f} pile_advantage={} omises={}", bot->GetGUID().GetCounter(), info->Id, nom,
             ok ? "OK" : "refus", raison, target && target != bot ? bot->GetExactDist2d(target) : 0.0f,
             bot->GetHealthPct(), target ? target->GetHealthPct() : -1.0f, CoaAreneTracePile(bot, etat), omises);
}

// La derniere action REUSSIE du moteur courant, lue dans son journal d'actions (Engine::LogAction : « |A:<nom> -
// OK », 512 caracteres au plus ; rempli parce que AiPlayerbot.LogInGroupOnly = 0 en service). « - » sinon.
std::string CoaAreneDerniereAction(PlayerbotAI* botAI)
{
    std::string const journal = botAI->HandleRemoteCommand("action");
    size_t pos = journal.size();
    while (pos > 0)
    {
        size_t const a = journal.rfind("|A:", pos - 1);
        if (a == std::string::npos)
            break;
        size_t fin = journal.find('|', a + 1);
        if (fin == std::string::npos)
            fin = journal.size();
        std::string const entree = journal.substr(a + 3, fin - a - 3);
        if (entree.size() > 5 && entree.compare(entree.size() - 5, 5, " - OK") == 0)
            return entree.substr(0, entree.size() - 5);
        pos = a;
    }
    return "-";
}

Unit* CoaAreneAdversaireVivant(Player* bot, CoaAreneEtat const& etat);  // plus bas

// Une ligne par seconde (appelee a chaque tour du moteur par CoaAreneMultiplier::ObserveQueue).
void CoaAreneTracerTick(PlayerbotAI* botAI, Player* bot)
{
    if (!CoaAreneTraceActive(botAI, bot))
        return;
    CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
    uint32 const maintenant = getMSTime();
    if (etat.traceTickMs && getMSTimeDiff(etat.traceTickMs, maintenant) < IN_MILLISECONDS)
        return;
    uint32 omises = 0;
    if (!CoaAreneTraceDebit(etat, omises))
        return;
    etat.traceTickMs = maintenant ? maintenant : 1;
    Unit* cible = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    std::string action = CoaAreneDerniereAction(botAI);
    std::replace(action.begin(), action.end(), ' ', '_');
    // Banc (outils/banc-arene.py) : la vue, la distance et la portee vers l'ADVERSAIRE de l'engagement, qu'il soit la
    // cible ou non (`vue` et `dist` ci-dessus suivent la cible courante, vide des que la vue se perd). Ajoutes EN FIN
    // de ligne : les lecteurs du format d'avant ne bougent pas. portee_adv : au contact pour un bot de melee, dans la
    // plage de tir lue sur ses sorts sinon, et en vue.
    Unit* adv = CoaAreneAdversaireVivant(bot, etat);
    bool const vueAdv = adv && bot->IsWithinLOSInMap(adv);
    bool porteeAdv = false;
    if (adv && vueAdv)
    {
        float bas = 0.0f, haut = 0.0f;
        if (CoaAreneRetourAuContact(botAI, bot))
            porteeAdv = bot->IsWithinMeleeRange(adv);
        else
        {
            CoaArenePlage(botAI, bot, bas, haut);
            porteeAdv = CoaAreneDistanceCombat(bot, adv) <= haut;
        }
    }
    LOG_INFO("playerbots.coa", "coa arene trace: guid={} tick action={} moteur={} cible={} cible_adversaire={} dist={:.1f}"
             " vue={} pv_bot={:.0f} pv_cible={:.0f} pile_advantage={} bouge={} omises={} adv={} vue_adv={} dist_adv={:.1f}"
             " portee_adv={} cacheur={}", bot->GetGUID().GetCounter(),
             action, botAI->GetState() == BOT_STATE_COMBAT ? "combat" : "hors_combat",
             cible ? cible->GetGUID().GetCounter() : 0, CoaAreneEstAdversaire(etat, cible) ? 1 : 0,
             cible ? bot->GetExactDist2d(cible) : -1.0f, cible && bot->IsWithinLOSInMap(cible) ? 1 : 0,
             bot->GetHealthPct(), cible ? cible->GetHealthPct() : -1.0f, CoaAreneTracePile(bot, etat),
             bot->isMoving() ? 1 : 0, omises, adv ? adv->GetGUID().GetCounter() : 0, vueAdv ? 1 : 0,
             adv ? bot->GetExactDist2d(adv) : -1.0f, porteeAdv ? 1 : 0, CoaAreneCacheurActif(bot, etat) ? 1 : 0);
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
// Retour (L1) et cachette (L2) : l'etat partage par les declencheurs, actions et le suivi de CoaAreneMaj.
// ---------------------------------------------------------------------------------------------------------------
bool CoaAreneCachetteEnCours(CoaAreneEtat const& etat)
{
    return etat.pilierDebutMs != 0;
}

// L'adversaire de l'engagement, s'il est la, vivant et sur la carte du bot.
Unit* CoaAreneAdversaireVivant(Player* bot, CoaAreneEtat const& etat)
{
    Unit* adv = etat.adversaire ? ObjectAccessor::GetUnit(*bot, etat.adversaire) : nullptr;
    if (!adv || !adv->IsInWorld() || !adv->IsAlive() || adv->GetMapId() != bot->GetMapId())
        return nullptr;
    return adv;
}

// Le bot peut se deplacer de lui-meme a ce tick.
bool CoaAreneLibreDeBouger(PlayerbotAI* botAI, Player* bot)
{
    // Revue m2 : ni pendant une incantation (un retour ou une cachette la couperait).
    return !bot->IsRooted() && !bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_LOST_CONTROL) && botAI->CanMove() &&
           !bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) && !bot->IsNonMeleeSpellCast(false, false, true);  // revue D1 : le tir automatique ne fige pas
}

/*
 * Banc du 10/10/2026, 22:56-22:58 (Paris), matchs 1 et 2 : 150 s chacun, 0 sort, 0 retour. Les deux chasseurs etaient
 * des Rangers furtifs (Elude pris pendant la preparation, « coa arene preparation »). Or le retour se taisait pour un
 * Ranger furtif (« l'ouverture s'approche deja elle-meme »), et l'ouverture ne s'approche qu'en LIGNE DROITE : un point
 * a (portee - 5) yd de l'adversaire sur la droite adversaire -> bot, borne par le lancer de rayon de
 * CheckCollisionAndGetValidCoords. Derriere un pilier (562, match 2 : 682 a 31-49 yd, vue_adv=0 pendant 138 s), ce point
 * reste hors de vue ; contre un relief (572, match 1 : 2617 fige a 79,4 yd, vue_adv=1, bouge=0), le rayon s'arrete au
 * bot. Les matchs 3 et 4 se sont battus : 559 est degagee (ligne droite libre), et le chasseur du 4 n'est pas un Ranger.
 *
 * Partage desormais : l'ouverture garde le bot quand elle peut tirer (adversaire en vue a 40 yd au plus) et que son
 * delai est ecoule ; partout ailleurs (hors de vue, trop loin, ouverture en attente apres une approche ratee), le
 * retour, qui suit un CHEMIN (MoveTo, PathGenerator) et s'arrete en vue a portee (CoaAreneMaj).
 */
bool CoaAreneOuvertureDansSaFenetre(Player* bot, Unit* adv)
{
    return adv && bot->GetExactDist2d(adv) <= CoaAreneOuvertureDistanceMax && bot->IsWithinLOSInMap(adv);
}

// L'ouverture attend (approche faite, ou essai rate) : son prochain essai est dans le futur.
bool CoaAreneOuvertureEnAttente(CoaAreneEtat const& etat)
{
    return etat.prochaineOuvertureMs && getMSTimeDiff(etat.prochaineOuvertureMs, getMSTime()) >= 0x80000000u;
}

// Un sort de degats du bot est pret (recharge, banc, ressource) ; portee et ligne de vue mises a part.
bool CoaAreneUnTirPret(PlayerbotAI* botAI, Player* bot)
{
    for (Usable const& spell : KnownAbilities(botAI, bot,
             [](uint16 kind) { return IsAttack(kind, false) && (kind & KIND_DAMAGE); }))
        if (CoaAreneSortPret(botAI, bot, spell))
            return true;
    return false;
}

/*
 * Pourquoi le retour existe (incident du 10/10 a 19:21 Paris, gladiateur 2596, carte 572) : apres un recul fini a
 * 32,2 yd, derriere la tombe, plus une ligne pendant 106 s, et le joueur a du venir au bot. Lu dans le code :
 * hors de vue, « invalid target » (InvalidTargetValue.cpp:25, AttackersValue::IsValidTarget exige la ligne de
 * vue) -> « drop target » (99) : cible effacee, moteur non-combat. Aucune valeur de cible ne retient un joueur
 * hors de vue (« possible targets » filtre IsWithinLOSInMap), donc « reach spell » n'a rien a rejoindre ; et
 * ArenaTactics, en combat, ne se replace que sur GetVictim(), vide pour un tireur sans cible. Le bot attend que
 * le joueur reapparaisse. Meme boucle au combat id=3 (17:28, plage_pct=2) : le bot restait hors de portee.
 *
 * Le retour : l'engagement connait l'adversaire (etat.adversaire, relu a chaque tick, meme si le joueur entre
 * apres le bot) ; s'il est hors de vue ou hors de portee, le bot va vers lui par un chemin, et s'arrete des
 * qu'il le voit a portee confortable (CoaAreneMaj) ; s'il le voit a portee sans l'avoir pour cible, il le
 * reprend pour cible et repasse en moteur de combat.
 */
enum CoaAreneRetourRaison : uint8
{
    COA_RETOUR_AUCUN = 0,
    COA_RETOUR_VUE = 1,
    COA_RETOUR_PORTEE = 2,
    COA_RETOUR_CIBLE = 3
};

char const* CoaAreneRetourNom(uint8 raison)
{
    switch (raison)
    {
        case COA_RETOUR_VUE: return "vue";
        case COA_RETOUR_PORTEE: return "portee";
        case COA_RETOUR_CIBLE: return "cible";
        default: return "aucun";
    }
}

uint8 CoaAreneRetourRaison(PlayerbotAI* botAI, Player* bot, Unit* adv)
{
    if (!bot->CanSeeOrDetect(adv) || !bot->IsValidAttackTarget(adv))
        return COA_RETOUR_AUCUN;  // furtif, ou plus attaquable : rien a poursuivre
    if (!bot->IsWithinLOSInMap(adv))
        return COA_RETOUR_VUE;
    // En vue : la cible est reprise D'ABORD, quelle que soit la distance (10/10 nuit ; avant, un tireur en vue mais
    // hors de portee courait sans cible jusqu'a portee). Le moteur de combat et « reach spell »/« reach melee »
    // retrouvent ainsi leur cible des que l'adversaire reparait.
    Unit* cible = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    // Meme test que « invalid target » (InvalidTargetValue.cpp:25) : sans lui, une cible que le moteur refuse
    // (immunite totale, zone sans JcJ...) serait reprise puis lachee chaque seconde.
    // Revue M1 : seulement si le bot n'a PAS de cible valide -- jamais par-dessus le choix de cible du moteur.
    if ((!cible || !AttackersValue::IsValidTarget(cible, bot)) && AttackersValue::IsValidTarget(adv, bot))
        return COA_RETOUR_CIBLE;
    // Melee : en vue et pour cible, « reach melee » fait le reste.
    if (CoaAreneRetourAuContact(botAI, bot))
        return COA_RETOUR_AUCUN;
    float bas = 0.0f, haut = 0.0f;
    CoaArenePlage(botAI, bot, bas, haut);
    if (CoaAreneDistanceCombat(bot, adv) > haut - CoaArenePlanteMarge)
        return COA_RETOUR_PORTEE;
    return COA_RETOUR_AUCUN;
}

// L2 : pourquoi se cacher maintenant (0 : pas de raison). Vie basse et cible au contact ou qui incante, ou
// aucun tir pret.
uint8 CoaArenePilierRaison(PlayerbotAI* botAI, Player* bot, Unit* adv)
{
    float bas = 0.0f, haut = 0.0f;
    CoaArenePlage(botAI, bot, bas, haut);
    if (bot->GetHealthPct() < CoaArenePilierPvBot &&
        (CoaAreneDistanceCombat(bot, adv) < bas + 3.0f || adv->IsNonMeleeSpellCast(false)))
        return COA_PILIER_PV;
    if (!CoaAreneUnTirPret(botAI, bot))
        return COA_PILIER_RECHARGE;
    return 0;
}

void CoaArenePilierSortie(Player* bot, CoaAreneEtat& etat, Unit* adv, char const* raison, uint32 maintenant)
{
    uint32 const duree = getMSTimeDiff(etat.pilierDebutMs, maintenant);
    etat.pilierCumulMs += duree;
    etat.pilierDebutMs = 0;
    etat.derniereSortiePilierMs = maintenant ? maintenant : 1;
    float const d = adv ? bot->GetExactDist2d(adv) : -1.0f;
    CoaAreneJournal(bot, etat, "sortie_pilier", d, d,
                    Acore::StringFormat(" raison={} duree_ms={} cumul_ms={}", raison, duree, etat.pilierCumulMs));
}

// Cachette en cours : sortie bornee (8 s au plus), des que l'adversaire voit le bot une fois arrive (la cachette
// a echoue : il a contourne), ou des qu'un tir est pret (et, pour la vie basse, que la cible n'est ni au contact
// ni en train d'incanter). Le retour ramene ensuite le bot en vue et a portee. Pas de nouvelle cachette avant
// 10 s : c'est ce qui empeche l'oscillation cachette <-> sortie.
void CoaArenePilierSuivre(PlayerbotAI* botAI, Player* bot, CoaAreneEtat& etat, Unit* adv, bool engage, uint32 maintenant)
{
    if (!engage || !adv || !adv->IsAlive() || !bot->IsAlive())
    {
        CoaArenePilierSortie(bot, etat, adv, "fin", maintenant);
        return;
    }
    uint32 const depuis = getMSTimeDiff(etat.pilierDebutMs, maintenant);
    if (depuis >= CoaArenePilierMaxMs)
    {
        CoaArenePilierSortie(bot, etat, adv, "duree", maintenant);
        return;
    }
    if (depuis > 1500 && !bot->isMoving() && adv->IsWithinLOSInMap(bot))
    {
        CoaArenePilierSortie(bot, etat, adv, "vu", maintenant);
        return;
    }
    if (depuis > IN_MILLISECONDS && CoaAreneUnTirPret(botAI, bot))
    {
        float bas = 0.0f, haut = 0.0f;
        CoaArenePlage(botAI, bot, bas, haut);
        bool const menace = CoaAreneDistanceCombat(bot, adv) < bas + 3.0f || adv->IsNonMeleeSpellCast(false);
        if (etat.pilierRaison == COA_PILIER_RECHARGE || (depuis > 3 * IN_MILLISECONDS && !menace))
            CoaArenePilierSortie(bot, etat, adv, "pret", maintenant);
    }
}

// Lot 3, « il se balade » : un bot a distance qui a l'adversaire pour cible, EN VUE et dans sa plage de tir (hors
// portee minimale), n'a aucune raison de bouger -- il reste plante et tire.
bool CoaArenePlante(PlayerbotAI* botAI, Player* bot, CoaAreneEtat const& etat)
{
    if (GetCoaStyle(bot) == CoaStyle::Melee)
        return false;
    Unit* adv = CoaAreneAdversaireVivant(bot, etat);
    if (!adv || botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get() != adv)
        return false;
    float bas = 0.0f, haut = 0.0f;
    // Revue D4 : seulement une plage lue sur de vrais sorts a distance (>= 25 yd) ; le repli 5-30 d'un kit sans
    // tir n'immobilise personne.
    if (!CoaArenePlage(botAI, bot, bas, haut))
        return false;
    float const d = CoaAreneDistanceCombat(bot, adv);
    return d >= bas + 1.0f && d <= haut - CoaArenePlanteMarge && bot->IsWithinLOSInMap(adv);
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
        std::string const nom = action->getName();

        // Banc, cacheur (avant le test du bras : le mode ne depend que du match de banc marque). Rien ne doit le
        // ramener en vue : ni rapprochement, ni ArenaTactics, ni retour, recul, cachette L2 ou ouverture. Pendant le
        // deplacement vers la cachette, ni « set facing » ni attaque (CastFirst l'arreterait pour une incantation).
        if (etat.actif && !etat.preparation && etat.cadre == COA_ARENE_ARENE &&
            CoaAreneCacheurActif(bot, CoaAreneEtatDe(botAI)))
        {
            if (nom == "reach spell" || nom == "reach melee" || nom == "arena tactics" || nom == "move random" ||
                nom == "flee" || nom == "coa arene retour" || nom == "coa arene pilier" || nom == "coa arene recul" ||
                nom == "coa arene ouverture")
                return 0.0f;
            if (etat.cacheurEnRouteMs && (nom == "set facing" || nom == "coa attack" || nom == "coa aoe"))
                return 0.0f;
        }

        if (!CoaAreneEnCombat(etat))
            return 1.0f;

        if (sPlayerbotAIConfig.coaAreneIaCommune)
        {
            if (nom == "food" || nom == "drink" || nom == "suggest what to do" || nom == "mount")
                return 0.0f;
            // « check mount state » monte ET descend : on ne lui laisse que la descente.
            if (nom == "check mount state" && !bot->IsMounted())
                return 0.0f;
            // Lot 3 (« il se balade ») : en arene ou en duel, un bot a distance en vue de son adversaire et dans sa
            // plage de tir reste plante. Aucun de ces deplacements n'a alors d'objet : rapprocher (« reach spell »,
            // « reach melee »), ArenaTactics (repli au centre, poursuite hors de vue), « move random », « flee » amont.
            // Le test (portee, ligne de vue) n'est paye que pour ces noms. Le recul, le retour et la cachette de L1/L2
            // ont leurs propres declencheurs et ne sont pas touches.
            if ((etat.cadre == COA_ARENE_ARENE || etat.cadre == COA_ARENE_DUEL) && etat.vuCeTick &&
                (nom == "reach spell" || nom == "reach melee" || nom == "arena tactics" || nom == "move random" ||
                 nom == "flee") &&
                CoaArenePlante(botAI, bot, etat))
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
            // L2 : pendant une cachette, rien ne doit ramener le bot vers la cible -- ni « reach spell »/« reach
            // melee », ni ArenaTactics (qui fonce sur la victime hors de vue, BattleGroundTactics.cpp:4280-4294),
            // ni la fuite amont, ni « set facing » (qui l'arreterait en chemin). Sans cachette (IaPiliers = 0, ou
            // aucune en cours), rien n'est touche.
            if (CoaAreneCachetteEnCours(etat) &&
                (nom == "reach spell" || nom == "reach melee" || nom == "arena tactics" || nom == "flee" ||
                 nom == "set facing"))
                return 0.0f;
        }
        return 1.0f;
    }

    // Une fois par tour du moteur : la ligne « tick » de la trace des gladiateurs (une par seconde au plus).
    void ObserveQueue(std::list<ActionBasket*> const& /*candidats*/) override
    {
        CoaAreneTracerTick(botAI, bot);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Declencheurs.
// ---------------------------------------------------------------------------------------------------------------

// Lot 3 (« pas de defensifs ») : en arene ou en duel, sous 50 % de vie ou apres un burst subi (20 points perdus en
// 2 a 4 s, vie sous 80 %). « coa defensive » n'etait arme que par « low health » (45 %, AiPlayerbot.LowHealth) a
// ACTION_HIGH + 8 = 28, SOUS le recul (38), la rupture (36), le retour (37,5) et la cachette (39) : tant que l'un
// d'eux avait quelque chose a faire, le defensif ne passait pas. Ici a 39,5, sous les interruptions (40), action
// « coa arene defensif » (sorts KIND_DEFENSIVE seuls, revue D2). Rien de pret : inutile (isUseful), le tour continue.
class CoaAreneDefensifTrigger : public Trigger
{
public:
    CoaAreneDefensifTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene defensif") {}

    bool IsActive() override
    {
        if (!sPlayerbotAIConfig.coaAreneIaCommune || !bot->IsAlive())
            return false;
        CoaAreneEtat const& etat = CoaAreneMaj(botAI, bot);
        if (!CoaAreneL1Actif(etat) || (etat.cadre != COA_ARENE_ARENE && etat.cadre != COA_ARENE_DUEL))
            return false;
        float const pv = bot->GetHealthPct();
        if (pv < CoaAreneDefensifPv)
            return true;
        return pv < 80.0f && std::max(etat.pvPrec, etat.pvRef) - pv >= CoaAreneDefensifBurstPv;
    }
};

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
        if (!CoaAreneL1Actif(etat) || CoaAreneReculEnCours(etat) || CoaAreneCachetteEnCours(etat))
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
        if (!CoaAreneL1Actif(etat) || CoaAreneReculEnCours(etat) || CoaAreneCachetteEnCours(etat))
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

// L1, arene ou duel : l'adversaire est hors de vue, hors de portee, ou n'est pas la cible courante.
class CoaAreneRetourTrigger : public Trigger
{
public:
    CoaAreneRetourTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene retour") {}

    bool IsActive() override
    {
        if (!CoaAreneRetourArme(bot))
            return false;
        CoaAreneEtat const& etat = CoaAreneMaj(botAI, bot);
        if (!CoaAreneL1Actif(etat) || (etat.cadre != COA_ARENE_ARENE && etat.cadre != COA_ARENE_DUEL) ||
            CoaAreneReculEnCours(etat) || CoaAreneCachetteEnCours(etat))
            return false;
        if (!bot->IsAlive())
            return false;
        Unit* adv = CoaAreneAdversaireVivant(bot, etat);
        if (!adv)
            return false;
        // Un retour en cours se relance au plus une fois par seconde : la destination suit l'adversaire. SAUF la reprise
        // de cible (banc du 10/10 23:31-23:37 Paris : 9 % des secondes du chasseur sans cible ALORS que l'adversaire
        // etait en vue, moteur hors combat) : elle ne bouge pas le bot et part au tick ou l'adversaire reparait.
        // Cout : un test de ligne de vue de plus par tour, pour un bot d'arene 1v1 ou de duel sans cible seulement.
        bool const relanceRecente =
            etat.dernierRetourMs && getMSTimeDiff(etat.dernierRetourMs, getMSTime()) < CoaAreneRetourReessaiMs;
        if (relanceRecente)
        {
            // CIBLE n'est possible que sans cible valide (CoaAreneRetourRaison) : test court d'abord.
            Unit* cible = AI_VALUE(Unit*, "current target");
            if (cible && AttackersValue::IsValidTarget(cible, bot))
                return false;
        }
        // Un Ranger furtif (Elude) ouvre par « coa arene ouverture » -- mais seulement quand elle peut tirer (en vue, a
        // 40 yd au plus) et n'attend pas : ailleurs, c'est le retour qui le mene (CoaAreneOuvertureDansSaFenetre, banc
        // du 10/10 22:56). Les autres classes n'ont pas cette ouverture : furtives, elles reviennent toujours.
        if (bot->HasStealthAura() && CoaAreneEstRangerDistance(bot) && CoaAreneOuvertureDansSaFenetre(bot, adv) &&
            !CoaAreneOuvertureEnAttente(etat))
            return false;
        uint8 const raison = CoaAreneRetourRaison(botAI, bot, adv);
        if (relanceRecente)
            return raison == COA_RETOUR_CIBLE;
        if (raison == COA_RETOUR_AUCUN)
            return false;
        return raison == COA_RETOUR_CIBLE || CoaAreneLibreDeBouger(botAI, bot);
    }
};

// L2 (CoaArene.IaPiliers, arene seulement) : se mettre hors de vue de l'adversaire.
class CoaArenePilierTrigger : public Trigger
{
public:
    CoaArenePilierTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene pilier") {}

    bool IsActive() override
    {
        if (!sPlayerbotAIConfig.coaAreneIaPiliers || !CoaAreneEstRangerDistance(bot))
            return false;
        CoaAreneEtat const& etat = CoaAreneMaj(botAI, bot);
        if (!CoaAreneL1Actif(etat) || etat.cadre != COA_ARENE_ARENE || CoaAreneReculEnCours(etat) ||
            CoaAreneCachetteEnCours(etat) || etat.pilierCumulMs >= CoaArenePilierCumulMaxMs)
            return false;
        uint32 const maintenant = getMSTime();
        if (etat.derniereSortiePilierMs && getMSTimeDiff(etat.derniereSortiePilierMs, maintenant) < CoaArenePilierPauseMs)
            return false;
        if (etat.dernierEssaiPilierMs && getMSTimeDiff(etat.dernierEssaiPilierMs, maintenant) < CoaArenePilierEssaiMs)
            return false;
        if (bot->HasStealthAura() || !CoaAreneLibreDeBouger(botAI, bot))
            return false;
        Unit* adv = CoaAreneAdversaireVivant(bot, etat);
        // Deja hors de vue : rien a casser (et le retour s'en occupe).
        if (!adv || !adv->IsWithinLOSInMap(bot))
            return false;
        return CoaArenePilierRaison(botAI, bot, adv) != 0;
    }
};

// Banc, cacheur : l'adversaire le voit (arene 1v1 en cours, aucun deplacement vers une cachette en cours, recharge
// courte ecoulee). L'action tire un instantane s'il le peut, sinon se remet hors de vue.
class CoaAreneCacheurTrigger : public Trigger
{
public:
    CoaAreneCacheurTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene cacheur") {}

    bool IsActive() override
    {
        if (!bot->IsAlive())
            return false;
        CoaAreneMaj(botAI, bot);
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        if (!etat.actif || etat.preparation || !etat.vuCeTick || etat.cadre != COA_ARENE_ARENE ||
            etat.cacheurEnRouteMs || !CoaAreneCacheurActif(bot, etat))
            return false;
        if (etat.cacheurEssaiMs && getMSTimeDiff(etat.cacheurEssaiMs, getMSTime()) < CoaAreneCacheurEssaiMs)
            return false;
        Unit* adv = CoaAreneAdversaireVivant(bot, etat);
        return adv && adv->IsWithinLOSInMap(bot);
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
        // Arene ou duel : hors de sa fenetre de tir (hors de vue, ou a plus de 40 yd), l'ouverture laisse le bot au
        // retour, qui suit un chemin -- sa propre approche en ligne droite restait bloquee derriere un pilier ou contre
        // un relief (banc du 10/10 22:56, voir CoaAreneOuvertureDansSaFenetre). Monde ouvert : inchange (pas de retour).
        // Seulement si le retour a une raison d'agir : un adversaire furtif (CanSeeOrDetect faux) ne se poursuit pas
        // par le retour, et l'ouverture garde alors son approche d'avant.
        if (adv && adv->IsAlive() && (etat.cadre == COA_ARENE_ARENE || etat.cadre == COA_ARENE_DUEL) &&
            CoaAreneRetourArme(bot) && !CoaAreneOuvertureDansSaFenetre(bot, adv) &&
            CoaAreneRetourRaison(botAI, bot, adv) != COA_RETOUR_AUCUN)
            return false;
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
        // Le recul part au tick suivant, tant que la cible est ralentie : la fenetre (Dazed 4 s) se consume vite.
        etat.dernierEssaiReculMs = 0;
        float const d = bot->GetExactDist2d(cible);
        CoaAreneJournal(bot, etat, "rupture", d, d, Acore::StringFormat(" sort={}", cast->Id));
        return true;
    }
};

/*
 * Le recul. Un point dans la direction qui eloigne de la cible, assez loin pour compenser sa poursuite ralentie
 * mais jamais au-dela de la portee maximale moins 2 yd, puis a +/-30, 60 et 90 degres si la premiere est
 * fermee. Un point n'est retenu que :
 *   - s'il a un sol et un chemin (Map::CheckCollisionAndGetValidCoords, sans echec sur collision : il ramene le
 *     point contre le mur, d'ou le controle de distance qui suit -- c'est ce qui ecarte le mur et le vide) ;
 *   - sans denivele de plus de 8 yd (ni falaise ni corniche) ;
 *   - a au moins (vise - 4) yd de la cible et sans depasser la portee maximale moins la marge ;
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
        float const viseeMax = CoaAreneDistanceVisee(bas, haut);
        float const rayons = bot->GetCombatReach() + cible->GetCombatReach();
        float const avant = bot->GetExactDist2d(cible);
        float const d0 = CoaAreneDistanceCombat(bot, cible);
        float const angle0 = cible->GetAngle(bot);

        // Revue du 10/10 soir (3 reculs sur 3 rates a 19:15, 8,8 -> 17,5 yd pour 23 vises). DEFAUT : le point etait
        // pris a 23 yd de la position de la cible AU DEPART ; une cible ralentie (Dazed, -50 %) SUIT, et le bot
        // s'arretait a 23 yd d'un point qu'elle avait quitte. PHYSIQUE : l'ecart ne se creuse que de
        // (vBot - vCible) x (duree restante de la fenetre) -- 14 yd au mieux pour un Dazed de 4 s.
        // D'ou : la visee est ramenee a ce que la fenetre permet (`vise`), le point est pousse plus loin pour
        // compenser la poursuite, mais jamais au-dela de la portee maximale moins la marge (si la cible cesse de
        // suivre, le bot doit rester a portee : incident de 19:21, recul fini a 32,2 yd, hors de portee) ; le bot
        // s'arrete des que l'ecart vise est atteint (CoaAreneMaj).
        float const vBot = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
        float const vCible = std::min(CoaAreneVitesseSuivie(cible), vBot - 0.1f);
        float const vise = std::min(viseeMax, CoaAreneReculAtteignable(bot, cible, d0));
        if (vise < bas + CoaAreneReculGainMin || vise < d0 + CoaAreneReculGainMin)
        {
            // Rien a gagner : la fenetre est trop courte, ou la cible presque aussi rapide. On reste et on frappe.
            if (!etat.dernierImpossibleMs || getMSTimeDiff(etat.dernierImpossibleMs, maintenant) >= CoaAreneTenirJournalMs)
            {
                etat.dernierImpossibleMs = maintenant ? maintenant : 1;
                CoaAreneJournal(bot, etat, "recul_inutile", avant, avant,
                                Acore::StringFormat(" vise={:.1f} vise_max={:.1f} v_cible={:.1f} fenetre_ms={}", vise,
                                                    viseeMax, CoaAreneVitesseSuivie(cible), CoaAreneDureeFenetreMs(cible)));
            }
            return false;
        }
        // Temps pour creuser l'ecart, puis la distance que le bot parcourt pendant ce temps, comptee depuis la cible.
        float const tS = (vise - d0) / (vBot - vCible);
        float const loin = std::min(d0 + vBot * tS, haut - CoaArenePlanteMarge);  // revue : aligne sur le seuil du retour (pas de va-et-vient d'1 yd)
        float const rayon = loin + rayons;

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
            if (atteinte < std::max(vise - 4.0f, d0 + CoaAreneReculGainMin) || atteinte > haut - CoaArenePlanteMarge + 0.5f)
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

            // Echeance : le trajet en ligne droite, plus 1 s pour un chemin qui contourne ; bornee a 6 s. Le recul
            // se clot d'ordinaire avant (ecart atteint, fenetre close, arret).
            uint32 const trajetMs = std::min<uint32>(uint32(bot->GetExactDist2d(x, y) / vBot * IN_MILLISECONDS) + 1000,
                                                     6 * IN_MILLISECONDS);
            etat.reculDebutMs = maintenant;
            etat.reculFinMs = (maintenant + trajetMs) ? maintenant + trajetMs : 1;
            etat.reculDistAvant = avant;
            etat.reculVise = vise;
            etat.reculViseMax = viseeMax;
            etat.retourEnCours = false;
            ++etat.reculs;
            return true;
        }

        if (!doublon &&
            (!etat.dernierImpossibleMs || getMSTimeDiff(etat.dernierImpossibleMs, maintenant) >= CoaAreneTenirJournalMs))
        {
            etat.dernierImpossibleMs = maintenant ? maintenant : 1;
            CoaAreneJournal(bot, etat, "recul_impossible", avant, avant, Acore::StringFormat(" vise={:.1f}", vise));
        }
        return false;
    }

    bool isUseful() override { return IsMovingAllowed(); }
};

// Le retour (voir le commentaire de CoaAreneRetourRaison) : reprendre la cible si elle est en vue et a portee ;
// sinon courir vers elle -- a portee confortable si on la voit, jusqu'a elle (par un chemin) sinon.
class CoaAreneRetourAction : public MovementAction
{
public:
    CoaAreneRetourAction(PlayerbotAI* botAI) : MovementAction(botAI, "coa arene retour") {}

    bool Execute(Event /*event*/) override
    {
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        uint32 const maintenant = getMSTime();
        etat.dernierRetourMs = maintenant ? maintenant : 1;
        Unit* adv = CoaAreneAdversaireVivant(bot, etat);
        if (!adv || !bot->GetMap())
            return false;
        uint8 const raison = CoaAreneRetourRaison(botAI, bot, adv);
        if (raison == COA_RETOUR_AUCUN)
            return false;

        float const avant = bot->GetExactDist2d(adv);
        auto Noter = [&](char const* suite)
        {
            if (etat.dernierJournalRetourMs && getMSTimeDiff(etat.dernierJournalRetourMs, maintenant) < CoaAreneRetourJournalMs)
                return;
            etat.dernierJournalRetourMs = maintenant ? maintenant : 1;
            CoaAreneJournal(bot, etat, "retour", avant, avant,
                            Acore::StringFormat(" raison={} vue={} cible_courante={}{}", CoaAreneRetourNom(raison),
                                                bot->IsWithinLOSInMap(adv) ? 1 : 0,
                                                AI_VALUE(Unit*, "current target") ? 1 : 0, suite));
        };

        if (raison == COA_RETOUR_CIBLE)
        {
            // En vue et a portee : la reprendre pour cible et repasser en combat (comme l'ouverture).
            context->GetValue<Unit*>("current target")->Set(adv);
            bot->SetSelection(adv->GetGUID());
            if (botAI->GetState() != BOT_STATE_COMBAT)
                botAI->ChangeEngine(BOT_STATE_COMBAT);
            ++etat.retours;
            Noter("");
            return true;
        }

        if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
            return false;

        float x = adv->GetPositionX();
        float y = adv->GetPositionY();
        float z = adv->GetPositionZ();
        if (raison == COA_RETOUR_PORTEE)
        {
            // En vue mais trop loin : un point a (portee max - 4) yd de la cible, sur la droite cible -> bot, en
            // vue de la cible ; sinon droit sur elle (le suivi de CoaAreneMaj arrete le bot a portee).
            float bas = 0.0f, haut = 0.0f;
            CoaArenePlage(botAI, bot, bas, haut);
            float const rayon = std::max(bas + 3.0f, haut - CoaAreneMargePortee - 2.0f) + bot->GetCombatReach() +
                                adv->GetCombatReach();
            float const angle = adv->GetAngle(bot);
            float px = adv->GetPositionX() + std::cos(angle) * rayon;
            float py = adv->GetPositionY() + std::sin(angle) * rayon;
            float pz = bot->GetPositionZ();
            if (bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                               bot->GetPositionZ(), px, py, pz, false) &&
                adv->IsWithinLOS(px, py, pz + 2.0f) && adv->GetExactDist2d(px, py) < avant)
            {
                x = px;
                y = py;
                z = pz;
            }
        }
        // Banc du 10/10 23:35:04-23:35:40 (Paris), chasseur 2600 : arrive a 3-8 yd du cacheur, TOUJOURS sans ligne de vue
        // (un angle de mur ou de tombe entre les deux), immobile -- le point vise etait la position de l'adversaire,
        // deja atteinte (IsDuplicateMove), et rien d'autre n'etait essaye. Pres de lui (10 yd) ou sur un point deja
        // demande : un point a 3 yd de lui, sur 8 directions, atteignable depuis LUI en ligne droite (pas derriere le mur
        // qui le cache) et en vue de lui. Cout borne : 8 lancers de rayon et 8 tests de vue, une fois par seconde au plus
        // (relance du retour), seulement dans ce cas.
        if (raison == COA_RETOUR_VUE && (avant <= 10.0f || IsDuplicateMove(x, y, z)))
        {
            float const rayon = 3.0f + bot->GetCombatReach() + adv->GetCombatReach();
            float const angle0 = adv->GetAngle(bot);
            for (uint8 i = 0; i < 8; ++i)
            {
                float const angle = angle0 + float(i) * float(M_PI) / 4.0f;
                float px = adv->GetPositionX() + std::cos(angle) * rayon;
                float py = adv->GetPositionY() + std::sin(angle) * rayon;
                float pz = adv->GetPositionZ();
                if (!bot->GetMap()->CheckCollisionAndGetValidCoords(adv, adv->GetPositionX(), adv->GetPositionY(),
                                                                    adv->GetPositionZ(), px, py, pz, false))
                    continue;
                if (adv->GetExactDist2d(px, py) < 1.5f || !adv->IsWithinLOS(px, py, pz + 2.0f) || IsDuplicateMove(px, py, pz))
                    continue;
                x = px;
                y = py;
                z = pz;
                break;
            }
        }
        if (IsDuplicateMove(x, y, z))
            return false;
        if (!MoveTo(bot->GetMapId(), x, y, z, false, false, true, false, MovementPriority::MOVEMENT_COMBAT, true, false))
        {
            Noter(" bouge=0");
            return false;
        }
        if (!etat.retourEnCours)
            ++etat.retours;
        etat.retourEnCours = true;
        Noter(" bouge=1");
        return true;
    }

    bool isUseful() override { return IsMovingAllowed(); }
};

/*
 * L2, la cachette (CoaArene.IaPiliers). Autour du BOT, au plus 3 rayons (8, 14, 20 yd) x 8 directions (en
 * commencant par celle qui l'eloigne de l'adversaire) : 24 points, dont on retient le premier qui
 *   - a un sol et un chemin direct (CheckCollisionAndGetValidCoords) sans etre ramene contre un mur, sans
 *     denivele de plus de 5 yd ;
 *   - ne rapproche pas le bot de l'adversaire sous 8 yd et reste a moins de 45 yd de lui ;
 *   - est HORS de la ligne de vue de l'adversaire (Map::isInLineOfSight par WorldObject::IsWithinLOS, toutes
 *     verifications : vmaps statiques ET modeles des objets de jeu, CheckGameObjectLoS = 1) ;
 *   - a un chemin de navmesh complet de moins de 2 x rayon + 5 yd (PathGenerator, 3 calculs au plus).
 * Cout borne : 24 lancers de rayon et 3 chemins au plus par recherche, une recherche toutes les 1,5 s au plus,
 * seulement pendant que la raison tient. Sans obstacle dans le rayon (aucun point cache), il ne fait rien de
 * special ; une ligne « pilier_aucun » toutes les 10 s au plus.
 */
// La recherche de point hors de vue, commune a la cachette L2 et au cacheur du banc.
struct CoaAreneCachette
{
    bool trouvee = false;
    float dPoint = 0.0f;    // distance du point a l'adversaire
    float chemin = 0.0f;    // longueur du chemin retenu
    uint32 chemins = 0;     // chemins calcules (3 au plus)
    uint32 caches = 0;      // points hors de vue trouves
    uint32 sondes = 0;      // points sondes (anneaux, puis points candidats de la carte)
    uint32 candidats = 0;   // points candidats de la carte (precalcul), 0 si aucun ou non demande
    bool parCandidat = false;  // le point retenu vient des candidats de la carte, pas des anneaux
};

/*
 * Cacheur du banc, POINTS CANDIDATS d'une carte d'arene (banc du 10/10 22:55, 1422 sur la carte 572 : 224
 * « cacheur_aucun », caches=0 chemins=0 a chaque fois, adversaire a 79-85 yd : tout point etait rejete par la borne
 * des 45 yd de L2, et le rayon direct bot -> point s'arretait devant chaque obstacle, jamais derriere).
 *
 * Precalcul PARESSEUX, une fois par carte et par processus (la geometrie statique est la meme pour toutes les
 * instances d'une carte) : une grille de 4 yd sur un disque autour du milieu des deux points de depart de l'arene
 * (Battleground::GetTeamStartPosition), rayon = demi-ecart + 10 yd, borne 45-60 (CoaAreneDisque). Un point est candidat s'il a un sol
 * (Map::GetHeight, vmaps comprises, sans les objets dynamiques) et si un obstacle STATIQUE le borde : au moins une des
 * 8 visees de 4 yd a hauteur d'yeux (Map::isInLineOfSight, LINEOFSIGHT_CHECK_VMAP) est coupee, mais pas les 8 (point
 * pris DANS un pilier ou un mur). Ce sont les pieds des piliers, de la tombe, des murs. Rien n'est ecrit a la main.
 * Cout : 709 points au plus (rayon 60, 394 pour 45) x 9 requetes vmaps, une fois (duree journalisee : action=cacheur_candidats). Liste vide : nouvel
 * essai 60 s plus tard au plus tot (grilles pas encore chargees), jamais mise en cache.
 *
 * FIL : appele dans la mise a jour d'IA du bot (fil de la carte d'arene). Ne lit que la carte du bot ; la table est
 * partagee entre cartes, donc sous verrou court (aucune requete vmaps sous verrou). Les valeurs d'un unordered_map ne
 * bougent pas a l'insertion : le pointeur rendu reste valide, et la liste n'est plus jamais modifiee.
 */
struct CoaArenePoint
{
    float x, y, z;
};

constexpr float CoaAreneCandidatPas = 4.0f;
constexpr uint32 CoaAreneCandidatsMax = 400;
constexpr uint32 CoaAreneCandidatsReessaiMs = 60 * IN_MILLISECONDS;

// Le disque de l'arene du bot : milieu des deux points de depart, rayon = demi-ecart + 10 yd, borne 45-60 (618 : departs a 21 yd l un de l autre). Faux hors
// arene ou sans points de depart. Sert au precalcul ET a borner les anneaux du cacheur : un point derriere le mur
// d'enceinte est toujours « hors de vue », et brulerait les 3 chemins de la recherche sans jamais etre atteignable.
bool CoaAreneDisque(Player* bot, float& cx, float& cy, float& zRef, float& rayon)
{
    Battleground* bg = bot->InArena() ? bot->GetBattleground() : nullptr;
    Position const* depA = bg ? bg->GetTeamStartPosition(TEAM_ALLIANCE) : nullptr;
    Position const* depH = bg ? bg->GetTeamStartPosition(TEAM_HORDE) : nullptr;
    if (!depA || !depH)
        return false;
    cx = (depA->GetPositionX() + depH->GetPositionX()) / 2.0f;
    cy = (depA->GetPositionY() + depH->GetPositionY()) / 2.0f;
    zRef = std::max(depA->GetPositionZ(), depH->GetPositionZ());
    rayon = std::clamp(depA->GetExactDist2d(depH) / 2.0f + 10.0f, 45.0f, 60.0f);
    return true;
}

std::vector<CoaArenePoint> const* CoaAreneCandidatsDe(Player* bot)
{
    static std::mutex verrou;
    static std::unordered_map<uint32, std::vector<CoaArenePoint>> cache;
    static std::unordered_map<uint32, uint32> dernierEssai;

    Map* carte = bot->GetMap();
    float cx = 0.0f, cy = 0.0f, zRef = 0.0f, rayon = 0.0f;
    if (!carte || !CoaAreneDisque(bot, cx, cy, zRef, rayon))
        return nullptr;
    uint32 const idCarte = bot->GetMapId();
    uint32 const maintenant = getMSTime();
    {
        std::lock_guard<std::mutex> garde(verrou);
        auto const trouve = cache.find(idCarte);
        if (trouve != cache.end())
            return &trouve->second;
        auto const essai = dernierEssai.find(idCarte);
        if (essai != dernierEssai.end() && getMSTimeDiff(essai->second, maintenant) < CoaAreneCandidatsReessaiMs)
            return nullptr;
        dernierEssai[idCarte] = maintenant;
    }

    std::vector<CoaArenePoint> points;
    uint32 sondes = 0;
    for (float dx = -rayon; dx <= rayon && points.size() < CoaAreneCandidatsMax; dx += CoaAreneCandidatPas)
        for (float dy = -rayon; dy <= rayon && points.size() < CoaAreneCandidatsMax; dy += CoaAreneCandidatPas)
        {
            if (dx * dx + dy * dy > rayon * rayon)
                continue;
            ++sondes;
            float const x = cx + dx, y = cy + dy;
            float const z = carte->GetHeight(x, y, zRef + 5.0f, true, 40.0f);
            if (z <= INVALID_HEIGHT)
                continue;
            uint8 coupees = 0;
            for (uint8 i = 0; i < 8; ++i)
            {
                float const a = float(i) * float(M_PI) / 4.0f;
                if (!carte->isInLineOfSight(x, y, z + 2.0f, x + std::cos(a) * 4.0f, y + std::sin(a) * 4.0f, z + 2.0f,
                                            PHASEMASK_NORMAL, LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing))
                    ++coupees;
            }
            if (coupees > 0 && coupees < 8)
                points.push_back({ x, y, z });
        }

    LOG_INFO("playerbots.coa", "coa arene ia: guid={} action=cacheur_candidats carte={} candidats={} sondes={} rayon={:.0f}"
             " duree_ms={}", bot->GetGUID().GetCounter(), idCarte, points.size(), sondes, rayon,
             GetMSTimeDiffToNow(maintenant));
    if (points.empty())
        return nullptr;
    std::lock_guard<std::mutex> garde(verrou);
    // Deux arenes de la meme carte ont pu calculer en meme temps : la premiere inseree reste (meme geometrie).
    return &cache.emplace(idCarte, std::move(points)).first->second;
}

class CoaAreneCachetteAction : public MovementAction
{
public:
    CoaAreneCachetteAction(PlayerbotAI* botAI, std::string const nom) : MovementAction(botAI, nom) {}

protected:
    // Un point hors de vue : chemin de navmesh complet, qui arrive bien au point (pas au polygone le plus proche
    // d'un point pris dans un obstacle), de longueur <= `cheminMax`, puis MoveTo (priorite de combat).
    // `exigerArrivee` : seulement pour les points pris au sol (cacheur), pas pour ceux de L2, deja au bout d'un rayon
    // libre depuis le bot (L2 inchangee).
    bool EssayerPoint(CoaAreneCachette& c, Unit* adv, float x, float y, float z, float cheminMax, bool exigerArrivee)
    {
        ++c.caches;
        if (IsDuplicateMove(x, y, z))
            return false;
        ++c.chemins;
        PathGenerator chemin(bot);
        chemin.CalculatePath(x, y, z, false);
        if ((chemin.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT)) ||
            chemin.getPathLength() > cheminMax)
            return false;
        if (exigerArrivee)
        {
            G3D::Vector3 const& fin = chemin.GetActualEndPosition();
            if ((fin.x - x) * (fin.x - x) + (fin.y - y) * (fin.y - y) > 2.0f * 2.0f)
                return false;
        }
        if (!MoveTo(bot->GetMapId(), x, y, z, false, false, true, false, MovementPriority::MOVEMENT_COMBAT, true, false))
            return false;
        c.trouvee = true;
        c.dPoint = adv->GetExactDist2d(x, y);
        c.chemin = chemin.getPathLength();
        return true;
    }

    // Cherche un point hors de vue de `adv` et y envoie le bot. Voir le commentaire de CoaArenePilierAction pour
    // les criteres de L2 (`loin` faux : bornes d'origine, inchangees).
    // `loin` (cacheur du banc) : anneaux de 8 a 45 yd, aucune borne de distance a l'adversaire (le but est de casser
    // la vue, pas de rester a portee), point pris AU SOL (Map::GetHeight) et non au bout du rayon direct bot -> point,
    // qui s'arrete devant l'obstacle : seul le chemin de navmesh juge s'il est atteignable ; puis les points candidats
    // de la carte (CoaAreneCandidatsDe), du plus proche au plus loin, dans 50 yd du bot. Cout borne par recherche :
    // 48 points d'anneau + 24 candidats au plus en ligne de vue, 3 chemins au plus (une recherche toutes les 700 ms
    // au plus, banc seulement).
    CoaAreneCachette AllerHorsDeVue(Unit* adv, bool loin = false)
    {
        CoaAreneCachette c;
        float const bx = bot->GetPositionX(), by = bot->GetPositionY(), bz = bot->GetPositionZ();
        float const dAdv = bot->GetExactDist2d(adv);
        float const angle0 = adv->GetAngle(bot);
        static constexpr float RayonsL2[] = { 8.0f, 14.0f, 20.0f };
        static constexpr float RayonsLoin[] = { 8.0f, 14.0f, 20.0f, 28.0f, 36.0f, 45.0f };
        static constexpr float Ecarts[] = { 0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f };  // x 45 degres
        float cx = 0.0f, cy = 0.0f, zDisque = 0.0f, rDisque = 0.0f;
        bool const disque = loin && CoaAreneDisque(bot, cx, cy, zDisque, rDisque);
        float const* const rayons = loin ? RayonsLoin : RayonsL2;
        size_t const nbRayons = loin ? std::size(RayonsLoin) : std::size(RayonsL2);
        for (size_t ir = 0; ir < nbRayons; ++ir)
        {
            float const r = rayons[ir];
            for (float const ecart : Ecarts)
            {
                if (c.chemins >= 3)
                    return c;
                ++c.sondes;
                float const angle = angle0 + ecart * float(M_PI) / 4.0f;
                float x = bx + std::cos(angle) * r;
                float y = by + std::sin(angle) * r;
                float z = bz;
                if (loin)
                {
                    if (disque && (x - cx) * (x - cx) + (y - cy) * (y - cy) > rDisque * rDisque)
                        continue;  // hors de l'enceinte
                    z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, bz + 5.0f, true, 15.0f);
                    if (z <= INVALID_HEIGHT || std::fabs(z - bz) > 8.0f)
                        continue;
                }
                else
                {
                    if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bx, by, bz, x, y, z, false))
                        continue;
                    if (std::fabs(z - bz) > 5.0f || bot->GetExactDist2d(x, y) < r * 0.6f)
                        continue;
                }
                float const dPoint = adv->GetExactDist2d(x, y);
                if (dPoint < std::min(dAdv, 8.0f) || (!loin && dPoint > 45.0f))
                    continue;
                if (adv->IsWithinLOS(x, y, z + 2.0f))
                    continue;  // visible : pas une cachette
                if (EssayerPoint(c, adv, x, y, z, r * 2.0f + 5.0f, loin))
                    return c;
            }
        }
        if (!loin)
            return c;

        std::vector<CoaArenePoint> const* candidats = CoaAreneCandidatsDe(bot);
        if (!candidats)
            return c;
        c.candidats = uint32(candidats->size());
        // Les 24 plus proches du bot, dans 50 yd (tri partiel d'indices : la liste partagee n'est jamais touchee).
        std::vector<std::pair<float, uint32>> proches;
        proches.reserve(candidats->size());
        for (uint32 i = 0; i < candidats->size(); ++i)
        {
            CoaArenePoint const& p = (*candidats)[i];
            float const d = bot->GetExactDist2d(p.x, p.y);
            if (d >= 3.0f && d <= 50.0f && std::fabs(p.z - bz) <= 15.0f)
                proches.emplace_back(d, i);
        }
        size_t const n = std::min<size_t>(proches.size(), 24);
        std::partial_sort(proches.begin(), proches.begin() + n, proches.end());
        for (size_t k = 0; k < n; ++k)
        {
            if (c.chemins >= 3)
                return c;
            ++c.sondes;
            CoaArenePoint const& p = (*candidats)[proches[k].second];
            if (adv->GetExactDist2d(p.x, p.y) < std::min(dAdv, 8.0f) || adv->IsWithinLOS(p.x, p.y, p.z + 2.0f))
                continue;
            if (EssayerPoint(c, adv, p.x, p.y, p.z, proches[k].first * 2.0f + 10.0f, true))
            {
                c.parCandidat = true;
                return c;
            }
        }
        return c;
    }
};

class CoaArenePilierAction : public CoaAreneCachetteAction
{
public:
    CoaArenePilierAction(PlayerbotAI* botAI) : CoaAreneCachetteAction(botAI, "coa arene pilier") {}

    bool Execute(Event /*event*/) override
    {
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        uint32 const maintenant = getMSTime();
        etat.dernierEssaiPilierMs = maintenant ? maintenant : 1;
        Unit* adv = CoaAreneAdversaireVivant(bot, etat);
        if (!adv || !bot->GetMap())
            return false;
        if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
            return false;
        uint8 const raison = CoaArenePilierRaison(botAI, bot, adv);
        if (!raison)
            return false;

        float const dAdv = bot->GetExactDist2d(adv);
        CoaAreneCachette const c = AllerHorsDeVue(adv);
        uint32 const chemins = c.chemins, caches = c.caches;
        if (c.trouvee)
        {
            etat.pilierDebutMs = maintenant ? maintenant : 1;
            etat.pilierRaison = raison;
            etat.retourEnCours = false;
            ++etat.piliers;
            CoaAreneJournal(bot, etat, "pilier", dAdv, c.dPoint,
                            Acore::StringFormat(" raison={} pv={:.0f} chemin={:.1f} carte={} essais={}",
                                                raison == COA_PILIER_PV ? "pv" : "recharge", bot->GetHealthPct(),
                                                c.chemin, bot->GetMapId(), chemins));
            return true;
        }

        if (!etat.dernierJournalPilierMs || getMSTimeDiff(etat.dernierJournalPilierMs, maintenant) >= CoaArenePilierJournalMs)
        {
            etat.dernierJournalPilierMs = maintenant ? maintenant : 1;
            CoaAreneJournal(bot, etat, "pilier_aucun", dAdv, dAdv,
                            Acore::StringFormat(" raison={} caches={} chemins={} carte={}",
                                                raison == COA_PILIER_PV ? "pv" : "recharge", caches, chemins,
                                                bot->GetMapId()));
        }
        return false;
    }

    bool isUseful() override { return IsMovingAllowed(); }
};

/*
 * Banc d'arene 1v1, mode CACHEUR (`.coa arene banc ... cacheur=A|B`). Le bot designe casse la ligne de vue le plus
 * possible pendant tout le match : des que l'adversaire le voit, il tire UN sort instantane s'il en a un pret et en
 * portee (puis pas d'autre tir avant 1,5 s), sinon il cherche un point hors de vue (AllerHorsDeVue en mode `loin` :
 * anneaux de 8 a 45 yd puis points candidats de la carte ; sans seuil de vie, recharge 700 ms) et y court. Coince (aucun point hors de vue, ou chemin refuse) : rien de special, la
 * rotation ordinaire joue (et une ligne « cacheur_aucun » toutes les 10 s). Le multiplicateur lui retire tout ce qui
 * le ramenerait en vue. Journal : action=cacheur_debut, cacheur_tir, cacheur_cache, cacheur_arrive, cacheur_aucun.
 */
class CoaAreneCacheurAction : public CoaAreneCachetteAction
{
public:
    CoaAreneCacheurAction(PlayerbotAI* botAI) : CoaAreneCachetteAction(botAI, "coa arene cacheur") {}

    bool Execute(Event /*event*/) override
    {
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        uint32 const maintenant = getMSTime();
        etat.cacheurEssaiMs = maintenant ? maintenant : 1;
        Unit* adv = CoaAreneAdversaireVivant(bot, etat);
        if (!adv || !bot->GetMap())
            return false;
        float const dAdv = bot->GetExactDist2d(adv);

        // 1. A decouvert : un instantane, une fois.
        if ((!etat.cacheurTirMs || getMSTimeDiff(etat.cacheurTirMs, maintenant) >= CoaAreneCacheurTirPauseMs) &&
            bot->IsWithinLOSInMap(adv) && AttackersValue::IsValidTarget(adv, bot))
        {
            std::vector<Usable> tirs =
                KnownAbilities(botAI, bot, [](uint16 kind) { return IsAttack(kind, false) && (kind & KIND_DAMAGE); });
            tirs.erase(std::remove_if(tirs.begin(), tirs.end(), [&](Usable const& spell)
                {
                    SpellInfo const* info = EffectiveSpell(bot, spell.info);
                    return info->CalcCastTime(bot) != 0 || info->IsChanneled() || !CoaAreneSortPret(botAI, bot, spell) ||
                           CoaAreneCiblePaisibleExigee(info, adv);
                }), tirs.end());
            if (!tirs.empty())
                DropUncastableHere(bot, adv, tirs);
            if (!tirs.empty())
            {
                context->GetValue<Unit*>("current target")->Set(adv);
                bot->SetSelection(adv->GetGUID());
                if (SpellInfo const* cast = RecordUsage(USAGE_ATTACK, CastFirst(botAI, bot, tirs, adv, USAGE_ATTACK)))
                {
                    etat.cacheurTirMs = maintenant ? maintenant : 1;
                    ++etat.cacheurTirs;
                    CoaAreneJournal(bot, etat, "cacheur_tir", dAdv, dAdv,
                                    Acore::StringFormat(" sort={} pv={:.0f}", cast->Id, bot->GetHealthPct()));
                    return true;
                }
            }
        }

        // 2. Se remettre hors de vue.
        if (!CoaAreneLibreDeBouger(botAI, bot) || IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
            return false;
        CoaAreneCachette const c = AllerHorsDeVue(adv, true);
        if (c.trouvee)
        {
            etat.cacheurEnRouteMs = maintenant ? maintenant : 1;
            etat.retourEnCours = false;
            ++etat.cacheurCaches;
            CoaAreneJournal(bot, etat, "cacheur_cache", dAdv, c.dPoint,
                            Acore::StringFormat(" pv={:.0f} chemin={:.1f} carte={} essais={} sondes={} candidats={}"
                                                " par_candidat={}", bot->GetHealthPct(), c.chemin, bot->GetMapId(),
                                                c.chemins, c.sondes, c.candidats, c.parCandidat ? 1 : 0));
            return true;
        }
        ++etat.cacheurAucun;
        if (!etat.cacheurJournalMs || getMSTimeDiff(etat.cacheurJournalMs, maintenant) >= CoaAreneCacheurJournalMs)
        {
            etat.cacheurJournalMs = maintenant ? maintenant : 1;
            CoaAreneJournal(bot, etat, "cacheur_aucun", dAdv, dAdv,
                            Acore::StringFormat(" caches={} chemins={} carte={} total={} sondes={} candidats={}",
                                                c.caches, c.chemins, bot->GetMapId(), etat.cacheurAucun, c.sondes,
                                                c.candidats));
        }
        return false;
    }

    bool isUseful() override { return bot->IsAlive(); }
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
        if (avant <= CoaAreneOuvertureDistanceMax && bot->IsWithinLOSInMap(adv))
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

// Defensif d'arene (revue D2) : les seuls sorts KIND_DEFENSIVE. Les soins sur soi restent a « coa defensive » (28) :
// un soin a incantation passerait sinon devant le recul, le retour et la cachette, et CastFirst arreterait le bot.
class CoaAreneDefensifAction : public Action
{
public:
    CoaAreneDefensifAction(PlayerbotAI* botAI) : Action(botAI, "coa arene defensif") {}

    bool Execute(Event /*event*/) override
    {
        return RecordUsage(USAGE_DEFENSIVE, CastFirst(botAI, bot,
            KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_DEFENSIVE) != 0; }), bot, USAGE_DEFENSIVE));
    }

    bool isUseful() override
    {
        return bot->IsAlive() && ClassHas(bot, KIND_DEFENSIVE) &&
               HasReadyAbility(botAI, bot, [](uint16 kind) { return (kind & KIND_DEFENSIVE) != 0; });
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
// Retour 37,5 : devant « set facing » (37, inutile sans cible en vue), « reach spell » (20) et ArenaTactics (1) ;
// sous le recul (38). Pilier 39 : devant le recul et le retour, sous les interruptions (40).
constexpr float CoaAreneRetourPriority = ACTION_MOVE + 7.5f;
constexpr float CoaArenePilierPriority = ACTION_MOVE + 9.0f;
// Defensif 39,5 : devant la cachette, le recul, le retour et la rupture ; sous les interruptions (40).
constexpr float CoaAreneDefensifPriority = ACTION_MOVE + 9.5f;
// Banc, cacheur 39,2 : devant la cachette L2 (39), le recul (38) et le retour (37,5), sous le defensif (39,5) et les
// interruptions (40).
constexpr float CoaAreneCacheurPriority = ACTION_MOVE + 9.2f;

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
        triggers.push_back(new TriggerNode("coa arene retour", { NextAction("coa arene retour", CoaAreneRetourPriority) }));
        triggers.push_back(new TriggerNode("coa arene pilier", { NextAction("coa arene pilier", CoaArenePilierPriority) }));
        triggers.push_back(new TriggerNode("coa arene defensif", { NextAction("coa arene defensif", CoaAreneDefensifPriority) }));
        triggers.push_back(new TriggerNode("coa arene cacheur", { NextAction("coa arene cacheur", CoaAreneCacheurPriority) }));
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
        creators["coa arene retour"] = &CoaAreneActionFactoryInternal::retour;
        creators["coa arene pilier"] = &CoaAreneActionFactoryInternal::pilier;
        creators["coa arene defensif"] = &CoaAreneActionFactoryInternal::defensif;
        creators["coa arene cacheur"] = &CoaAreneActionFactoryInternal::cacheur;
    }

private:
    static Action* rupture(PlayerbotAI* botAI) { return new CoaAreneRuptureAction(botAI); }
    static Action* recul(PlayerbotAI* botAI) { return new CoaAreneReculAction(botAI); }
    static Action* ouverture(PlayerbotAI* botAI) { return new CoaAreneOuvertureAction(botAI); }
    static Action* preparation(PlayerbotAI* botAI) { return new CoaArenePreparationAction(botAI); }
    static Action* retour(PlayerbotAI* botAI) { return new CoaAreneRetourAction(botAI); }
    static Action* pilier(PlayerbotAI* botAI) { return new CoaArenePilierAction(botAI); }
    static Action* defensif(PlayerbotAI* botAI) { return new CoaAreneDefensifAction(botAI); }
    static Action* cacheur(PlayerbotAI* botAI) { return new CoaAreneCacheurAction(botAI); }
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
        creators["coa arene retour"] = &CoaAreneTriggerFactoryInternal::retour;
        creators["coa arene pilier"] = &CoaAreneTriggerFactoryInternal::pilier;
        creators["coa arene defensif"] = &CoaAreneTriggerFactoryInternal::defensif;
        creators["coa arene cacheur"] = &CoaAreneTriggerFactoryInternal::cacheur;
    }

private:
    static Trigger* rupture(PlayerbotAI* botAI) { return new CoaAreneRuptureTrigger(botAI); }
    static Trigger* recul(PlayerbotAI* botAI) { return new CoaAreneReculTrigger(botAI); }
    static Trigger* ouverture(PlayerbotAI* botAI) { return new CoaAreneOuvertureTrigger(botAI); }
    static Trigger* preparation(PlayerbotAI* botAI) { return new CoaArenePreparationTrigger(botAI); }
    static Trigger* retour(PlayerbotAI* botAI) { return new CoaAreneRetourTrigger(botAI); }
    static Trigger* pilier(PlayerbotAI* botAI) { return new CoaArenePilierTrigger(botAI); }
    static Trigger* defensif(PlayerbotAI* botAI) { return new CoaAreneDefensifTrigger(botAI); }
    static Trigger* cacheur(PlayerbotAI* botAI) { return new CoaAreneCacheurTrigger(botAI); }
};
