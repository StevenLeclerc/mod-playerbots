/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MERCENARYREWARDS_H
#define PLAYERBOTS_MERCENARYREWARDS_H

#include "ObjectGuid.h"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <deque>
#include <shared_mutex>
#include <unordered_map>

class Player;

// --- PvP mercenaire (CoA) ---------------------------------------------------
//
// Progression des mercenaires par le PvP seul. Hors champ de bataille, le coeur
// ne donne AUCUNE experience pour un joueur tue : KillRewarder::_InitXP exige
// `!_isPvP`, et _RewardPlayer exige `!_isPvP || _isBattleGround`. Rien non plus
// du cote de l'honneur quand la victime est de la meme faction, le royaume
// n'etant pas FFA (World::IsFFAPvPRealm, GameType != 16). Un mercenaire qui tue
// un autre bot ne gagne donc rien : c'est ce que cette classe repare.
//
// Elle porte quatre choses, et rien d'autre :
//   1. la recompense d'experience, avec ses garde-fous ;
//   2. la treve de ciblage qui suit une mort, pour qu'on ne soit pas retue au
//      cimetiere ;
//   3. la mediane des niveaux des bots en ligne, qui plafonne la montee d'un
//      mercenaire ;
//   4. la purge periodique de ses propres tables.
//
// CONCURRENCE. OnPlayerPVPKill part depuis Unit::Kill, donc depuis l'un des
// fils de MapUpdate.Threads, et IsUnderTruce est lu depuis le choix de cible de
// chaque bot. Les deux tables sont donc protegees par un verrou unique. Ce soin
// n'est pas de la prudence de principe : c'est P-050, deux conteneurs globaux
// sans verrou touches depuis dix fils, et le monde qui tombe.
//
// LE VERROU EST UN std::shared_mutex, ET C'EST LA LECTURE QUI L'EXIGE. La
// version precedente prenait un std::mutex et se reposait sur un chemin rapide
// << aucune treve armee, aucun verrou >>, garde par le compteur atomique
// _activeTruces. Ce chemin rapide ne se realise JAMAIS en service, et l'en-tete
// affirmait donc une propriete fausse : _activeTruces est recopie de
// _truces.size(), qui compte aussi les treves EXPIREES jusqu'a la purge, et la
// purge ne passe que toutes les 60 s (PURGE_PERIOD_MS) alors que
// TruceAfterDeathSec vaut 60 pour ~250 mercenaires qui se battent en continu :
// la table ne se vide jamais. Chaque appel prenait donc le verrou exclusif, et
// l'appelant est NearestEnemyPlayersValue::AcceptUnit (EnemyPlayerValue.cpp),
// evalue pour chaque candidat de chaque mercenaire, depuis les trois fils de
// carte les plus charges. C'etait un point de serialisation unique sur le
// chemin le plus frequent du serveur. En shared_mutex, ces lectures ne se
// bloquent plus entre elles ; seules les poses de treve et les purges, rares,
// prennent l'exclusivite. Le chemin rapide _activeTruces est conserve : il ne
// mord pas en service, mais il reste juste et gratuit quand WildPvp est eteint.
//
// CE QUE CE CHOIX COUTE, DIT FRANCHEMENT. Un shared_mutex de la glibc est par
// defaut favorable aux LECTEURS : un ecrivain peut theoriquement attendre tant
// que des lecteurs se succedent sans interruption. Le risque est ici tenu pour
// negligeable, et par raisonnement, pas par mesure : les sections critiques en
// lecture sont d'une recherche dans une table de hachage, trois fils de carte
// seulement les prennent, et les ecrivains sont rares (une pose par mort PvP,
// une purge par minute). Si un jour le nombre de fils de carte monte ou que la
// section critique s'allonge, c'est la premiere hypothese a remettre en cause.
//
// CE QUE CETTE CLASSE NE PROTEGE PAS, ET QU'ELLE EXPOSE DAVANTAGE. Le verrou
// ci-dessus ne couvre que _killers et _truces. EstMercenaire, lui, appelle
// RandomPlayerbotMgr::IsRandomBot (RandomPlayerbotMgr.cpp:2206-2213), qui lit
// `currentBots` -- un std::unordered_set SANS AUCUN VERROU
// (RandomPlayerbotMgr.h:252), mute depuis le fil du monde : insert a
// RandomPlayerbotMgr.cpp:756 et :2277, erase a :1355, :1436 et :2748. Un
// `contains()` concurrent d'un rehash est un COMPORTEMENT INDEFINI, pas une
// lecture perimee : c'est la forme exacte de P-050. Le defaut preexiste a cette
// classe et sa correction est dans RandomPlayerbotMgr, pas ici -- mais c'est
// EstMercenaire qui l'a amene sur le chemin de selection de cible
// (NearestEnemyPlayersValue::AcceptUnit, trois fils de carte, a chaque tick),
// donc c'est ici qu'il doit etre ecrit. Attenuation en place, et rien de plus :
// EnemyPlayerValue.cpp memorise le resultat par bot et par tick, ce qui divise
// la frequence par le nombre de candidats evalues sans rendre l'appel sur.
// Quiconque ajoute un appelant de EstMercenaire sur un chemin chaud herite de
// ce risque et doit faire de meme.
//
// AUCUN VERROU N'EST TENU PENDANT UN APPEL AU CODE DE JEU. Player::GiveXP part
// de OnPvpKill hors de toute section critique, et la consignation du kill
// (NoteKillPaid) ne vient qu'apres. C'est une regle du projet, pas une
// coincidence : monter de niveau declenche des crochets de script dont on ne
// sait pas ce qu'ils reprennent.
//
// LA MEDIANE, ELLE, N'A AUCUN ETAT. Elle etait tenue par un histogramme
// incremente a la connexion et decremente a la deconnexion ; ce comptage etait
// faux de deux facons, et muettement :
//   - RandomPlayerbotMgr::ProcessBot fait `currentBots.erase(bot)` AVANT
//     `LogoutPlayerBot` dans son chemin << bot hors groupe >>
//     (RandomPlayerbotMgr.cpp:1356-1358 ; son autre chemin, :1435-1436, fait
//     l'inverse), or IsRandomBot n'est rien d'autre que `currentBots.contains`
//     : le crochet OnPlayerLogout trouvait alors deja faux le predicat qui
//     avait ete vrai a la connexion, et le decrement etait saute ;
//   - le predicat depend aussi de IsMercenary, donc de WildPvp.Enabled et de
//     MercenaryPercent, rechargeables a chaud par `.bot reload` et
//     `.rndbot reload` : un rechargement laissait des entrees fantomes pour la
//     duree de vie du processus.
// Les entrees fantomes ne se voyaient nulle part, et la mediane est l'UNIQUE
// entree du plafond de montee (ComputeReward). On rebalaie donc la population
// en ligne toutes les 30 s : sans etat, rien ne peut deriver.
class MercenaryRewards
{
public:
    static MercenaryRewards& instance()
    {
        static MercenaryRewards instance;
        return instance;
    }

    // Appele depuis un WorldScript : purge les tables et recalcule la mediane.
    void Update(uint32 diff);

    // Un joueur (bot ou humain) en a tue un autre. Decide et verse la
    // recompense, puis arme la treve de la victime. Les deux parties sont
    // independantes : une victime beneficie de la treve meme quand le tueur
    // n'a droit a rien.
    void OnPvpKill(Player* killer, Player* killed);

    // Vrai tant que la victime est sous treve : aucun mercenaire ne doit la
    // convoiter. Chemin chaud, appele a chaque choix de cible.
    bool IsUnderTruce(uint64 guid) const;

    // 0 quand la population est trop maigre pour qu'une mediane veuille dire
    // quelque chose : l'appelant doit alors renoncer au plafond, pas le serrer.
    uint8 MedianBotLevel() const { return _medianBotLevel.load(std::memory_order_relaxed); }

    // LE camp mercenaire, en un seul endroit. Deux conditions, dans cet ordre :
    // le hachage de GUID (IsMercenary, quelques instructions) puis la nature du
    // bot (IsRandomBot, une recherche de cache et un parcours de la liste des
    // comptes aleatoires). Sans IsRandomBot, IsMercenary hache aussi bien le
    // GUID d'un personnage humain ou d'un alt ajoute par `.bot add`, et en fait
    // un mercenaire a l'insu de son proprietaire.
    //
    // Ce predicat existe parce que la regle etait ecrite a la main sur plusieurs
    // sites qui ne la traduisaient pas tous pareil : ComputeReward exigeait
    // IsRandomBot, MercenaryAttackSlotFree non. Une regle, un endroit.
    //
    // La forme par uint64 (PlayerbotAIConfig::IsMercenary) reste la bonne pour
    // le panneau et les sondes, qui n'ont pas de Player* sous la main.
    static bool EstMercenaire(Player* bot);

    // Comptes rendus, pour la sonde et la console. Le releve periodique de
    // Update() les emploie ; ce sont eux, et non les membres, qui sont la
    // surface publique de ces compteurs.
    uint32 KillsRewarded() const { return _killsRewarded.load(std::memory_order_relaxed); }
    uint32 KillsRefused() const { return _killsRefused.load(std::memory_order_relaxed); }

private:
    MercenaryRewards() = default;
    ~MercenaryRewards() = default;

    MercenaryRewards(MercenaryRewards const&) = delete;
    MercenaryRewards& operator=(MercenaryRewards const&) = delete;

    // Ce que le tueur a fait recemment. Deux garde-fous distincts : ne pas
    // payer deux fois la meme victime, et ne pas payer plus de N fois par
    // heure quelle que soit la victime.
    struct KillerRecord
    {
        std::unordered_map<uint64, time_t> LastKillOf;
        std::deque<time_t> RecentKills;
    };

    // << Puis-je payer, et combien ? >> Renvoie 0 quand un garde-fou refuse, et
    // incremente alors le compteur de la cause. N'ECRIT RIEN dans _killers :
    // c'est tout l'objet de la separation d'avec NoteKillPaid. Prend le verrou
    // en lecture partagee.
    //
    // Avant, cette fonction consignait le kill dans les tables anti-farm au
    // moment ou elle accordait l'experience -- c'est-a-dire AVANT que
    // Player::GiveXP ait dit s'il versait quoi que ce soit. GiveXP sort sans un
    // mot dans plusieurs cas (Player.cpp:2469 xp<1, :2472 tueur mort hors champ
    // de bataille, :2475 PLAYER_FLAGS_NO_XP_GAIN / NO_PLAY_TIME, :2501 niveau
    // maximum, :2518 xp retombe sous 1 apres multiplicateurs). Cas concret et
    // frequent : une mort mutuelle, ou un poison qui acheve la cible au tick
    // suivant -- Unit::Kill declenche le crochet avec un tueur DEJA MORT. Le
    // mercenaire brulait alors un de ses credits horaires et posait 900 s de
    // delai anti-recidive sur cette victime pour un paiement nul.
    uint32 ComputeReward(Player* killer, Player* killed);

    // << Le paiement a eu lieu : je le consigne. >> Debite le quota horaire et
    // arme le delai anti-recidive. Appele UNIQUEMENT apres avoir constate que
    // l'experience du tueur a effectivement bouge. Prend le verrou en exclusif.
    void NoteKillPaid(uint64 killerGuid, uint64 victimGuid);

    // Un bot compte dans la mediane s'il fait partie de la population CHASSEE :
    // bot aleatoire, et pas mercenaire lui-meme.
    static bool CompteDansLaMediane(Player* bot);

    // Rebalaie la population en ligne et republie la mediane. Appele depuis le
    // fil du monde uniquement (MercenaryRewardsWorldScript::OnUpdate). Le
    // parcours se fait sans verrou : la demonstration, qui ne tient PAS a la
    // seule nature de la table, est dans le corps de la fonction, paragraphe
    // FIL.
    void RecomputeMedian();

    mutable std::shared_mutex _mutex;
    std::unordered_map<uint64, KillerRecord> _killers;
    std::unordered_map<uint64, time_t> _truces;

    // Chemin chaud : tant qu'il vaut 0, IsUnderTruce sort sans prendre meme le
    // verrou partage. Il compte les treves POSEES, expirees comprises, jusqu'a
    // la purge : il ne retombe donc a 0 que quand plus personne ne meurt. Voir
    // l'en-tete, paragraphe LE VERROU.
    std::atomic<uint32> _activeTruces{ 0 };

    std::atomic<uint8> _medianBotLevel{ 0 };

    // Compteurs separes par CAUSE. Un seul compteur de refus ne permettait pas
    // de distinguer << personne ne tue >> de << des gens tuent, mais jamais un
    // mercenaire >> : les deux donnaient zero recompense et zero refus, parce
    // qu'un tueur non mercenaire sortait sans rien compter du tout.
    //
    // L'INVARIANT QUE CES COMPTEURS DOIVENT FERMER, et que le releve publie :
    //   _pvpKillsSeen == _killerNotMercenary + _killsRewarded + _killsRefused
    //   _killsRefused == gris + plafond + recidive + cadence
    //                  + desarme + niveauMax + tiers + nonVerse
    // Les quatre dernieres causes sont NOUVELLES : elles etaient des `return 0`
    // muets, et l'ecart qu'elles creusaient ne se voyait nulle part. La plus
    // frequente en service est le niveau maximum -- des qu'un mercenaire atteint
    // CONFIG_MAX_PLAYER_LEVEL, chacun de ses meurtres faisait monter
    // _pvpKillsSeen et rien d'autre. Le releve emet desormais l'ecart sous le
    // nom `non explique` : un futur chemin muet se denoncera tout seul.
    std::atomic<uint32> _pvpKillsSeen{ 0 };       // toute mort joueur par joueur
    std::atomic<uint32> _killerNotMercenary{ 0 }; // tueur hors du camp mercenaire
    std::atomic<uint32> _killsRewarded{ 0 };
    std::atomic<uint32> _killsRefused{ 0 };
    std::atomic<uint32> _refusedGrey{ 0 };
    std::atomic<uint32> _refusedLevelCap{ 0 };
    std::atomic<uint32> _refusedRepeat{ 0 };
    std::atomic<uint32> _refusedRate{ 0 };
    // KillXpDivisor = 0 : la conf annonce << aucune experience versee >>, et le
    // systeme entier se desarme. Sans ce compteur, rien dans le releve ne
    // changeait de forme.
    std::atomic<uint32> _refusedDisabled{ 0 };
    std::atomic<uint32> _refusedMaxLevel{ 0 };
    // Un autre module a ramene le montant a zero dans OnPlayerGiveXP, qui prend
    // le montant par reference NON CONSTANTE (mod-dynamic-xp est charge ici).
    std::atomic<uint32> _refusedByHook{ 0 };
    // Aucune experience n'a atteint le tueur : soit GiveXP est rentre sans rien
    // verser (voir ComputeReward pour la liste de ses cinq sorties muettes),
    // soit le montant calcule etait deja nul.
    std::atomic<uint32> _refusedNotPaid{ 0 };

    // Numero de sequence du releve. Avec l'uptime publie sur la meme ligne, il
    // rend la differenciation sure cote panneau : un redemarrage remet les deux
    // a zero, ce qu'aucune heuristique n'avait a deviner jusqu'ici.
    uint32 _rapportSeq = 0;

    uint32 _purgeTimer = 0;
    uint32 _medianTimer = 0;
    uint32 _rapportTimer = 0;
};

// --- L'INDEX << OU SONT LES PROIES >> (CoA) ----------------------------------
//
// LOT 5 de docs/CONCEPTION-mercenaire-chasseur.md. Le probleme qu'il ferme : la
// SEULE detection de proie d'un mercenaire porte a grindDistance, 75 yd
// (NearestEnemyPlayersValue, Ai/Base/Value/EnemyPlayerValue.h:20 ; conf en
// service : AiPlayerbot.GrindDistance = 75.0). Au-dela, la chasse du lot 2 tire
// son poste de guet a l'aveugle -- ponderee par l'enrichissement MOYEN d'une
// classe de lieu (auberge x18, volerie x9, cimetiere x3,4, carrefour x1), jamais
// par ce qui s'y trouve MAINTENANT. Deux auberges du meme palier ont le meme
// poids, que l'une soit deserte et l'autre pleine.
//
// CE QU'IL EST. Une table de cellules de 250 yd par carte, rebatie toutes les
// 30 s par le fil du monde, lue sans verrou par les fils de carte. Une cellule
// porte le NOMBRE de proies, ventile par palier de dix niveaux, ET la part de
// ce nombre qui est faite de MERCENAIRES -- voir le paragraphe << L'INDEX NE SE
// MANGE PAS LUI-MEME >> plus bas, qui dit pourquoi cette ventilation n'est pas
// un ornement de journal mais la condition pour que le facteur de tirage ne
// soit pas une boucle fermee sur elle-meme.
//
// UNE LECTURE PORTE SUR QUATRE CELLULES, PAS UNE. Le carre 2x2 le plus proche
// du point : la cellule du point, et les trois voisines du cote ou le point
// tombe dans sa cellule. La raison est au commentaire de TailleCellule.
//
// OU IL SE REMPLIT, ET POURQUOI LA. Dans le balayage que
// MercenaryRewards::RecomputeMedian fait DEJA toutes les 30 s (MEDIAN_PERIOD_MS)
// sur toute la population de bots, depuis MercenaryRewardsWorldScript::OnUpdate
// -- un WorldScript branche sur WORLDHOOK_ON_UPDATE qui sort en une instruction
// quand wildPvpEnabled est faux. L'index ne coute donc AUCUN parcours nouveau :
// il s'accroche a celui qui existe. La demonstration que ce balayage est sur
// sans verrou est dans le corps de RecomputeMedian, paragraphe FIL ; elle vaut
// telle quelle ici, puisque c'est le meme parcours.
//
// CE QU'IL N'AJOUTE PAS AU CHEMIN CHAUD, et le dossier est formel : rien n'est
// RANGE dans IsUnderTruce ni dans MercenaryAttackSlotFree
// (Ai/Base/Value/EnemyPlayerValue.cpp). Ces deux fonctions sont sur le chemin le
// plus chaud du serveur -- un appel par candidat par mercenaire, depuis trois
// fils de carte -- et l'en-tete ci-dessus documente qu'un point de serialisation
// y a ete SUPPRIME. L'index n'y ajoute rien : il n'est lu qu'a l'etat CHOIX de
// la chasse, au plus une fois toutes les dix secondes par bot
// (CoaChasseAction::RepliChoixSecondes).
//
// EN REVANCHE, Compter() APPELLE IsUnderTruce -- et c'est l'inverse du meme
// raisonnement, pas une entorse. L'interdit porte sur le cote LECTURE, ou
// l'appel se paie par candidat par mercenaire depuis trois fils de carte. Cote
// ECRITURE il se paie une fois par bot toutes les 30 s, sur le fil du monde,
// soit ~500 appels par demi-minute pour la population en service ; et
// IsUnderTruce (corps dans ce fichier) sort sur un load relaxe tant que
// _activeTruces vaut zero, et ne prend qu'un shared_lock sinon -- donc sans
// jamais se serialiser contre les fils de carte, qui prennent le meme verrou en
// partage. Ce que cela achete : une proie sous treve n'entre pas dans l'index,
// donc l'index cesse de designer comme riche l'endroit exact ou l'on vient de
// se battre et ou, pour TruceAfterDeathSec secondes, AcceptUnit
// (EnemyPlayerValue.cpp:215-220) refusera tout le monde.
//
// MercenaryAttackSlotFree, lui, N'EST PAS filtrable a l'ecriture : il depend du
// COUPLE (bot, cible) et non de la cible seule (EnemyPlayerValue.cpp:217). Il
// reste donc dans << CE QU'IL NE VOIT PAS >>, plus bas.
//
// LA CONCURRENCE, ET EN QUOI CE N'EST PAS P-050. P-050, c'est un
// std::unordered_set global mute par le fil du monde et lu par trois fils de
// carte : un contains() concurrent d'un rehash est un COMPORTEMENT INDEFINI, et
// le monde tombe. Ici il n'y a ni conteneur ni allocation : deux tableaux de
// taille FIXE de std::atomic<uint64>, et un indice atomique qui dit lequel des
// deux est publie. Le fil du monde ecrit dans celui qui n'est PAS publie, puis
// echange l'indice par un store release ; les lecteurs chargent l'indice en
// acquire puis lisent les cases en relaxed. Tout acces est atomique : il n'y a
// donc pas de course de donnees au sens de la norme, et aucun comportement
// indefini possible -- ce qui est exactement ce que P-050 n'avait pas.
//
// CE QUE CE CHOIX COUTE, DIT FRANCHEMENT. Un lecteur qui aurait charge l'indice
// puis serait suspendu plus de 30 s (MEDIAN_PERIOD_MS) verrait le fil du monde
// reecrire SOUS LUI le tampon qu'il lit, et rendrait un melange de deux
// generations. Le dommage est alors un NOMBRE DE PROIES FAUX, jamais une
// destination illegale : le seul juge d'une destination reste CoaLieuSansPvp,
// appele par CoaChasseAction::EstChassable (INVARIANT 3 du dossier), et l'index
// n'intervient que comme ponderation de tirage. Une lecture dure quelques
// centaines de nanosecondes ; le risque est tenu pour negligeable par
// raisonnement, pas par mesure.
//
// L'INDEX NE SE MANGE PAS LUI-MEME -- et sans cette regle, il se mangerait.
//
// LE DEFAUT, TEL QU'IL ETAIT. EstProie n'exclut aucune identite : tout ce qui
// porte le bit FFA est recense. Or un mercenaire porte ce bit par construction
// -- meriteLeDrapeauFFA (Bot/PlayerbotAI.cpp) le pose des que wildPvpEnabled,
// niveau >= wildPvpMinLevel, IsMercenary et !CoaLieuSansPvp, c'est-a-dire aux
// conditions memes de CoaChasseAction::isUseful (Ai/Coa/CoaChasse.h). Deux
// consequences, et les deux etaient fausses :
//   - un mercenaire figurait dans SA PROPRE cellule au moment ou il
//     l'interrogeait. Seul dans un desert, il se lisait 1 proie, donc facteur 2
//     sur tout poste a portee de vue de la ou il se tenait deja, contre 1 pour
//     le reste de la carte : l'index poussait a NE PAS bouger, soit l'inertie
//     exacte que le chantier existe pour guerir. Et le filtre de niveau ne le
//     rattrapait pas, un ecart de 0 passant (PossibleTargetsValue.cpp:94) ;
//   - avec MercenaryPercent = 50 (playerbots.conf:2810), environ la moitie de
//     ce que l'index appelait << proies >> etait faite de CHASSEURS. Le tirage
//     envoyait k mercenaires dans une cellule, le balayage suivant les y
//     recensait, la cellule gardait son facteur, et ainsi de suite : un
//     attracteur auto-entretenu, dont rien dans la ligne de journal ne
//     permettait de voir qu'il l'etait.
//
// CE QU'ON FAIT, ET POURQUOI C'EST CELA. La cellule porte deux comptes : les
// proies, et la part mercenaire. Le facteur de tirage (CoaChasse.h,
// FacteurPresence) n'emploie que la DIFFERENCE, c'est-a-dire la population
// EXOGENE. La justification n'est pas que les mercenaires ne seraient pas des
// proies -- ils le sont, BotsFightBots = 1 et EnemyPlayerValue.cpp:202-204 les
// convoite -- mais qu'ils ne portent AUCUNE information : leur position est la
// SORTIE de l'index, et remettre une sortie dans son entree ne mesure plus rien.
// La population non mercenaire, elle, est placee par le gestionnaire de bots et
// par le monde, independamment de ce que la chasse decide.
//
// ET CELA REGLE LE PREMIER POINT SANS AUCUN TEST D'IDENTITE. Le lecteur est
// mercenaire (isUseful l'exige) ou n'est pas dans l'index du tout (Compter()
// n'est appele que sur les bots de sRandomPlayerbotMgr) : dans les deux cas il
// ne peut plus peser sur son propre tirage. C'est ce qui a fait preferer cette
// forme a un retranchement du lecteur par comparaison de position, qui aurait
// compare la position d'AUJOURD'HUI a un recensement d'il y a jusqu'a 30 s.
//
// CE QUE CE CHOIX COUTE, DIT FRANCHEMENT : le facteur ignore une moitie de la
// population reellement frappable. Il SOUS-ESTIME donc, partout et du meme
// ordre, ce qui deplace peu le classement relatif des postes -- alors que la
// boucle, elle, ne deplacait qu'un endroit et toujours le meme.
//
// CE QU'IL NE VOIT PAS, et il faut le savoir avant de lire ses chiffres :
//   - il ne recense que les bots de sRandomPlayerbotMgr, seule population que le
//     balayage porteur parcourt. Un JOUEUR humain marque FFA (mode High Risk,
//     AscensionRulesets.cpp) n'y figure pas. Il reste trouve par la detection a
//     75 yd, comme avant ce lot ;
//   - il ne voit pas MercenaryAttackSlotFree, donc pas le quota
//     MaxAttackersPerTarget = 1 (playerbots.conf:2827) : trois proies deja
//     engagees chacune par un attaquant comptent pour trois et n'en valent
//     aucune. Ce refus-la depend du couple (bot, cible) et n'est pas calculable
//     a l'ecriture ; c'est la seule des deux bornes d'AcceptUnit qui reste
//     invisible a l'index, la treve etant desormais filtree par Compter() ;
//   - c'est un INSTANTANE de 30 s, et le dossier previent (section 5, << la
//     persistance dans le temps >>) que la population se deplace : 25 proies
//     relevees sur un poste, 18 une heure plus tard au metre pres. L'index dit
//     << il y avait du monde la il y a moins de 30 s >>, pas << il y en aura
//     quand le bot arrivera >>. C'est pourquoi il PONDERE un tirage au lieu de
//     le decider.
class CoaIndexProies
{
public:
    static CoaIndexProies& instance()
    {
        static CoaIndexProies instance;
        return instance;
    }

    // Cote de la cellule, en yards. CONSTANTE DE COMPILATION ET PAS OPTION DE
    // CONF : elle entre dans la CLE, et un rechargement a chaud qui la changerait
    // entre l'ecriture et la lecture ferait chercher des cles qui n'existent pas,
    // en silence. 250 yd : un poste de guet est un point, les proies bougent, et
    // la borne d'engagement du coeur est de 60 yd (EnemyPlayerValue.cpp:315-318)
    // -- une cellule trop fine dirait << desert >> pour une proie a 80 yd.
    //
    // CE QU'UN DECOUPAGE FIXE FAIT PERDRE AU BORD, et c'est ce qui a impose la
    // lecture en 2x2. Une cellule n'est pas centree sur le poste : un poste a
    // 3 yd d'une coupure ne voit rien de l'autre cote, et voit en revanche
    // jusqu'au coin oppose, a 350 yd, qui ne l'interessera jamais. Le defaut ne
    // depend pas de la densite reelle de proies mais de la POSITION du poste
    // dans sa cellule, et il ne s'amortit a aucune taille de cellule tant qu'on
    // ne lit qu'une case. Pire, le plafond a 4 (CoaChasse.h, PlafondProies)
    // l'amplifie au lieu de l'attenuer : tout le pouvoir discriminant de l'index
    // se joue dans la plage 0-4 proies, exactement celle ou une coupure fait
    // basculer le compte de 4 a 0.
    //
    // LA REPONSE : Proies() somme les QUATRE cellules du carre 2x2 le plus
    // proche du point -- la sienne, et les voisines du cote ou le point tombe
    // dans sa cellule. Tout point est alors a au plus 125 yd de chacun des deux
    // bords du bloc qui le regardent, la ou une lecture simple pouvait le
    // laisser a 0 yd d'un bord. Ce qui reste, et qu'on ne cherche pas a
    // supprimer : le bloc porte 500 yd de cote, donc il compte encore des proies
    // hors des 75 yd de NearestEnemyPlayersValue. L'index PONDERE un tirage, il
    // ne decide d'aucune cible.
    static constexpr float TailleCellule = 250.0f;

    // Puissance de deux : l'indice de sondage est un ET binaire, jamais un
    // modulo. 4096 cases pour au plus MaxRandomBots cles distinctes (500 en
    // service, une par bot au pire) : taux de remplissage sous 15 %, ou le
    // sondage lineaire ne degrade pas.
    static constexpr uint32 Cases = 4096;

    // Un sondage borne. Au-dela, la proie n'est pas comptee et le debordement
    // est publie : un index sature doit se voir dans le journal, pas se deviner.
    static constexpr uint32 MaxSondages = 64;

    // Paliers de dix niveaux, huit paliers (0-9, 10-19, ... 70-79 et 80).
    static constexpr uint32 TaillePalier = 10;
    static constexpr uint32 Paliers = 8;

    // --- ECRITURE : FIL DU MONDE UNIQUEMENT -----------------------------------
    // Les trois s'appellent dans cet ordre, une fois par balayage. Debuter()
    // choisit le tampon cache et l'efface, Compter() y range une proie, Publier()
    // echange les tampons. Rien d'autre n'ecrit.
    // FIL DU MONDE, ET PAS SEULEMENT PAR CONVENTION : Compter() appelle
    // MercenaryRewards::EstMercenaire, donc RandomPlayerbotMgr::IsRandomBot, qui
    // lit `currentBots` SANS VERROU (voir l'en-tete de MercenaryRewards, P-050).
    // Sur le fil du monde -- celui qui mute ce conteneur -- la lecture est sure ;
    // depuis un fil de carte elle serait un comportement indefini.
    void Debuter();
    void Compter(Player* joueur);
    void Publier();

    // --- LECTURE : N'IMPORTE QUEL FIL -----------------------------------------
    // Nombre de proies recensees dans le BLOC 2x2 le plus proche de (carte, x, y)
    // dont le palier n'est pas categoriquement refuse par le filtre de niveau de
    // l'attaque pour un chasseur de ce niveau. Rend 0 sur un lieu hors bornes,
    // sur une coordonnee non finie, ou quand l'index est vide.
    //
    // `mercenaires`, quand il est fourni, recoit la part de ce nombre qui est
    // faite de mercenaires. C'est le SEUL moyen d'obtenir la population exogene
    // -- voir << L'INDEX NE SE MANGE PAS LUI-MEME >> ci-dessus -- et c'est
    // pourquoi la difference n'est pas calculee ici : le journal a besoin des
    // deux termes, le tirage n'a besoin que de la difference.
    uint32 Proies(uint32 carte, float x, float y, uint8 niveauChasseur,
                  uint32* mercenaires = nullptr) const;

    // Comptes rendus du dernier balayage publie, pour la ligne de journal.
    uint32 Cellules() const { return _cellules.load(std::memory_order_relaxed); }
    uint32 Recensees() const { return _recensees.load(std::memory_order_relaxed); }
    uint32 Debordements() const { return _debordements.load(std::memory_order_relaxed); }
    // Proies ecartees parce que leur lieu ne tient pas dans l'encodage de la cle
    // (carte au-dela de 4095, coordonnee non finie ou aberrante). Doit rester a
    // zero : au-dela, une part de la population est invisible a l'index sans que
    // rien d'autre ne le dise.
    uint32 HorsBornes() const { return _horsBornes.load(std::memory_order_relaxed); }
    // Part mercenaire de Recensees(). Elle ne pese PAS sur le tirage ; elle est
    // publiee pour que la ligne de journal reste falsifiable -- sans elle, nul
    // ne peut dire si un facteur eleve vient des proies ou des chasseurs.
    uint32 Mercenaires() const { return _mercenaires.load(std::memory_order_relaxed); }
    // Proies ecartees par la treve de TruceAfterDeathSec. Un nombre eleve dit
    // qu'on vient de beaucoup se battre, et que l'index a EVITE d'y renvoyer.
    uint32 SousTreve() const { return _sousTreve.load(std::memory_order_relaxed); }

    // CE QUI FAIT UNE PROIE, en un seul endroit. Le drapeau FFA porte deja tout
    // le reste : c'est le module qui le pose (Bot/PlayerbotAI.cpp, meriteLeDrapeauFFA),
    // et le lot 1 l'a subordonne a CoaLieuSansPvp -- un bot en sanctuaire ou en
    // capitale ne le porte donc PAS.
    //
    // CE QUE CELA ETABLIT, ET CE QUE CELA N'ETABLIT PAS. Une redaction
    // precedente concluait que l'index ne contient << que des proies situees
    // dans un lieu ou l'on peut frapper >>. C'est vrai du LIEU et faux de la
    // PROIE : le predicat d'acceptation reel (NearestEnemyPlayersValue::AcceptUnit,
    // EnemyPlayerValue.cpp:215-220) ajoute deux refus. La treve est desormais
    // filtree, mais par Compter() et non ici -- elle depend d'un etat de
    // MercenaryRewards, pas du joueur. Le quota MaxAttackersPerTarget, lui,
    // depend du couple (bot, cible) et reste invisible a l'index.
    static bool EstProie(Player* joueur);

private:
    CoaIndexProies();
    ~CoaIndexProies() = default;

    CoaIndexProies(CoaIndexProies const&) = delete;
    CoaIndexProies& operator=(CoaIndexProies const&) = delete;

    // Cle de cellule SANS le palier (bits de palier a zero) : elle ne depend que
    // du lieu, donc un seul calcul sert aux huit paliers d'une lecture. Rend
    // false quand le lieu ne tient pas dans l'encodage -- voir le corps.
    // Ecriture seulement : elle ne designe QUE la cellule du point.
    static bool CleDuLieu(uint32 carte, float x, float y, uint64& cle);

    // Les indices de cellule BIAISES du point, plus ceux de la voisine du cote
    // ou le point tombe dans sa cellule -- de quoi fabriquer les quatre cles du
    // bloc 2x2 sans recalculer un seul floor. Rend false aux memes conditions
    // que CleDuLieu, dont elle porte desormais tous les controles.
    static bool IndicesDuLieu(uint32 carte, float x, float y, int32& celluleX,
                              int32& celluleY, int32& voisinX, int32& voisinY);

    // Compose une cle a partir d'indices DEJA biaises. Rend false quand l'un
    // d'eux sort des dix bits : c'est ainsi qu'une voisine hors du monde est
    // ecartee, sans cas particulier chez l'appelant.
    static bool CleDeCellule(uint32 carte, int32 celluleX, int32 celluleY, uint64& cle);

    static uint32 Empreinte(uint64 cle);

    void Ajouter(uint64 cle, bool estMercenaire);

    // Deux tampons de taille fixe, jamais realloues, jamais liberes : c'est ce
    // qui rend la lecture concurrente sure sans verrou. 2 x 4096 x 8 = 64 Kio.
    std::atomic<uint64> _table[2][Cases];

    // Le tampon PUBLIE. Ecrit en release par Publier(), lu en acquire par
    // Proies() : la paire ordonne les ecritures de cases avant les lectures.
    std::atomic<uint8> _publie{ 0 };

    // Etat du balayage en cours. Fil du monde seulement : pas d'atomique, et
    // c'est voulu -- les rendre atomiques laisserait croire qu'un autre fil les
    // touche.
    uint8 _enEcriture = 1;
    uint32 _cellulesEnCours = 0;
    uint32 _recenseesEnCours = 0;
    uint32 _debordementsEnCours = 0;
    uint32 _horsBornesEnCours = 0;
    uint32 _mercenairesEnCours = 0;
    uint32 _sousTreveEnCours = 0;

    std::atomic<uint32> _cellules{ 0 };
    std::atomic<uint32> _recensees{ 0 };
    std::atomic<uint32> _debordements{ 0 };
    std::atomic<uint32> _horsBornes{ 0 };
    std::atomic<uint32> _mercenaires{ 0 };
    std::atomic<uint32> _sousTreve{ 0 };
};

void AddSC_mercenary_rewards();

#endif // PLAYERBOTS_MERCENARYREWARDS_H
