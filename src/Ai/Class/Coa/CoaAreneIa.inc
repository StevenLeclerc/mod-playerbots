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
// Lot 19, ouverture montee : en dessous, les attaques montees partent (Hellfire Trample : cone 24 de 25 yd devant, bond de
// 25 yd ; marge pour la course de l'adversaire) ; essais a portee sans rien lancer avant de descendre.
constexpr float CoaAreneOuvertureMonteeDistance = 20.0f;
constexpr uint8 CoaAreneOuvertureMonteeEssaisMax = 3;
// Monte plus longtemps que ceci portes ouvertes sans avoir pu frapper (adversaire cache, chemin sans fin) : descendre.
constexpr uint32 CoaAreneOuvertureMonteeMaxMs = 15 * IN_MILLISECONDS;
// Preparation : delai laisse au familier avant de monter (monte, plus rien ne part).
constexpr uint32 CoaArenePreparationMontureDelaiMs = 6 * IN_MILLISECONDS;
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
// Retour d'urgence (immobile, hors de vue, sous les degats) : une relance par 300 ms au plus, hors du delai ci-dessus.
constexpr uint32 CoaAreneRetourUrgenceMs = 300;
// Retour d'un tireur (11/10, match id=18) : la vue doit tenir ce temps, course continuee, avant l'arret.
constexpr uint32 CoaAreneRetourVueTenueMs = 700;
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
// Survie d'arene (11/10, AiPlayerbot.CoaArene.IaSurvie ; retour du commanditaire, Onyx contre les matchs id=89 a 93 :
// « ils n'utilisent aucun sort de protection, aucun sort de recuperation de points de vie, rien »). Paliers : pression
// (perte de CoaAreneSurviePentePv points sur le releve de 2 a 4 s du defensif, DoT prevus >= CoaAreneSurvieDotsPct de
// la vie sur 6 s, ou vie < CoaAreneSurviePressionPv), danger (< 50 %), critique (< 30 %).
constexpr float CoaAreneSurviePressionPv = 70.0f;
constexpr float CoaAreneSurvieDangerPv = 50.0f;
constexpr float CoaAreneSurvieCritiquePv = 30.0f;
constexpr float CoaAreneSurviePentePv = 15.0f;
// Lot 18 : age au-dela duquel le pire coup ne compte plus si la vie ne baisse plus.
constexpr uint32 CoaAreneSurviePireCoupFraisMs = 6000;
// Lot 18 : fenetre du controle de secours. 10 s : au-dela de la duree utile d'une peur ou d'un etourdissement de
// controle (Storm Alert 707905 : DurationIndex 8 ; rompue par les degats), en dessous d'une recharge globale de match.
constexpr uint32 CoaAreneSurvieControleFenetreMs = 10000;
constexpr float CoaAreneSurvieDotsPct = 25.0f;
// Palier pression : soin DIRECT seulement sous 80 % (un soin periodique, lui, part a toute vie).
constexpr float CoaAreneSurvieSoinPressionPv = 80.0f;
// Sort d'entrave qui rend de la mana (Time Out!) : outil de mana seulement sous ce pourcentage.
constexpr float CoaAreneSurvieManaOutilPct = 25.0f;
// Defensif « a faible cout » du palier pression : recharge (RecoveryTime / CategoryRecoveryTime) de 60 s au plus.
constexpr uint32 CoaAreneSurvieRechargeLegereMs = 60 * IN_MILLISECONDS;
// Fuite hors de vue du palier critique (arene) : duree max, pause apres une fuite, cumul par engagement, essais.
constexpr uint32 CoaAreneSurvieFuiteMaxMs = 6 * IN_MILLISECONDS;
constexpr uint32 CoaAreneSurvieFuitePauseMs = 8 * IN_MILLISECONDS;
constexpr uint32 CoaAreneSurvieFuiteCumulMaxMs = 20 * IN_MILLISECONDS;
constexpr uint32 CoaAreneSurvieFuiteEssaiMs = 1500;
// Dissipation sur soi rentable (IaSurvie) : vie sous 90 %, mana >= 30 %, attente apres un succes 3 s doublee a chaque
// reapplication vue dans les 4 s (plafond 15 s).
constexpr float CoaAreneDispelPvMax = 90.0f;
constexpr float CoaAreneDispelManaMin = 30.0f;
constexpr uint32 CoaAreneDispelAttenteBaseMs = 3 * IN_MILLISECONDS;
constexpr uint32 CoaAreneDispelAttenteMaxMs = 15 * IN_MILLISECONDS;
constexpr uint32 CoaAreneDispelReapplicationMs = 4 * IN_MILLISECONDS;
// Reserve de mana : sous ce seuil, la rotation d'attaque garde de quoi lancer un sort de survie.
constexpr float CoaAreneReserveManaPct = 25.0f;
// Etendards (« coa arene soutien ») : un essai toutes les 3 s au plus ; adversaire a 30 yd au plus.
constexpr uint32 CoaAreneSoutienReessaiMs = 3 * IN_MILLISECONDS;
constexpr float CoaAreneSoutienDistanceMax = 30.0f;

// Survie d'arene : l'option est armee (IaSurvie n'agit que sous IaCommune, qui compose la strategie).
bool CoaAreneSurvieArmee()
{
    return sPlayerbotAIConfig.coaAreneIaSurvie && sPlayerbotAIConfig.coaAreneIaCommune;
}

// Defini plus bas (dissipation sur soi). `tous` : toutes les auras de degats periodiques nuisibles, dissipables ou non
// (palier de survie) ; sinon seulement celles que `masque` dissipe.
float CoaAreneDotsPrevus(Player* bot, uint32 masque, uint32& nombre, bool tous = false);

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
    return etat.actif && (etat.reculFinMs || etat.retourEnCours || etat.pilierDebutMs || etat.cacheurEnRouteMs ||
                          etat.survieFuiteDebutMs);
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

// ---------------------------------------------------------------------------------------------------------------
// Combat contre un humain (11/10/2026) : interruption, dissipation sur soi, fenetre de burst. Retour du commanditaire
// (Onyx, Chronomancer 30, arene 1v1 id=3 du 10/10 20:00:42-20:01:04 UTC contre Zolun, Ranger 2617) : soigne de 42 a
// 100 % sans etre interrompu ; ses DoT tuent le bot sans reaction. Arene 1v1 (type CoaArene1v1::TYPE_ARENE) et duel
// seulement, sous CoaArene.IaCommune, bras tire. Fil de l'IA du bot ; l'adversaire est lu sur la carte du bot (P-137).
// ---------------------------------------------------------------------------------------------------------------

// Duel en cours, ou arene 1v1 de CoaArene1v1 : jamais une 2v2/3v3/5v5 de bots, jamais le monde ouvert.
bool CoaAreneUnContreUn(Player* bot)
{
    if (bot->duel && bot->duel->Opponent)
        return true;
    Battleground* bg = bot->InArena() ? bot->GetBattleground() : nullptr;
    return bg && bg->isArena() && bg->GetArenaType() == CoaArene1v1::TYPE_ARENE;
}

// L'incantation adverse qu'un sort pourrait couper : sort a incantation en preparation (GetCastTime > 0), ou
// canalisation en cours, que le coeur declare interruptible (Spell::IsInterruptable). Memes etats que
// Spell::EffectInterruptCast (SpellEffects.cpp:4096-4098). Ni tir automatique ni sort de melee.
Spell* CoaAreneIncantationAdverse(Unit* adv, bool& canalisee)
{
    canalisee = false;
    if (!adv)
        return nullptr;
    if (Spell* spell = adv->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        if (spell->getState() == SPELL_STATE_PREPARING && spell->GetCastTime() > 0 && spell->IsInterruptable())
            return spell;
    if (Spell* spell = adv->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        if (spell->getState() == SPELL_STATE_CASTING && spell->IsInterruptable())
        {
            canalisee = true;
            return spell;
        }
    return nullptr;
}

// Un soin : HEAL, HEAL_PCT, HEAL_MAX_HEALTH ou aura PERIODIC_HEAL, par ses effets ou par un sort qu'il declenche
// (un niveau, auras de proc ecartees comme dans Classify). Sur soi ou sur un allie : l'adversaire se soigne lui-meme.
bool CoaAreneEstSoin(SpellInfo const* info, uint8 profondeur = 0)
{
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (!effect.IsEffect())
            continue;
        if (effect.Effect == SPELL_EFFECT_HEAL || effect.Effect == SPELL_EFFECT_HEAL_PCT ||
            effect.Effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
            (IsAuraEffect(effect) && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL))
            return true;
        bool const proc = IsAuraEffect(effect) &&
            (effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL || effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE ||
             effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_DAMAGE || effect.ApplyAuraName == SPELL_AURA_ADD_TARGET_TRIGGER);
        if (effect.TriggerSpell && !proc && !profondeur)
            if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                if (CoaAreneEstSoin(declenche, 1))
                    return true;
    }
    return false;
}

// Incantation qui merite une interruption PURE (le sort d'interruption a une recharge : ne pas le gacher) : un soin,
// ou un sort NON positif (degats, controle) d'au moins 1,5 s d'incantation, ou une canalisation non positive. Un buff,
// une monture, une invocation ou une incantation courte ne la meritent pas.
constexpr int32 CoaAreneIncantationDigneMs = 1500;
bool CoaAreneIncantationDigne(Spell const* spell, bool canalisee, bool soin)
{
    if (soin)
        return true;
    SpellInfo const* info = spell->GetSpellInfo();
    if (info->IsPositive())
        return false;
    return canalisee || spell->GetCastTime() >= CoaAreneIncantationDigneMs;
}

// Le type de la cible interdit le sort : RECOPIE des tests de type de SpellInfo::CheckTarget (SpellInfo.cpp:1951-1983),
// que le coeur applique a la cible explicite (Spell::CheckCast, Spell.cpp:5965) ET a chaque cible de zone (selection
// implicite, Spell.cpp:1965 et 2363) : un sort qu'il refuse ici ne touche donc jamais cette cible, quelle que soit sa
// facon de viser. Banc du 11/10 00:30-00:47 UTC (tour 7), TARGET_IS_PLAYER x4, tous en interruption : Overload
// Mechanical 707365 (TargetCreatureType 256, mecaniques) x3 et Hir'eek's Jinx 803325 (1, betes), proposes contre un joueur.
bool CoaAreneTypeCibleRefuse(SpellInfo const* info, Unit* cible)
{
    if (cible->IsPlayer())
    {
        if (info->HasAttribute(SPELL_ATTR5_NOT_ON_PLAYER))
            return true;
    }
    else
    {
        if (info->HasAttribute(SPELL_ATTR3_ONLY_ON_PLAYER))
            return true;
        if (info->HasAttribute(SPELL_ATTR5_NOT_ON_PLAYER_CONTROLLED_NPC) && cible->IsControlledByPlayer())
            return true;
    }
    return !info->CheckTargetCreatureType(cible);
}

constexpr uint8 COA_COUPE_NON = 0;
constexpr uint8 COA_COUPE_PURE = 1;     // SPELL_EFFECT_INTERRUPT_CAST, ou silence
constexpr uint8 COA_COUPE_CONTROLE = 2; // etourdissement, peur, desorientation

/*
 * Comment `info` (sort du bot) couperait `vise` (l'incantation adverse), lu dans les effets et dans le coeur :
 *   - SPELL_EFFECT_INTERRUPT_CAST : Spell::EffectInterruptCast (SpellEffects.cpp:4096-4101) exige
 *     PreventionType == SILENCE et SPELL_INTERRUPT_FLAG_INTERRUPT (incantation) ou CHANNEL_INTERRUPT_FLAG_INTERRUPT
 *     (canalisation) sur le sort vise ;
 *   - aura SILENCE ou PACIFY_SILENCE : AuraEffect::HandleAuraModSilence (SpellAuraEffects.cpp:3252-3257) coupe tout
 *     sort dont PreventionType == SILENCE, quels que soient ses drapeaux ;
 *   - aura STUN, FEAR, CONFUSE : Unit::SetStunned / SetControlled (Unit.cpp:15074, 15084, 15185) appellent CastStop.
 * Seul un effet vise sur un ENNEMI compte (pas un etourdissement du lanceur), et pas un effet auquel la cible est
 * immunisee (Unit::IsImmunedToSpellEffect, le test du coeur : mecanisme, etat, ecole). Sorts declenches : un niveau,
 * auras de proc ecartees. La racine ne coupe rien. Une silence posee par SCRIPT (Toxic Dart 807237 -> 807821, du
 * module, AscensionClassMechanics.cpp:778-789) n'est lisible nulle part dans le DBC : elle n'est pas vue.
 */
uint8 CoaAreneCoupe(Player* bot, SpellInfo const* info, SpellInfo const* vise, bool canalisee, Unit* cible,
                    uint8 profondeur = 0)
{
    // Relecture : immunite au sort ENTIER (mecanisme du sort, dissipation, ecole -- bouclier divin), que le test par
    // effet ne voit pas : Unit::IsImmunedToSpell, le test du coeur.
    if (!profondeur && cible->IsImmunedToSpell(info, static_cast<Unit const*>(bot)))
        return COA_COUPE_NON;
    // Banc du 11/10 (tour 7) : sort a cible unite explicite que le type de l'adversaire interdit -- PlayerbotAI::CastSpell
    // lui donne l'adversaire pour cible (sauf sort a point au sol ou a source, PlayerbotAI.cpp:3961-3970) et
    // Spell::CheckCast refuse tout le lancer, sorts declenches compris (CoaAreneTypeCibleRefuse).
    if (!profondeur && (info->GetExplicitTargetMask() & TARGET_FLAG_UNIT_MASK) &&
        !(info->Targets & (TARGET_FLAG_DEST_LOCATION | TARGET_FLAG_SOURCE_LOCATION)) && CoaAreneTypeCibleRefuse(info, cible))
        return COA_COUPE_NON;
    uint8 meilleur = COA_COUPE_NON;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = info->Effects[i];
        if (!effect.IsEffect())
            continue;
        bool const ennemi = IsEnemyTarget(effect.TargetA.GetTarget()) || IsEnemyTarget(effect.TargetB.GetTarget());
        bool const silenceVise = vise->PreventionType == SPELL_PREVENTION_TYPE_SILENCE;
        uint8 coupe = COA_COUPE_NON;
        // Un effet qui ne vise pas l'adversaire ne coupe rien ; un sort qu'il declenche est juge plus bas, sur ses
        // propres cibles.
        if (ennemi && effect.Effect == SPELL_EFFECT_INTERRUPT_CAST)
        {
            if (silenceVise && (canalisee ? (vise->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT)
                                          : (vise->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT)))
                coupe = COA_COUPE_PURE;
        }
        else if (ennemi && IsAuraEffect(effect))
        {
            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_MOD_SILENCE: case SPELL_AURA_MOD_PACIFY_SILENCE:
                    if (silenceVise)
                        coupe = COA_COUPE_PURE;
                    break;
                case SPELL_AURA_MOD_STUN: case SPELL_AURA_MOD_FEAR: case SPELL_AURA_MOD_CONFUSE:
                    coupe = COA_COUPE_CONTROLE;
                    break;
                default:
                    break;
            }
        }
        // Relecture : une zone autour du lanceur ou un cone (ni cible unite explicite ni point au sol, que CastFirst
        // poserait sur l'adversaire) partirait dans le vide si l'adversaire est hors de son rayon -- une peur de zone a
        // 8 yd lancee a 30 yd gachait l'interruption. Au premier niveau seulement : un sort declenche part de la cible.
        if (coupe != COA_COUPE_NON && !profondeur &&
            (IsEnemyAreaTarget(effect.TargetA.GetTarget()) || IsEnemyAreaTarget(effect.TargetB.GetTarget())) &&
            !(info->GetExplicitTargetMask() & (TARGET_FLAG_UNIT_MASK | TARGET_FLAG_DEST_LOCATION)))
        {
            float const rayon = effect.CalcRadius(bot);
            if (rayon <= 0.0f || !bot->IsWithinDistInMap(cible, rayon))
                coupe = COA_COUPE_NON;
        }
        // Un effet de zone dont le type de l'adversaire interdit le sort ne le touche pas (selection implicite, meme test).
        if (coupe != COA_COUPE_NON && !cible->IsImmunedToSpellEffect(info, i, bot) && !CoaAreneTypeCibleRefuse(info, cible))
        {
            if (coupe == COA_COUPE_PURE)
                return COA_COUPE_PURE;
            meilleur = coupe;
        }

        bool const proc = IsAuraEffect(effect) &&
            (effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL || effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE ||
             effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_DAMAGE || effect.ApplyAuraName == SPELL_AURA_ADD_TARGET_TRIGGER);
        if (effect.TriggerSpell && !proc && !profondeur)
            if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
            {
                uint8 const sous = CoaAreneCoupe(bot, declenche, vise, canalisee, cible, 1);
                if (sous == COA_COUPE_PURE)
                    return COA_COUPE_PURE;
                if (sous != COA_COUPE_NON)
                    meilleur = sous;
            }
    }
    return meilleur;
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

// Survie, palier critique : fin d'une fuite hors de vue (journal « survie_fuite_fin », pause, cumul).
void CoaAreneSurvieFuiteSortie(Player* bot, CoaAreneEtat& etat, Unit* adv, char const* raison, uint32 maintenant)
{
    uint32 const duree = getMSTimeDiff(etat.survieFuiteDebutMs, maintenant);
    etat.survieFuiteCumulMs += duree;
    etat.survieFuiteDebutMs = 0;
    etat.survieFuiteFinMs = maintenant ? maintenant : 1;
    float const d = adv ? bot->GetExactDist2d(adv) : -1.0f;
    CoaAreneJournal(bot, etat, "survie_fuite_fin", d, d,
                    Acore::StringFormat(" raison={} duree_ms={} cumul_ms={} pv={:.0f}", raison, duree,
                                        etat.survieFuiteCumulMs, bot->GetHealthPct()));
}

// Cloture d'un engagement : recul et cachette en cours clos, bilan ecrit (les deux bras), etat desarme. `fin` dit
// pourquoi : « grace » (10 s sans engagement vu) ou « nouvelle_instance » (une autre arene, ou le passage
// arene <-> duel/monde, avant la fin de la grace). Champ ajoute EN FIN de ligne (outils/banc-arene.py lit cle=valeur).
void CoaAreneClore(PlayerbotAI* botAI, Player* bot, CoaAreneEtat& etat, Unit* adv, uint32 maintenant, char const* fin)
{
    if (etat.reculFinMs)
        CoaAreneCloreRecul(bot, etat, adv, "fin_engagement");
    if (etat.pilierDebutMs)
        CoaArenePilierSuivre(botAI, bot, etat, nullptr, false, maintenant);
    if (etat.survieFuiteDebutMs)
        CoaAreneSurvieFuiteSortie(bot, etat, adv, "fin_engagement", maintenant);
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
                    Acore::StringFormat(" instance={} fin={} familiers={}", etat.instance, fin, etat.familiers) +
                    // Combat contre un humain (11/10) : champs ajoutes EN FIN de ligne (contrat de outils/banc-arene.py).
                    Acore::StringFormat(" incantations_vues={} soins_vus={} interruptions={} interruptions_soin={} "
                                        "interruptions_ratees={} dispels_soi={} fenetres_vues={} fenetres_burst={} "
                                        "fenetre_sorts={}", etat.incantationsVues, etat.soinsVus, etat.interruptions,
                                        etat.interruptionsSoin, etat.interruptionsRatees, etat.dispelsSoi,
                                        etat.fenetresVues, etat.fenetresBurst, etat.fenetreSorts) +
                    // Banc du 11/10 (tour 7) : ratees rattrapees sur la meme incantation (deja retirees des ratees).
                    Acore::StringFormat(" interruptions_rattrapees={}", etat.interruptionsRattrapees) +
                    // 11/10 (match id=89) : sorts sans effet ecartes, par sort et par 2 s (CoaAreneNoterRefusTactique).
                    Acore::StringFormat(" refus_tactiques={}", etat.refusTactiques) +
                    // Survie d'arene (11/10, IaSurvie) : sorts par palier, soins sur soi, fuites, dissipations refusees par
                    // raison (par raison et par 2 s), sorts ecartes faute de ressource (par sort et par 5 s), etendards
                    // poses, sources de degats hors de vue notees. EN FIN de ligne.
                    Acore::StringFormat(" survie_pression={} survie_danger={} survie_critique={} soins_soi={} "
                                        "survie_fuites={} dispel_refus_pv={} dispel_refus_mana={} dispel_refus_rentable={} "
                                        "dispel_refus_reapplique={} refus_ressource={} soutiens={} sources_hors_vue={} survie_controles={}",
                                        etat.survieSorts[1], etat.survieSorts[2], etat.survieSorts[3], etat.soinsSoi,
                                        etat.survieFuites, etat.dispelRefus[1], etat.dispelRefus[2], etat.dispelRefus[3],
                                        etat.dispelRefus[4], etat.refusRessource, etat.soutiens, etat.sourcesHorsVue,
                                        etat.survieControles) +
                    // Lot 20 (13/10) : soins ou boucliers deja poses ecartes (par sort et par 5 s), generateurs de ressource
                    // lances par la survie, soins retenus par la reserve de mana (par sort et par 5 s). EN FIN de ligne.
                    Acore::StringFormat(" deja_pose={} survie_generateurs={} survie_reserve_mana={}", etat.dejaPose,
                                        etat.survieGenerateurs, etat.survieReserveMana));
    etat.actif = false;
    etat.survieNiveau = 0;
    etat.survieFuiteDebutMs = 0;
    etat.sourceGuetDepuisMs = 0;
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

    // Combat contre un humain (11/10) : chaque incantation adverse comptee une fois (les deux bras : c'est la base de
    // comparaison des interruptions), soins a part. Arene 1v1 et duel seulement.
    {
        bool canalisee = false;
        Spell* incantation = (cadre == COA_ARENE_ARENE || cadre == COA_ARENE_DUEL) && !preparation && adv &&
                             adv->IsAlive() && adv->GetMap() == bot->GetMap() && CoaAreneUnContreUn(bot)
                                 ? CoaAreneIncantationAdverse(adv, canalisee) : nullptr;
        uintptr_t const adresse = reinterpret_cast<uintptr_t>(incantation);
        uint32 const sort = incantation ? incantation->GetSpellInfo()->Id : 0;
        if (incantation && (adresse != etat.incantationAdvSpell || sort != etat.incantationAdvSort))
        {
            ++etat.incantationsVues;
            etat.soinsVus += CoaAreneEstSoin(incantation->GetSpellInfo()) ? 1 : 0;
            etat.interruptionJugeeSpell = 0;
            etat.interruptionJugeeSort = 0;
            etat.interruptionJugeeRatee = false;
        }
        etat.incantationAdvSpell = adresse;
        etat.incantationAdvSort = sort;
    }

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
            // 11/10 (match humain id=18, 477 classe 26) : arrete au PREMIER tick en vue, le tireur se posait au ras du
            // pilier ; l'adversaire faisait un pas, la vue se perdait (vue 0/1 a 13-26 yd, bouge=0, 2 sorts de degats en
            // tout le match). La vue doit tenir CoaAreneRetourVueTenueMs sans interruption, le bot continuant sa course,
            // avant l'arret : il sort de l'angle du pilier.
            bool const enVue = bot->IsWithinLOSInMap(adv);
            if (!enVue)
                etat.retourVuDepuisMs = 0;
            else if (!etat.retourVuDepuisMs)
                etat.retourVuDepuisMs = maintenant ? maintenant : 1;
            if (enVue && CoaAreneDistanceCombat(bot, adv) <= haut - CoaAreneMargePortee - 2.0f &&
                (getMSTimeDiff(etat.retourVuDepuisMs, maintenant) >= CoaAreneRetourVueTenueMs || !bot->isMoving()))
            {
                if (bot->isMoving())
                    bot->StopMoving();
                etat.retourEnCours = false;
                etat.retourVuDepuisMs = 0;
            }
            else if (!bot->isMoving() && etat.dernierRetourMs && getMSTimeDiff(etat.dernierRetourMs, maintenant) > 500)
                etat.retourEnCours = false;  // arrive, ou mouvement repris par autre chose : le declencheur jugera
        }
    }
    // Revue survie v2 : la vue tenue repart de zero a chaque retour -- sinon une heure de vue restee d'un retour clos par
    // une autre branche (melee, adversaire mort, arret) ferait arreter le retour suivant des son premier tick en vue.
    if (!etat.retourEnCours)
        etat.retourVuDepuisMs = 0;

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

    // Survie, palier critique : fuite hors de vue en cours, close a la fin de l'engagement, a l'echeance (6 s), quand la
    // vie remonte au palier pression ou au-dessus (le soin est passe), ou quand l'adversaire voit le bot arrete (fuite
    // ratee : il a contourne).
    if (etat.survieFuiteDebutMs)
    {
        uint32 const depuis = getMSTimeDiff(etat.survieFuiteDebutMs, maintenant);
        if (cadre == COA_ARENE_AUCUN || preparation || !adv || !adv->IsAlive() || !bot->IsAlive())
            CoaAreneSurvieFuiteSortie(bot, etat, adv, "fin", maintenant);
        else if (depuis >= CoaAreneSurvieFuiteMaxMs)
            CoaAreneSurvieFuiteSortie(bot, etat, adv, "duree", maintenant);
        // Revue survie v2 : et seulement si le palier du tick precedent est retombe sous danger (2) -- un danger arme par
        // la vie PROJETEE (DoT, raison « dots ») a vie haute fermait la fuite des le tick suivant (« pv »), pause de 8 s.
        else if (etat.survieNiveau < 2 && bot->GetHealthPct() >= std::max(CoaAreneSurvieDangerPv, etat.survieSeuilDanger))
            CoaAreneSurvieFuiteSortie(bot, etat, adv, "pv", maintenant);
        else if (depuis > 1500 && !bot->isMoving() && adv->IsWithinLOSInMap(bot))
            CoaAreneSurvieFuiteSortie(bot, etat, adv, "vu", maintenant);
    }

    if (cadre == COA_ARENE_AUCUN && getMSTimeDiff(etat.dernierContactMs, maintenant) > CoaAreneGraceMs)
    {
        CoaAreneClore(botAI, bot, etat, adv, maintenant, "grace");
        return etat;
    }

    // Survie d'arene (IaSurvie) : le palier du tick, lu par les trois declencheurs « coa arene survie », le recul ouvert
    // aux tireurs et le burst. Arene 1v1 ou duel, engagement vu, bras tire. La pente se lit sur le releve de vie du
    // defensif (deux releves espaces de 2 s : une perte mesuree sur 2 a 4 s).
    etat.survieNiveau = 0;
    etat.survieRaison = "-";
    etat.survieSeuilDanger = CoaAreneSurvieDangerPv;
    if (CoaAreneSurvieArmee() && etat.bras && etat.vuCeTick && (cadre == COA_ARENE_ARENE || cadre == COA_ARENE_DUEL) &&
        bot->IsAlive() && CoaAreneUnContreUn(bot))
    {
        float const pv = bot->GetHealthPct();
        // Seuils haussés par le pire coup recent X (% de la vie maximale, 15 s ; 11/10, match id=10 : un tick de Melt
        // Reality a retire 38 % d'un coup, de 40 a 2 %) : soigner et se proteger AVANT d'arriver sous 2X (danger), le
        // dernier recours avant de tomber sous X (critique). Bornes : danger 85 %, critique 60 %, pression au-dessus du
        // danger (+15, 95 au plus). Sans coup recent, les seuils fixes (70/50/30).
        // Lot 18 (11/10, match humain id=43, Stormbringer 425) : le pire coup ne hausse les seuils que s'il est FRAIS
        // (CoaAreneSurviePireCoupFraisMs) ou si le bot perd encore de la vie sur le releve de 2 a 4 s. Vie stable ou qui
        // remonte apres un seul gros coup : retour aux seuils fixes avant les 15 s de la fenetre.
        uint32 const agePire = etat.pireCoupMs ? getMSTimeDiff(etat.pireCoupMs, maintenant) : CoaAreneSurviePireCoupMs + 1;
        bool const pirePertinent = agePire <= CoaAreneSurviePireCoupFraisMs ||
                                   std::max(etat.pvPrec, etat.pvRef) - bot->GetHealthPct() >= 1.0f;
        float const pire = etat.pireCoupMs && agePire <= CoaAreneSurviePireCoupMs && pirePertinent
            ? etat.pireCoupPct : 0.0f;
        float const seuilCritique = std::max(CoaAreneSurvieCritiquePv, std::min(pire, 60.0f));
        float const seuilDanger = std::max(CoaAreneSurvieDangerPv, std::min(2.0f * pire, 85.0f));
        float const seuilPression = std::max(CoaAreneSurviePressionPv, std::min(seuilDanger + 15.0f, 95.0f));
        etat.survieSeuilDanger = seuilDanger;
        if (pv < seuilCritique)
        {
            etat.survieNiveau = 3;
            etat.survieRaison = pire > CoaAreneSurvieCritiquePv ? "coup" : "pv";
        }
        else if (pv < seuilDanger)
        {
            etat.survieNiveau = 2;
            etat.survieRaison = pv < CoaAreneSurvieDangerPv ? "pv" : "coup";
        }
        else
        {
            // Part de la vie que les DoT portes prevoient sur 6 s, lue une fois et seulement si un palier en depend.
            float dotsPct = -1.0f;
            auto dots = [&]()
            {
                if (dotsPct < 0.0f)
                {
                    uint32 nombre = 0;
                    dotsPct = bot->GetMaxHealth()
                        ? CoaAreneDotsPrevus(bot, 0, nombre, true) * 100.0f / float(bot->GetMaxHealth()) : 0.0f;
                }
                return dotsPct;
            };
            // Vie PROJETEE a 6 s (vie moins DoT prevus) sous le seuil danger : le palier danger s'arme maintenant (11/10,
            // match id=12 : Felsworn a 85 % sous 60 % de DoT prevus, rien lance, mort 8 s plus tard ; match id=6 : 84 % et
            // 47 %). « dots_lourds » si elle passe meme sous le seuil critique : les gros defensifs sont alors permis.
            if (dots() >= CoaAreneSurvieDotsPct && pv - dots() < seuilDanger)
            {
                etat.survieNiveau = 2;
                etat.survieRaison = pv - dots() < seuilCritique ? "dots_lourds" : "dots";
            }
            else if (pv < seuilPression)
            {
                etat.survieNiveau = 1;
                etat.survieRaison = "pv";
            }
            else if (std::max(etat.pvPrec, etat.pvRef) - pv >= CoaAreneSurviePentePv)
            {
                etat.survieNiveau = 1;
                etat.survieRaison = "pente";
            }
            else if (dots() >= CoaAreneSurvieDotsPct)
            {
                etat.survieNiveau = 1;
                etat.survieRaison = "dots";
            }
        }
    }

    // Source du premier degat subi hors de vue (gladiateur trace, arene) : le guet s'ouvre quand l'adversaire sort de la
    // vue du bot, se referme quand il y revient ; le crochet de degats (CoaArene1v1.cpp) note le premier coup du guet, et
    // la ligne s'ecrit ici. Un test de ligne de vue par mise a jour, gladiateurs traces seulement.
    if (cadre == COA_ARENE_ARENE && !preparation && adv && adv->IsAlive() && bot->IsAlive() &&
        CoaAreneTraceActive(botAI, bot))
    {
        if (bot->IsWithinLOSInMap(adv))
        {
            etat.sourceGuetDepuisMs = 0;
            etat.sourceNotee = false;
        }
        else if (!etat.sourceGuetDepuisMs)
        {
            etat.sourceGuetDepuisMs = maintenant ? maintenant : 1;
            etat.sourceNotee = false;
        }
    }
    else
        etat.sourceGuetDepuisMs = 0;
    if (etat.sourceAEcrire)
    {
        etat.sourceAEcrire = false;
        ++etat.sourcesHorsVue;
        static char const* const Genres[] = { "-", "joueur", "familier", "autre" };
        float const d = adv ? bot->GetExactDist2d(adv) : -1.0f;
        CoaAreneJournal(bot, etat, "source_degat", d, d,
                        Acore::StringFormat(" genre={} entree={} sort={} periodique={} degats={} furtif_au_coup={} "
                                            "hors_vue_ms={} pv={:.0f}",
                                            Genres[etat.sourceGenre < 4 ? etat.sourceGenre : 0], etat.sourceEntree,
                                            etat.sourceSort, etat.sourcePeriodique ? 1 : 0, etat.sourceDegats,
                                            etat.sourceFurtif ? 1 : 0,
                                            etat.sourceMs,
                                            bot->GetHealthPct()));
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
// 804802 portent l'attribut (AttributesEx 0x8000) SANS aucun effet d'immunite -- 32 refus SPELL_FAILED_STUNNED que le
// filtre laissait passer, l'attribut seul suffisant a les exempter. RECTIFIE (tour 4, 11/10) : Vigil of the Moon
// 802797, cite ici a tort, porte DEUX effets SPELL_AURA_DISPEL_IMMUNITY (41) ; vrai pour lui, le coeur juge alors
// aura par aura -- voir CoaAreneImmuniteInsuffisante, qui recopie ce jugement.
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

// Le sort accorde une immunite, et pourtant le coeur le refuserait : RECOPIE de la seconde boucle de
// Spell::CheckCasterAuras (Spell.cpp:7044-7104), masques compris. Tour 4 du banc (10/10 22:07:29 UTC, Starcaller
// 1422) : Vigil of the Moon 802797 porte SPELL_ATTR1_IMMUNITY_PURGES_EFFECT ET deux effets SPELL_AURA_DISPEL_IMMUNITY
// (41, MiscValue 5 et 6 : furtivite, invisibilite -- lu dans Spell.dbc) ; CoaAreneAccordeImmunite le laissait donc au
// coeur, qui le refusait SPELL_FAILED_STUNNED x3 sous Battle Rush 501546 (MOD_STUN, mecanisme etourdissement, que
// l'immunite de dissipation ne couvre pas). Appele seulement quand le lanceur est deja entrave ET que le sort accorde
// une immunite : la boucle sur les auras ne coute rien au tour ordinaire.
bool CoaAreneImmuniteInsuffisante(Player* bot, SpellInfo const* info)
{
    uint8 school_immune = 0;
    uint64 mechanic_immune = 0;
    uint32 dispel_immune = 0;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (info->Effects[i].ApplyAuraName == SPELL_AURA_SCHOOL_IMMUNITY)
            school_immune |= uint32(info->Effects[i].MiscValue);
        else if (info->Effects[i].ApplyAuraName == SPELL_AURA_MECHANIC_IMMUNITY)
            mechanic_immune |= 1ULL << uint32(info->Effects[i].MiscValue);
        else if (info->Effects[i].ApplyAuraName == SPELL_AURA_DISPEL_IMMUNITY)
            dispel_immune |= SpellInfo::GetDispelMask(DispelType(info->Effects[i].MiscValue));
    }
    switch (info->Id)
    {
        case 42292: case 59752: case 65547: case 53490: case 19574: case 34471: case 46227:
            mechanic_immune = IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK;
            break;
        default:
            break;
    }
    // Le coeur : aucune immunite reelle -> refus (prevented_reason). CoaAreneAccordeImmunite garantit qu'il y en a une.
    if (!school_immune && !mechanic_immune && !dispel_immune)
        return true;

    bool usableInStun = info->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_STUNNED);
    if (info->Id == 33206 && !bot->HasAura(63248))
        usableInStun = false;
    for (auto const& [id, application] : bot->GetAppliedAuras())
    {
        Aura const* aura = application->GetBase();
        SpellInfo const* auraInfo = aura->GetSpellInfo();
        if (auraInfo->GetAllEffectsMechanicMask() & mechanic_immune)
            continue;
        if ((auraInfo->GetSchoolMask() & school_immune) &&
            !auraInfo->HasAttribute(SPELL_ATTR1_IMMUNITY_TO_HOSTILE_AND_FRIENDLY_EFFECTS))
            continue;
        if (auraInfo->GetDispelMask() & dispel_immune)
            continue;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            AuraEffect* part = aura->GetEffect(i);
            if (!part)
                continue;
            switch (part->GetAuraType())
            {
                case SPELL_AURA_MOD_STUN:
                {
                    uint64 mask = 1ULL << MECHANIC_STUN;
                    if (info->Id == 22812)
                        mask |= 1ULL << MECHANIC_SAPPED | 1ULL << MECHANIC_HORROR | 1ULL << MECHANIC_SLEEP;
                    if (info->Id == 1044)
                        mask |= (1ULL << MECHANIC_SAPPED) | (1ULL << MECHANIC_FEAR);
                    if (!usableInStun || !(auraInfo->GetAllEffectsMechanicMask() & mask))
                        return true;
                    break;
                }
                case SPELL_AURA_MOD_CONFUSE:
                    if (!info->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_CONFUSED))
                        return true;
                    break;
                case SPELL_AURA_MOD_FEAR:
                    if (!info->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_FLEEING))
                        return true;
                    break;
                case SPELL_AURA_MOD_SILENCE:
                case SPELL_AURA_MOD_PACIFY:
                case SPELL_AURA_MOD_PACIFY_SILENCE:
                    if (info->PreventionType == SPELL_PREVENTION_TYPE_PACIFY ||
                        info->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
                        return true;
                    break;
                default:
                    break;
            }
        }
    }
    return false;
}

bool CoaAreneBloqueParEtat(Player* bot, SpellInfo const* info)
{
    if (info->HasAttribute(SPELL_ATTR6_NOT_AN_ATTACK))
        return false;

    // Ce que le coeur appelle prevented_reason (Spell.cpp:6997-7038), sans le tri des mecanismes des sorts utilisables
    // etourdi (laisse au coeur : jamais plus strict).
    bool entrave = false;
    uint32 const drapeaux = bot->GetUnitFlags();
    if (drapeaux & UNIT_FLAG_STUNNED)
        entrave = !info->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_STUNNED);
    else if (drapeaux & UNIT_FLAG_CONFUSED)
        entrave = !info->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_CONFUSED);
    else if (drapeaux & UNIT_FLAG_FLEEING)
        entrave = !info->HasAttribute(SPELL_ATTR5_ALLOW_WHILE_FLEEING);
    if (!entrave && (drapeaux & UNIT_FLAG_SILENCED) && info->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
        entrave = true;
    if (!entrave && (drapeaux & UNIT_FLAG_PACIFIED) && info->PreventionType == SPELL_PREVENTION_TYPE_PACIFY)
        entrave = true;
    if (!entrave)
        return false;

    // Un sort qui accorde une immunite : le coeur compare chaque aura du lanceur a l'immunite (recopie ci-dessus).
    if (CoaAreneAccordeImmunite(info))
        return CoaAreneImmuniteInsuffisante(bot, info);
    return true;
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
    // Survie d'arene (11/10, IaSurvie) : sous 50 % de vie, pas de burst -- la ressource va d'abord a la survie (paliers
    // danger et critique). SEULE exception gardee : la cible sous 30 % (« vie_cible »), ou le burst est la fin du
    // combat. « vie_bot » (le bot sous 35 % depense sa pile) est donc eteint sous IaSurvie.
    // Revue du 11/10 : lu sur le palier de CoaAreneMaj (2 = danger, 3 = critique), qui porte deja le cadre de la survie
    // (arene 1v1 ou duel, bras tire, engagement vu) -- le test de vie seul eteignait aussi le burst du Ranger en arene a
    // plusieurs des bots et en un contre un du monde ouvert (MondeOuvert), ou la survie ne joue pas.
    if (etat.survieNiveau >= 2 && std::strcmp(burst.raison, "vie_cible") != 0)
        burst.raison = "";
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

// ---------------------------------------------------------------------------------------------------------------
// Rapprochement (tour 4 du banc, 11/10) : une melee CoA en duel ou en arene 1v1 dont l'adversaire est a plus de
// CoaAreneRapprochementMin yd met en tete de sa liste ses sorts de charge ou de saut VISES sur la cible. Aucun sort
// ajoute : seulement l'ordre de ceux que l'action d'attaque a deja retenus (et filtres a cette distance), le tourniquet
// les tentant sinon dans son ordre a lui. Le classifieur (KIND_*, 16 bits, tous pris) n'a pas de genre « mobilite » :
// le test se lit sur les effets. Mesure d'avant (ticks du tour 4, moteur combat, sans cacheur) : la melee voyait son
// adversaire a plus de 10 yd 14 a 42 % du temps, et sur 16 matchs deux charges seulement sont parties (Battle Rush
// 501546 a 12 yd, Lunge 500126 a 9 yd). Hors arene 1v1 et hors duel : un test de booleen, rien d'autre.
// ---------------------------------------------------------------------------------------------------------------
constexpr float CoaAreneRapprochementMin = 10.0f;

// Charge ou saut vers une cible unite explicite (Battle Rush, Lunge). Pas Hellfire Trample 804704 (CHARGE_DEST vers
// l'avant du lanceur, cible implicite 47) : il ne vise pas l'adversaire.
bool CoaAreneSortDeRapprochement(SpellInfo const* info)
{
    if (!(info->GetExplicitTargetMask() & TARGET_FLAG_UNIT_MASK))
        return false;
    for (SpellEffectInfo const& effect : info->Effects)
        switch (effect.Effect)
        {
            case SPELL_EFFECT_CHARGE: case SPELL_EFFECT_CHARGE_DEST: case SPELL_EFFECT_JUMP: case SPELL_EFFECT_JUMP_DEST:
            {
                // Revue tour 5 : l'effet de mobilite lui-meme doit viser l'ADVERSAIRE (6 TARGET_UNIT_TARGET_ENEMY,
                // 25 TARGET_UNIT_TARGET_ANY, 53 TARGET_DEST_TARGET_ENEMY, 63-71 TARGET_DEST_TARGET_*), pas un allie
                // (Intervene, Temple Guardian 801165) ni un point sans rapport (Heroic Leap 606544, Grand Entrance).
                auto versAdversaire = [](uint32 t) { return t == 6 || t == 25 || t == 53 || (t >= 63 && t <= 71); };
                if (versAdversaire(effect.TargetA.GetTarget()) || versAdversaire(effect.TargetB.GetTarget()))
                    return true;
                break;
            }
            default:
                break;
        }
    return false;
}

void CoaAreneOrdonnerRapprochement(PlayerbotAI* botAI, Player* bot, Unit* cible, std::vector<Usable>& usable)
{
    if (!sPlayerbotAIConfig.coaAreneIaCommune || !cible || usable.size() < 2 ||
        !(bot->InArena() || (bot->duel && bot->duel->Opponent)))
        return;
    if (bot->GetExactDist2d(cible) <= CoaAreneRapprochementMin)
        return;
    CoaAreneEtat& etat = CoaAreneMaj(botAI, bot);
    if (!CoaAreneL1Actif(etat) || !CoaAreneEstAdversaire(etat, cible) || CoaAreneMouvementVoulu(botAI) ||
        CoaAreneCacheurActif(bot, etat))
        return;
    // Melee seulement (le retour la mene au contact) ; le Ranger de L1 garde sa distance.
    if (!CoaAreneRetourArme(bot) || CoaAreneEstRangerDistance(bot) || !CoaAreneRetourAuContact(botAI, bot))
        return;
    std::stable_partition(usable.begin(), usable.end(),
                          [bot](Usable const& spell) { return CoaAreneSortDeRapprochement(EffectiveSpell(bot, spell.info)); });
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
// Fenetre de burst (11/10, combat contre un humain) : le bot vient d'etourdir, d'effrayer ou -- s'il tire a distance
// -- d'enraciner l'adversaire ; ses sorts de degats les plus lourds passent en tete de liste pendant le controle.
// Generalisation PRUDENTE du burst du Ranger (CoaAreneOrdonnerBurst, qui reste seul juge pour le Ranger de L1) :
//   - aucun sort n'est retire : seul l'ordre change (le tourniquet et CastFirst jugent le reste) ;
//   - en tete : KIND_DAMAGE sans KIND_CONTROL (ne pas recontroler ni gacher le controle), pret (CoaAreneSortPret),
//     incantation plus courte que le controle restant, et cible NON immunisee (Unit::IsImmunedToDamage et
//     Unit::IsImmunedToSpell, les tests du coeur ; un bouclier d'immunite renvoie le sort en queue, pas a la porte) ;
//   - ordre : degats de base lus dans les effets (CoaAreneDegatsDeBase), puis cout. Pas de rotation par specialisation.
// Le controle compte s'il est pose par le bot ou par ce qu'il controle (familier), et s'il lui reste 0,5 s au moins.
// ---------------------------------------------------------------------------------------------------------------
struct CoaAreneFenetre
{
    bool ouverte = false;
    uint32 aura = 0;
    char const* controle = "";
    int32 resteMs = 0;
    std::vector<SpellInfo const*> tete;
    std::vector<float> degats;
};

constexpr int32 CoaAreneFenetreResteMinMs = 500;

// Degats de base d'un sort, lus dans ses effets : pour ORDONNER, jamais pour filtrer. Un sort scripte (effet DUMMY)
// vaut 0 ici et se departage par son cout.
float CoaAreneDegatsDeBase(Player* bot, SpellInfo const* info)
{
    WeaponAttackType const attaque = info->DmgClass == SPELL_DAMAGE_CLASS_RANGED ? RANGED_ATTACK : BASE_ATTACK;
    float const arme = (bot->GetWeaponDamageRange(attaque, MINDAMAGE) + bot->GetWeaponDamageRange(attaque, MAXDAMAGE)) / 2.0f;
    float total = 0.0f;
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (!effect.IsEffect())
            continue;
        float const valeur = float(std::max(0, effect.CalcValue(bot)));
        switch (effect.Effect)
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE: case SPELL_EFFECT_HEALTH_LEECH:
                total += valeur;
                break;
            case SPELL_EFFECT_WEAPON_DAMAGE: case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL: case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                total += arme + valeur;
                break;
            case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                total += arme * valeur / 100.0f;
                break;
            default:
                if (IsAuraEffect(effect) && effect.Amplitude > 0 &&
                    (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE || effect.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH))
                {
                    int32 const duree = info->GetMaxDuration();
                    if (duree > 0)
                        total += valeur * float(duree / effect.Amplitude);
                }
                break;
        }
    }
    return total;
}

// Le controle pose par le bot (ou son familier) sur la cible, le plus long qui reste. Faux s'il n'y en a pas.
bool CoaAreneControleDuBot(Player* bot, Unit* cible, bool tireur, CoaAreneFenetre& fenetre)
{
    struct Genre { AuraType type; char const* nom; };
    // Revue tour 7 : pas la peur -- le premier gros coup la briserait (fenetre qui gache son propre controle).
    Genre const genres[] = { { SPELL_AURA_MOD_STUN, "etourdissement" }, { SPELL_AURA_MOD_ROOT, "racine" } };
    for (Genre const& genre : genres)
    {
        if (genre.type == SPELL_AURA_MOD_ROOT && !tireur)
            continue;
        for (AuraEffect const* effet : cible->GetAuraEffectsByType(genre.type))
        {
            Aura const* aura = effet ? effet->GetBase() : nullptr;
            if (!aura)
                continue;
            Unit* lanceur = aura->GetCaster();
            if (!lanceur || lanceur->GetCharmerOrOwnerOrSelf() != bot)
                continue;
            int32 const duree = aura->GetDuration();
            int32 const reste = duree > 0 ? duree : int32(CoaAreneFenetreParDefautMs);
            if (reste < CoaAreneFenetreResteMinMs || reste <= fenetre.resteMs)
                continue;
            fenetre.aura = aura->GetId();
            fenetre.resteMs = reste;
            fenetre.controle = genre.nom;
        }
    }
    return fenetre.aura != 0;
}

CoaAreneFenetre CoaAreneOrdonnerFenetre(PlayerbotAI* botAI, Player* bot, Unit* cible, std::vector<Usable>& usable,
                                        bool burstRanger)
{
    CoaAreneFenetre fenetre;
    if (!sPlayerbotAIConfig.coaAreneIaCommune || burstRanger || !cible || usable.empty() ||
        CoaAreneEstRangerDistance(bot) || !CoaAreneUnContreUn(bot))
        return fenetre;
    CoaAreneEtat& etat = CoaAreneMaj(botAI, bot);
    if (!CoaAreneL1Actif(etat) || !CoaAreneEstAdversaire(etat, cible) || cible->GetMap() != bot->GetMap())
        return fenetre;
    if (!CoaAreneControleDuBot(bot, cible, !CoaAreneRetourAuContact(botAI, bot), fenetre))
        return fenetre;

    uint32 const maintenant = getMSTime();
    bool const echue = !etat.fenetreFinMs || getMSTimeDiff(etat.fenetreFinMs, maintenant) < 0x80000000u;
    if (fenetre.aura != etat.fenetreAura || echue)
    {
        ++etat.fenetresVues;
        etat.fenetreAura = fenetre.aura;
        etat.fenetreExploitee = false;
    }
    uint32 const fin = maintenant + uint32(fenetre.resteMs);
    etat.fenetreFinMs = fin ? fin : 1;
    fenetre.ouverte = true;

    auto const enTete = [&](Usable const& spell)
    {
        if (!(spell.kind & KIND_DAMAGE) || (spell.kind & KIND_CONTROL))
            return false;
        SpellInfo const* info = EffectiveSpell(bot, spell.info);
        return int32(info->CalcCastTime(bot)) < fenetre.resteMs && !cible->IsImmunedToDamage(bot, info) &&
               !cible->IsImmunedToSpell(info, static_cast<Unit const*>(bot)) && CoaAreneSortPret(botAI, bot, spell);
    };
    auto const finTete = std::stable_partition(usable.begin(), usable.end(), enTete);

    std::vector<std::pair<std::pair<float, int32>, Usable>> classes;
    for (auto itr = usable.begin(); itr != finTete; ++itr)
    {
        SpellInfo const* info = EffectiveSpell(bot, itr->info);
        classes.push_back({ { CoaAreneDegatsDeBase(bot, info), info->CalcPowerCost(bot, info->GetSchoolMask()) }, *itr });
    }
    std::stable_sort(classes.begin(), classes.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    for (size_t i = 0; i < classes.size(); ++i)
    {
        usable[i] = classes[i].second;
        fenetre.tete.push_back(classes[i].second.info);
        fenetre.degats.push_back(classes[i].first.first);
    }
    return fenetre;
}

void CoaAreneNoterFenetre(PlayerbotAI* botAI, Player* bot, Unit* cible, CoaAreneFenetre const& fenetre, SpellInfo const* cast)
{
    if (!fenetre.ouverte || !cast)
        return;
    auto const itr = std::find(fenetre.tete.begin(), fenetre.tete.end(), cast);
    if (itr == fenetre.tete.end())
        return;
    CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
    if (!etat.fenetreExploitee)
    {
        etat.fenetreExploitee = true;
        ++etat.fenetresBurst;
    }
    ++etat.fenetreSorts;
    size_t const rang = size_t(std::distance(fenetre.tete.begin(), itr));
    float const d = cible ? bot->GetExactDist2d(cible) : -1.0f;
    CoaAreneJournal(bot, etat, "fenetre_burst", d, d,
                    Acore::StringFormat(" sort={} controle={} aura={} reste_ms={} rang={} tete={} degats_base={:.0f} "
                                        "pv_cible={:.0f}", cast->Id, fenetre.controle, fenetre.aura, fenetre.resteMs, rang,
                                        fenetre.tete.size(), fenetre.degats[rang], cible ? cible->GetHealthPct() : -1.0f));
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
    // Survie d'arene (11/10, match id=93 : « elle ne faisait plus rien », mana jamais tracee) : mana en % (-1 sans mana),
    // energie (-1 sans energie), furtivite, DoT prevus sur 6 s en % de la vie (toutes auras de degats periodiques
    // nuisibles), palier de survie. EN FIN de ligne.
    uint32 nDots = 0;
    float const dotsPrevus = CoaAreneDotsPrevus(bot, 0, nDots, true);
    LOG_INFO("playerbots.coa", "coa arene trace: guid={} tick action={} moteur={} cible={} cible_adversaire={} dist={:.1f}"
             " vue={} pv_bot={:.0f} pv_cible={:.0f} pile_advantage={} bouge={} omises={} adv={} vue_adv={} dist_adv={:.1f}"
             " portee_adv={} cacheur={} mana_pct={:.0f} energie={} furtif={} dot_prevu_pct={:.0f} survie={} rage={} focus={}"
             " verrou={} runique={}",
             bot->GetGUID().GetCounter(),
             action, botAI->GetState() == BOT_STATE_COMBAT ? "combat" : "hors_combat",
             cible ? cible->GetGUID().GetCounter() : 0, CoaAreneEstAdversaire(etat, cible) ? 1 : 0,
             cible ? bot->GetExactDist2d(cible) : -1.0f, cible && bot->IsWithinLOSInMap(cible) ? 1 : 0,
             bot->GetHealthPct(), cible ? cible->GetHealthPct() : -1.0f, CoaAreneTracePile(bot, etat),
             bot->isMoving() ? 1 : 0, omises, adv ? adv->GetGUID().GetCounter() : 0, vueAdv ? 1 : 0,
             adv ? bot->GetExactDist2d(adv) : -1.0f, porteeAdv ? 1 : 0, CoaAreneCacheurActif(bot, etat) ? 1 : 0,
             bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : -1.0f,
             bot->GetMaxPower(POWER_ENERGY) ? int32(bot->GetPower(POWER_ENERGY)) : -1, bot->HasStealthAura() ? 1 : 0,
             bot->GetMaxHealth() ? dotsPrevus * 100.0f / float(bot->GetMaxHealth()) : 0.0f, etat.survieNiveau,
             // 11/10 (classe 31 spe 95, Boulder Dash) : rage et focus en points (comme le cout des sorts, rage x10), -1 sans
             // reserve ; verrou = aura 263 en cours (plus aucun sort). EN FIN de ligne (contrat de outils/banc-arene.py).
             bot->GetMaxPower(POWER_RAGE) ? int32(bot->GetPower(POWER_RAGE)) : -1,
             bot->GetMaxPower(POWER_FOCUS) ? int32(bot->GetPower(POWER_FOCUS)) : -1,
             bot->HasPlayerFlag(PLAYER_ALLOW_ONLY_ABILITY) ? 1 : 0,
             // 11/10 (Necromancer 432, match id=13) : Command: Undead et Plague of Undeath se paient en puissance runique
             // (PowerType 6, x10 comme le cout des sorts) ; -1 sans reserve. EN FIN de ligne.
             bot->GetMaxPower(POWER_RUNIC_POWER) ? int32(bot->GetPower(POWER_RUNIC_POWER)) : -1);
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

// ---------------------------------------------------------------------------------------------------------------
// Choix tactiques (11/10/2026, arene 1v1 id=89, gladiateur 477, classe 26, contre un humain, 11:54 Paris) : 11 lancers,
// 0 degat inflige (« coa score: degats_joueur=0 »). Celestial Cleave 802435 (cone 104, rayon 6) lance a 103,8 puis
// 30,3 yd ; Eclipse of Fury 570218 (zone SRC_CASTER, rayon 10) lance trois fois, a 55,7, 38,6 et 29,0 yd ; Vigil of the
// Moon 802797 (DISPEL_IMMUNITY furtivite/invisibilite sur les ennemis de la zone) contre un adversaire ni furtif ni
// invisible. Rayons et cibles lus dans Spell.dbc / SpellRadius.dbc le 11/10.
//
// Ce ne sont PAS des pre-controles : le coeur accepte ces sorts (aucune cible unitaire exigee), ils partent, coutent la
// recharge globale et de la ressource, et ne touchent personne. Le cadre est donc borne : arene 1v1 ou duel
// (CoaAreneUnContreUn), engagement vu, bras tire -- l'adversaire est alors le SEUL ennemi qui compte. Un sort ecarte
// ici n'est ni mis au banc ni compte en echec : il repart des que l'adversaire est dans la zone.
// ---------------------------------------------------------------------------------------------------------------

// L'adversaire du 1v1 si les choix tactiques s'appliquent a ce tick, sinon nullptr. Hors duel et hors arene : deux
// tests de champ, pas de mise a jour d'etat.
Unit* CoaAreneTactiqueAdversaire(PlayerbotAI* botAI, Player* bot)
{
    if (!sPlayerbotAIConfig.coaAreneIaCommune || !bot || !CoaAreneUnContreUn(bot))
        return nullptr;
    CoaAreneEtat const& etat = CoaAreneMaj(botAI, bot);
    if (!CoaAreneL1Actif(etat) || (etat.cadre != COA_ARENE_ARENE && etat.cadre != COA_ARENE_DUEL))
        return nullptr;
    return CoaAreneAdversaireVivant(bot, etat);
}

// L'effet vise les ennemis d'une zone ou d'un cone centre sur le lanceur : reference CASTER (cones, zones autour du
// lanceur), ou SRC posee sur le lanceur par TargetA = TARGET_SRC_CASTER (Eclipse of Fury : A=22, B=15).
// Zone posee par script sans rayon dans la donnee : 10 yd, le rayon reel de Cloudburst (802385, revue du 11/10) --
// seul sort connu concerne ; 15 laissait partir des lancers vains entre 10 et ~17 yd.
constexpr float CoaAreneRayonScript = 10.0f;

bool CoaAreneEffetZoneDuLanceur(SpellEffectInfo const& effet)
{
    for (SpellImplicitTargetInfo const* cible : { &effet.TargetA, &effet.TargetB })
    {
        if (!cible->GetTarget() || !cible->IsArea() || cible->GetCheckType() != TARGET_CHECK_ENEMY)
            continue;
        SpellTargetReferenceTypes const ref = cible->GetReferenceType();
        if (ref == TARGET_REFERENCE_TYPE_CASTER)
            return true;
        if (ref == TARGET_REFERENCE_TYPE_SRC && effet.TargetA.GetTarget() == TARGET_SRC_CASTER)
            return true;
        // 11/10 (match humain id=11) : Cloudburst 801838, zone ennemie 16 (DEST_AREA) autour d'une destination posee SUR
        // le lanceur (18, TARGET_DEST_CASTER) -- lance a 111 yd de l'adversaire, non reconnu comme zone du lanceur.
        if (ref == TARGET_REFERENCE_TYPE_DEST && effet.TargetA.GetTarget() == TARGET_DEST_CASTER)
            return true;
    }
    return false;
}

// L'effet frappe ou ronge (degats directs, d'arme, drain, brulure, degats periodiques).
bool CoaAreneEffetFrappe(SpellEffectInfo const& effet)
{
    switch (effet.Effect)
    {
        case SPELL_EFFECT_SCHOOL_DAMAGE: case SPELL_EFFECT_HEALTH_LEECH: case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
        case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE: case SPELL_EFFECT_WEAPON_DAMAGE: case SPELL_EFFECT_POWER_BURN:
        case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
            return true;
        default:
            break;
    }
    switch (effet.ApplyAuraName)
    {
        case SPELL_AURA_PERIODIC_DAMAGE: case SPELL_AURA_PERIODIC_LEECH: case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
        case SPELL_AURA_PERIODIC_MANA_LEECH:
            return true;
        default:
            return false;
    }
}

// Un effet qui ne rapporte rien au lanceur par lui-meme : aura factice ou declencheur periodique sur soi,
// declenchement d'un sort qui n'est pas benefique (ou inerte). Stellar Drift 800501 : aura 23 (declenche le recul 800500)
// et aura 4 sur soi, plus le ralentissement 802773 en zone. Tout autre effet (soin, bouclier, ressource, aura de
// stats sur soi) donne au sort une valeur sans adversaire dans la zone : il n'est alors jamais ecarte.
bool CoaAreneEffetAccessoire(SpellEffectInfo const& effet)
{
    // Le sort declenche n'est pas benefique, ou il est inerte (que des effets ou auras factices : Vigil of the Moon
    // 802797 declenche 802798, PERSISTENT_AREA_AURA d'aura 4, que IsPositive range parmi les benefiques).
    auto declencheNonBenefique = [](uint32 id)
    {
        SpellInfo const* declenche = id ? sSpellMgr->GetSpellInfo(id) : nullptr;
        if (!declenche)
            return false;
        // Revue du 11/10 : un sort declenche qui vise une unite explicite la recoit du lanceur (Spell::EffectTriggerSpell :
        // m_targets.GetUnitTarget(), soit la cible de CastSpell -- l'adversaire), A TOUTE DISTANCE de la zone. Ex. Poison
        // Nova 1589205 -> Vile Infliction 1589214 (aura 271, A=6). Jamais accessoire.
        if (declenche->NeedsExplicitUnitTarget())
            return false;
        if (!declenche->IsPositive())
            return true;
        bool unEffet = false;
        for (SpellEffectInfo const& e : declenche->Effects)
        {
            if (!e.Effect)
                continue;
            unEffet = true;
            bool const factice = e.Effect == SPELL_EFFECT_DUMMY ||
                                 ((e.Effect == SPELL_EFFECT_APPLY_AURA || e.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA) &&
                                  (e.ApplyAuraName == SPELL_AURA_DUMMY || e.ApplyAuraName == SPELL_AURA_PERIODIC_DUMMY));
            if (!factice)
                return false;
        }
        return unEffet;
    };
    // SPELL_EFFECT_DUMMY n'en est PAS : un script peut y faire tout le travail du sort (degats, ressource).
    switch (effet.Effect)
    {
        case SPELL_EFFECT_TRIGGER_SPELL:
        // Lot 20 (13/10, banc du 12/10 : Waves of Time 801277 du Chronomancer lance 3 fois sur 6 a 106-119 yd). Son effet 0
        // est le declenchement DIFFERE Ascension (183) du recul en cone 802600 (KNOCK_BACK, cible 24, rayon 13 yd), pose
        // sur le lanceur ; l'effet 1 est le cone (TRIGGER_SPELL, cible 24) du visuel 65633. Le 183 n'etait ni zone ni
        // accessoire : il comptait comme « valeur propre » et le sort n'etait jamais ecarte, alors que sa portee est
        // « soi » (RangeEntry 1 : le pre-controle de portee ne juge rien) et que tout son effet est dans le cone. Le
        // declenchement differe est lu comme un declenchement immediat : accessoire si le sort declenche n'est pas
        // benefique (ou inerte), sinon il garde sa valeur propre.
        case SPELL_EFFECT_ASCENSION_TRIGGER_SPELL_DELAYED:
            return declencheNonBenefique(effet.TriggerSpell);
        case SPELL_EFFECT_APPLY_AURA:
            if (effet.TargetA.GetTarget() != TARGET_UNIT_CASTER)
                return false;
            if (effet.ApplyAuraName == SPELL_AURA_DUMMY || effet.ApplyAuraName == SPELL_AURA_PERIODIC_DUMMY)
                return true;
            return effet.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL && declencheNonBenefique(effet.TriggerSpell);
        default:
            return false;
    }
}

// L'effet revele un ennemi furtif ou invisible : detection, ou immunite de dissipation furtivite/invisibilite posee sur
// les ennemis (Vigil of the Moon 802797 : PERSISTENT_AREA_AURA, aura 41, MiscValue 5 et 6, B=16 zone ennemie).
bool CoaAreneEffetRevele(SpellEffectInfo const& effet)
{
    if (effet.ApplyAuraName == SPELL_AURA_MOD_STEALTH_DETECT || effet.ApplyAuraName == SPELL_AURA_MOD_INVISIBILITY_DETECT)
        return true;
    if (effet.ApplyAuraName != SPELL_AURA_DISPEL_IMMUNITY ||
        (effet.MiscValue != DISPEL_STEALTH && effet.MiscValue != DISPEL_INVISIBILITY))
        return false;
    return effet.TargetA.GetCheckType() == TARGET_CHECK_ENEMY || effet.TargetB.GetCheckType() == TARGET_CHECK_ENEMY;
}

// Angle du cone, recopie de Spell::SelectImplicitConeTargets (Spell.cpp:1245-1263).
float CoaAreneAngleCone(SpellInfo const* info, SpellImplicitTargetInfo const& cible)
{
    if (SpellCone const* cone = sSpellMgr->GetSpellCone(info->Id))
        return G3D::toRadians(static_cast<float>(cone->cone_degrees));
    switch (cible.GetTarget())
    {
        case TARGET_UNIT_CONE_ENEMY_24:  return G3D::toRadians(24.0f);
        case TARGET_UNIT_CONE_ENEMY_54:  return G3D::toRadians(54.0f);
        case TARGET_UNIT_CONE_ENEMY_104: return G3D::toRadians(104.0f);
        default:                         return G3D::toRadians(60.0f);
    }
}

// Marge ajoutee au rayon du coeur : l'adversaire bouge entre la decision et l'effet. Jouer large ne fait que laisser
// partir un sort comme avant ce correctif ; jouer serre en jetterait un utile.
constexpr float CoaAreneMargeZone = 1.5f;
// Un debuff qui expire dans moins de ce delai peut etre renouvele.
constexpr int32 CoaAreneDebuffRenouvelerMs = 1500;

// CoaAreneRefusTactiqueType : declare dans CoaAiObjectContext.cpp, avant CastFirst qui s'en sert.
char const* CoaAreneRefusTactiqueNom(uint8 refus)
{
    switch (refus)
    {
        case COA_TACTIQUE_HORS_ZONE:  return "tactique_hors_zone";
        case COA_TACTIQUE_HORS_CONE:  return "tactique_hors_cone";
        case COA_TACTIQUE_REVELATION: return "tactique_revelation_inutile";
        case COA_TACTIQUE_DEJA_POSE:  return "tactique_debuff_deja_pose";
        case COA_TACTIQUE_VERROU:     return "tactique_verrou";
        case COA_TACTIQUE_SURVIE_CHARGE: return "tactique_survie_charge";
        case COA_TACTIQUE_SURVIE_INCANTATION: return "tactique_survie_incantation";
        default:                      return "-";
    }
}

// Pourquoi ce sort, lance maintenant contre `adv`, ne servirait a rien (COA_TACTIQUE_AUCUN : il sert, ou on ne sait
// pas). `info` : le sort effectif (EffectiveSpell), celui que PlayerbotAI::CastSpell lance vraiment, donc celui dont
// l'aura se pose. `cible` : la cible du lancer (CastFirst), comptee si c'est un autre ennemi que l'adversaire.
// Lit seulement le sort, le bot, l'adversaire, ses unites controlees et la cible.
//
// Revue du 11/10 : une zone qui touche un AUTRE ennemi (familier, gardien, totem de l'adversaire, ou la cible courante
// du bot) a un effet reel -- elle n'est jamais ecartee pour la seule absence de l'adversaire (regles 1 et 3).
// Regle 4, le verrou (11/10, banc de 13:00-13:20 Paris, classe 31 spe 95, gladiateur 440) : Boulder Dash 500692 enferme
// le lanceur 10 s -- chaque coup 500693 (toutes les 500 ms) repose sur lui l'aura 263 (ALLOW_ONLY_ABILITY : plus aucun
// sort, soin compris ; CastFirst les ecarte par `seulementCapacites`) et 500692 porte l'aura 241 (FORCE_MOVE_FORWARD,
// que seul un client execute : un bot reste sur place). Lance a 20-28 yd de l'adversaire, a la reprise de vue, le bot
// restait fige 8 a 10 s sans toucher personne ni se soigner : mana gelee, 0 sort, 0 degat jusqu'a la mort (matchs 13,
// 21 et 23 du banc). Rend le rayon de ses coups (zone d'ennemis autour du lanceur, lue sur le sort et ses sorts
// declenches) si le sort pose l'aura 263 sur le lanceur, 0 si aucune zone n'est lue, -1 si ce n'est pas un verrou.
// Un verrou qui rend le lanceur INSENSIBLE (immunite aux degats ou a une ecole posee sur lui : Liquify 806310, 263 +
// immunite 40) est un defensif, pas une ruee : il n'est pas un verrou ici (-1), la survie le lance quand il faut.
float CoaAreneRayonVerrou(Player* bot, SpellInfo const* info)
{
    bool verrou = false;
    bool immunite = false;
    float rayon = 0.0f;
    auto lire = [&](SpellInfo const* sort)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& effet = sort->Effects[i];
            if (!effet.Effect)
                continue;
            bool const surSoi = !effet.TargetA.GetTarget() || effet.TargetA.GetTarget() == TARGET_UNIT_CASTER;
            if (surSoi && effet.IsAura(SPELL_AURA_ALLOW_ONLY_ABILITY))
                verrou = true;
            if (surSoi && (effet.IsAura(SPELL_AURA_DAMAGE_IMMUNITY) || effet.IsAura(SPELL_AURA_SCHOOL_IMMUNITY)))
                immunite = true;
            if (CoaAreneEffetZoneDuLanceur(effet) && effet.HasRadius())
                rayon = std::max(rayon, effet.CalcRadius(bot));
        }
    };
    lire(info);
    // Un niveau de declenchement : sort declenche, ou aura periodique qui declenche (Boulder Dash : aura 23 -> 500693).
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effet = info->Effects[i];
        bool const declenche = effet.Effect == SPELL_EFFECT_TRIGGER_SPELL ||
                               effet.IsAura(SPELL_AURA_PERIODIC_TRIGGER_SPELL) ||
                               effet.IsAura(SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE);
        if (!declenche || !effet.TriggerSpell || effet.TriggerSpell == info->Id)
            continue;
        SpellInfo const* sort = sSpellMgr->GetSpellInfo(effet.TriggerSpell);
        if (!sort)
            continue;
        // Revue du 11/10 : une aura periodique posee sur un AUTRE que le lanceur declenche son sort depuis ce porteur
        // (AuraEffect::HandlePeriodicTriggerSpellAuraTick : `target`, sauf NeedsToBeTriggeredByCaster) -- son 263
        // « sur soi » enfermerait le porteur, pas le bot. Seul le sort declenche par le bot lui-meme est lu.
        bool const auraSurSoi = !effet.TargetA.GetTarget() || effet.TargetA.GetTarget() == TARGET_UNIT_CASTER;
        if (effet.Effect == SPELL_EFFECT_TRIGGER_SPELL || auraSurSoi || sort->NeedsToBeTriggeredByCaster(info, i))
            lire(sort);
    }
    return verrou && !immunite ? rayon : -1.0f;
}

uint8 CoaAreneRefusTactique(Player* bot, SpellInfo const* info, Unit* adv, Unit* cible)
{
    if (!bot || !info || !adv)
        return COA_TACTIQUE_AUCUN;

    // Regle 4 : pour un bot qui n'est pas de melee, un verrou ne part que si l'adversaire est deja sous ses coups (voir
    // CoaAreneRayonVerrou). Une melee le garde comme avant : la ruee (+70 % de vitesse, aura 31) la porte au contact par
    // ses propres deplacements (retour, « reach melee »), et ses coups frappent a l'arrivee (spe 59, 2543 et 461, nuit
    // du 10-11/10). Un tireur, lui, se plante a portee de tir (CoaArenePlante) et reste enferme sans rien toucher.
    // Un verrou a cible designee suit le coeur (arbitrage plus bas).
    // Revue du 11/10 : le rayon (lecture de SpellInfo, sans verrou) d'abord ; le style ensuite, et seulement pour un
    // verrou -- GetCoaStyle prend le verrou global de AscensionClassService (GetActiveSpecialization), et ce test tourne
    // pour chaque sort a chaque CastFirst.
    float rayonVerrou = adv->GetMap() != bot->GetMap() ? -1.0f : CoaAreneRayonVerrou(bot, info);
    if (rayonVerrou >= 0.0f && GetCoaStyle(bot) == CoaStyle::Melee)
        rayonVerrou = -1.0f;
    // Revue du 11/10 (arbitrage) : seulement un verrou dont la zone est LUE et qui ne vise pas l'adversaire. Un verrou a
    // cible designee (Hurricane 645435 : son script frappe la cible a toute distance) ou a zone non reconnue (Fury of the
    // Illidari 561216, cibles 18/16) suit le coeur -- jamais plus strict que lui sans preuve que le sort est vain.
    if (rayonVerrou > 0.0f && !info->NeedsExplicitUnitTarget() &&
        bot->GetExactDist(adv) > rayonVerrou + bot->GetLeewayBonusRadius() + adv->GetObjectSize() + CoaAreneMargeZone)
        return COA_TACTIQUE_VERROU;

    if (info->NeedsExplicitUnitTarget())
        return COA_TACTIQUE_AUCUN;

    // Les ennemis qui comptent : l'adversaire d'abord, puis la cible du lancer et les unites qu'il controle (familier,
    // gardiens, totems), vivants, sur la carte du bot, attaquables par lui. Borne : 8 unites.
    std::array<Unit*, 8> ennemis{};
    uint8 nEnnemis = 0;
    auto ajouter = [&](Unit* u)
    {
        if (!u || nEnnemis >= ennemis.size() || !u->IsInWorld() || !u->IsAlive() || u->GetMap() != bot->GetMap())
            return;
        for (uint8 k = 0; k < nEnnemis; ++k)
            if (ennemis[k] == u)
                return;
        if (u != adv && !bot->IsValidAttackTarget(u))
            return;
        ennemis[nEnnemis++] = u;
    };
    ajouter(adv);
    if (!nEnnemis)
        return COA_TACTIQUE_AUCUN;
    ajouter(cible);
    for (Unit* u : adv->m_Controlled)
        ajouter(u);

    bool revele = false;           // un effet revele la furtivite ou l'invisibilite
    bool autreQueRevele = false;   // un effet qui sert a autre chose qu'a reveler
    bool zone = false;             // un effet vise les ennemis d'une zone autour du lanceur
    bool valeurPropre = false;     // un effet sert sans adversaire dans la zone (ni zone, ni accessoire)
    bool dansUneZone = false;      // un ennemi qui compte est dans au moins une zone du sort
    bool autreDansZone = false;    // ... et cet ennemi n'est pas l'adversaire (regle 3 : il n'a pas le debuff)
    bool horsCone = false;         // l'adversaire est dans le rayon d'un cone, mais pas devant
    bool queDesAuras = true;       // toutes les zones posent une aura (regle 3)
    bool frappe = false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effet = info->Effects[i];
        if (!effet.Effect)
            continue;
        frappe = frappe || CoaAreneEffetFrappe(effet);
        bool const revelation = CoaAreneEffetRevele(effet);
        bool const zoneLanceur = !revelation && CoaAreneEffetZoneDuLanceur(effet);
        bool const accessoire = !revelation && !zoneLanceur && CoaAreneEffetAccessoire(effet);
        revele = revele || revelation;
        autreQueRevele = autreQueRevele || (!revelation && !accessoire);
        valeurPropre = valeurPropre || (!zoneLanceur && !accessoire);
        if (!zoneLanceur)
            continue;

        zone = true;
        if (effet.Effect != SPELL_EFFECT_APPLY_AURA && effet.Effect != SPELL_EFFECT_APPLY_AREA_AURA_ENEMY)
            queDesAuras = false;
        SpellImplicitTargetInfo const& zoneCible =
            effet.TargetA.GetSelectionCategory() == TARGET_SELECT_CATEGORY_CONE ? effet.TargetA : effet.TargetB;
        bool const cone = zoneCible.GetSelectionCategory() == TARGET_SELECT_CATEGORY_CONE;
        // Sans rayon lu dans SpellRadius.dbc, la zone du coeur ne touche personne : seul un script agit, a une distance que
        // la donnee ne dit pas (Cloudburst : « ten-yard area », AscensionStormbringerTalents.cpp). 11/10 : un rayon
        // SUPPOSE de CoaAreneRayonScript (15 yd, plus large que les 10 yd lus dans le seul script recense) remplace le
        // « jamais ecarte » d'avant, qui laissait partir Cloudburst a 111 yd. Un cone sans rayon reste inconnu.
        if (!effet.HasRadius() && cone)
        {
            dansUneZone = true;
            autreDansZone = true;
            continue;
        }
        // Rayon du coeur (CalcRadius avec les modificateurs du lanceur, plus la marge de deplacement du lanceur).
        float const rayonCoeur = (effet.HasRadius() ? effet.CalcRadius(bot) : CoaAreneRayonScript) +
                                 bot->GetLeewayBonusRadius();
        for (uint8 k = 0; k < nEnnemis; ++k)
        {
            Unit* u = ennemis[k];
            // Taille de la cible (WorldObject::IsWithinDist3d), plus CoaAreneMargeZone.
            if (bot->GetExactDist(u) > rayonCoeur + u->GetObjectSize() + CoaAreneMargeZone)
                continue;
            bool dansLeCone = true;
            if (cone)
            {
                // Recopie de WorldObjectSpellConeTargetCheck (Spell.cpp:9355-9372).
                if (info->HasAttribute(SPELL_ATTR0_CU_CONE_BACK))
                    dansLeCone = bot->isInBack(u, CoaAreneAngleCone(info, zoneCible));
                else if (info->HasAttribute(SPELL_ATTR0_CU_CONE_LINE))
                    dansLeCone = bot->HasInLine(u, u->GetObjectSize(), bot->GetObjectSize());
                else
                    dansLeCone = bot->IsWithinBoundaryRadius(u) || bot->isInFront(u, CoaAreneAngleCone(info, zoneCible));
            }
            if (dansLeCone)
            {
                dansUneZone = true;
                autreDansZone = autreDansZone || u != adv;
            }
            // Se tourner vers l'adversaire ne sert qu'a un cone devant (ou en ligne) : pas a un cone dans le dos.
            else if (u == adv && !info->HasAttribute(SPELL_ATTR0_CU_CONE_BACK))
                horsCone = true;
        }
    }

    // Regle 2 : un sort qui ne fait que reveler, contre un adversaire ni furtif ni invisible.
    if (revele && !autreQueRevele && !adv->HasStealthAura() && !adv->HasInvisibilityAura())
        return COA_TACTIQUE_REVELATION;

    // Regle 1 : aucun ennemi qui compte dans les zones d'un sort qui ne vaut que par elles.
    if (!zone || valeurPropre)
        return COA_TACTIQUE_AUCUN;
    if (!dansUneZone)
        return horsCone ? COA_TACTIQUE_HORS_CONE : COA_TACTIQUE_HORS_ZONE;

    // Regle 3 : debuff de zone deja pose par le bot sur l'adversaire, seul ennemi dans la zone, ni a renouveler, ni a
    // empiler. L'aura lue est celle du sort EFFECTIF (revue du 11/10 : lire aussi celle de l'id de la rotation ecartait
    // un remplacement qui pose une AUTRE aura, parce que l'aura de base etait deja la).
    if (frappe || !queDesAuras || autreDansZone)
        return COA_TACTIQUE_AUCUN;
    if (Aura const* aura = adv->GetAura(info->Id, bot->GetGUID()))
    {
        bool const aRenouveler = aura->GetDuration() >= 0 && aura->GetDuration() < CoaAreneDebuffRenouvelerMs;
        uint32 const pileMax = aura->GetSpellInfo()->StackAmount;
        bool const aEmpiler = pileMax > 1 && aura->GetStackAmount() < pileMax;
        if (!aRenouveler && !aEmpiler)
            return COA_TACTIQUE_DEJA_POSE;
    }
    return COA_TACTIQUE_AUCUN;
}

// Trace du refus (gladiateur d'arene 1v1, AiPlayerbot.Arene1v1.Trace) : une ligne par sort et par 2 s au plus.
void CoaAreneNoterRefusTactique(PlayerbotAI* botAI, Player* bot, SpellInfo const* info, Unit* adv, uint8 refus, bool trace)
{
    CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
    uint32 const maintenant = getMSTime();
    uint32& derniere = etat.refusTactiqueTraceMs[info->Id];
    if (derniere && getMSTimeDiff(derniere, maintenant) < 2 * IN_MILLISECONDS)
        return;
    derniere = maintenant ? maintenant : 1;
    // Le bilan compte les refus par sort et par 2 s (un sort ecarte a chaque tour en vaudrait dix par seconde).
    ++etat.refusTactiques;
    if (trace)
        CoaAreneTracerSort(botAI, bot, info, adv, SPELL_CAST_OK, CoaAreneRefusTactiqueNom(refus));
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

/*
 * Lot 19 (11/10/2026, ~23:30 Paris) -- « Le chevalier est cense etre sur sa monture pour me charger. Je ne vois aucun
 * bot qui m'attaque en etant invisible. » Mesure : `ouvertures=1` dans 29 bilans d'arene sur 1 686, tous du Ranger 2239 ;
 * la preparation et l'ouverture n'etaient armees que pour le Ranger de degats a distance (L1).
 *
 * Ouvreur FURTIF : le Ranger L1 comme avant (monde ouvert compris) ; sous IaCommune, en arene 1v1 ou en duel, toute
 * classe CoA dont le kit porte une furtivite (KIND_STEALTH, aura MOD_STEALTH sur soi), hors soigneur. Releve des 68
 * gladiateurs (character_spell, Spell.dbc, sonde-ouverture-kits.py) : Ranger 21 (Elude 801345, ouverture Flank),
 * Venomancer 29 (Skulk 800843 ; Parasite 800921, Arachnophobia 804970, Lure 807759 « seulement furtif »), classe 30
 * (Underwalk 800797, Shade 573038 ; Haunt 573425, Deathstalker 804054, Soulstone Lure 561376), Runemaster 32
 * (Runeshroud 500288), Witch Hunter 15 (Witching Shroud 805770, apres un critique magique seulement : StrictCheck
 * la refuse hors de cette fenetre), Felsworn 14 (Twisted Sight 532229, CANALISEE : jamais prise ici). Mirage 501136
 * (classe 13) est une INVISIBILITE (aura 18) avec copie laissee derriere : pas une furtivite d'approche, non traitee.
 */
bool CoaAreneOuvreurFurtif(Player* bot)
{
    if (CoaAreneEstRangerDistance(bot))
        return true;
    // Revue lot 19 : CoaAreneUnContreUn (deux lectures de champ) avant le role et le kit -- ce predicat est evalue a
    // chaque tick par le declencheur de preparation pour TOUS les bots CoA, presque tous hors duel et hors arene.
    return sPlayerbotAIConfig.coaAreneIaCommune && IsAscensionCustomClassId(bot->getClass()) &&
           CoaAreneUnContreUn(bot) && GetCoaRole(bot) != CoaRole::Heal && ClassHas(bot, KIND_STEALTH);
}

/*
 * Ouvreur MONTE (lot 19) : monture DE CLASSE (sort du kit qui pose SPELL_AURA_MOUNTED sur le lanceur) et attaques
 * qui partent montees (SPELL_ATTR0_ALLOW_WHILE_MOUNTED, attaque au sens d'IsAttack). Releve du kit des 21 classes
 * (AscensionCustomClassData.h x Spell.dbc) : seul le Knight of Xoroth (17) a les deux -- Xorothian Warsteed 804703
 * (niveau 20, aura 78 + 32 +20 % de vitesse, « usable in combat, castable while moving » ; une pile de Path of
 * Hellfire 804787 toutes les 3 s sans degat donne ni recu, 3 piles au niveau 30, 5 au 40), Hellfire Trample 804704
 * (bond de 25 yd vers l'avant, degats de feu en cone 24 de rayon 25 devant, +X % par pile) et Dreadrider's Stomp
 * 804702 (feu autour du lanceur, rayon 8). Le module (AscensionXorothAbilities.cpp:147) refuse 804702/804704 sans
 * l'aura 804703, ou sous Hellbound Charge 807248. Le coeur accepte une monture en arene (Spell::CheckCast,
 * SPELL_AURA_MOUNTED : IsBattlegroundOrArena) ; StrictCheck le verifie de toute facon.
 *
 * Monte, le bot ne lance RIEN d'autre (Spell::CheckCast : SPELL_FAILED_NOT_MOUNTED sans ALLOW_WHILE_MOUNTED, puis banc
 * de 20 s) : tant qu'il est sur sa monture de classe en engagement, l'ouverture garde la main -- elle approche, frappe
 * monte, puis DESCEND (CheckMountStateAction ne joue pas en arene : isUseful faux sous InArena -- garde AMONT, toutes
 * les arenes, pas propre a ce lot. En DUEL elle joue : le multiplicateur ne lui laisse que la descente pendant
 * l'engagement, et l'ouverture (57) passe devant elle (54) tant que le bot est en selle.)
 */
bool CoaAreneEstMontureClasse(SpellInfo const* info)
{
    return info && !info->IsPassive() && info->HasAura(SPELL_AURA_MOUNTED);
}

bool CoaAreneEstAttaqueMontee(Usable const& spell)
{
    return IsAttack(spell.kind, false) && spell.info->HasAttribute(SPELL_ATTR0_ALLOW_WHILE_MOUNTED);
}

// Monture de classe du kit, si le bot connait aussi au moins une attaque montee ; lue une fois par engagement.
SpellInfo const* CoaAreneMontureOuvreur(PlayerbotAI* botAI, Player* bot, CoaAreneEtat& etat)
{
    if (!etat.montureLue)
    {
        etat.montureLue = true;
        etat.montureClasseId = 0;
        std::vector<Usable> const kit = KnownAbilities(botAI, bot, [](uint16) { return true; });
        bool attaque = std::any_of(kit.begin(), kit.end(), [](Usable const& s) { return CoaAreneEstAttaqueMontee(s); });
        if (attaque)
            for (Usable const& s : kit)
                if (CoaAreneEstMontureClasse(s.info))
                {
                    etat.montureClasseId = s.info->Id;
                    break;
                }
    }
    return etat.montureClasseId ? sSpellMgr->GetSpellInfo(etat.montureClasseId) : nullptr;
}

bool CoaAreneOuvreurMonteArme(Player* bot)
{
    return sPlayerbotAIConfig.coaAreneIaCommune && IsAscensionCustomClassId(bot->getClass()) &&
           CoaAreneUnContreUn(bot) && GetCoaRole(bot) != CoaRole::Heal;
}

// Sur sa monture de classe (l'aura du sort de monture, posee par lui).
bool CoaAreneMonteClasse(PlayerbotAI* botAI, Player* bot, CoaAreneEtat& etat)
{
    SpellInfo const* monture = CoaAreneMontureOuvreur(botAI, bot, etat);
    return monture && bot->HasAura(monture->Id, bot->GetGUID());
}

// Furtivite d'arene : la premiere furtivite du kit que le coeur accepte, JAMAIS canalisee (Twisted Sight 532229 lierait
// la vue a un oeil invoque et planterait le bot), hors combat, a pied. Meme filtre de kit que CoaChasseEntrerEnFurtivite
// (une furtivite, jamais un sort hostile) ; rend le sort parti, nullptr sinon.
SpellInfo const* CoaAreneEntrerEnFurtivite(PlayerbotAI* botAI, Player* bot)
{
    if (bot->IsInCombat() || bot->IsMounted() || !bot->IsAlive() || bot->HasAuraType(SPELL_AURA_MOD_STEALTH))
        return nullptr;
    std::vector<Usable> furtivites = KnownAbilities(botAI, bot, [](uint16 kind)
        { return (kind & KIND_STEALTH) && !(kind & (KIND_HOSTILE | KIND_DAMAGE)); });
    // Une furtivite qui dure (Underwalk, Skulk, Runeshroud, Elude : duree infinie) avant une furtivite breve a recharge
    // (Shade 573038, 10 s) : la breve tomberait pendant l'approche et sa recharge serait perdue.
    std::stable_partition(furtivites.begin(), furtivites.end(),
                          [](Usable const& s) { return s.info->GetMaxDuration() < 0; });
    for (Usable const& spell : furtivites)
    {
        if (spell.info->IsChanneled() || StrictCheck(bot, spell.info, bot) != SPELL_CAST_OK)
            continue;
        if (botAI->CastSpell(spell.info->Id, bot))
            return RecordUsage(USAGE_STEALTH, spell.info);
    }
    return nullptr;
}

// Ligne « ouverture_refus » : une par raison toutes les 5 s (diagnostic du lot 19 : pourquoi un bot furtif ou monte
// n'ouvre pas). Raisons : 1 l1_inactif, 2 attente, 3 hors_fenetre (le retour mene), 4 fenetre_recul, 5 recul_en_cours,
// 6 rien_pret (action), 7 adversaire_invalide (action), 8 descente (action, monte).
constexpr uint32 CoaAreneOuvertureRefusTraceMs = 5 * IN_MILLISECONDS;

void CoaAreneOuvertureRefus(Player* bot, CoaAreneEtat& etat, uint8 raison, float dist, std::string const& suite = "")
{
    static char const* const noms[] = { "?", "l1_inactif", "attente", "hors_fenetre", "fenetre_recul",
                                        "recul_en_cours", "rien_pret", "adversaire_invalide", "descente" };
    uint32 const maintenant = getMSTime();
    if (etat.ouvertureRefusRaison == raison && etat.ouvertureRefusMs &&
        getMSTimeDiff(etat.ouvertureRefusMs, maintenant) < CoaAreneOuvertureRefusTraceMs)
        return;
    etat.ouvertureRefusRaison = raison;
    etat.ouvertureRefusMs = maintenant ? maintenant : 1;
    CoaAreneJournal(bot, etat, "ouverture_refus", dist, dist,
                    Acore::StringFormat(" raison={} classe={} furtif={} monte={}{}", raison < 9 ? noms[raison] : "?",
                                        uint32(bot->getClass()), bot->HasStealthAura() ? 1 : 0,
                                        bot->IsMounted() ? 1 : 0, suite));
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
            // Combat contre un humain (11/10) : en arene 1v1 et en duel, « coa arene interruption » remplace
            // « coa interrupt », qui coupait toute incantation interruptible, meme courte (voir CoaAreneIncantationDigne).
            if (nom == "coa interrupt" && CoaAreneUnContreUn(bot))
                return 0.0f;
            // Survie d'arene (11/10, IaSurvie) : en arene 1v1 et en duel, « coa arene survie » remplace « coa defensive »
            // (28, sous tous les deplacements d'arene), et la dissipation sur soi rentable remplace « coa dispel » (15) --
            // qui lancait Sanctify 524968 a 100 % de vie, sans regarder la mana (match id=93, ~10 des 14 lancers).
            if (CoaAreneSurvieArmee() && (nom == "coa defensive" || nom == "coa dispel") && CoaAreneUnContreUn(bot))
                return 0.0f;
            // Survie, palier critique : pendant la fuite hors de vue, rien ne ramene le bot vers l'adversaire (memes noms
            // que la cachette L2 et le cacheur), ni ne le retourne (« set facing » l'arreterait en chemin).
            if (etat.survieFuiteDebutMs &&
                (nom == "reach spell" || nom == "reach melee" || nom == "arena tactics" || nom == "move random" ||
                 nom == "flee" || nom == "set facing" || nom == "coa arene retour" || nom == "coa arene pilier" ||
                 nom == "coa arene recul" || nom == "coa arene ouverture"))
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

        // Un recul en cours ne se laisse pas defaire : se retourner vers la cible (set facing, 37) ou lancer un sort a
        // incantation (CastFirst arrete le bot) le figeraient au contact. Ranger de L1, et depuis le 11/10 (IaSurvie) tout
        // tireur a qui le recul est ouvert (CoaAreneReculOuvert).
        if (CoaAreneReculEnCours(etat) && (nom == "set facing" || nom == "coa attack" || nom == "coa aoe"))
            return 0.0f;

        if (CoaAreneEstRangerDistance(bot))
        {
            // La fuite amont (« enemy too close for spell » -> « flee », 5 yd a reculons, RangedCombatStrategy)
            // est remplacee par le recul de L1 : les deux se disputeraient le bot.
            if (nom == "flee" && CoaAreneL1Actif(etat))
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
        // Survie d'arene (11/10) : remplace par les trois paliers « coa arene survie » en arene 1v1 et en duel. Le
        // defensif reste pour les arenes a plusieurs des bots (hors CoaAreneUnContreUn), ou la survie ne joue pas.
        if (!sPlayerbotAIConfig.coaAreneIaCommune || !bot->IsAlive() || (CoaAreneSurvieArmee() && CoaAreneUnContreUn(bot)))
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

// ---------------------------------------------------------------------------------------------------------------
// Interruption (11/10, combat contre un humain). Cause du 10/10 (id=3, Onyx soigne de 42 a 100 % en 4 s) : le Ranger
// 2617 n'avait AUCUN sort que Classify marque KIND_INTERRUPT -- la silence de Toxic Dart est posee par script, et Toxic
// Dart comme Bushwhack (etourdissement) exigent la furtivite (Stances = 1 << 29, refus ONLY_SHAPESHIFT a 20:00:52) --
// donc « coa enemy casting » ne se levait jamais ; et un etourdissement n'etait jamais propose pour couper un soin.
// Ici : interruption PURE (INTERRUPT_CAST compatible, silence) sur une incantation qui le merite
// (CoaAreneIncantationDigne) ; contre un SOIN (incantation ou canalisation), tout sort qui coupe (etourdissement, peur,
// desorientation) en repli, apres les pures (CastFirst prend la premiere qui part : une pure en recharge passe la main).
// « coa interrupt » (40) est coupee en 1v1 par le multiplicateur, bras tire : elle frappait toute incantation
// interruptible, aussi courte soit-elle, et sans lire PreventionType.
// ---------------------------------------------------------------------------------------------------------------
struct CoaAreneCoupeCandidats
{
    Unit* adv = nullptr;
    Spell* incantation = nullptr;
    bool canalisee = false;
    bool soin = false;
    uint32 pures = 0;
    std::vector<Usable> sorts;
};

// Une echeance getMSTime() est passee (0 : aucune).
bool CoaAreneEchu(uint32 echeance)
{
    return !echeance || getMSTimeDiff(echeance, getMSTime()) < 0x80000000u;
}

// Pret comme dans CastFirst, hors portee et vue (le pre-controle de CastFirst en juge) : recharge, banc, ressource,
// forme, arme, aura de lanceur exigee, familier, refus certains lus sur la cible.
bool CoaAreneSortPretContre(PlayerbotAI* botAI, Player* bot, Usable const& spell, Unit* cible)
{
    SpellInfo const* info = EffectiveSpell(bot, spell.info);
    if (CoaRefuseParAuraDuLanceur(bot, info))
        return false;
    return CoaAreneSortPret(botAI, bot, spell) && !CoaExigeFamilierAbsent(bot, info) &&
           !CoaRefusCibleCertain(bot, info, cible);
}

bool CoaAreneCandidatsInterruption(PlayerbotAI* botAI, Player* bot, CoaAreneCoupeCandidats& c)
{
    if (!sPlayerbotAIConfig.coaAreneIaCommune || !bot->IsAlive() || !CoaAreneUnContreUn(bot))
        return false;
    CoaAreneEtat& etat = CoaAreneMaj(botAI, bot);
    if (!CoaAreneL1Actif(etat) || (etat.cadre != COA_ARENE_ARENE && etat.cadre != COA_ARENE_DUEL) ||
        !CoaAreneEchu(etat.prochaineInterruptionMs))
        return false;
    c.adv = CoaAreneAdversaireVivant(bot, etat);
    if (!c.adv || c.adv->GetMap() != bot->GetMap())
        return false;
    c.incantation = CoaAreneIncantationAdverse(c.adv, c.canalisee);
    if (!c.incantation)
        return false;
    SpellInfo const* vise = c.incantation->GetSpellInfo();
    c.soin = CoaAreneEstSoin(vise);
    if (!CoaAreneIncantationDigne(c.incantation, c.canalisee, c.soin))
        return false;
    int32 const reste = c.incantation->GetCastTimeRemaining();
    if (reste <= 0)
        return false;

    std::vector<Usable> controles;
    for (Usable const& spell : KnownAbilities(botAI, bot,
             [](uint16 kind) { return (kind & (KIND_INTERRUPT | KIND_CONTROL | KIND_HOSTILE)) != 0; }))
    {
        SpellInfo const* info = EffectiveSpell(bot, spell.info);
        // Un sort a incantation qui finirait apres celle de l'adversaire ne coupe rien (300 ms de marge).
        uint32 const duree = info->CalcCastTime(bot);
        if (duree && int32(duree) + 300 >= reste)
            continue;
        if (CoaAreneCiblePaisibleExigee(info, c.adv) || !CoaAreneSortPretContre(botAI, bot, spell, c.adv))
            continue;
        uint8 const coupe = CoaAreneCoupe(bot, info, vise, c.canalisee, c.adv);
        if (coupe == COA_COUPE_PURE)
            c.sorts.push_back(spell);
        else if (coupe == COA_COUPE_CONTROLE && c.soin)
            controles.push_back(spell);
    }
    c.pures = uint32(c.sorts.size());
    c.sorts.insert(c.sorts.end(), controles.begin(), controles.end());
    return !c.sorts.empty();
}

class CoaAreneInterruptionTrigger : public Trigger
{
public:
    CoaAreneInterruptionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene interruption") {}

    bool IsActive() override
    {
        CoaAreneCoupeCandidats c;
        return CoaAreneCandidatsInterruption(botAI, bot, c);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Dissipation sur soi (11/10, « ses DoT tuent le bot sans que celui-ci reagisse »). « coa dispel » vise bien le bot
// (NearbyGroup le met en tete, CoaDispelTrigger accepte member == bot pour toute dissipation) mais a la priorite 15
// (DPS ; 26 soigneur) : sous « reach spell » (20), le retour (37,5), le recul (38), la cachette (39) -- en 1v1 elle ne
// passait presque jamais. Ici, a 39,3 : des que les DoT que le kit sait dissiper sur le bot font 2 auras ou plus, ou
// 15 % de sa vie prevus sur 6 s. Une tentative au plus toutes les 3 s apres un succes, 1 s apres un echec (pas de
// boucle : un DoT reapplique aussitot ne fait pas tourner le bot sur sa dissipation).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32 CoaAreneDotsSeuil = 2;
constexpr float CoaAreneDotsPvPct = 15.0f;
constexpr int32 CoaAreneDotsHorizonMs = 6 * IN_MILLISECONDS;
constexpr uint32 CoaAreneDispelSoiApresSuccesMs = 3 * IN_MILLISECONDS;
constexpr uint32 CoaAreneDispelSoiApresEchecMs = IN_MILLISECONDS;

// DoT nuisibles sur le bot que `masque` dissipe : nombre d'auras, degats prevus sur 6 s (montant courant par tick,
// ticks entiers dans l'horizon ou la duree restante ; pourcentage de vie pour PERIODIC_DAMAGE_PERCENT).
float CoaAreneDotsPrevus(Player* bot, uint32 masque, uint32& nombre, bool tous)
{
    nombre = 0;
    float total = 0.0f;
    for (auto const& [id, application] : bot->GetAppliedAuras())
    {
        if (application->IsPositive())
            continue;
        Aura const* aura = application->GetBase();
        SpellInfo const* auraInfo = aura->GetSpellInfo();
        if (!tous && (!auraInfo->Dispel || !(masque & (1u << auraInfo->Dispel))))
            continue;
        int32 const duree = aura->GetDuration();
        int32 const horizon = duree > 0 ? std::min(duree, CoaAreneDotsHorizonMs) : CoaAreneDotsHorizonMs;
        bool dot = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            AuraEffect const* effet = aura->GetEffect(i);
            if (!effet)
                continue;
            AuraType const type = effet->GetAuraType();
            if (type != SPELL_AURA_PERIODIC_DAMAGE && type != SPELL_AURA_PERIODIC_LEECH &&
                type != SPELL_AURA_PERIODIC_DAMAGE_PERCENT)
                continue;
            dot = true;
            int32 const periode = effet->GetAmplitude();
            if (periode <= 0)
                continue;
            float const parTick = type == SPELL_AURA_PERIODIC_DAMAGE_PERCENT
                                      ? float(bot->GetMaxHealth()) * float(effet->GetAmount()) / 100.0f
                                      : float(effet->GetAmount());
            total += std::max(0.0f, parTick) * float(horizon / periode);
        }
        nombre += dot ? 1 : 0;
    }
    return total;
}

struct CoaAreneDispelCandidat
{
    Usable sort;
    uint32 dots;
    float prevu;
};

// ---------------------------------------------------------------------------------------------------------------
// Dissipation RENTABLE (11/10, IaSurvie). Match id=93, Sun Cleric 478 contre Onyx : Sanctify 524968 (12 % de mana de
// base, lu dans Spell.dbc) lance 14 fois en une minute, dont plusieurs a 100 % de vie, par « coa arene dispel soi » ET par
// « coa dispel » generique ; Onyx reposait ses DoT aussitot, et la Sun Cleric est restee 18 s sans un sort a la fin
// (refus faute de mana, muets avant ce lot). Sous IaSurvie, en arene 1v1 et en duel : « coa dispel » est coupee
// (multiplicateur), et la dissipation sur soi exige en plus
//   - vie < 90 % ;
//   - mana >= 30 % si le sort coute de la mana ;
//   - que les DoT qu'elle retirerait valent plus que son cout : degats restants prevus des auras retirees (les `n` plus
//     lourdes par effet DISPEL, n = valeur de l'effet) contre le cout du sort converti en vie par le rendement du meilleur
//     soin sur soi connu (vie par point de mana, lu dans les effets). Sans soin paye en mana : 1 % de la mana maximale
//     vaut 1 % de la vie maximale (heuristique, dite comme telle). Un sort qui ne coute pas de mana est toujours
//     rentable s'il retire un DoT ;
//   - pas d'attente de reapplication en cours : apres un succes, 3 s ; si l'un des DoT retires revient dans les 4 s,
//     l'attente double (6, 12, plafond 15 s), et elle repart de 3 s quand un DoT retire ne revient pas.
// Refus comptes au bilan par raison (dispel_refus_*), au plus une fois par raison et par 2 s.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint8 COA_DISPEL_REFUS_PV = 1;
constexpr uint8 COA_DISPEL_REFUS_MANA = 2;
constexpr uint8 COA_DISPEL_REFUS_RENTABLE = 3;
constexpr uint8 COA_DISPEL_REFUS_REAPPLIQUE = 4;

void CoaAreneNoterRefusDispel(CoaAreneEtat& etat, uint8 raison)
{
    uint32 const maintenant = getMSTime();
    if (etat.dispelRefusMs[raison] && getMSTimeDiff(etat.dispelRefusMs[raison], maintenant) < 2 * IN_MILLISECONDS)
        return;
    etat.dispelRefusMs[raison] = maintenant ? maintenant : 1;
    ++etat.dispelRefus[raison];
}

// Vie rendue par un sort de soin, lue dans ses effets (montants de base, sans bonus de puissance des sorts : une
// estimation BASSE pour les soins qui montent avec la mana ou la puissance, comme Hand of Elune 801990). Un niveau de
// sort declenche, par TRIGGER_SPELL ou par aura PERIODIC_TRIGGER_SPELL (alors multiplie par le nombre de ticks).
float CoaAreneSoinEstime(Player* bot, SpellInfo const* info, uint8 profondeur = 0)
{
    float total = 0.0f;
    float const vieMax = float(bot->GetMaxHealth());
    int32 const duree = info->GetMaxDuration();
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (!effect.IsEffect())
            continue;
        float const valeur = float(std::max(0, effect.CalcValue(bot)));
        int32 const ticks = (duree > 0 && effect.Amplitude > 0) ? duree / effect.Amplitude : 1;
        switch (effect.Effect)
        {
            case SPELL_EFFECT_HEAL: total += valeur; break;
            case SPELL_EFFECT_HEAL_PCT: total += vieMax * valeur / 100.0f; break;
            case SPELL_EFFECT_HEAL_MAX_HEALTH: total += vieMax; break;
            case SPELL_EFFECT_TRIGGER_SPELL:
                if (!profondeur)
                    if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                        total += CoaAreneSoinEstime(bot, declenche, 1);
                break;
            default:
                if (!IsAuraEffect(effect))
                    break;
                if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL)
                    total += valeur * float(ticks);
                else if (effect.ApplyAuraName == SPELL_AURA_OBS_MOD_HEALTH)
                    total += vieMax * valeur / 100.0f * float(ticks);
                else if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL && !profondeur)
                    if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                        total += CoaAreneSoinEstime(bot, declenche, 1) * float(ticks);
                break;
        }
    }
    return total;
}

// Vie par point de ressource `type` du meilleur soin sur soi connu qui coute cette ressource (0 : aucun).
float CoaAreneRendementSoin(PlayerbotAI* botAI, Player* bot, uint32 type)
{
    float meilleur = 0.0f;
    for (Usable const& spell : KnownAbilities(botAI, bot, [](uint16 kind)
             { return (kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE)); }))
    {
        SpellInfo const* info = EffectiveSpell(bot, spell.info);
        if (info->PowerType != type)
            continue;
        int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
        if (cout <= 0)
            continue;
        meilleur = std::max(meilleur, CoaAreneSoinEstime(bot, info) / float(cout));
    }
    return meilleur;
}

// Degats restants prevus (toute la duree restante, 6 s pour une aura sans duree) d'une aura nuisible : la valeur de la
// retirer. 0 si ce n'est pas un DoT.
float CoaAreneValeurDot(Player* bot, Aura const* aura)
{
    int32 const duree = aura->GetDuration();
    int32 const horizon = duree > 0 ? duree : CoaAreneDotsHorizonMs;
    float total = 0.0f;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        AuraEffect const* effet = aura->GetEffect(i);
        if (!effet)
            continue;
        AuraType const type = effet->GetAuraType();
        if (type != SPELL_AURA_PERIODIC_DAMAGE && type != SPELL_AURA_PERIODIC_LEECH &&
            type != SPELL_AURA_PERIODIC_DAMAGE_PERCENT)
            continue;
        int32 const periode = effet->GetAmplitude();
        if (periode <= 0)
            continue;
        float const parTick = type == SPELL_AURA_PERIODIC_DAMAGE_PERCENT
                                  ? float(bot->GetMaxHealth()) * float(effet->GetAmount()) / 100.0f
                                  : float(effet->GetAmount());
        total += std::max(0.0f, parTick) * float(horizon / periode);
    }
    return total;
}

// Ce que `info` retirerait du bot : par effet DISPEL, les `n` DoT les plus lourds du type vise (n = valeur de l'effet,
// 1 au moins). `retires` : les ids des auras comptees.
float CoaAreneValeurDispel(Player* bot, SpellInfo const* info, std::vector<uint32>* retires = nullptr)
{
    std::vector<std::pair<float, Aura const*>> dots;
    for (auto const& [id, application] : bot->GetAppliedAuras())
    {
        if (application->IsPositive())
            continue;
        Aura const* aura = application->GetBase();
        if (!aura->GetSpellInfo()->Dispel)
            continue;
        float const valeur = CoaAreneValeurDot(bot, aura);
        if (valeur > 0.0f && std::none_of(dots.begin(), dots.end(), [aura](auto const& d) { return d.second == aura; }))
            dots.emplace_back(valeur, aura);
    }
    std::stable_sort(dots.begin(), dots.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    std::vector<bool> pris(dots.size(), false);
    float total = 0.0f;
    for (SpellEffectInfo const& effect : info->Effects)
    {
        if (effect.Effect != SPELL_EFFECT_DISPEL)
            continue;
        uint32 const masque = SpellInfo::GetDispelMask(DispelType(effect.MiscValue));
        int32 reste = std::max(1, effect.CalcValue(bot));
        for (size_t k = 0; k < dots.size() && reste > 0; ++k)
        {
            if (pris[k] || !(masque & (1u << dots[k].second->GetSpellInfo()->Dispel)))
                continue;
            pris[k] = true;
            --reste;
            total += dots[k].first;
            if (retires)
                retires->push_back(dots[k].second->GetId());
        }
    }
    return total;
}

// Le cout du sort, en vie (voir l'en-tete du bloc). 0 s'il ne coute rien, ou s'il ne coute pas de MANA : energie, rage
// et ressources de classe reviennent en quelques secondes, et la regle « 1 % de la ressource = 1 % de la vie » ferait
// payer 30 % de vie a une dissipation de 30 d'energie -- seules la recharge globale et l'attente de reapplication la
// bornent alors.
float CoaAreneCoutEnVie(PlayerbotAI* botAI, Player* bot, SpellInfo const* info)
{
    if (info->PowerType != POWER_MANA)
        return 0.0f;
    int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
    if (cout <= 0)
        return 0.0f;
    if (float const rendement = CoaAreneRendementSoin(botAI, bot, info->PowerType); rendement > 0.0f)
        return float(cout) * rendement;
    uint32 const maxRessource = bot->GetMaxPower(Powers(info->PowerType));
    return maxRessource ? float(cout) / float(maxRessource) * float(bot->GetMaxHealth()) : 0.0f;
}

// Reapplication : un DoT retire par la derniere dissipation revient dans les 4 s -> attente doublee (plafond 15 s).
// Aucun retour dans les 4 s -> l'attente repart de 3 s. Appele a chaque evaluation (declencheur), avant l'attente.
void CoaAreneSuivreReapplication(Player* bot, CoaAreneEtat& etat)
{
    if (!etat.dernierDispelMs)
        return;
    uint32 const depuis = getMSTimeDiff(etat.dernierDispelMs, getMSTime());
    if (depuis > CoaAreneDispelReapplicationMs)
    {
        if (!etat.dispelReapplicationVue)
            etat.dispelAttenteMs = CoaAreneDispelAttenteBaseMs;
        etat.dernierDispelMs = 0;
        etat.dispelRetires.clear();
        return;
    }
    if (etat.dispelReapplicationVue)
        return;
    for (uint32 const id : etat.dispelRetires)
    {
        if (!bot->HasAura(id))
            continue;
        etat.dispelReapplicationVue = true;
        uint32 const avant = etat.dispelAttenteMs ? etat.dispelAttenteMs : CoaAreneDispelAttenteBaseMs;
        etat.dispelAttenteMs = std::min(avant * 2, CoaAreneDispelAttenteMaxMs);
        uint32 const prochain = etat.dernierDispelMs + etat.dispelAttenteMs;
        etat.prochainDispelSoiMs = prochain ? prochain : 1;
        CoaAreneJournal(bot, etat, "dispel_reapplication", -1.0f, -1.0f,
                        Acore::StringFormat(" aura={} apres_ms={} attente_ms={}", id, depuis, etat.dispelAttenteMs));
        return;
    }
}

// Les dissipations du bot qui retireraient assez de DoT pour franchir le seuil, la plus utile d'abord. Vide sinon.
std::vector<CoaAreneDispelCandidat> CoaAreneCandidatsDispelSoi(PlayerbotAI* botAI, Player* bot)
{
    std::vector<CoaAreneDispelCandidat> candidats;
    if (!sPlayerbotAIConfig.coaAreneIaCommune || !bot->IsAlive() || !CoaAreneUnContreUn(bot) ||
        !ClassHas(bot, KIND_DISPEL))
        return candidats;
    CoaAreneEtat& etat = CoaAreneMaj(botAI, bot);
    if (!CoaAreneL1Actif(etat) || (etat.cadre != COA_ARENE_ARENE && etat.cadre != COA_ARENE_DUEL))
        return candidats;
    bool const survie = CoaAreneSurvieArmee();
    if (survie)
        CoaAreneSuivreReapplication(bot, etat);
    if (!CoaAreneEchu(etat.prochainDispelSoiMs))
    {
        // Une attente de reapplication (doublee) en cours qui retient un DoT dissipable : refus compte.
        if (survie && etat.dispelReapplicationVue && etat.dernierDispelMs)
            CoaAreneNoterRefusDispel(etat, COA_DISPEL_REFUS_REAPPLIQUE);
        return candidats;
    }
    float const seuilPv = float(bot->GetMaxHealth()) * CoaAreneDotsPvPct / 100.0f;
    for (Usable const& spell : KnownAbilities(botAI, bot, IsFriendlyDispel))
    {
        if (!spell.dispelMask || !HasDispellable(bot, spell.dispelMask))
            continue;
        uint32 dots = 0;
        float const prevu = CoaAreneDotsPrevus(bot, spell.dispelMask, dots);
        if (!dots || (dots < CoaAreneDotsSeuil && prevu < seuilPv))
            continue;
        if (survie)
        {
            SpellInfo const* info = EffectiveSpell(bot, spell.info);
            if (bot->GetHealthPct() >= CoaAreneDispelPvMax)
            {
                CoaAreneNoterRefusDispel(etat, COA_DISPEL_REFUS_PV);
                continue;
            }
            if (info->PowerType == POWER_MANA && info->CalcPowerCost(bot, info->GetSchoolMask()) > 0 &&
                bot->GetMaxPower(POWER_MANA) && bot->GetPowerPct(POWER_MANA) < CoaAreneDispelManaMin)
            {
                CoaAreneNoterRefusDispel(etat, COA_DISPEL_REFUS_MANA);
                continue;
            }
            if (CoaAreneValeurDispel(bot, info) <= CoaAreneCoutEnVie(botAI, bot, info))
            {
                CoaAreneNoterRefusDispel(etat, COA_DISPEL_REFUS_RENTABLE);
                continue;
            }
        }
        if (!CoaAreneSortPretContre(botAI, bot, spell, bot))
            continue;
        candidats.push_back({ spell, dots, prevu });
    }
    std::stable_sort(candidats.begin(), candidats.end(), [](CoaAreneDispelCandidat const& a, CoaAreneDispelCandidat const& b)
                     { return a.dots != b.dots ? a.dots > b.dots : a.prevu > b.prevu; });
    return candidats;
}

class CoaAreneDispelSoiTrigger : public Trigger
{
public:
    CoaAreneDispelSoiTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene dispel soi") {}

    bool IsActive() override { return !CoaAreneCandidatsDispelSoi(botAI, bot).empty(); }
};

// ---------------------------------------------------------------------------------------------------------------
// Survie d'arene (11/10/2026, AiPlayerbot.CoaArene.IaSurvie, arene 1v1 et duel, bras tire). Retour du commanditaire apres
// les matchs id=89 a 93 contre Onyx (Knight of Xoroth 30, DoT magiques et diablotins) : « ils n'utilisent aucun sort de
// protection, aucun sort de recuperation de points de vie, rien [...] ils ne se soignent pas, ne se protegent pas ».
// Lu dans le code (analyse du 11/10) : les soins sur soi ne passaient que par « coa defensive » (28, sous 45 %), SOUS tous
// les deplacements d'arene (37,5 a 39,2) -- au match 89, le retour partait a chaque tick entre 32 et 12 % et Hand of
// Elune 801990, instantanee et utilisable en courant, n'est jamais partie ; le defensif d'arene (39,5) ne s'armait que sous
// 50 % ou apres 20 points perdus, et ne prenait que KIND_DEFENSIVE.
//
// Trois paliers, calcules par CoaAreneMaj (CoaAreneEtat::survieNiveau), un declencheur et une action par palier :
//   pression (39,4) : perte de 15 points en 2-4 s, DoT prevus >= 25 % de la vie sur 6 s, ou vie < 70 % -- soin sur soi
//                     (periodique a toute vie, direct sous 80 %, incante seulement hors de vue), puis un defensif a
//                     recharge courte (60 s au plus), le moins cher d'abord ;
//   danger (39,6)   : vie < 50 % (ou 2X, X le pire coup des 15 dernieres s), ou vie projetee a 6 s (vie moins DoT
//                     prevus >= 25 %) sous ce seuil -- soin sur soi utilisable en mouvement
//                     (instantane, ou sans interruption par le mouvement), soin INCANTE seulement hors de vue de l'adversaire
//                     ou sous un defensif du bot, jamais pendant un deplacement voulu de l'IA ; PUIS defensif majeur
//                     (recharge la plus longue d'abord ; recharge courte seulement tant que la vie reste >= 50 %) ;
//   (11/10, match id=6) un sort qui met le lanceur hors d'etat d'agir (stase Time Out!) n'est jamais un defensif de ces
//   paliers : dernier recours au palier critique, ou outil de mana (pression, mana < 25 %, hors de vue) ;
//   critique (41)   : vie < 30 % -- tout ce qui precede, au-dessus des interruptions (40,5) ; si seul un soin incante
//                     reste et que l'adversaire voit le bot, fuite hors de vue (arene : cachette de L2, 6 s au plus, 8 s
//                     de pause, 20 s par engagement), puis le soin ; sans cachette, le soin incante quand meme.
// Danger et pression passent devant tous les deplacements d'arene, sous les interruptions. Une action qui ne lance rien
// rend faux : le moteur passe a la suivante (Engine::DoNextAction), le retour et la rotation ne sont pas bloques.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint8 COA_SURVIE_PRESSION = 1;
constexpr uint8 COA_SURVIE_DANGER = 2;
constexpr uint8 COA_SURVIE_CRITIQUE = 3;

char const* CoaAreneSurvieNom(uint8 niveau)
{
    switch (niveau)
    {
        case COA_SURVIE_PRESSION: return "pression";
        case COA_SURVIE_DANGER:   return "danger";
        case COA_SURVIE_CRITIQUE: return "critique";
        default:                  return "aucun";
    }
}

// Utilisable en courant : RECOPIE de la condition de Spell::prepare (voir CoaRefuseEnMouvement), sans le test « bouge »
// ni la grace de Manastorm (on juge le sort, pas l'instant).
bool CoaAreneSortMobile(Player* bot, SpellInfo const* info)
{
    if (!(info->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT) || bot->CanCastSpellWhileMoving(info))
        return true;
    if (info->CalcCastTime(bot))
        return false;
    return !info->IsChanneled() || info->IsActionAllowedChannel();
}

// Sort qui met son LANCEUR hors d'etat d'agir (11/10, match humain id=6, 13:54 Paris, Chronomancer 494) : Time Out! 803896
// declenche (effet 142, TRIGGER_SPELL_WITH_VALUE, sur le lanceur) l'aura 802228 -- MOD_DAMAGE_PERCENT_TAKEN (87) et
// MOD_STUN (12) sur soi, 6 s, « Unable to act ». Classify voit l'aura 87 du declenche et le classe KIND_DEFENSIVE ; sa
// regle « selfIncapacitating » ne regarde que le sort lui-meme, pas ce qu'il declenche. Choisi comme « defensif majeur »
// au palier danger (la recharge la plus longue d'abord), le bot est reste etourdi de 11:54:51 a 11:54:55, mana 74 -> 100,
// sans aucun soin, l'adversaire hors de vue ; premier soin a 9 % de vie. Lu sur le sort et ses declenches (deux
// niveaux) : une aura d'incapacite posee sur le lanceur (etourdissement, pacification, silence, peur, confusion, ou 263
// qui n'autorise plus que le sort lui-meme). Le classement general n'est pas touche (monde ouvert inchange) : seule la
// survie d'arene l'ecarte, sauf en dernier recours (critique, rien d'autre) ou comme outil de mana.
// Revue survie v2 : un sort qui rend son lanceur INSENSIBLE (immunite aux degats ou a des ecoles posee sur lui, au sort
// lui-meme) n'est pas une entrave mais un defensif majeur, comme pour la regle du verrou (CoaAreneRayonVerrou) : Liquify
// 806310 (263 + immunite 40, 55 personnages le connaissent), Burrow 806154 (etourdissement + immunite 39, 36). Mesure sur
// Spell.dbc du 11/10 : sans cette exception, ils ne partaient plus qu'en dernier recours, apres le controle de secours.
bool CoaAreneImmuniseLanceur(SpellInfo const* info)
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effet = info->Effects[i];
        bool const surSoi = !effet.TargetA.GetTarget() || effet.TargetA.GetTarget() == TARGET_UNIT_CASTER;
        if (surSoi && IsAuraEffect(effet) &&
            (effet.ApplyAuraName == SPELL_AURA_DAMAGE_IMMUNITY || effet.ApplyAuraName == SPELL_AURA_SCHOOL_IMMUNITY))
            return true;
    }
    return false;
}

bool CoaAreneSortEntrave(SpellInfo const* info, uint8 profondeur = 0)
{
    if (!profondeur && CoaAreneImmuniseLanceur(info))
        return false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effet = info->Effects[i];
        if (!effet.IsEffect())
            continue;
        // Un sort declenche n'est lu que pour ses effets EXPLICITEMENT sur le lanceur (une cible implicite nulle y designe
        // la cible du declenchement, pas forcement le bot).
        bool const surSoi = effet.TargetA.GetTarget() == TARGET_UNIT_CASTER || (!profondeur && !effet.TargetA.GetTarget());
        if (surSoi && IsAuraEffect(effet))
            switch (effet.ApplyAuraName)
            {
                case SPELL_AURA_MOD_STUN: case SPELL_AURA_MOD_PACIFY: case SPELL_AURA_MOD_SILENCE:
                case SPELL_AURA_MOD_PACIFY_SILENCE: case SPELL_AURA_MOD_FEAR: case SPELL_AURA_MOD_CONFUSE:
                case SPELL_AURA_ALLOW_ONLY_ABILITY:
                    return true;
                default:
                    break;
            }
        // Sort declenche par le lanceur lui-meme : effet direct, ou aura periodique posee sur soi.
        bool const declenche = effet.Effect == SPELL_EFFECT_TRIGGER_SPELL ||
                               effet.Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE ||
                               (surSoi && (effet.IsAura(SPELL_AURA_PERIODIC_TRIGGER_SPELL) ||
                                           effet.IsAura(SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE)));
        if (declenche && effet.TriggerSpell && effet.TriggerSpell != info->Id && profondeur < 2)
            if (SpellInfo const* sort = sSpellMgr->GetSpellInfo(effet.TriggerSpell))
                if (CoaAreneSortEntrave(sort, profondeur + 1))
                    return true;
    }
    return false;
}

// Le sort change la FORME du lanceur (aura 36 MOD_SHAPESHIFT posee sur soi). Match humain id=29 (11/10, 13:13:52 UTC,
// Bloodmage 1248 spe 27) : la survie (palier pression) a pris Blood Curse 562720 -> Accursed Form 562572 (25 % de la vie,
// aura 36 forme 57) pour un defensif ; la forme pose 524861 (SyncCursedFormRequirement, AscensionBloodmageTalents.cpp:105),
// que Bloodmoon Blast, Sanguine Rupture et Taldaram's Torment excluent (ExcludeCasterAuraSpell) : il ne restait que les
// sorts maudits payes en rage (Bloodbolt, Veinburst : 300), a 12-27 de rage -- 8 s sans un sort, de 66 a 4 % de vie.
// Une forme change le kit entier : ce n'est ni un soin ni un defensif de survie (la rotation garde la main dessus).
bool CoaAreneSortForme(SpellInfo const* info)
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effet = info->Effects[i];
        if ((!effet.TargetA.GetTarget() || effet.TargetA.GetTarget() == TARGET_UNIT_CASTER) &&
            effet.IsAura(SPELL_AURA_MOD_SHAPESHIFT))
            return true;
    }
    return false;
}

// Liste et sort effectif : l'un ou l'autre change la forme (Blood Curse 562720 est remplace par Accursed Form 562572).
bool CoaAreneSortFormeUsable(Player* bot, Usable const& spell)
{
    return CoaAreneSortForme(spell.info) || CoaAreneSortForme(EffectiveSpell(bot, spell.info));
}

// Le sort rend de la mana au lanceur (Time Out! : ENERGIZE_PCT 137 et aura PERIODIC_ENERGIZE 24, MiscValue 0 = mana).
bool CoaAreneRendMana(SpellInfo const* info)
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effet = info->Effects[i];
        if (effet.MiscValue != POWER_MANA)
            continue;
        if (effet.Effect == SPELL_EFFECT_ENERGIZE || effet.Effect == SPELL_EFFECT_ENERGIZE_PCT ||
            effet.IsAura(SPELL_AURA_PERIODIC_ENERGIZE))
            return true;
    }
    return false;
}

// Recharge propre du sort (la plus longue de RecoveryTime et CategoryRecoveryTime).
uint32 CoaAreneRechargeMs(SpellInfo const* info)
{
    return std::max(info->RecoveryTime, info->CategoryRecoveryTime);
}

// L'aura du sort posee par le bot sur lui-meme (le sort effectif ou celui de la liste).
bool CoaAreneAuraDeSoi(Player* bot, SpellInfo const* liste, SpellInfo const* effectif)
{
    ObjectGuid const moi = bot->GetGUID();
    return bot->HasAura(effectif->Id, moi) || bot->HasAura(liste->Id, moi);
}

// Un defensif du bot le couvre en ce moment.
bool CoaAreneSousDefensif(PlayerbotAI* botAI, Player* bot)
{
    for (Usable const& spell : KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_DEFENSIVE) != 0; }))
        if (CoaAreneAuraDeSoi(bot, spell.info, EffectiveSpell(bot, spell.info)))
            return true;
    return false;
}

// Lot 20 (13/10) : soin DIRECT estime du sort (a la difference de CoaAreneSoinEstime, qui compte aussi les soins
// periodiques sur toute leur duree : Vital Shield y vaudrait 360 ticks de 2 %), sans bonus de puissance des sorts (HEAL : valeur de base au niveau du
// bot ; HEAL_PCT : part de la vie maximale du bot ; HEAL_MAX_HEALTH : vie maximale), lui-meme et ses sorts declenches
// sans aura (deux niveaux). 0 pour un soin a aura (periodique, differe) ou un soin fait par script (valeur 1 au DBC) :
// sert a ORDONNER les soins directs de la survie, jamais a en ecarter un.
float CoaAreneSoinDirectEstime(Player* bot, SpellInfo const* info, uint8 profondeur = 0)
{
    float total = 0.0f;
    for (SpellEffectInfo const& effet : info->Effects)
    {
        if (!effet.IsEffect())
            continue;
        if (effet.Effect == SPELL_EFFECT_HEAL)
            total += std::max(0, effet.CalcValue(bot));
        else if (effet.Effect == SPELL_EFFECT_HEAL_PCT)
            total += std::max(0, effet.CalcValue(bot)) * float(bot->GetMaxHealth()) / 100.0f;
        else if (effet.Effect == SPELL_EFFECT_HEAL_MAX_HEALTH)
            total += float(bot->GetMaxHealth());
        else if ((effet.Effect == SPELL_EFFECT_TRIGGER_SPELL || effet.Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE ||
                  effet.Effect == SPELL_EFFECT_ASCENSION_TRIGGER_SPELL_DELAYED) &&
                 effet.TriggerSpell && effet.TriggerSpell != info->Id && profondeur < 2)
            if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effet.TriggerSpell))
                total += CoaAreneSoinDirectEstime(bot, declenche, profondeur + 1);
    }
    return total;
}

/*
 * Lot 20 (13/10, banc du 12/10, Cultist niveau 60 : trois Dark Infusion 570249 par match, 30 % de la mana de base chacun,
 * pour 207 de soin au prochain coup recu ; la mana tombait a 0-10 % et Eldritch Mending 574325 puis Abyssal Ward 804670
 * etaient refuses faute de mana pendant que la vie s'effondrait). RESERVE DE MANA DU MEILLEUR OUTIL DE SURVIE, en survie
 * d'arene et de duel :
 *   - outil principal : soin direct sur soi (CoaSoinDirect) ou defensif, paye en mana, hors recharge, ni entrave ni
 *     sort de forme ; la reserve est le cout du PLUS CHER d'entre eux (le meilleur outil, qu'il faut pouvoir payer) ;
 *   - un soin SANS soin direct (aura periodique, differe, conditionnel), paye en mana, n'est pas lance s'il laisserait la
 *     mana sous cette reserve, ou sous CoaAreneSurvieReserveOutilPct de la mana maximale.
 * Sans outil principal paye en mana (classe sans mana, ou dont tous les vrais soins se paient autrement), rien n'est
 * retenu. Rien hors de la survie d'arene et de duel.
 */
constexpr float CoaAreneSurvieReserveOutilPct = 40.0f;

int32 CoaAreneReserveOutilMana(PlayerbotAI* botAI, Player* bot)
{
    if (!bot->GetMaxPower(POWER_MANA))
        return 0;
    int32 reserve = 0;
    for (Usable const& spell : KnownAbilities(botAI, bot, [](uint16 kind)
             { return (kind & KIND_DEFENSIVE) || ((kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE))); }))
    {
        SpellInfo const* info = EffectiveSpell(bot, spell.info);
        if (info->PowerType != POWER_MANA || bot->HasSpellCooldown(info->Id) || CoaAreneSortEntrave(info) ||
            CoaAreneSortFormeUsable(bot, spell))
            continue;
        if (!(spell.kind & KIND_DEFENSIVE) && !CoaSoinDirect(info) && !CoaSoinDirect(spell.info))
            continue;
        reserve = std::max(reserve, info->CalcPowerCost(bot, info->GetSchoolMask()));
    }
    return reserve;
}

// Le soin `spell` (sort effectif `info`) serait retenu par la reserve `reserve` (CoaAreneReserveOutilMana).
bool CoaAreneSousReserveOutil(Player* bot, Usable const& spell, SpellInfo const* info, int32 reserve)
{
    if (reserve <= 0 || !(spell.kind & KIND_HEAL) || (spell.kind & KIND_DEFENSIVE) || info->PowerType != POWER_MANA ||
        CoaSoinDirect(info) || CoaSoinDirect(spell.info))
        return false;
    uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
    int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
    if (!maxMana || cout <= 0)
        return false;
    int32 const apres = int32(bot->GetPower(POWER_MANA)) - cout;
    // Hors de portee de toute facon (mana sous le cout) : CastFirst l'ecarte deja (refus_ressource), rien a retenir.
    if (apres < 0)
        return false;
    return apres < reserve || float(apres) * 100.0f / float(maxMana) < CoaAreneSurvieReserveOutilPct;
}

// Lot 20 : le sort rend au LANCEUR la ressource `pouvoir` -- ENERGIZE ou ENERGIZE_PCT de ce type, ou aura PERIODIC_ENERGIZE,
// visant le lanceur -- lui-meme ou par un sort qu'il declenche sans aura (deux niveaux). Bloodmoon Blast 501611/501614
// (Bloodmage) : effet 142 (TRIGGER_SPELL_WITH_VALUE) -> 804092, ENERGIZE MiscValue 1 (rage) sur le lanceur, 4 a 10 de
// rage. Un generateur fait par script seul (Sanguine Rupture, « Rage for each enemy hit ») n'est pas vu.
bool CoaAreneGenereRessource(SpellInfo const* info, Powers pouvoir, uint8 profondeur = 0)
{
    for (SpellEffectInfo const& effet : info->Effects)
    {
        if (!effet.IsEffect())
            continue;
        uint32 const cible = effet.TargetA.GetTarget();
        bool const surSoi = !cible || cible == TARGET_UNIT_CASTER;
        if (surSoi && effet.MiscValue == int32(pouvoir) &&
            (effet.Effect == SPELL_EFFECT_ENERGIZE || effet.Effect == SPELL_EFFECT_ENERGIZE_PCT ||
             effet.IsAura(SPELL_AURA_PERIODIC_ENERGIZE)))
            return true;
        if ((effet.Effect == SPELL_EFFECT_TRIGGER_SPELL || effet.Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE ||
             effet.Effect == SPELL_EFFECT_ASCENSION_TRIGGER_SPELL_DELAYED) &&
            effet.TriggerSpell && effet.TriggerSpell != info->Id && profondeur < 2)
            if (SpellInfo const* declenche = sSpellMgr->GetSpellInfo(effet.TriggerSpell))
                if (CoaAreneGenereRessource(declenche, pouvoir, profondeur + 1))
                    return true;
    }
    return false;
}

// Les sorts de survie du palier : `defensifs` puis `mobiles` (soins utilisables en courant), et a part `incantes` (soins
// incantes). Defensif deja actif et soin periodique deja pose par le bot retires ; au palier pression, seuls les
// defensifs a recharge courte, le moins cher d'abord.
// Sorts qui soignent ou protegent PAR SERVITEUR (11/10, match humain id=13 : Necromancer 432 a lance Sacrifice Undead a
// 38 % de vie sans aucun serviteur -- soin nul). Aucun critere de donnee commun ne les distingue : Sacrifice Undead 807939
// soigne par 805031 (HEAL sur soi) declenche sur chaque serviteur (TARGET_SRC_CASTER) ; Bone Tithe 802121 n'a qu'un
// SCRIPT_EFFECT ; Command: Undead 504868 deux DUMMY ; Transfer Life 801530 deja retire des soins par Classify (aura exigee
// sur la cible). Le module les refuse a sa facon (AscensionNecromancerAbilities.cpp:146-157 : Minions(player) vide ->
// NO_PET pour 802121 et 801530, CANT_DO_THAT pour les « Command ») ou pas du tout (807939). Liste explicite, raison
// lue dans leur description : « for each Undead minion destroyed », « Siphon life from your undead minions »,
// « Command your Undead minions », « only ever targets Raised minions ».
// I6 du 17/10 (coa-project/docs/DIAG-equilibrage-classes-20261017.md) : la liste nommait deux rangs de Sacrifice Undead
// (805027 rang 1, 807939 rang 3) ; Usable.info porte le rang le plus haut connu, et le rang 5 807941 (38 personnages au
// 17/10) lui echappait -- 14 lancers a vide sur 16 au banc ia-B. Le premier rang de la chaine (spell_ranks : 805027 ->
// 807938, 807939, 807940, 807941) est teste en plus de l'id : tous les rangs sont couverts, et ceux a venir.
bool CoaAreneSortParServiteur(uint32 id)
{
    for (uint32 const candidat : { id, sSpellMgr->GetFirstSpellInChain(id) })
        switch (candidat)
        {
            case 807939: case 805027: case 802121: case 504868: case 801530:
                return true;
            default:
                break;
        }
    return false;
}

// Le bot a au moins une unite controlee vivante issue d'une invocation (familier, serviteurs, gardiens).
bool CoaAreneAServiteur(Player* bot)
{
    for (Unit* u : bot->m_Controlled)
        if (u && u->IsInWorld() && u->IsAlive() && u->GetUInt32Value(UNIT_CREATED_BY_SPELL))
            return true;
    return false;
}

// `entraves` (11/10, match id=6) : defensifs et soins qui mettent le lanceur hors d'etat d'agir (CoaAreneSortEntrave),
// sortis des trois autres listes -- dernier recours au palier critique, ou outil de mana (voir l'action).
struct CoaAreneSurvieListes
{
    std::vector<Usable> defensifs, mobiles, incantes, entraves;
};

CoaAreneSurvieListes CoaAreneSurvieSorts(PlayerbotAI* botAI, Player* bot, uint8 niveau)
{
    CoaAreneSurvieListes l;
    bool const sansServiteur = !CoaAreneAServiteur(bot);
    l.defensifs = KnownAbilities(botAI, bot, [](uint16 kind) { return (kind & KIND_DEFENSIVE) != 0; });
    l.defensifs.erase(std::remove_if(l.defensifs.begin(), l.defensifs.end(), [&](Usable const& spell)
        {
            SpellInfo const* info = EffectiveSpell(bot, spell.info);
            if (CoaAreneAuraDeSoi(bot, spell.info, info) || CoaAreneSortFormeUsable(bot, spell) ||
                (sansServiteur && (CoaAreneSortParServiteur(spell.info->Id) || CoaAreneSortParServiteur(info->Id))))
                return true;
            if (CoaAreneSortEntrave(info))
            {
                l.entraves.push_back(spell);
                return true;
            }
            return niveau == COA_SURVIE_PRESSION && CoaAreneRechargeMs(info) > CoaAreneSurvieRechargeLegereMs;
        }), l.defensifs.end());
    if (niveau == COA_SURVIE_PRESSION)
        CheapestFirst(bot, l.defensifs);
    else
        std::stable_sort(l.defensifs.begin(), l.defensifs.end(), [bot](Usable const& a, Usable const& b)
            { return CoaAreneRechargeMs(EffectiveSpell(bot, a.info)) > CoaAreneRechargeMs(EffectiveSpell(bot, b.info)); });
    // Soins sur soi a TOUS les paliers depuis le 11/10 (match id=6 : le palier pression ne soignait jamais) ; l'action
    // trie ce qu'elle en garde selon le palier.
    for (Usable const& spell : KnownAbilities(botAI, bot, [](uint16 kind)
             { return (kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE)); }))
    {
        SpellInfo const* info = EffectiveSpell(bot, spell.info);
        // Lot 20 : aussi tout soin sans soin direct dont l'aura est deja posee par le bot (CoaSoinDejaPose ; Vital Shield).
        if (((spell.kind & KIND_HOT) && CoaAreneAuraDeSoi(bot, spell.info, info)) || CoaSoinDejaPose(bot, spell, info, bot) ||
            CoaAreneSortFormeUsable(bot, spell))
            continue;
        if (sansServiteur && (CoaAreneSortParServiteur(spell.info->Id) || CoaAreneSortParServiteur(info->Id)))
            continue;
        if (CoaAreneSortEntrave(info))
        {
            // Un sort a la fois defensif et soin n'y entre qu'une fois.
            if (std::none_of(l.entraves.begin(), l.entraves.end(),
                             [&](Usable const& e) { return e.info == spell.info; }))
                l.entraves.push_back(spell);
            continue;
        }
        (CoaAreneSortMobile(bot, info) ? l.mobiles : l.incantes).push_back(spell);
    }
    // Lot 20 (13/10, banc du 12/10 : le Bloodmage lancait Vital Shield, soin de 2 % toutes les 5 s, avant Fleshcraft 801952,
    // 50 % de la vie) : aux paliers danger et critique, le soin DIRECT le plus fort d'abord (CoaAreneSoinDirectEstime), puis les
    // soins a aura (periodiques, differes) dans leur ordre. Au palier pression, l'ordre du kit reste (le soin periodique y
    // est le premier outil).
    if (niveau >= COA_SURVIE_DANGER)
        for (std::vector<Usable>* soins : { &l.mobiles, &l.incantes })
            std::stable_sort(soins->begin(), soins->end(), [bot](Usable const& a, Usable const& b)
                { return CoaAreneSoinDirectEstime(bot, EffectiveSpell(bot, a.info)) > CoaAreneSoinDirectEstime(bot, EffectiveSpell(bot, b.info)); });
    return l;
}

// Un soin sur soi du kit est pret (recharge, ressource, etat : CoaAreneSortPret) ET la survie peut le lancer a ce tick.
// Revue survie v2 : « quelle que soit la vue » figeait le bot -- un soin incante pret mais vu (fuite impossible : duel,
// pause de 8 s, cumul de 20 s, aucune cachette ; ou soin periodique deja pose, que la survie retire) comptait comme pret,
// la survie ne le lancait pas, et toute attaque de 2 s ou plus etait refusee : une classe dont toutes les attaques sont
// longues restait plantee au palier danger (jusqu'a 85 % de vie quand un gros coup a hausse le seuil). Memes retraits que
// CoaAreneSurvieSorts ; un soin incante ne compte que si l'adversaire ne voit pas le bot, ou sous un defensif.
bool CoaAreneSoinPret(PlayerbotAI* botAI, Player* bot)
{
    Unit* adv = CoaAreneAdversaireVivant(bot, CoaAreneEtatDe(botAI));
    int8 incanteLibre = -1;  // lu une fois, et seulement si un soin incante est pret
    int32 reserveOutil = -1; // lot 20 : lue une fois, et seulement si un soin est pret
    for (Usable const& spell : KnownAbilities(botAI, bot, [](uint16 kind)
             { return (kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE)); }))
    {
        SpellInfo const* info = EffectiveSpell(bot, spell.info);
        if (CoaAreneSortEntrave(info) || CoaAreneSortParServiteur(spell.info->Id) || CoaAreneSortParServiteur(info->Id) ||
            CoaAreneSortFormeUsable(bot, spell) ||
            ((spell.kind & KIND_HOT) && CoaAreneAuraDeSoi(bot, spell.info, info)) || CoaSoinDejaPose(bot, spell, info, bot) ||
            !CoaAreneSortPret(botAI, bot, spell))
            continue;
        // Lot 20 : un soin que la reserve de mana du meilleur outil retient n'est pas pret pour la survie (sinon il
        // bloquerait les attaques longues sans jamais partir).
        if (reserveOutil < 0)
            reserveOutil = CoaAreneReserveOutilMana(botAI, bot);
        if (CoaAreneSousReserveOutil(bot, spell, info, reserveOutil))
            continue;
        if (CoaAreneSortMobile(bot, info))
            return true;
        if (incanteLibre < 0)
            incanteLibre = (!adv || !adv->IsWithinLOSInMap(bot) || CoaAreneSousDefensif(botAI, bot)) ? 1 : 0;
        if (incanteLibre)
            return true;
    }
    return false;
}

// Incantation longue d'attaque en survie (11/10, match humain id=17 : deux Geode Barrage de 2,5 s pendant que la vie
// tombait de 94 a 26 %) : au palier danger ou critique, un sort d'attaque de 2 s d'incantation ou plus n'est pas lance
// tant qu'un soin du kit est pret -- la survie ne l'a pas lance parce que l'adversaire voit le bot (soin incante) : il
// faut rompre la vue (fuite) et soigner, pas s'enfermer 2 s dans une attaque. Sans soin pret (ressource a construire),
// l'incantation part : elle peut etre ce qui construit la ressource.
constexpr uint32 CoaAreneSurvieIncantationLongueMs = 2000;
bool CoaAreneSurvieRefuseIncantation(PlayerbotAI* botAI, Player* bot, SpellInfo const* info, uint16 kind)
{
    if (CoaAreneEtatDe(botAI).survieNiveau < COA_SURVIE_DANGER || (kind & KIND_HEAL) ||
        !(kind & (KIND_DAMAGE | KIND_HOSTILE)) || info->CalcCastTime(bot) < int32(CoaAreneSurvieIncantationLongueMs))
        return false;
    return CoaAreneSoinPret(botAI, bot);
}

class CoaAreneSurvieTrigger : public Trigger
{
public:
    CoaAreneSurvieTrigger(PlayerbotAI* botAI, std::string const nom, uint8 niveau) : Trigger(botAI, nom), niveau(niveau) {}

    bool IsActive() override
    {
        if (!CoaAreneSurvieArmee() || !bot->IsAlive())
            return false;
        return CoaAreneMaj(botAI, bot).survieNiveau == niveau;
    }

private:
    uint8 niveau;
};

// Sort au sol de SOUTIEN (11/10, match id=91 : Standard of Might 706805 lance VERS l'adversaire, refuse OUT_OF_RANGE a
// 29 yd puis pose a 8,5 yd de lui a 8 s de la fin). Lu dans les effets du sort lui-meme, sans id : destination explicite
// (TARGET_FLAG_DEST_LOCATION), aucun effet sur un ennemi ni de degats ni de controle, et au moins un effet AU SOL qui
// invoque (etendard, totem, tourelle), pose une aura persistante benefique, ou vise les allies d'une zone. Standard of
// Might : SUMMON en 87 (DEST_DEST) ; son affaiblissement d'armure 562372 part du LANCEUR (TRIGGER_SPELL en 1) -- le
// poser a ses pieds au contact est donc la bonne place. Tranquil Circle 801205 (zone ennemie 16) et Resonance Rune 803679
// (dissipe les ennemis, 16) ne le sont pas : ils restent vises sur l'adversaire.
bool CoaAreneEstSoutienAuSol(SpellInfo const* info)
{
    if (!(info->Targets & TARGET_FLAG_DEST_LOCATION) || !(info->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION))
        return false;
    bool soutien = false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = info->Effects[i];
        if (!effect.IsEffect())
            continue;
        if (IsEnemyTarget(effect.TargetA.GetTarget()) || IsEnemyTarget(effect.TargetB.GetTarget()) || IsDamage(effect) ||
            (IsAuraEffect(effect) && IsControlAura(effect)))
            return false;
        bool const auSol = effect.TargetA.GetReferenceType() == TARGET_REFERENCE_TYPE_DEST ||
                           effect.TargetB.GetReferenceType() == TARGET_REFERENCE_TYPE_DEST;
        if (!auSol)
            continue;
        if (effect.Effect == SPELL_EFFECT_SUMMON ||
            (effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA && info->IsPositiveEffect(i)) ||
            IsAllyAreaTarget(effect.TargetA.GetTarget()) || IsAllyAreaTarget(effect.TargetB.GetTarget()))
            soutien = true;
    }
    return soutien;
}

// Ce que le sort a pose est encore la : objet dynamique du sort, ou creature creee par lui (familier, gardien, ou
// emplacement d'invocation -- totems). G-31 (25/10, coa-project/docs/REVUE-classe-guardian-20261024.md) : l'etendard du
// Gardien n'est dans aucun des deux (TempSummon sans SummonProperties) : la garde ne le voyait jamais, et Rallying et
// Recovery se remplacaient en boucle. Il se lit par CoaEtendardDejaTenu (un etendard a la fois, sauf un etendard qui
// soigne a la place d'un etendard qui ne soigne pas).
bool CoaAreneSoutienActif(Player* bot, SpellInfo const* info)
{
    if (bot->GetDynObject(info->Id) || CoaEtendardDejaTenu(bot, info))
        return true;
    auto creeParLui = [info](Unit const* u)
        { return u && u->IsInWorld() && u->IsAlive() && u->GetUInt32Value(UNIT_CREATED_BY_SPELL) == info->Id; };
    for (Unit* u : bot->m_Controlled)
        if (creeParLui(u))
            return true;
    for (uint8 slot = 0; slot < MAX_SUMMON_SLOT; ++slot)
        if (bot->m_SummonSlot[slot])
            if (Creature const* c = ObjectAccessor::GetCreature(*bot, bot->m_SummonSlot[slot]))
                if (creeParLui(c))
                    return true;
    return false;
}

// Les sorts au sol de soutien du kit, prets, et dont la pose n'est plus la.
std::vector<Usable> CoaAreneSoutiensPrets(PlayerbotAI* botAI, Player* bot)
{
    std::vector<Usable> soutiens = KnownAbilities(botAI, bot, [](uint16 kind) { return kind != 0; });
    soutiens.erase(std::remove_if(soutiens.begin(), soutiens.end(), [&](Usable const& spell)
        {
            SpellInfo const* info = EffectiveSpell(bot, spell.info);
            return !CoaAreneEstSoutienAuSol(info) || CoaAreneSoutienActif(bot, info) || CoaAreneSoutienActif(bot, spell.info) ||
                   !CoaAreneSortPret(botAI, bot, spell);
        }), soutiens.end());
    return soutiens;
}

// Le sort soigne-t-il (effet de soin, soin periodique, ou declenchement periodique d'un soin ; deux niveaux) ?
bool CoaAreneSortSoigne(SpellInfo const* info, uint8 profondeur = 0)
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effet = info->Effects[i];
        if (effet.Effect == SPELL_EFFECT_HEAL || effet.Effect == SPELL_EFFECT_HEAL_PCT ||
            effet.IsAura(SPELL_AURA_PERIODIC_HEAL))
            return true;
        if (effet.TriggerSpell && effet.TriggerSpell != info->Id && profondeur < 2)
            if (SpellInfo const* sort = sSpellMgr->GetSpellInfo(effet.TriggerSpell))
                if (CoaAreneSortSoigne(sort, profondeur + 1))
                    return true;
    }
    return false;
}

// Soutien au sol qui SOIGNE (11/10, match humain id=14 : Standard of Recovery 500260 du Gardien 448, pose a 0 % de vie).
// Le soin d'un etendard du Gardien n'est pas dans le sort : l'invocation (SUMMON 50053) porte une aura posee par le script
// (npc_ascension_guardian_standard) -- 500266, declenchement periodique de 500248 (HEAL) toutes les 3 s. Le lien sort ->
// aura est la table generee du module, GuardianStandards::Contracts ; pour tout autre soutien, le sort lui-meme.
bool CoaAreneSoutienSoigne(SpellInfo const* info)
{
    if (CoaAreneSortSoigne(info))
        return true;
    for (GuardianStandards::Contract const& contrat : GuardianStandards::Contracts)
        if (contrat.spell == info->Id)
        {
            SpellInfo const* aura = sSpellMgr->GetSpellInfo(contrat.field);
            return aura && CoaAreneSortSoigne(aura);
        }
    return false;
}

// Rotation d'attaque et de zone en arene 1v1 ou en duel (IaSurvie) : les sorts au sol de soutien en sont retires --
// « coa arene soutien » les pose sur le bot ; vises sur l'adversaire, ils partaient a sa position (PlayerbotAI::CastSpell :
// targets.SetDst(*target)).
void CoaAreneRetirerSoutiens(PlayerbotAI* botAI, Player* bot, std::vector<Usable>& usable)
{
    if (!CoaAreneSurvieArmee() || usable.empty() || !CoaAreneTactiqueAdversaire(botAI, bot))
        return;
    usable.erase(std::remove_if(usable.begin(), usable.end(), [bot](Usable const& spell)
        { return CoaAreneEstSoutienAuSol(EffectiveSpell(bot, spell.info)); }), usable.end());
}

// Combat engage (arene 1v1 ou duel, bras tire, engagement vu), adversaire vivant a 30 yd au plus et en vue, un soutien
// pret dont la pose n'est plus la ; un essai toutes les 3 s au plus.
class CoaAreneSoutienTrigger : public Trigger
{
public:
    CoaAreneSoutienTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene soutien") {}

    bool IsActive() override
    {
        if (!CoaAreneSurvieArmee() || !bot->IsAlive() || !bot->IsInCombat() || !CoaAreneUnContreUn(bot))
            return false;
        CoaAreneEtat& etat = CoaAreneMaj(botAI, bot);
        if (!CoaAreneL1Actif(etat) || (etat.cadre != COA_ARENE_ARENE && etat.cadre != COA_ARENE_DUEL) ||
            !CoaAreneEchu(etat.prochainSoutienMs) || etat.survieFuiteDebutMs)
            return false;
        // 11/10 : en survie, l'etendard est l'affaire de « coa arene survie » (etendard de SOIN seulement). Sinon les deux
        // se relaieraient : un seul etendard du Gardien a la fois, recharge commune de 5 s (categorie 1254) -- Might
        // remplacerait Recovery, que la survie reposerait, et ainsi de suite.
        if (etat.survieNiveau >= COA_SURVIE_PRESSION)
            return false;
        Unit* adv = CoaAreneAdversaireVivant(bot, etat);
        if (!adv || bot->GetExactDist(adv) > CoaAreneSoutienDistanceMax || !bot->IsWithinLOSInMap(adv))
            return false;
        if (!CoaAreneSoutiensPrets(botAI, bot).empty())
            return true;
        // Relecture : rien a poser (kit sans soutien, ou pose encore la, ou en recharge) -- le kit entier n'est relu que
        // toutes les 3 s, pas a chaque tick.
        uint32 const prochain = getMSTime() + CoaAreneSoutienReessaiMs;
        etat.prochainSoutienMs = prochain ? prochain : 1;
        return false;
    }
};

// Reserve de mana (11/10, IaSurvie, match id=93) : en arene 1v1 ou en duel, sous 25 % de mana, la rotation d'attaque
// prend le moins cher d'abord (CheapestFirst) et ne lance aucun sort de mana qui laisserait le bot sous le cout du moins
// cher de ses sorts de survie (defensif, ou soin sur soi) payes en mana. Sans sort de survie paye en mana, seul l'ordre
// change. Rien n'est retire hors de ce cadre.
// Reserve d'une ressource qui SE CONSTRUIT pour le soin (11/10, match humain id=17 : Hand of the Earthmother 502804 de
// la classe 31 coute 350 de rage -- 35 affiches ; le bot est mort a 178). En survie (palier pression ou plus, arene 1v1
// ou duel), pour chaque ressource autre que la mana dont un soin sur soi se paie, aucun sort d'attaque paye dans cette
// ressource ne la fait passer sous le cout du moins cher de ces soins : la ressource s'accumule jusqu'au soin. Les sorts
// payes autrement (generateurs payes en mana, comme Geode Barrage 502772) ne sont pas touches.
void CoaAreneReserveSoin(PlayerbotAI* botAI, Player* bot, std::vector<Usable>& usable)
{
    if (!CoaAreneSurvieArmee() || usable.empty())
        return;
    Unit* const adv = CoaAreneTactiqueAdversaire(botAI, bot);
    uint8 const niveau = CoaAreneEtatDe(botAI).survieNiveau;
    if (!adv || niveau < COA_SURVIE_PRESSION)
        return;
    // Revue survie v2 : seulement les soins que la survie lancerait une fois la ressource la -- ni en recharge (la reserve
    // tenait toute la recharge pour rien), ni entrave, ni « par serviteur » sans serviteur, ni soin periodique deja pose ;
    // au palier pression, un soin direct seulement sous CoaAreneSurvieSoinPressionPv ; un soin incante seulement hors de vue
    // de l'adversaire ou sous un defensif. Sinon un soin que la survie ne lance jamais bloquait pour de bon toute attaque
    // dont le cout plus la reserve depasse le maximum de la ressource (energie 100 : soin a 50, attaques a 55 et plus).
    bool const sansServiteur = !CoaAreneAServiteur(bot);
    bool const soinDirect = niveau >= COA_SURVIE_DANGER || bot->GetHealthPct() < CoaAreneSurvieSoinPressionPv;
    int8 incanteLibre = -1;
    int32 reserve[MAX_POWERS] = {};
    for (Usable const& spell : KnownAbilities(botAI, bot, [](uint16 kind)
             { return (kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE)); }))
    {
        SpellInfo const* info = EffectiveSpell(bot, spell.info);
        if (info->PowerType >= MAX_POWERS || info->PowerType == POWER_MANA || info->PowerType == POWER_HEALTH)
            continue;
        if (bot->HasSpellCooldown(info->Id) || CoaAreneSortEntrave(info) || CoaAreneSortFormeUsable(bot, spell) ||
            (!soinDirect && !(spell.kind & KIND_HOT)) ||
            (sansServiteur && (CoaAreneSortParServiteur(spell.info->Id) || CoaAreneSortParServiteur(info->Id))) ||
            ((spell.kind & KIND_HOT) && CoaAreneAuraDeSoi(bot, spell.info, info)) || CoaSoinDejaPose(bot, spell, info, bot))
            continue;
        if (!CoaAreneSortMobile(bot, info))
        {
            if (incanteLibre < 0)
                incanteLibre = (!adv->IsWithinLOSInMap(bot) || CoaAreneSousDefensif(botAI, bot)) ? 1 : 0;
            if (!incanteLibre)
                continue;
        }
        int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
        int32& r = reserve[info->PowerType];
        if (cout > 0 && (!r || cout < r))
            r = cout;
    }
    usable.erase(std::remove_if(usable.begin(), usable.end(), [bot, &reserve](Usable const& spell)
        {
            if (spell.kind & KIND_HEAL)
                return false;
            SpellInfo const* info = EffectiveSpell(bot, spell.info);
            if (info->PowerType >= MAX_POWERS || !reserve[info->PowerType])
                return false;
            int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
            return cout > 0 && int32(bot->GetPower(Powers(info->PowerType))) - cout < reserve[info->PowerType];
        }), usable.end());
}

void CoaAreneReserveMana(PlayerbotAI* botAI, Player* bot, std::vector<Usable>& usable)
{
    CoaAreneReserveSoin(botAI, bot, usable);
    if (!CoaAreneSurvieArmee() || usable.empty() || !bot->GetMaxPower(POWER_MANA) ||
        bot->GetPowerPct(POWER_MANA) >= CoaAreneReserveManaPct || !CoaAreneTactiqueAdversaire(botAI, bot))
        return;
    CheapestFirst(bot, usable);
    int32 reserve = 0;
    for (Usable const& spell : KnownAbilities(botAI, bot, [](uint16 kind)
             { return (kind & KIND_DEFENSIVE) || ((kind & KIND_HEAL) && !(kind & (KIND_GROUP_HEAL | KIND_CONTROL | KIND_HOSTILE))); }))
    {
        SpellInfo const* info = EffectiveSpell(bot, spell.info);
        if (info->PowerType != POWER_MANA)
            continue;
        int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
        if (cout > 0 && (!reserve || cout < reserve))
            reserve = cout;
    }
    if (!reserve)
        return;
    int32 const mana = int32(bot->GetPower(POWER_MANA));
    usable.erase(std::remove_if(usable.begin(), usable.end(), [bot, mana, reserve](Usable const& spell)
        {
            SpellInfo const* info = EffectiveSpell(bot, spell.info);
            if (info->PowerType != POWER_MANA)
                return false;
            int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
            return cout > 0 && mana - cout < reserve;
        }), usable.end());
}

// Sort ecarte faute de ressource (CastFirst, gladiateur trace) : une ligne par sort et par 5 s, et le compte du bilan.
void CoaAreneNoterRefusRessource(PlayerbotAI* botAI, Player* bot, SpellInfo const* info, Unit* target)
{
    CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
    uint32 const maintenant = getMSTime();
    uint32& derniere = etat.refusRessourceTraceMs[info->Id];
    if (derniere && getMSTimeDiff(derniere, maintenant) < 5 * IN_MILLISECONDS)
        return;
    derniere = maintenant ? maintenant : 1;
    ++etat.refusRessource;
    CoaAreneTracerSort(botAI, bot, info, target, SPELL_FAILED_NO_POWER, "refus_ressource");
}

// Lot 20 : soin ou bouclier ecarte parce que son aura est deja posee par le bot (CastFirst, CoaSoinDejaPose), en arene 1v1
// ou en duel : une ligne « deja_pose » par sort et par 5 s, comptee au bilan ; et la trace du sort pour un gladiateur trace.
void CoaAreneNoterDejaPose(PlayerbotAI* botAI, Player* bot, SpellInfo const* info, Unit* target, bool trace)
{
    CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
    if (!etat.actif)
        return;
    uint32 const maintenant = getMSTime();
    uint32& derniere = etat.dejaPoseTraceMs[info->Id];
    if (derniere && getMSTimeDiff(derniere, maintenant) < 5 * IN_MILLISECONDS)
        return;
    derniere = maintenant ? maintenant : 1;
    ++etat.dejaPose;
    Aura const* aura = target ? target->GetAura(info->Id, bot->GetGUID()) : nullptr;
    Unit* adv = CoaAreneAdversaireVivant(bot, etat);
    float const d = adv ? bot->GetExactDist2d(adv) : -1.0f;
    CoaAreneJournal(bot, etat, "deja_pose", d, d,
                    Acore::StringFormat(" niveau={} sort={} cible_soi={} reste_ms={} pv={:.0f}",
                                        CoaAreneSurvieNom(etat.survieNiveau), info->Id, target == bot ? 1 : 0,
                                        aura ? aura->GetDuration() : -2, bot->GetHealthPct()));
    if (trace)
        CoaAreneTracerSort(botAI, bot, info, target, SPELL_CAST_OK, "deja_pose");
}

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

// Le recul de L1 est ouvert a ce bot : le Ranger a distance ; et, sous IaSurvie (11/10), toute specialisation CoA a
// distance (plage de tir lue sur de vrais sorts de 25 yd et plus) en arene 1v1 ou en duel, des le palier pression --
// seulement le recul : ni rupture, ni burst, ni ouverture furtive, qui restent au Ranger.
bool CoaAreneReculOuvert(PlayerbotAI* botAI, Player* bot, CoaAreneEtat const& etat)
{
    if (CoaAreneEstRangerDistance(bot))
        return true;
    if (!CoaAreneSurvieArmee() || etat.survieNiveau < 1 || !IsAscensionCustomClassId(bot->getClass()) ||
        GetCoaStyle(bot) == CoaStyle::Melee || !CoaAreneUnContreUn(bot))
        return false;
    float bas = 0.0f, haut = 0.0f;
    return CoaArenePlage(botAI, bot, bas, haut);
}

// Au contact d'une cible qui NE PEUT PAS suivre, bot libre de ses mouvements.
class CoaAreneReculTrigger : public Trigger
{
public:
    CoaAreneReculTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene recul") {}

    bool IsActive() override
    {
        if (!CoaAreneEstRangerDistance(bot) && !CoaAreneSurvieArmee())
            return false;
        CoaAreneEtat const& etat = CoaAreneMaj(botAI, bot);
        if (!CoaAreneReculOuvert(botAI, bot, etat))
            return false;
        if (!CoaAreneL1Actif(etat) || CoaAreneReculEnCours(etat) || CoaAreneCachetteEnCours(etat) ||
            etat.survieFuiteDebutMs)
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
        // « Au contact » : sous la portee minimale pour le Ranger (5 yd) ; pour un tireur sans portee minimale (lanceur de
        // sorts, bas = 0), a portee de melee de l'adversaire (5 yd de combat).
        float const contact = CoaAreneEstRangerDistance(bot) ? bas + 1.0f : std::max(bas, 4.0f) + 1.0f;
        return CoaAreneDistanceCombat(bot, cible) < contact;
    }
};

// L1, arene ou duel : l'adversaire est hors de vue, hors de portee, ou n'est pas la cible courante.
// Le retour presse : le bot est en combat, immobile, l'adversaire hors de sa vue, et sa vie baisse (releve de 2 s du
// defensif, CoaAreneMaj). Cout : un test de ligne de vue, seulement apres les tests de champ.
bool CoaAreneRetourUrgent(Player* bot, CoaAreneEtat const& etat, Unit* adv)
{
    return bot->IsInCombat() && !bot->isMoving() &&
           std::max(etat.pvPrec, etat.pvRef) - bot->GetHealthPct() >= 1.0f && !bot->IsWithinLOSInMap(adv);
}

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
        bool relanceRecente =
            etat.dernierRetourMs && getMSTimeDiff(etat.dernierRetourMs, getMSTime()) < CoaAreneRetourReessaiMs;
        // Urgence (11/10, arene 1v1 id=90, Ranger 2239, 11:56:03-11:56:05 Paris) : immobile, en combat, hors de vue de
        // l'adversaire et sous ses DoT (94 -> 74 % en 2 s, sans un sort). Le retour repart au tick suivant, sans
        // attendre la seconde de relance (une relance d'urgence par CoaAreneRetourUrgenceMs au plus).
        if (relanceRecente && CoaAreneRetourUrgent(bot, etat, adv) &&
            (!etat.retourUrgenceMs || getMSTimeDiff(etat.retourUrgenceMs, getMSTime()) >= CoaAreneRetourUrgenceMs))
            relanceRecente = false;
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
        // Lot 19 : tout ouvreur furtif (CoaAreneOuvreurFurtif), plus seulement le Ranger.
        if (bot->HasStealthAura() && CoaAreneOuvreurFurtif(bot) && CoaAreneOuvertureDansSaFenetre(bot, adv) &&
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

// Le bot est furtif pendant l'engagement (Elude avant le combat, ou Dust Toss en combat) : ouvrir. Lot 19 : tout ouvreur
// furtif (CoaAreneOuvreurFurtif), et l'ouvreur MONTE tant qu'il est sur sa monture de classe en engagement.
class CoaAreneOuvertureTrigger : public Trigger
{
public:
    CoaAreneOuvertureTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene ouverture") {}

    bool IsActive() override
    {
        bool const furtif = bot->HasStealthAura() && CoaAreneOuvreurFurtif(bot);
        bool const monteArme = !furtif && bot->IsMounted() && CoaAreneOuvreurMonteArme(bot);
        if (!furtif && !monteArme)
            return false;
        CoaAreneMaj(botAI, bot);
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        if (monteArme)
        {
            // Monte sur sa monture de classe, portes ouvertes : l'ouverture garde la main jusqu'a la descente (rien
            // d'autre ne part monte). En preparation, rien a faire : on attend les portes, piles de Path of Hellfire.
            return CoaAreneEnCombat(etat) && CoaAreneMonteClasse(botAI, bot, etat);
        }
        if (!CoaAreneL1Actif(etat) || !etat.adversaire)
        {
            if (etat.actif && etat.bras && !etat.preparation)
                CoaAreneOuvertureRefus(bot, etat, 1, -1.0f);
            return false;
        }
        if (CoaAreneReculEnCours(etat))
        {
            CoaAreneOuvertureRefus(bot, etat, 5, -1.0f);
            return false;
        }
        if (etat.prochaineOuvertureMs && getMSTimeDiff(etat.prochaineOuvertureMs, getMSTime()) >= 0x80000000u)
            return false;
        // Blackjack et Dust Toss ne rompent pas la furtivite (SPELL_ATTR1_ALLOW_WHILE_STEALTHED, lu dans Spell.dbc) :
        // apres un sap, le bot est encore furtif, au contact d'une cible etourdie. C'est au recul de jouer d'abord ;
        // ouvrir la tout de suite romprait le sap au contact.
        Unit* adv = ObjectAccessor::GetUnit(*bot, etat.adversaire);
        float const dist = adv ? bot->GetExactDist2d(adv) : -1.0f;
        // Arene ou duel : hors de sa fenetre de tir (hors de vue, ou a plus de 40 yd), l'ouverture laisse le bot au
        // retour, qui suit un chemin -- sa propre approche en ligne droite restait bloquee derriere un pilier ou contre
        // un relief (banc du 10/10 22:56, voir CoaAreneOuvertureDansSaFenetre). Monde ouvert : inchange (pas de retour).
        // Seulement si le retour a une raison d'agir : un adversaire furtif (CanSeeOrDetect faux) ne se poursuit pas
        // par le retour, et l'ouverture garde alors son approche d'avant.
        if (adv && adv->IsAlive() && (etat.cadre == COA_ARENE_ARENE || etat.cadre == COA_ARENE_DUEL) &&
            CoaAreneRetourArme(bot) && !CoaAreneOuvertureDansSaFenetre(bot, adv) &&
            CoaAreneRetourRaison(botAI, bot, adv) != COA_RETOUR_AUCUN)
        {
            CoaAreneOuvertureRefus(bot, etat, 3, dist);
            return false;
        }
        // Le recul apres un sap n'existe que pour le Ranger L1 : les autres ouvreurs furtifs n'ont pas a l'attendre.
        if (adv && adv->IsAlive() && CoaAreneEstRangerDistance(bot) && CoaAreneFenetreDeRecul(bot, adv))
        {
            float bas = 0.0f, haut = 0.0f;
            CoaArenePlage(botAI, bot, bas, haut);
            if (CoaAreneDistanceCombat(bot, adv) < CoaAreneDistanceVisee(bas, haut) - 5.0f)
            {
                CoaAreneOuvertureRefus(bot, etat, 4, dist);
                return false;
            }
        }
        return true;
    }
};

// Arene portes fermees, ou duel en compte a rebours : prendre la furtivite (Elude...) pour ouvrir depuis elle, ou monter
// sa monture de classe (Knight of Xoroth) pour ouvrir en chargeant. Lot 19 : tout ouvreur furtif ou monte.
// Ce qu'il y a a faire : 0 rien, 1 monter, 2 furtivite. Monter d'abord : la monture romprait la furtivite.
bool CoaAreneAFamilier(Player* bot);

uint8 CoaArenePreparationAFaire(PlayerbotAI* botAI, Player* bot, CoaAreneEtat& etat)
{
    if (bot->IsInCombat() || !bot->IsAlive())
        return 0;
    // Monte, plus rien ne part (NOT_MOUNTED) ; furtif, le sort d'invocation romprait la furtivite : laisser d'abord le
    // familier se lever (« coa arene familier », 24), ou 6 s de preparation au plus si le kit n'en a pas ou si
    // l'invocation echoue.
    // Revue lot 19 : sans IaCommune, « coa arene familier » ne tourne pas (son declencheur l'exige) et familierAucun
    // n'est jamais pose -- le Ranger L1 (IaRanger seul) attendait 6 s pour rien, et ne prenait plus Elude pendant les
    // 3 s du compte a rebours d'un duel. Il n'attend plus, comme avant le lot 19.
    bool const familierFait = !sPlayerbotAIConfig.coaAreneIaCommune || etat.familierAucun || etat.familiers ||
                              CoaAreneAFamilier(bot) ||
                              (etat.debutMs && getMSTimeDiff(etat.debutMs, getMSTime()) >= CoaArenePreparationMontureDelaiMs);
    if (CoaAreneOuvreurMonteArme(bot) && CoaAreneMontureOuvreur(botAI, bot, etat))
        return CoaAreneMonteClasse(botAI, bot, etat) || !familierFait ? 0 : 1;
    if (CoaAreneOuvreurFurtif(bot) && ClassHas(bot, KIND_STEALTH) && !bot->HasStealthAura() && familierFait)
        return 2;
    return 0;
}

class CoaArenePreparationTrigger : public Trigger
{
public:
    CoaArenePreparationTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene preparation") {}

    bool IsActive() override
    {
        // Test court d'abord : ni furtif possible, ni monture a chercher.
        if (bot->IsInCombat() || (!CoaAreneOuvreurFurtif(bot) && !CoaAreneOuvreurMonteArme(bot)))
            return false;
        CoaAreneMaj(botAI, bot);
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        if (!etat.actif || !etat.bras || !etat.preparation)
            return false;
        if (etat.prochainePreparationMs && getMSTimeDiff(etat.prochainePreparationMs, getMSTime()) >= 0x80000000u)
            return false;
        return CoaArenePreparationAFaire(botAI, bot, etat) != 0;
    }
};

/*
 * Familier (banc d'arene du 11/10/2026 00:36-00:56 Paris). Aucune action CoA n'invoquait rien : une invocation sort de
 * Classify avec kind 0 (ni degat, ni soin, ni aura), et IsAttack ecarte kind 0. Le Necromancer 537 connait six « Raise: »
 * et n'a jamais leve un serviteur -- d'ou Transfer Life refuse NO_PET x34 ; le Tinker 752 connait quatre « Build: »
 * familiers (SUMMON_PET) et n'en a construit aucun (character_pet vide). Le coeur renvoie en outre le familier a
 * l'entree en arene (Battleground::AddPlayer -> UnsummonPetTemporaryIfAny, Battleground.cpp:1222).
 *
 * Invocation PERMANENTE, lue dans Spell.dbc, sans id ecrit ici : duree infinie (GetMaxDuration() < 0 : index 21, -1) ET
 * un effet SUMMON_PET, ou SUMMON dont les SummonProperties sont allie ou familier (categorie 1 ou 2) de type familier,
 * gardien ou serviteur. Releve du 11/10 sur les familles 18-40 : les 13 « Raise: » (Necromancer), 26 SUMMON_PET
 * (« Build: » du Tinker, Summon: Greater Imp, Houndmaster...), Houndmaster's Whistle, Primal Spirit: Tiger/Elemental,
 * Phoenix Egg. Tourelles, totems, etendards et pieges ont une duree : jamais retenus.
 */
bool CoaAreneEstFamilier(SpellInfo const* info)
{
    if (!info || info->IsPassive() || info->GetMaxDuration() >= 0)
        return false;
    // Revue du 11/10 (Necromancer 453, match id=30, aucun familier en arene depuis toujours) : le module reecrit au
    // chargement les SummonProperties des « Raise: » du Necromancer (AscensionNecromancerContracts.cpp:67-69) et des
    // invocations du Witch Doctor (AscensionWitchDoctorCompletion.cpp:548-550) en 64 (categorie 0, type 0) : c'est son
    // script qui cree l'unite, avec les proprietes 61 (allie, gardien ; AscensionNecromancerSummons.cpp:252). Le SpellInfo
    // en memoire ne passait donc jamais ce test : l'action posait familierAucun au premier essai, en silence, et
    // CoaAreneAFamilier ne voyait jamais un serviteur. La ligne BRUTE de Spell.dbc (sSpellStore, que le module ne touche
    // pas) garde 61 : c'est elle que le releve du commentaire ci-dessus avait lue.
    SpellEntry const* const brut = sSpellStore.LookupEntry(info->Id);
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = info->Effects[i];
        if (effect.Effect == SPELL_EFFECT_SUMMON_PET)
            return true;
        if (effect.Effect != SPELL_EFFECT_SUMMON)
            continue;
        for (int32 const misc : { effect.MiscValueB, brut ? brut->EffectMiscValueB[i] : effect.MiscValueB })
        {
            SummonPropertiesEntry const* prop = sSummonPropertiesStore.LookupEntry(uint32(misc));
            if (prop && (prop->Category == SUMMON_CATEGORY_ALLY || prop->Category == SUMMON_CATEGORY_PET) &&
                (prop->Type == SUMMON_TYPE_PET || prop->Type == SUMMON_TYPE_GUARDIAN || prop->Type == SUMMON_TYPE_MINION))
                return true;
        }
    }
    return false;
}

/*
 * Le bot a-t-il deja UN familier vivant : son familier (GetPet), ou une unite qu'il controle creee par une invocation
 * permanente (UNIT_CREATED_BY_SPELL, pose par Map::SummonCreature, Object.cpp:2325). Un seul suffit, A DESSEIN : deux
 * invocations qui se remplacent (Primal Spirit: Tiger / Elemental) s'invoqueraient sinon l'une l'autre sans fin, et le
 * Necromancer ne relance pas tant qu'il a un serviteur (sa capacite de Life Force n'est lisible que du module).
 * Lit le bot et ses unites, fil de l'IA du bot (P-137).
 */
bool CoaAreneAFamilier(Player* bot)
{
    if (Pet* pet = bot->GetPet())
        if (pet->IsAlive())
            return true;
    for (Unit* unite : bot->m_Controlled)
        if (unite && unite->IsInWorld() && unite->IsAlive())
            if (CoaAreneEstFamilier(sSpellMgr->GetSpellInfo(unite->GetUInt32Value(UNIT_CREATED_BY_SPELL))))
                return true;
    return false;
}

/*
 * Life Force du Necromancer (11/10, match humain id=13, Necromancer 432 entre SANS serviteur ; analyse : aucun
 * Necromancer n'a jamais leve un serviteur en arene). Capacite de 2 de base, chaque « Raise: » en occupe 1 a 3
 * (Cost/Capacity, AscensionNecromancer.cpp:81-110) ; le module tient a jour deux auras sur le joueur (Sync, meme fichier
 * :179-198) : 805011 = capacite totale (piles), 525004 = Life Force LIBRE (piles = capacite - occupee ; retiree a 0).
 * Rend le nombre de piles libres, 0 sans l'aura. Revue du 11/10 : Sync tourne toutes les 500 ms pour tout Necromancer
 * vivant et dans le monde (necromancer_lifecycle::OnUnitUpdate, AscensionNecromancer.cpp:393-406), et aussitot apres
 * chaque invocation (Summon, AscensionNecromancerSummons.cpp:285) : l'aura retarde au plus d'une demi-seconde. Le module
 * refuse de toute facon une invocation au-dela de la capacite (OnSpellCheckCast : SPELL_FAILED_ALREADY_HAVE_SUMMON,
 * AscensionNecromancerAbilities.cpp:146, avant tout paiement -- pas de mana perdue sur ce refus).
 */
constexpr uint32 CoaAreneLifeForceTotale = 805011;
constexpr uint32 CoaAreneLifeForceLibre = 525004;

uint8 CoaAreneLifeForceLibrePiles(Player* bot)
{
    Aura const* libre = bot->GetAura(CoaAreneLifeForceLibre);
    return libre ? libre->GetStackAmount() : 0;
}

// Kit a Life Force : le bot porte l'aura de capacite du module (posee par Sync, toutes les 500 ms, sur tout Necromancer
// vivant, au moins 1 pile). Sans elle (autre classe, ou Necromancer mort), la regle « un seul familier » s'applique.
bool CoaAreneKitLifeForce(Player* bot)
{
    return bot->HasAura(CoaAreneLifeForceTotale);
}

// Invocations reussies par engagement pour un kit a Life Force EN ARENE : capacite totale (piles de 805011, bornees a 6)
// en plus du plafond commun -- de quoi remplir la capacite en preparation et remplacer quelques pertes. Au plus 3 + 6 = 9.
// En duel (aucune preparation a remplir), le plafond commun (revue du 11/10).
uint32 CoaAreneFamilierPlafond(Player* bot, CoaAreneEtat const& etat);

// Raison d'un refus d'invocation, dans l'ordre exact de CastFirst (cible = le bot) : ce que le journal « familier_refus »
// ecrit. Rejoue les memes tests SANS effet de bord (pas de demi-tour, pas de banc, pas d'arret). « refus_castspell » :
// tout passait, c'est PlayerbotAI::CastSpell ou le script du module qui a refuse (raison invisible d'ici).
// Revue du 11/10 : le rejeu tourne APRES CastFirst, qui vient peut-etre de mettre le sort au banc (refus de CastSpell :
// 8 s ; refus durable de StrictCheck : 20 s). « banc » seul aurait masque la cause dans presque tous les cas : un sort
// au banc rend « banc_<raison des tests suivants> » (« banc_refus_castspell » si tous passent).
std::string CoaAreneRaisonRefusFamilier(PlayerbotAI* botAI, Player* bot, Usable const& spell)
{
    SpellInfo const* const info = EffectiveSpell(bot, spell.info);
    if (CoaAreneFiltreEtatActif(botAI, bot) && CoaAreneBloqueParEtat(bot, info))
        return "etat";
    if (bot->HasPlayerFlag(PLAYER_ALLOW_ONLY_ABILITY) && !info->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE) &&
        !info->HasAttribute(SPELL_ATTR0_PASSIVE))
        return "verrou";
    if (bot->HasSpellCooldown(info->Id))
        return "recharge";
    if (CoaRefuseParAuraDuLanceur(bot, info))
        return "aura_lanceur";
    if (CoaRefuseParFormeOuArme(bot, info))
        return "forme_arme";
    if (!CoaPeutPayer(bot, info))
        return "ressource";
    if (CoaExigeFamilierAbsent(bot, info))
        return "familier_requis";
    if (CoaRefusCibleCertain(bot, info, bot))
        return "cible";
    if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
        return "recharge_globale";
    auto const& banc = static_cast<CoaAiObjectContext*>(botAI->GetAiObjectContext())->benchedSpells;
    time_t const maintenant = time(nullptr);
    bool auBanc = false;
    for (uint32 const id : { spell.info->Id, info->Id })
    {
        auto const it = banc.find(id);
        if (it != banc.end() && it->second > maintenant)
            auBanc = true;
    }
    std::string const prefixe = auBanc ? "banc_" : "";
    if (Unit* adv = CoaAreneTactiqueAdversaire(botAI, bot))
    {
        // Memes refus que CastFirst, dans le meme ordre (survie : charge vers l'adversaire, incantation longue).
        uint8 refus = CoaAreneRefusTactique(bot, info, adv, bot);
        if (refus == COA_TACTIQUE_AUCUN && CoaAreneEtatDe(botAI).survieNiveau >= 2 && CoaAreneSortDeRapprochement(info))
            refus = COA_TACTIQUE_SURVIE_CHARGE;
        if (refus == COA_TACTIQUE_AUCUN && CoaAreneSurvieRefuseIncantation(botAI, bot, info, spell.kind))
            refus = COA_TACTIQUE_SURVIE_INCANTATION;
        if (refus != COA_TACTIQUE_AUCUN)
            return prefixe + CoaAreneRefusTactiqueNom(refus);
    }
    int8 vue = -1;
    SpellCastResult const pre = CoaPreControleCible(bot, info, bot, vue);
    if (pre != SPELL_CAST_OK)
        return prefixe + Acore::StringFormat("pre_{}", uint32(pre));
    SpellCastResult const check = StrictCheck(bot, info, bot);
    if (check != SPELL_CAST_OK)
        return prefixe + Acore::StringFormat("check_{}", uint32(check));
    if (CoaRefuseEnMouvement(bot, info))
        return prefixe + "en_mouvement";
    return prefixe + "refus_castspell";
}

// Entre deux essais d'invocation (reussi ou non) : borne le cout a un CheckCast par sort candidat toutes les 3 s, et
// laisse l'incantation (2 a 3 s pour les « Raise: ») aboutir avant de rejuger.
constexpr uint32 CoaAreneFamilierReessaiMs = 3 * IN_MILLISECONDS;
// Portes ouvertes : incantation d'invocation la plus longue tentee (au-dela, le bot resterait plante sous les coups).
constexpr uint32 CoaAreneFamilierIncantationMaxMs = 3 * IN_MILLISECONDS;
// Revue c2 : un adversaire qui tue le familier en boucle ne doit pas occuper le match. Attente apres la perte d'un
// familier, et invocations reussies au plus par engagement (preparation comprise).
constexpr uint32 CoaAreneFamilierApresPerteMs = 15 * IN_MILLISECONDS;
constexpr uint32 CoaAreneFamilierMaxParEngagement = 3;
// Journal « familier_refus » : au plus une ligne par sort sur cette duree.
constexpr uint32 CoaAreneFamilierRefusTraceMs = 5 * IN_MILLISECONDS;

uint32 CoaAreneFamilierPlafond(Player* bot, CoaAreneEtat const& etat)
{
    if (etat.cadre != COA_ARENE_ARENE || !CoaAreneKitLifeForce(bot))
        return CoaAreneFamilierMaxParEngagement;
    Aura const* totale = bot->GetAura(CoaAreneLifeForceTotale);
    uint32 const capacite = totale ? std::min<uint32>(totale->GetStackAmount(), 6) : 0;
    return CoaAreneFamilierMaxParEngagement + capacite;
}

// Un serviteur de PLUS (kit a Life Force) : seulement en preparation d'arene (portes fermees, rien d'autre a faire) et
// tant que le module annonce de la Life Force libre. Portes ouvertes, la regle « un seul » reprend : relever une armee
// sous les coups coute des recharges globales que la survie et l'attaque doivent garder.
bool CoaAreneServiteurEnPlus(Player* bot, CoaAreneEtat const& etat)
{
    return etat.preparation && etat.cadre == COA_ARENE_ARENE && CoaAreneKitLifeForce(bot) &&
           CoaAreneLifeForceLibrePiles(bot) > 0;
}

// Arene ou duel (preparation comprise), bras tire, CoaArene.IaCommune : le bot n'a aucun familier et son kit en a un.
// Jamais en monde ouvert (cadre MONDE) ni hors engagement : inerte pour tous les autres bots.
class CoaAreneFamilierTrigger : public Trigger
{
public:
    CoaAreneFamilierTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa arene familier") {}

    // 11/10 (Necromancer 453, match id=30) : le declencheur restait MUET -- ni « familier » ni « familier_refus ». Une ligne
    // « familier_attente » toutes les 5 s au plus tant que la raison ne change pas (une ligne a chaque changement de
    // raison), seulement pour un kit qui a une invocation permanente (KitAInvocation, en cache), en arene et en
    // preparation ou engagement vu : la cause se lit au premier match. Raisons 1-6 ici, 7 dans l'action.
    bool Attendre(CoaAreneEtat& etat, uint8 raison, char const* nom, bool present)
    {
        if (etat.cadre == COA_ARENE_ARENE && (etat.preparation || etat.vuCeTick) && KitAInvocation())
        {
            uint32 const maintenant = getMSTime();
            if (raison != etat.familierAttenteRaison || !etat.familierAttenteMs ||
                getMSTimeDiff(etat.familierAttenteMs, maintenant) >= CoaAreneFamilierRefusTraceMs)
            {
                etat.familierAttenteRaison = raison;
                etat.familierAttenteMs = maintenant ? maintenant : 1;
                CoaAreneJournal(bot, etat, "familier_attente", -1.0f, -1.0f,
                                Acore::StringFormat(" raison={} preparation={} present={} familiers={} plafond={}"
                                                    " life_force_libre={} aucun={}",
                                                    nom, etat.preparation ? 1 : 0, present ? 1 : 0, etat.familiers,
                                                    CoaAreneFamilierPlafond(bot, etat),
                                                    CoaAreneKitLifeForce(bot) ? int32(CoaAreneLifeForceLibrePiles(bot)) : -1,
                                                    etat.familierAucun ? 1 : 0));
            }
        }
        return false;
    }

    // Le kit connait au moins une invocation permanente. Relu au plus toutes les 30 s, et aussitot si le niveau ou le
    // nombre de sorts connus a change (revue : le gladiateur est realigne en niveau a la mise en file, 33 -> 41 au
    // match id=30 ; sans cette cle, le cache d'un match precedent fini depuis moins de 30 s aurait servi).
    bool KitAInvocation()
    {
        uint32 const maintenant = getMSTime();
        uint8 const niveau = bot->GetLevel();
        uint32 const sorts = uint32(bot->GetSpellMap().size());
        if (!kitLuMs || getMSTimeDiff(kitLuMs, maintenant) >= 30 * IN_MILLISECONDS || niveau != kitNiveau ||
            sorts != kitSorts)
        {
            kitLuMs = maintenant ? maintenant : 1;
            kitNiveau = niveau;
            kitSorts = sorts;
            kitAInvocation = false;
            for (Usable const& s : KnownAbilities(botAI, bot, [](uint16) { return true; }))
                if (CoaAreneEstFamilier(EffectiveSpell(bot, s.info)))
                {
                    kitAInvocation = true;
                    break;
                }
        }
        return kitAInvocation;
    }

    bool IsActive() override
    {
        if (!sPlayerbotAIConfig.coaAreneIaCommune || !bot->IsAlive())
            return false;
        CoaAreneEtat& etat = CoaAreneMaj(botAI, bot);
        // Relecture : engagement VU a ce tick ou preparation -- pas pendant les 10 s de grace d'apres un duel, ou le bot
        // est deja rendu au monde ouvert (etat.cadre garde DUEL).
        if (!etat.actif || (etat.cadre != COA_ARENE_ARENE && etat.cadre != COA_ARENE_DUEL))
            return false;
        if (!etat.bras)
            return Attendre(etat, 1, "bras", false);
        if (etat.familierAucun)
            return Attendre(etat, 2, "aucun", false);
        if (!etat.vuCeTick && !etat.preparation)
            return false;
        // Revue c2 : plafond par engagement (l'etat est remis a zero a chaque engagement, CoaAreneMaj).
        if (etat.familiers >= CoaAreneFamilierPlafond(bot, etat))
            return Attendre(etat, 3, "plafond", false);
        uint32 const maintenant = getMSTime();
        // Revue c2 : perte d'un familier vu vivant (tue, ou renvoye) -> CoaAreneFamilierApresPerteMs avant de relancer.
        // Teste a chaque tour, avant le delai d'essai, pour dater la perte au tour pres.
        bool const present = CoaAreneAFamilier(bot);
        if (present)
            etat.familierVu = true;
        else if (etat.familierVu)
        {
            etat.familierVu = false;
            etat.familierPerduMs = maintenant ? maintenant : 1;
        }
        // 11/10 (Necromancer 432) : un kit a Life Force remplit sa capacite en preparation (CoaAreneServiteurEnPlus).
        if (present && !CoaAreneServiteurEnPlus(bot, etat))
            // Un familier present est l'etat normal du combat : journalise en preparation seulement.
            return etat.preparation ? Attendre(etat, 4, "present", true) : false;
        if (etat.familierPerduMs && getMSTimeDiff(etat.familierPerduMs, maintenant) < CoaAreneFamilierApresPerteMs)
            return Attendre(etat, 5, "perte", present);
        if (etat.prochainFamilierMs && getMSTimeDiff(etat.prochainFamilierMs, maintenant) >= 0x80000000u)
            return false;  // reessai : attente normale apres un essai deja journalise
        if (bot->IsNonMeleeSpellCast(false, true, true))
            return Attendre(etat, 6, "incantation", present);
        return true;
    }

private:
    uint32 kitLuMs = 0;
    uint8 kitNiveau = 0;
    uint32 kitSorts = 0;
    bool kitAInvocation = false;
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
        Unit* adv = CoaAreneAdversaireVivant(bot, etat);
        // Relance d'urgence (voir CoaAreneRetourTrigger) : notee AVANT que dernierRetourMs ne soit remis a maintenant.
        if (adv && etat.dernierRetourMs && getMSTimeDiff(etat.dernierRetourMs, maintenant) < CoaAreneRetourReessaiMs)
            etat.retourUrgenceMs = maintenant ? maintenant : 1;
        etat.dernierRetourMs = maintenant ? maintenant : 1;
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

        // Arene 1v1 id=90 (11/10 11:56:01-11:56:05 Paris) : le retour de 11:55:59 (50,9 yd) avait pose une attente de 5 s
        // (MoveTo : distance / vitesse, plafonnee a AiPlayerbot.MaxWaitForMove = 5000). CoaAreneMaj l'a arrete a 27,7 yd,
        // en vue et a portee ; l'adversaire est repasse derriere un obstacle 2 s plus tard, et chaque retour rendait faux
        // ici jusqu'a l'echeance, bot immobile sous les DoT. L'attente ne protege qu'un deplacement EN COURS : immobile,
        // il n'y en a plus, et elle est levee (MoveTo la teste aussi, MovementActions.cpp:181 : la sauter ici ne
        // suffisait pas). Seul lastdelayTime est remis a zero : IsDuplicateMove lit msTime et lastMoveShort, intacts.
        // Revue du 11/10 : jamais une attente de priorite superieure (MOVEMENT_FORCED : SetNextMovementDelay pose une pause
        // VOULUE, bot immobile) -- IsWaitingForLastMove(COMBAT) ne la levait pas non plus avant ce correctif.
        LastMovement& dernier = AI_VALUE(LastMovement&, "last movement");
        if (!bot->isMoving() && dernier.priority <= MovementPriority::MOVEMENT_COMBAT)
            dernier.lastdelayTime = 0.0f;
        else if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
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

// Survie d'arene, un palier (voir CoaAreneSurvieTrigger et l'en-tete du bloc « Survie d'arene »). Derivee de la
// cachette pour la fuite hors de vue du palier critique (AllerHorsDeVue, bornes de L2).
class CoaAreneSurvieAction : public CoaAreneCachetteAction
{
public:
    CoaAreneSurvieAction(PlayerbotAI* botAI, std::string const nom, uint8 niveau)
        : CoaAreneCachetteAction(botAI, nom), niveau(niveau) {}

    bool Execute(Event /*event*/) override
    {
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        Unit* adv = CoaAreneAdversaireVivant(bot, etat);
        CoaAreneSurvieListes const l = CoaAreneSurvieSorts(botAI, bot, niveau);
        bool const vu = adv && adv->IsWithinLOSInMap(bot);
        // Un deplacement voulu de l'IA d'arene EN COURS (le bot court) : un soin incante serait ecarte par CastFirst sans
        // arreter le bot. Arrive (fuite finie de courir, cachette atteinte), le deplacement reste « voulu » mais n'empeche
        // plus rien : c'est precisement la que la fuite doit laisser passer le soin.
        bool const voulu = CoaAreneMouvementVoulu(botAI) && bot->isMoving();
        // Palier danger ou critique, adversaire HORS DE VUE, un soin incante pret : le RETOUR (qui ramenerait le bot en vue)
        // est arrete, et le tour s'arrete la -- le soin part au tick suivant, bot immobile (memes raisons que CastFirst, qui
        // arrete le bot et lance plus tard). Pas sous la recharge globale : sinon le retour repartirait et l'on oscillerait.
        // 11/10 (match id=18) : au plus un arret toutes les 2 s -- jamais une suite d'arrets qui figerait le bot hors de vue.
        if (niveau >= COA_SURVIE_DANGER && !vu && voulu && etat.retourEnCours && !bot->HasUnitState(UNIT_STATE_LOST_CONTROL) &&
            (!etat.survieArretMs || getMSTimeDiff(etat.survieArretMs, getMSTime()) >= 2000) &&
            std::any_of(l.incantes.begin(), l.incantes.end(), [&](Usable const& spell)
                {
                    // Revue du 11/10 : les memes refus sans banc que CastFirst (etat du lanceur : silence, pacification ;
                    // aura du lanceur ; refus certain sur la cible) -- sinon, sous silence, arret puis retour puis arret a
                    // chaque relance, bot fige hors de vue sans rien lancer.
                    SpellInfo const* info = EffectiveSpell(bot, spell.info);
                    return CoaAreneSortPretContre(botAI, bot, spell, bot) && !CoaAreneBloqueParEtat(bot, info) &&
                           !bot->GetGlobalCooldownMgr().HasGlobalCooldown(info);
                }))
        {
            etat.retourEnCours = false;
            bot->StopMoving();
            uint32 const t = getMSTime();
            etat.survieArretMs = t ? t : 1;
            float const d = adv ? bot->GetExactDist2d(adv) : -1.0f;
            CoaAreneJournal(bot, etat, "survie_arret", d, d,
                            Acore::StringFormat(" niveau={} pv={:.0f}", CoaAreneSurvieNom(niveau), bot->GetHealthPct()));
            return true;
        }
        // Soin incante : hors de vue de l'adversaire, ou sous un defensif ; jamais en courant ; au palier critique, en
        // dernier recours (apres la fuite).
        bool const incanteLibre = !voulu && (!vu || CoaAreneSousDefensif(botAI, bot));

        // Ordre (11/10, match id=6 : « elle s'est mise a se soigner bien trop tard ») : le SOIN d'abord, puis le defensif.
        // Palier pression : un soin periodique (HoT) a toute vie, un soin direct sous CoaAreneSurvieSoinPressionPv, un
        // soin incante seulement hors de vue ; danger et critique : soins utilisables en courant, puis incantes (hors de
        // vue ou sous defensif), puis defensifs -- un defensif reste le recours quand aucun soin ne peut partir.
        float const pv = bot->GetHealthPct();
        std::vector<Usable> liste;
        if (niveau == COA_SURVIE_PRESSION)
        {
            bool const soinDirect = pv < CoaAreneSurvieSoinPressionPv;
            for (Usable const& spell : l.mobiles)
                if (soinDirect || (spell.kind & KIND_HOT))
                    liste.push_back(spell);
            if (!vu && !voulu)
                for (Usable const& spell : l.incantes)
                    if (soinDirect || (spell.kind & KIND_HOT))
                        liste.push_back(spell);
        }
        else
        {
            liste = l.mobiles;
            if (incanteLibre)
                liste.insert(liste.end(), l.incantes.begin(), l.incantes.end());
        }
        // Danger arme par les seuls DoT (vie de 50 a 60 %) : pas encore les gros defensifs, seulement ceux a recharge
        // courte, comme au palier pression -- le soin est la reponse a un DoT ; la recharge longue est gardee pour la chute.
        // Etendard de soin (11/10, match id=14) : pose aux pieds du bot des le palier pression, apres les soins directs.
        for (Usable const& spell : CoaAreneSoutiensPrets(botAI, bot))
            if (CoaAreneSoutienSoigne(EffectiveSpell(bot, spell.info)))
                liste.push_back(spell);
        // 11/10 (match id=14, Gardien 448) : Heroic Resolve, son SEUL defensif, parti a 100 % de vie au palier pression
        // sur la seule raison « dots » -- gaspille, plus rien 6 s plus tard a 0 %. Au palier pression arme par les DoT
        // (aucune vie encore perdue), aucun defensif : le soin (periodique) seul.
        bool const sansDefensif = niveau == COA_SURVIE_PRESSION && std::strcmp(etat.survieRaison, "dots") == 0;
        bool const defensifsCourts = niveau == COA_SURVIE_PRESSION ||
                                     (niveau == COA_SURVIE_DANGER && std::strcmp(etat.survieRaison, "dots") == 0);
        if (!sansDefensif)
            for (Usable const& spell : l.defensifs)
                if (!defensifsCourts || CoaAreneRechargeMs(EffectiveSpell(bot, spell.info)) <= CoaAreneSurvieRechargeLegereMs)
                    liste.push_back(spell);
        // Lot 20 : reserve de mana du meilleur outil de survie (CoaAreneReserveOutilMana), lue une fois par tour.
        int32 const reserveOutil = CoaAreneReserveOutilMana(botAI, bot);
        Reserver(etat, adv, reserveOutil, liste);
        if (Noter(etat, adv, CastFirst(botAI, bot, liste, bot, USAGE_DEFENSIVE)))
            return true;

        // 11/10 (match id=11 et 17) : des le palier DANGER -- « jouer la montre » : a decouvert, un soin incante pret,
        // rompre la vue d'abord (arene), puis le soin ; au palier critique seulement, le lancer quand meme a decouvert.
        if (niveau >= COA_SURVIE_DANGER && !incanteLibre && !voulu && !l.incantes.empty())
        {
            bool const incantePret = std::any_of(l.incantes.begin(), l.incantes.end(),
                                                 [&](Usable const& spell) { return CoaAreneSortPret(botAI, bot, spell); });
            if (incantePret && Fuir(etat, adv))
                return true;
            if (niveau == COA_SURVIE_CRITIQUE)
            {
                std::vector<Usable> incantes = l.incantes;
                Reserver(etat, adv, reserveOutil, incantes);
                if (Noter(etat, adv, CastFirst(botAI, bot, incantes, bot, USAGE_DEFENSIVE)))
                    return true;
            }
        }
        // Lot 20 (13/10, banc du 12/10, Bloodmage : Fleshcraft 801952 et ses autres soins refuses faute de rage -- 0 a 44
        // au refus --, alors que Bloodmoon Blast, qui en genere, partait 0 a 2 fois par match) : aux paliers danger et
        // critique, rien n'ayant pu partir, si un soin ou un defensif du palier n'attend QUE sa ressource, lancer d'abord
        // un sort qui la genere (CoaAreneGenereRessource). Le soin passe devant des que la ressource le permet : il est
        // tente avant, a chaque tour.
        if (niveau >= COA_SURVIE_DANGER && Generer(etat, adv, l, liste))
            return true;
        // Ni soin ni defensif n'a pu partir (11/10, match humain id=11 : Stormancer 488, a 44 % puis 33 %, n'a rien lance --
        // son seul soin, Invigorating Surge, coute 25 « Static », une ressource de script que le bot ne suit pas) : aux
        // paliers danger et critique, un CONTROLE sur l'adversaire pour casser la pression (etourdir, effrayer, enraciner,
        // ralentir), s'il est en vue. Rien a dire de plus au cas ou il est deja controle : CastFirst ecarte deja un sort
        // refuse sur sa cible (CoaRefusCibleCertain) et la regle « sous controle » du coeur.
        // Lot 18 (11/10, match humain id=43 : Stormbringer 425 a relance Storm Alert toutes les ~2 s pendant 17 s, PV
        // stables a 42 %, sans une attaque -- la peur, rompue par les degats, disparaissait aussitot) : au plus UN
        // controle par fenetre, et seulement sous une pression REELLE : le bot perd de la vie sur le releve de 2 a 4 s, ou
        // l'adversaire incante. Sinon la survie rend la main (false) : l'attaque reprend ce tick-ci.
        uint32 const maintenantSurvie = getMSTime();
        bool const fenetreLibre = !etat.survieControleMs ||
                                  getMSTimeDiff(etat.survieControleMs, maintenantSurvie) >= CoaAreneSurvieControleFenetreMs;
        bool const pressionReelle = std::max(etat.pvPrec, etat.pvRef) - pv >= 1.0f ||
                                    (adv && adv->IsNonMeleeSpellCast(false, false, true));
        if (niveau >= COA_SURVIE_DANGER && adv && vu && !etat.survieFuiteDebutMs && fenetreLibre && pressionReelle)
        {
            std::vector<Usable> controles = KnownAbilities(botAI, bot, [](uint16 kind)
                { return (kind & KIND_CONTROL) && (kind & KIND_HOSTILE) && !(kind & KIND_HEAL); });
            // Revue survie v2 : un controle deja pose par le bot sur l'adversaire n'est pas relance -- le coeur accepte de
            // rafraichir un ralentissement, et un sort bon marche sans recharge prenait alors chaque recharge globale du
            // palier danger (jusqu'a 15 s apres un gros coup, a vie haute) au lieu des attaques.
            controles.erase(std::remove_if(controles.begin(), controles.end(), [&](Usable const& spell)
                {
                    SpellInfo const* info = EffectiveSpell(bot, spell.info);
                    return adv->HasAura(info->Id, bot->GetGUID()) || adv->HasAura(spell.info->Id, bot->GetGUID());
                }), controles.end());
            if (SpellInfo const* cast = CastFirst(botAI, bot, controles, adv, USAGE_DEFENSIVE))
            {
                RecordUsage(USAGE_DEFENSIVE, cast);
                ++etat.survieControles;
                etat.survieControleMs = maintenantSurvie ? maintenantSurvie : 1;
                float const d = bot->GetExactDist2d(adv);
                CoaAreneJournal(bot, etat, "survie_controle", d, d,
                                Acore::StringFormat(" niveau={} sort={} pv={:.0f}", CoaAreneSurvieNom(niveau), cast->Id, pv));
                return true;
            }
        }
        if (l.entraves.empty())
            return false;
        // Sort d'entrave (stase, bloc) : au palier critique en DERNIER recours, aucun soin ni defensif n'ayant pu partir
        // et aucune fuite en cours ; ou comme outil de mana, au palier pression, mana sous le quart, adversaire hors de vue
        // et vie au-dessus de la moitie (Time Out! rend 15 % puis 5 % toutes les 0,5 s, mais etourdit 6 s).
        bool const dernierRecours = niveau == COA_SURVIE_CRITIQUE && !etat.survieFuiteDebutMs;
        bool const outilMana = niveau == COA_SURVIE_PRESSION && !vu && pv > CoaAreneSurvieDangerPv &&
                               bot->GetMaxPower(POWER_MANA) && bot->GetPowerPct(POWER_MANA) < CoaAreneSurvieManaOutilPct;
        if (!dernierRecours && !outilMana)
            return false;
        std::vector<Usable> entraves;
        for (Usable const& spell : l.entraves)
            if (dernierRecours || CoaAreneRendMana(EffectiveSpell(bot, spell.info)))
                entraves.push_back(spell);
        return !entraves.empty() && Noter(etat, adv, CastFirst(botAI, bot, entraves, bot, USAGE_DEFENSIVE));
    }

    bool isUseful() override { return bot->IsAlive() && ClassHas(bot, KIND_DEFENSIVE | KIND_HEAL); }

private:
    uint8 niveau;

    // Lot 20 : retire de `liste` les soins que la reserve de mana du meilleur outil retient (CoaAreneSousReserveOutil) ;
    // une ligne « survie_reserve_mana » par sort et par 5 s, comptee au bilan.
    void Reserver(CoaAreneEtat& etat, Unit* adv, int32 reserve, std::vector<Usable>& liste)
    {
        if (reserve <= 0 || liste.empty())
            return;
        uint32 const maintenant = getMSTime();
        liste.erase(std::remove_if(liste.begin(), liste.end(), [&](Usable const& spell)
            {
                SpellInfo const* info = EffectiveSpell(bot, spell.info);
                if (!CoaAreneSousReserveOutil(bot, spell, info, reserve))
                    return false;
                uint32& derniere = etat.reserveManaTraceMs[info->Id];
                if (!derniere || getMSTimeDiff(derniere, maintenant) >= 5 * IN_MILLISECONDS)
                {
                    derniere = maintenant ? maintenant : 1;
                    ++etat.survieReserveMana;
                    float const d = adv ? bot->GetExactDist2d(adv) : -1.0f;
                    CoaAreneJournal(bot, etat, "survie_reserve_mana", d, d,
                                    Acore::StringFormat(" niveau={} sort={} cout={} mana={} reserve={} mana_pct={:.0f} pv={:.0f}",
                                                        CoaAreneSurvieNom(niveau), info->Id,
                                                        info->CalcPowerCost(bot, info->GetSchoolMask()),
                                                        bot->GetPower(POWER_MANA), reserve, bot->GetPowerPct(POWER_MANA),
                                                        bot->GetHealthPct()));
                }
                return true;
            }), liste.end());
    }

    // Lot 20 : generateur de la ressource qui manque au premier soin ou defensif du palier (voir Execute).
    bool Generer(CoaAreneEtat& etat, Unit* adv, CoaAreneSurvieListes const& l, std::vector<Usable> const& liste)
    {
        // Par ressource, le premier sort de survie (ordre du palier : liste du tour, puis soins incantes, puis defensifs)
        // qui n'attend qu'elle : ni en recharge, ni payable, paye dans une ressource autre que la vie. Une ressource dont
        // aucun generateur ne part laisse sa chance a la suivante (un soin bloque par la mana sans generateur ne doit pas
        // masquer un soin bloque par la rage qui en a un).
        struct Manque { SpellInfo const* pour; Powers pouvoir; int32 manque; };
        std::vector<Manque> manques;
        auto examiner = [&](std::vector<Usable> const& sorts)
        {
            for (Usable const& spell : sorts)
            {
                SpellInfo const* info = EffectiveSpell(bot, spell.info);
                if (info->PowerType >= MAX_POWERS || info->PowerType == POWER_HEALTH || bot->HasSpellCooldown(info->Id) ||
                    CoaPeutPayer(bot, info) ||
                    std::any_of(manques.begin(), manques.end(), [info](Manque const& m) { return m.pouvoir == Powers(info->PowerType); }))
                    continue;
                int32 const cout = info->CalcPowerCost(bot, info->GetSchoolMask());
                if (cout > 0)
                    manques.push_back({ info, Powers(info->PowerType), cout - int32(bot->GetPower(Powers(info->PowerType))) });
            }
        };
        examiner(liste);
        examiner(l.incantes);
        examiner(l.defensifs);
        // Revue du lot 20 : rien n'attend sa ressource -> ni recherche de generateur ni « coa attack » compte comme tente
        // (RecordUsage) a chaque tour de survie sans soin.
        if (manques.empty())
            return false;
        SpellInfo const* cast = nullptr;
        Manque const* retenu = nullptr;
        for (Manque const& m : manques)
        {
            // Les generateurs : tout sort connu (attaque ou sort sans marque) qui rend cette ressource au lanceur et ne se
            // paie pas dans cette ressource ; ni soin (deja tente), ni posture, ni furtivite, ni entrave, ni sort de forme.
            std::vector<Usable> surAdv, surSoi;
            for (Usable const& spell : KnownAbilities(botAI, bot, [](uint16 kind)
                     { return !(kind & (KIND_HEAL | KIND_STANCE | KIND_STEALTH)); }))
            {
                SpellInfo const* info = EffectiveSpell(bot, spell.info);
                if (info->PowerType == uint32(m.pouvoir) || CoaAreneSortEntrave(info) || CoaAreneSortFormeUsable(bot, spell) ||
                    !CoaAreneGenereRessource(info, m.pouvoir))
                    continue;
                ((spell.kind & KIND_HOSTILE) ? surAdv : surSoi).push_back(spell);
            }
            if (adv && !surAdv.empty())
                cast = CastFirst(botAI, bot, surAdv, adv, USAGE_ATTACK);
            if (!cast && !surSoi.empty())
                cast = CastFirst(botAI, bot, surSoi, bot, USAGE_ATTACK);
            if (cast)
            {
                retenu = &m;
                break;
            }
        }
        if (!RecordUsage(USAGE_ATTACK, cast) || !retenu)
            return false;
        SpellInfo const* pour = retenu->pour;
        Powers const pouvoir = retenu->pouvoir;
        int32 const manque = retenu->manque;
        ++etat.survieGenerateurs;
        float const d = adv ? bot->GetExactDist2d(adv) : -1.0f;
        CoaAreneJournal(bot, etat, "survie_generateur", d, d,
                        Acore::StringFormat(" niveau={} sort={} pour={} ressource={} manque={} pv={:.0f}",
                                            CoaAreneSurvieNom(niveau), cast->Id, pour->Id, uint32(pouvoir), manque,
                                            bot->GetHealthPct()));
        return true;
    }

    bool Noter(CoaAreneEtat& etat, Unit* adv, SpellInfo const* cast)
    {
        if (!RecordUsage(USAGE_DEFENSIVE, cast))
            return false;
        bool const soin = CoaAreneEstSoin(EffectiveSpell(bot, cast));
        ++etat.survieSorts[niveau];
        etat.soinsSoi += soin ? 1 : 0;
        float const d = adv ? bot->GetExactDist2d(adv) : -1.0f;
        CoaAreneJournal(bot, etat, "survie", d, d,
                        Acore::StringFormat(" niveau={} raison={} sort={} soin={} pv={:.0f} mana_pct={:.0f}",
                                            CoaAreneSurvieNom(niveau), etat.survieRaison, cast->Id, soin ? 1 : 0,
                                            bot->GetHealthPct(),
                                            bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : -1.0f));
        return true;
    }

    // Fuite hors de vue (arene seulement : en duel, le monde ouvert n'a pas de cachette bornee). Bornes : pause de 8 s
    // apres une fuite, 20 s par engagement, un essai toutes les 1,5 s.
    bool Fuir(CoaAreneEtat& etat, Unit* adv)
    {
        uint32 const maintenant = getMSTime();
        if (!adv || etat.cadre != COA_ARENE_ARENE || etat.survieFuiteDebutMs ||
            etat.survieFuiteCumulMs >= CoaAreneSurvieFuiteCumulMaxMs ||
            (etat.survieFuiteFinMs && getMSTimeDiff(etat.survieFuiteFinMs, maintenant) < CoaAreneSurvieFuitePauseMs) ||
            (etat.survieFuiteEssaiMs && getMSTimeDiff(etat.survieFuiteEssaiMs, maintenant) < CoaAreneSurvieFuiteEssaiMs))
            return false;
        etat.survieFuiteEssaiMs = maintenant ? maintenant : 1;
        if (!bot->GetMap() || !CoaAreneLibreDeBouger(botAI, bot))
            return false;
        // Revue du 11/10 : meme levee d'attente que le retour (CoaAreneRetourAction) -- un retour arrete en chemin par
        // CoaAreneMaj laisse une attente de MoveTo jusqu'a 5 s ; bot immobile, elle ne protege plus rien et refusait la
        // fuite (et MoveTo, dans EssayerPoint, la teste aussi). Jamais une attente de priorite superieure.
        LastMovement& dernier = AI_VALUE(LastMovement&, "last movement");
        if (!bot->isMoving() && dernier.priority <= MovementPriority::MOVEMENT_COMBAT)
            dernier.lastdelayTime = 0.0f;
        else if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
            return false;
        float const dAdv = bot->GetExactDist2d(adv);
        CoaAreneCachette const c = AllerHorsDeVue(adv);
        if (!c.trouvee)
        {
            CoaAreneJournal(bot, etat, "survie_fuite_aucune", dAdv, dAdv,
                            Acore::StringFormat(" caches={} chemins={} pv={:.0f}", c.caches, c.chemins, bot->GetHealthPct()));
            return false;
        }
        etat.survieFuiteDebutMs = maintenant ? maintenant : 1;
        etat.retourEnCours = false;
        ++etat.survieFuites;
        CoaAreneJournal(bot, etat, "survie_fuite", dAdv, c.dPoint,
                        Acore::StringFormat(" pv={:.0f} chemin={:.1f} carte={} essais={}", bot->GetHealthPct(), c.chemin,
                                            bot->GetMapId(), c.chemins));
        return true;
    }
};

// Etendard et autres sorts au sol de soutien, poses sur le bot (voir CoaAreneEstSoutienAuSol).
class CoaAreneSoutienAction : public Action
{
public:
    CoaAreneSoutienAction(PlayerbotAI* botAI) : Action(botAI, "coa arene soutien") {}

    bool Execute(Event /*event*/) override
    {
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        uint32 const prochain = getMSTime() + CoaAreneSoutienReessaiMs;
        etat.prochainSoutienMs = prochain ? prochain : 1;
        SpellInfo const* cast = RecordUsage(USAGE_BUFF, CastFirst(botAI, bot, CoaAreneSoutiensPrets(botAI, bot), bot, USAGE_BUFF));
        if (!cast)
            return false;
        ++etat.soutiens;
        Unit* adv = CoaAreneAdversaireVivant(bot, etat);
        float const d = adv ? bot->GetExactDist2d(adv) : -1.0f;
        CoaAreneJournal(bot, etat, "soutien", d, d, Acore::StringFormat(" sort={} pv={:.0f}", cast->Id, bot->GetHealthPct()));
        return true;
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

        // Lot 19 : monte sur sa monture de classe (le declencheur ne l'arme qu'en engagement, IaCommune, 1v1 ou duel).
        if (!bot->HasStealthAura() && bot->IsMounted() && CoaAreneOuvreurMonteArme(bot) &&
            CoaAreneMonteClasse(botAI, bot, etat))
            return OuvrirMonte(etat);

        etat.prochaineOuvertureMs = maintenant + CoaAreneOuvertureReessaiMs;

        Unit* adv = etat.adversaire ? ObjectAccessor::GetUnit(*bot, etat.adversaire) : nullptr;
        if (!adv || !adv->IsInWorld() || !adv->IsAlive() || !bot->IsValidAttackTarget(adv))
        {
            CoaAreneOuvertureRefus(bot, etat, 7, -1.0f);
            return false;
        }

        context->GetValue<Unit*>("current target")->Set(adv);
        bot->SetSelection(adv->GetGUID());

        // Le controle d'ouverture (sap) puis le recul n'existent que pour le Ranger L1 ; les autres ouvreurs furtifs
        // ouvrent par leur plus gros coup « seulement furtif », puis le reste (lot 19).
        bool const ranger = CoaAreneEstRangerDistance(bot);
        bool const horsCombat = !adv->IsInCombat();
        bool const furtifAvant = bot->HasStealthAura();
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
            // l'ouverture par-dessus : controle (cible hors combat, Ranger), puis « seulement furtif », puis le reste,
            // dont les consommateurs dans l'ordre que le burst leur a donne (tris stables).
            std::vector<Usable> pretes = ouvertures;
            // Revue lot 19 (arbitrage) : un ouvreur furtif autre que le Ranger qui a un coup « seulement furtif » hors
            // recharge ne lance QUE celui-la -- sinon une attaque ordinaire a 40 yd romprait la furtivite avant le
            // contact. Rien de pret a cette distance : l'approche ci-dessous l'amene a portee de ce coup.
            if (!ranger && std::any_of(ouvertures.begin(), ouvertures.end(), [this](Usable const& s)
                    { return s.info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) && !bot->HasSpellCooldown(s.info->Id); }))
                pretes.erase(std::remove_if(pretes.begin(), pretes.end(), [](Usable const& s)
                    { return !s.info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED); }), pretes.end());
            DropUncastableHere(bot, adv, pretes);
            CoaAreneBurst const burst = CoaAreneOrdonnerBurst(botAI, bot, adv, pretes);
            std::stable_partition(pretes.begin(), pretes.end(),
                [](Usable const& spell) { return spell.info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED); });
            if (horsCombat && ranger)
                std::stable_partition(pretes.begin(), pretes.end(),
                    [](Usable const& spell) { return (spell.kind & KIND_CONTROL) != 0; });
            if (SpellInfo const* cast = RecordUsage(USAGE_ATTACK, CastFirst(botAI, bot, pretes, adv, USAGE_ATTACK)))
            {
                ++etat.ouvertures;
                CoaAreneNoterBurst(botAI, bot, adv, burst, cast);
                bool const controle = std::any_of(pretes.begin(), pretes.end(),
                    [cast](Usable const& s) { return s.info == cast && (s.kind & KIND_CONTROL); });
                CoaAreneJournal(bot, etat, "ouverture", avant, bot->GetExactDist2d(adv),
                                Acore::StringFormat(" sort={} controle={} furtif_seul={} classe={} furtif={} monte=0",
                                                    cast->Id, controle ? 1 : 0,
                                                    cast->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) ? 1 : 0,
                                                    uint32(bot->getClass()), furtifAvant ? 1 : 0));
                if (botAI->GetState() != BOT_STATE_COMBAT)
                    botAI->ChangeEngine(BOT_STATE_COMBAT);
                // Toxic Dart et Dust Toss ne rompent pas non plus la furtivite : sans ce delai, l'ouverture
                // redeviendrait la rotation tant que dure Elude. Une ouverture par furtivite (recharge d'Elude).
                etat.prochaineOuvertureMs = maintenant + CoaAreneOuvertureApresSuccesMs;
                return true;
            }
        }

        // Rien n'est parti : s'approcher. Ranger : sans entrer au contact (a 5 yd sous sa portee, ou a portee de son
        // controle). Autres ouvreurs furtifs (lot 19) : a la portee de leur meilleur coup « seulement furtif » pret
        // (Parasite, Haunt... souvent au contact), sinon a leur portee d'attaque.
        float bas = 0.0f, haut = 0.0f;
        CoaArenePlage(botAI, bot, bas, haut);
        float approche = haut - 5.0f;
        if (ranger)
        {
            if (horsCombat)
                for (Usable const& spell : ouvertures)
                {
                    if (!(spell.kind & KIND_CONTROL) || bot->HasSpellCooldown(spell.info->Id))
                        continue;
                    approche = std::max(1.0f, spell.info->GetMaxRange(false, bot) - 2.0f);
                    break;
                }
        }
        else
        {
            float meilleure = -1.0f;
            for (Usable const& spell : ouvertures)
                if (spell.info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) && !bot->HasSpellCooldown(spell.info->Id))
                    meilleure = std::max(meilleure, spell.info->GetMaxRange(false, bot));
            approche = std::max(1.0f, (meilleure >= 0.0f ? meilleure : haut) - 1.5f);
        }
        if (CoaAreneDistanceCombat(bot, adv) <= approche)
        {
            CoaAreneOuvertureRefus(bot, etat, 6, avant, Acore::StringFormat(" approche={:.1f}", approche));
            return false;
        }
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

private:
    /*
     * Lot 19, ouverture montee. Tant que le bot est monte, rien d'autre ne part (SPELL_FAILED_NOT_MOUNTED) : cette
     * branche rend donc toujours vrai, et se termine par une DESCENTE -- apres l'attaque montee si rien d'autre n'est pret,
     * ou au troisieme tick a portee sans lancer (recharge, refus), ou si l'adversaire n'est plus valide.
     *   - a portee (CoaAreneOuvertureMonteeDistance, sous le cone de 25 yd de Hellfire Trample) et en vue : les attaques
     *     montees (ALLOW_WHILE_MOUNTED), par CastFirst -- qui juge le cone (regle tactique 1) et tourne le bot ;
     *   - sinon : galoper vers un point a 15 yd de l'adversaire, par un CHEMIN (MoveTo, PathGenerator), comme le retour.
     */
    bool OuvrirMonte(CoaAreneEtat& etat)
    {
        Unit* adv = etat.adversaire ? ObjectAccessor::GetUnit(*bot, etat.adversaire) : nullptr;
        if (!adv || !adv->IsInWorld() || !adv->IsAlive() || adv->GetMap() != bot->GetMap() ||
            !bot->IsValidAttackTarget(adv))
        {
            CoaAreneOuvertureRefus(bot, etat, 7, -1.0f);
            return Descendre(etat, "adversaire_invalide", -1.0f);
        }
        context->GetValue<Unit*>("current target")->Set(adv);
        bot->SetSelection(adv->GetGUID());

        float const dist = bot->GetExactDist2d(adv);
        uint32 const maintenant = getMSTime();
        if (!etat.ouvertureMonteeDebutMs)
            etat.ouvertureMonteeDebutMs = maintenant ? maintenant : 1;
        // En danger, monte, la survie ne peut rien lancer : descendre tout de suite. Plafond de duree : jamais coince
        // en selle derriere un adversaire cache ou sur un chemin sans fin.
        if (etat.survieNiveau >= 2)
            return Descendre(etat, "survie", dist);
        if (getMSTimeDiff(etat.ouvertureMonteeDebutMs, maintenant) >= CoaAreneOuvertureMonteeMaxMs)
            return Descendre(etat, "duree", dist);
        if (dist <= CoaAreneOuvertureMonteeDistance && bot->IsWithinLOSInMap(adv))
        {
            std::vector<Usable> pretes = KnownAbilities(botAI, bot, [](uint16 kind) { return IsAttack(kind, false); });
            pretes.erase(std::remove_if(pretes.begin(), pretes.end(),
                                        [](Usable const& s) { return !CoaAreneEstAttaqueMontee(s); }),
                         pretes.end());
            DropUncastableHere(bot, adv, pretes);
            if (SpellInfo const* cast = RecordUsage(USAGE_ATTACK, CastFirst(botAI, bot, pretes, adv, USAGE_ATTACK)))
            {
                ++etat.ouvertures;
                etat.ouvertureMonteeEssais = 0;
                CoaAreneJournal(bot, etat, "ouverture", dist, bot->GetExactDist2d(adv),
                                Acore::StringFormat(" sort={} controle=0 furtif_seul=0 classe={} furtif=0 monte=1",
                                                    cast->Id, uint32(bot->getClass())));
                if (botAI->GetState() != BOT_STATE_COMBAT)
                    botAI->ChangeEngine(BOT_STATE_COMBAT);
                return true;
            }
            // Plus rien de monte a lancer : on descend au troisieme essai. Revue lot 19 : la recharge globale, une
            // incantation ou le bond de Hellfire Trample en cours ne comptent PAS -- trois tours d'IA (~0,3 s) tombaient
            // tous dans la recharge globale de 1,5 s qui suit la premiere attaque montee, et Dreadrider's Stomp ne
            // partait jamais apres Trample. Borne : CoaAreneOuvertureMonteeMaxMs.
            bool const attente = bot->IsNonMeleeSpellCast(false, true, true) || bot->HasUnitState(UNIT_STATE_CHARGING) ||
                                 std::any_of(pretes.begin(), pretes.end(), [this](Usable const& s)
                                             { return bot->GetGlobalCooldownMgr().HasGlobalCooldown(s.info); });
            if (!attente && ++etat.ouvertureMonteeEssais >= CoaAreneOuvertureMonteeEssaisMax)
                return Descendre(etat, "rien_pret", dist);
            return true;
        }

        // Galoper vers lui : un point a 15 yd sur la droite adversaire -> bot, le chemin le contournera (MoveTo).
        float const angle = adv->GetAngle(bot);
        float const rayon = 15.0f + bot->GetCombatReach() + adv->GetCombatReach();
        float x = adv->GetPositionX() + std::cos(angle) * rayon;
        float y = adv->GetPositionY() + std::sin(angle) * rayon;
        float z = adv->GetPositionZ();
        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(adv, adv->GetPositionX(), adv->GetPositionY(),
                                                             adv->GetPositionZ(), x, y, z, false) ||
            IsDuplicateMove(x, y, z))
        {
            x = adv->GetPositionX();
            y = adv->GetPositionY();
            z = adv->GetPositionZ();
        }
        // Revue lot 19 : MoveTo rend FAUX tant que le deplacement precedent n'a pas fini son delai
        // (IsWaitingForLastMove, MovementActions.cpp:900, jusqu'a MaxWaitForMove = 5 s) et sur une destination repetee
        // (IsDuplicateMove). Compte comme un echec, cela faisait descendre le bot trois tours d'IA apres son premier
        // MoveTo, en plein galop, des que l'adversaire etait a plus de 20 yd aux portes. Un bot qui avance n'echoue pas.
        if (!MoveTo(bot->GetMapId(), x, y, z, false, false, true, false, MovementPriority::MOVEMENT_COMBAT, true, false) &&
            !bot->isMoving() && ++etat.ouvertureMonteeEssais >= CoaAreneOuvertureMonteeEssaisMax)
            return Descendre(etat, "approche_impossible", dist);
        return true;
    }

    // Descendre de la monture de classe (le joueur, lui, est descendu par son client des qu'il lance un autre sort).
    bool Descendre(CoaAreneEtat& etat, char const* raison, float dist)
    {
        // Jamais pendant le bond de Hellfire Trample (charge du coeur) : l'arreter le couperait en l'air.
        if (bot->isMoving() && !bot->HasUnitState(UNIT_STATE_CHARGING))
            bot->StopMoving();
        if (SpellInfo const* monture = CoaAreneMontureOuvreur(botAI, bot, etat))
            bot->RemoveAurasDueToSpell(monture->Id);
        if (bot->IsMounted())
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        etat.ouvertureMonteeEssais = 0;
        CoaAreneOuvertureRefus(bot, etat, 8, dist, Acore::StringFormat(" detail={}", raison));
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

// Banc du 11/10 (tour 7) : POURQUOI CastFirst n'a rien lance, candidat par candidat, dans l'ordre de ses tests (les
// refus de StrictCheck et de CastSpell sont deja traces par CastFirst ; ici, les ecarts silencieux). Diagnostic seul :
// aucun effet sur le jeu. Tour 7, 24 lignes reussi=0 sans cause, reconstituees a la main : portee 10, recharge
// globale 6 (dont 4 rattrapees ensuite), TARGET_IS_PLAYER 4, trop pres (portee minimale) 3, bot etourdi 1.
// « recharge_globale » arrete la liste, comme dans CastFirst.
std::string CoaAreneCausesRatee(PlayerbotAI* botAI, Player* bot, CoaAreneCoupeCandidats const& c, char const*& premiere)
{
    bool const filtreEtat = CoaAreneFiltreEtatActif(botAI, bot);
    int8 vue = -1;
    std::string detail;
    premiere = nullptr;
    for (Usable const& spell : c.sorts)
    {
        SpellInfo const* info = EffectiveSpell(bot, spell.info);
        char const* cause = "refus";
        bool arret = false;
        if (filtreEtat && CoaAreneBloqueParEtat(bot, info))
            cause = "sous_controle";
        // Meme ecart que `seulementCapacites` dans CastFirst (aura SPELL_AURA_ALLOW_ONLY_ABILITY).
        else if (bot->HasPlayerFlag(PLAYER_ALLOW_ONLY_ABILITY) && !info->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE) &&
                 !info->HasAttribute(SPELL_ATTR0_PASSIVE))
            cause = "capacites_seules";
        else if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
        {
            cause = "recharge_globale";
            arret = true;
        }
        else
            switch (CoaPreControleCible(bot, info, c.adv, vue))
            {
                case SPELL_FAILED_OUT_OF_RANGE: cause = "portee"; break;
                case SPELL_FAILED_TOO_CLOSE: cause = "trop_pres"; break;
                case SPELL_FAILED_LINE_OF_SIGHT: cause = "vue"; break;
                case SPELL_FAILED_UNIT_NOT_INFRONT: cause = "dos"; break;
                case SPELL_FAILED_TARGET_IS_PLAYER: cause = "cible_joueur"; break;  // lot 18 (CoaPreControleCible)
                default: cause = CoaRefuseEnMouvement(bot, info) ? "en_mouvement" : "refus"; break;
            }
        if (!premiere)
            premiere = cause;
        detail += Acore::StringFormat("{}{}:{}", detail.empty() ? "" : ",", info->Id, cause);
        if (arret)
            break;
    }
    if (!premiere)
        premiere = "aucun";
    return detail.empty() ? std::string("-") : detail;
}

// Interruption (voir CoaAreneCandidatsInterruption). Une ligne `action=interruption` par sort parti, et une seule
// `reussi=0` par incantation adverse ou rien n'est parti. Prochain essai : 1 s apres un succes, 250 ms apres un echec
// (une recharge globale ne doit pas faire manquer un soin de 1,5 s).
// Banc du 11/10 (tour 7) : un succes apres un echec sur la MEME incantation (identite sort + adresse) retire la ratee
// (`rattrapee=1`, bilan `interruptions_rattrapees`) ; un echec apres un succes sur la meme incantation n'est pas compte.
// La ligne reussi=0 porte en fin `cause=` (premier candidat) et `detail=` (sort:cause de chaque candidat essaye).
class CoaAreneInterruptionAction : public Action
{
public:
    CoaAreneInterruptionAction(PlayerbotAI* botAI) : Action(botAI, "coa arene interruption") {}

    bool Execute(Event /*event*/) override
    {
        CoaAreneCoupeCandidats c;
        if (!CoaAreneCandidatsInterruption(botAI, bot, c))
            return false;
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        // Lu AVANT le lancer : l'incantation adverse peut etre annulee par lui (le Spell n'est plus relu ensuite).
        SpellInfo const* vise = c.incantation->GetSpellInfo();
        int32 const reste = c.incantation->GetCastTimeRemaining();
        uintptr_t const adresse = reinterpret_cast<uintptr_t>(c.incantation);
        float const d = bot->GetExactDist2d(c.adv);

        SpellInfo const* lance = CastFirst(botAI, bot, c.sorts, c.adv, USAGE_INTERRUPT);
        uint32 const prochain = getMSTime() + (lance ? IN_MILLISECONDS : 250u);
        etat.prochaineInterruptionMs = prochain ? prochain : 1;
        // Meme incantation que celle deja jugee (identite seule : c.incantation n'est plus dereferencee apres CastFirst).
        bool const memeIncantation = etat.interruptionJugeeSpell == adresse && etat.interruptionJugeeSort == vise->Id;
        if (lance)
        {
            auto const itr = std::find_if(c.sorts.begin(), c.sorts.end(), [lance](Usable const& s) { return s.info == lance; });
            bool const pure = itr != c.sorts.end() && size_t(std::distance(c.sorts.begin(), itr)) < c.pures;
            bool canalisee = false;
            bool const coupee = reinterpret_cast<uintptr_t>(CoaAreneIncantationAdverse(c.adv, canalisee)) != adresse;
            bool const rattrapee = memeIncantation && etat.interruptionJugeeRatee;
            if (rattrapee && etat.interruptionsRatees)
            {
                --etat.interruptionsRatees;
                ++etat.interruptionsRattrapees;
            }
            etat.interruptionJugeeSpell = adresse;
            etat.interruptionJugeeSort = vise->Id;
            etat.interruptionJugeeRatee = false;
            ++etat.interruptions;
            etat.interruptionsSoin += c.soin ? 1 : 0;
            CoaAreneJournal(bot, etat, "interruption", d, d,
                            Acore::StringFormat(" reussi=1 sort={} outil={} sort_adverse={} soin={} canalisee={} "
                                                "reste_ms={} coupee={} candidats={} pv_adverse={:.0f} rattrapee={}",
                                                lance->Id, pure ? "pure" : "controle", vise->Id, c.soin ? 1 : 0,
                                                c.canalisee ? 1 : 0, reste, coupee ? 1 : 0, c.sorts.size(),
                                                c.adv->GetHealthPct(), rattrapee ? 1 : 0));
        }
        else if (!memeIncantation)
        {
            etat.interruptionJugeeSpell = adresse;
            etat.interruptionJugeeSort = vise->Id;
            etat.interruptionJugeeRatee = true;
            ++etat.interruptionsRatees;
            char const* cause = nullptr;
            std::string const detail = CoaAreneCausesRatee(botAI, bot, c, cause);
            CoaAreneJournal(bot, etat, "interruption", d, d,
                            Acore::StringFormat(" reussi=0 sort_adverse={} soin={} canalisee={} reste_ms={} "
                                                "candidats={} pures={} cause={} detail={}", vise->Id, c.soin ? 1 : 0,
                                                c.canalisee ? 1 : 0, reste, c.sorts.size(), c.pures, cause, detail));
        }
        return RecordUsage(USAGE_INTERRUPT, lance);
    }
};

// Dissipation sur soi (voir CoaAreneCandidatsDispelSoi). Une ligne `action=dispel_soi` par sort parti.
class CoaAreneDispelSoiAction : public Action
{
public:
    CoaAreneDispelSoiAction(PlayerbotAI* botAI) : Action(botAI, "coa arene dispel soi") {}

    bool Execute(Event /*event*/) override
    {
        std::vector<CoaAreneDispelCandidat> const candidats = CoaAreneCandidatsDispelSoi(botAI, bot);
        if (candidats.empty())
            return false;
        std::vector<Usable> sorts;
        for (CoaAreneDispelCandidat const& candidat : candidats)
            sorts.push_back(candidat.sort);
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        bool const survie = CoaAreneSurvieArmee();
        // IaSurvie : les DoT presents AVANT le lancer, pour savoir ensuite lesquels il a vraiment retires (une
        // dissipation resistee ou partielle laisse l'aura : ce ne serait pas une reapplication).
        std::vector<uint32> avant;
        if (survie)
            for (auto const& [id, application] : bot->GetAppliedAuras())
                if (!application->IsPositive() && application->GetBase()->GetSpellInfo()->Dispel)
                    avant.push_back(id);
        SpellInfo const* lance = CastFirst(botAI, bot, sorts, bot, USAGE_DISPEL);
        uint32 const attente = !lance ? CoaAreneDispelSoiApresEchecMs
                               : survie && etat.dispelAttenteMs ? etat.dispelAttenteMs
                                                                : CoaAreneDispelSoiApresSuccesMs;
        uint32 const maintenant = getMSTime();
        uint32 const prochain = maintenant + attente;
        etat.prochainDispelSoiMs = prochain ? prochain : 1;
        if (lance && survie)
        {
            if (!etat.dispelAttenteMs)
                etat.dispelAttenteMs = CoaAreneDispelAttenteBaseMs;
            etat.dernierDispelMs = maintenant ? maintenant : 1;
            etat.dispelReapplicationVue = false;
            etat.dispelRetires.clear();
            for (uint32 const id : avant)
                if (!bot->HasAura(id))
                    etat.dispelRetires.push_back(id);
        }
        if (lance)
        {
            auto const itr = std::find_if(candidats.begin(), candidats.end(),
                                          [lance](CoaAreneDispelCandidat const& c) { return c.sort.info == lance; });
            ++etat.dispelsSoi;
            CoaAreneJournal(bot, etat, "dispel_soi", -1.0f, -1.0f,
                            Acore::StringFormat(" sort={} dots={} prevu={:.0f} prevu_pct={:.0f} pv_bot={:.0f}", lance->Id,
                                                itr != candidats.end() ? itr->dots : 0,
                                                itr != candidats.end() ? itr->prevu : 0.0f,
                                                itr != candidats.end() && bot->GetMaxHealth()
                                                    ? itr->prevu * 100.0f / float(bot->GetMaxHealth()) : 0.0f,
                                                bot->GetHealthPct()));
        }
        return RecordUsage(USAGE_DISPEL, lance);
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
        uint8 const quoi = CoaArenePreparationAFaire(botAI, bot, etat);
        SpellInfo const* parti = nullptr;
        if (quoi == 1)
        {
            // Lot 19 : la monture de classe (Knight of Xoroth : Xorothian Warsteed), portes fermees -- ses piles de Path
            // of Hellfire montent pendant la preparation (une toutes les 3 s sans degat donne ni recu).
            SpellInfo const* monture = CoaAreneMontureOuvreur(botAI, bot, etat);
            if (monture && StrictCheck(bot, monture, bot) == SPELL_CAST_OK && botAI->CastSpell(monture->Id, bot))
                parti = monture;
        }
        else if (quoi == 2)
            // Lot 19 : toute furtivite du kit, jamais canalisee (CoaAreneEntrerEnFurtivite). Le Ranger prenait deja Elude
            // par CoaChasseEntrerEnFurtivite (meme filtre de kit, distance 0) : meme sort pour lui.
            parti = CoaAreneEntrerEnFurtivite(botAI, bot);
        // 11/10 (match id=90) : la preparation n'ecrivait rien -- on ne savait pas si le Ranger etait entre furtif.
        CoaAreneJournal(bot, etat, "preparation", -1.0f, -1.0f,
                        Acore::StringFormat(" reussi={} quoi={} sort={} furtif={} monte={}", parti ? 1 : 0,
                                            quoi == 1 ? "monture" : quoi == 2 ? "furtivite" : "rien",
                                            parti ? parti->Id : 0, bot->HasStealthAura() ? 1 : 0, bot->IsMounted() ? 1 : 0));
        return parti != nullptr;
    }
};

// Invoque le premier familier du kit que le coeur accepte (CastFirst : StrictCheck, banc, mouvement). Un seul essai
// toutes les CoaAreneFamilierReessaiMs, quoi qu'il arrive : pas de boucle sur un refus (capacite de Life Force pleine,
// ressource, GCD). Kit sans invocation permanente : note une fois pour tout l'engagement.
class CoaAreneFamilierAction : public Action
{
public:
    CoaAreneFamilierAction(PlayerbotAI* botAI) : Action(botAI, "coa arene familier") {}

    bool Execute(Event /*event*/) override
    {
        CoaAreneEtat& etat = CoaAreneEtatDe(botAI);
        uint32 const prochain = getMSTime() + CoaAreneFamilierReessaiMs;
        etat.prochainFamilierMs = prochain ? prochain : 1;

        std::vector<Usable> familiers = KnownAbilities(botAI, bot, [](uint16) { return true; });
        familiers.erase(std::remove_if(familiers.begin(), familiers.end(), [this](Usable const& s)
                                       { return !CoaAreneEstFamilier(EffectiveSpell(bot, s.info)); }),
                        familiers.end());
        if (familiers.empty())
        {
            etat.familierAucun = true;
            CoaAreneJournal(bot, etat, "familier_aucun", -1.0f, -1.0f,
                            Acore::StringFormat(" connus={} preparation={}",
                                                KnownAbilities(botAI, bot, [](uint16) { return true; }).size(),
                                                etat.preparation ? 1 : 0));
            return false;
        }
        // Portes ouvertes, seulement une incantation courte : les « Build: » du Tinker durent 6 s (SpellCastTimes 171),
        // les « Raise: » 1,5 a 3 s. Tout le kit en preparation d'ARENE seulement (revue c3) : le compte a rebours d'un
        // duel (DUEL_STATE_COUNTDOWN) ne dure que 3 s, il suit la regle des portes ouvertes.
        if (!etat.preparation || etat.cadre != COA_ARENE_ARENE)
            familiers.erase(std::remove_if(familiers.begin(), familiers.end(), [this](Usable const& s)
                                           { return EffectiveSpell(bot, s.info)->CalcCastTime(bot) >
                                                    CoaAreneFamilierIncantationMaxMs; }),
                            familiers.end());
        // Diagnostic du 11/10 : portes ouvertes, toutes les invocations trop longues -- sans cette ligne, l'essai etait muet.
        // Revue : meme debit que le declencheur (raison 7, une ligne par 5 s) -- sans lui, un Tinker sans familier portes
        // ouvertes (« Build: » de 6 s) ecrivait une ligne a chaque essai, toutes les 3 s, et aussi en duel.
        if (familiers.empty())
        {
            uint32 const maintenant = getMSTime();
            if (etat.familierAttenteRaison != 7 || !etat.familierAttenteMs ||
                getMSTimeDiff(etat.familierAttenteMs, maintenant) >= CoaAreneFamilierRefusTraceMs)
            {
                etat.familierAttenteRaison = 7;
                etat.familierAttenteMs = maintenant ? maintenant : 1;
                CoaAreneJournal(bot, etat, "familier_attente", -1.0f, -1.0f,
                                Acore::StringFormat(" raison=incantation_longue preparation={} present={}",
                                                    etat.preparation ? 1 : 0, CoaAreneAFamilier(bot) ? 1 : 0));
            }
            return false;
        }
        // 11/10 (Necromancer 432) : kit a Life Force -> le moins cher d'abord (cout en ressource du sort reel), pour
        // remplir la capacite en preparation : les squelettes (Life Force 1, ~30 % de mana de base) avant le Crypt Fiend
        // (2, 64 %) et l'Abomination (3, 67 %).
        if (CoaAreneKitLifeForce(bot))
            std::stable_sort(familiers.begin(), familiers.end(), [this](Usable const& a, Usable const& b)
                             {
                                 SpellInfo const* ia = EffectiveSpell(bot, a.info);
                                 SpellInfo const* ib = EffectiveSpell(bot, b.info);
                                 return ia->CalcPowerCost(bot, ia->GetSchoolMask()) <
                                        ib->CalcPowerCost(bot, ib->GetSchoolMask());
                             });

        SpellInfo const* lance = CastFirst(botAI, bot, familiers, bot);
        if (!lance)
        {
            // 11/10 (analyse du Necromancer 432) : l'echec etait muet -- seuls les succes etaient journalises, CastFirst
            // sans usage ne consigne rien et la trace d'arene ne couvre pas la preparation. Une ligne par candidat, au
            // plus une par sort toutes les CoaAreneFamilierRefusTraceMs, preparation comprise.
            uint32 const maintenant = getMSTime();
            for (Usable const& spell : familiers)
            {
                SpellInfo const* const info = EffectiveSpell(bot, spell.info);
                auto const it = refusTraceMs.find(info->Id);
                if (it != refusTraceMs.end() && getMSTimeDiff(it->second, maintenant) < CoaAreneFamilierRefusTraceMs)
                    continue;
                refusTraceMs[info->Id] = maintenant;
                CoaAreneJournal(bot, etat, "familier_refus", -1.0f, -1.0f,
                                Acore::StringFormat(" sort={} raison={} preparation={} present={} life_force_libre={}"
                                                    " mana_pct={:.0f}",
                                                    info->Id, CoaAreneRaisonRefusFamilier(botAI, bot, spell),
                                                    etat.preparation ? 1 : 0, CoaAreneAFamilier(bot) ? 1 : 0,
                                                    CoaAreneKitLifeForce(bot) ? int32(CoaAreneLifeForceLibrePiles(bot)) : -1,
                                                    bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) : -1.0f));
            }
            return false;
        }
        ++etat.familiers;
        CoaAreneJournal(bot, etat, "familier", -1.0f, -1.0f,
                        Acore::StringFormat(" sort={} preparation={} candidats={} life_force_libre={}", lance->Id,
                                            etat.preparation ? 1 : 0, familiers.size(),
                                            CoaAreneKitLifeForce(bot) ? int32(CoaAreneLifeForceLibrePiles(bot)) : -1));
        return true;
    }

    bool isUseful() override { return bot->IsAlive(); }

private:
    // Derniere ligne « familier_refus » par sort (horloge getMSTime) : une instance d'action par bot, fil de son IA.
    std::unordered_map<uint32, uint32> refusTraceMs;
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
// Familier 24 = ACTION_HIGH + 4 : devant la rotation (10), « reach spell » (20) et « coa aoe » (22-23) ; sous
// « coa defensive » (26-28), les soins (30+) et tous les deplacements d'arene (37,5-39,5). En preparation, sous l'ouverture
// et la furtivite (57).
constexpr float CoaAreneFamilierPriority = ACTION_HIGH + 4.0f;
// Combat contre un humain (11/10) : interruption 40,5, devant « coa interrupt » (40, coupee en 1v1 par le
// multiplicateur) et tout deplacement d'arene ; dissipation sur soi 39,3, sous le defensif (39,5), devant le cacheur
// (39,2), la cachette (39), le recul (38) et le retour (37,5).
constexpr float CoaAreneInterruptionPriority = ACTION_INTERRUPT + 0.5f;
constexpr float CoaAreneDispelSoiPriority = ACTION_MOVE + 9.3f;
// Survie d'arene (11/10, IaSurvie) : critique 41, devant les interruptions (40,5) ; danger 39,6 et pression 39,4, devant
// le defensif d'arene (39,5, eteint en 1v1 sous IaSurvie), la dissipation sur soi (39,3), le cacheur (39,2), la cachette
// (39), le recul (38) et le retour (37,5). Soutien 26 = ACTION_HIGH + 6 : devant le familier (24), « coa aoe » (22-23),
// « reach spell » (20) et la rotation (10), sous « coa defensive » (28, coupee en 1v1).
constexpr float CoaAreneSurvieCritiquePriority = ACTION_INTERRUPT + 1.0f;
constexpr float CoaAreneSurvieDangerPriority = ACTION_MOVE + 9.6f;
constexpr float CoaAreneSurviePressionPriority = ACTION_MOVE + 9.4f;
constexpr float CoaAreneSoutienPriority = ACTION_HIGH + 6.0f;

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
        triggers.push_back(new TriggerNode("coa arene familier", { NextAction("coa arene familier", CoaAreneFamilierPriority) }));
        triggers.push_back(new TriggerNode("coa arene interruption", { NextAction("coa arene interruption", CoaAreneInterruptionPriority) }));
        triggers.push_back(new TriggerNode("coa arene dispel soi", { NextAction("coa arene dispel soi", CoaAreneDispelSoiPriority) }));
        triggers.push_back(new TriggerNode("coa arene survie critique", { NextAction("coa arene survie critique", CoaAreneSurvieCritiquePriority) }));
        triggers.push_back(new TriggerNode("coa arene survie danger", { NextAction("coa arene survie danger", CoaAreneSurvieDangerPriority) }));
        triggers.push_back(new TriggerNode("coa arene survie pression", { NextAction("coa arene survie pression", CoaAreneSurviePressionPriority) }));
        triggers.push_back(new TriggerNode("coa arene soutien", { NextAction("coa arene soutien", CoaAreneSoutienPriority) }));
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
        creators["coa arene familier"] = &CoaAreneActionFactoryInternal::familier;
        creators["coa arene interruption"] = &CoaAreneActionFactoryInternal::interruption;
        creators["coa arene dispel soi"] = &CoaAreneActionFactoryInternal::dispelSoi;
        creators["coa arene survie critique"] = &CoaAreneActionFactoryInternal::survieCritique;
        creators["coa arene survie danger"] = &CoaAreneActionFactoryInternal::survieDanger;
        creators["coa arene survie pression"] = &CoaAreneActionFactoryInternal::surviePression;
        creators["coa arene soutien"] = &CoaAreneActionFactoryInternal::soutien;
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
    static Action* familier(PlayerbotAI* botAI) { return new CoaAreneFamilierAction(botAI); }
    static Action* interruption(PlayerbotAI* botAI) { return new CoaAreneInterruptionAction(botAI); }
    static Action* dispelSoi(PlayerbotAI* botAI) { return new CoaAreneDispelSoiAction(botAI); }
    static Action* survieCritique(PlayerbotAI* botAI)
    {
        return new CoaAreneSurvieAction(botAI, "coa arene survie critique", COA_SURVIE_CRITIQUE);
    }
    static Action* survieDanger(PlayerbotAI* botAI)
    {
        return new CoaAreneSurvieAction(botAI, "coa arene survie danger", COA_SURVIE_DANGER);
    }
    static Action* surviePression(PlayerbotAI* botAI)
    {
        return new CoaAreneSurvieAction(botAI, "coa arene survie pression", COA_SURVIE_PRESSION);
    }
    static Action* soutien(PlayerbotAI* botAI) { return new CoaAreneSoutienAction(botAI); }
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
        creators["coa arene familier"] = &CoaAreneTriggerFactoryInternal::familier;
        creators["coa arene interruption"] = &CoaAreneTriggerFactoryInternal::interruption;
        creators["coa arene dispel soi"] = &CoaAreneTriggerFactoryInternal::dispelSoi;
        creators["coa arene survie critique"] = &CoaAreneTriggerFactoryInternal::survieCritique;
        creators["coa arene survie danger"] = &CoaAreneTriggerFactoryInternal::survieDanger;
        creators["coa arene survie pression"] = &CoaAreneTriggerFactoryInternal::surviePression;
        creators["coa arene soutien"] = &CoaAreneTriggerFactoryInternal::soutien;
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
    static Trigger* familier(PlayerbotAI* botAI) { return new CoaAreneFamilierTrigger(botAI); }
    static Trigger* interruption(PlayerbotAI* botAI) { return new CoaAreneInterruptionTrigger(botAI); }
    static Trigger* dispelSoi(PlayerbotAI* botAI) { return new CoaAreneDispelSoiTrigger(botAI); }
    static Trigger* survieCritique(PlayerbotAI* botAI)
    {
        return new CoaAreneSurvieTrigger(botAI, "coa arene survie critique", COA_SURVIE_CRITIQUE);
    }
    static Trigger* survieDanger(PlayerbotAI* botAI)
    {
        return new CoaAreneSurvieTrigger(botAI, "coa arene survie danger", COA_SURVIE_DANGER);
    }
    static Trigger* surviePression(PlayerbotAI* botAI)
    {
        return new CoaAreneSurvieTrigger(botAI, "coa arene survie pression", COA_SURVIE_PRESSION);
    }
    static Trigger* soutien(PlayerbotAI* botAI) { return new CoaAreneSoutienTrigger(botAI); }
};
