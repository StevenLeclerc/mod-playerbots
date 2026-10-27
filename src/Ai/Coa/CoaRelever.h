/*
 * Conquest of Azeroth -- des bots relevent les joueurs HUMAINS morts (27/10/2026).
 *
 * POURQUOI. Chez l'amont, un bot ne releve qu'un membre de SON groupe (PartyMemberValue::FindPartyMember, sans groupe
 * il ne regarde que lui-meme), et seules les strategies hors combat des pretre, paladin, chaman et druide portent
 * « party member dead ». Aucune classe CoA n'y est branchee (seul Tinker Demolition, par une regle en base inactive au
 * PTR). Le compagnon d'un humain n'est jamais dans son groupe (CoaCompagnons n'invite personne). Resultat : un humain
 * mort a cote de bots de sa faction n'etait jamais releve.
 *
 * CE QUE CA FAIT (AiPlayerbot.Coa.Relever.*, inerte par defaut) :
 *  - un REGISTRE des humains morts (CoaCompagnons::EstHumain), tenu par les crochets de mort, de resurrection et de
 *    connexion (Playerbots.cpp) ; un compteur atomique a zero rend le declencheur gratuit ;
 *  - le declencheur « coa humain a relever » (1 s, comme PartyMemberDeadTrigger) et l'action « coa relever »,
 *    portes par la strategie hors combat « coa relever », posee sur TOUS les bots (classes vanille comprises) hors
 *    champ de bataille (AiFactory::AddDefaultNonCombatStrategies) ;
 *  - le COMPAGNON eveille de l'humain passe en premier (portee PorteeCompagnon, preemption de la reservation, les
 *    autres bots attendent ATTENTE_COMPAGNON) ; avec Etrangers = 1, tout bot de la meme faction a moins de Portee
 *    yards du corps peut relever aussi ;
 *  - un seul bot a la fois par corps (reservation bornee dans le temps), Delai secondes entre deux essais ; un bot
 *    renonce a une mort apres 2 approches abandonnees ou lancers rates (4 pour le compagnon) ; au plus 2 fenetres
 *    par mort (au-dela : refus suppose, on cesse) ;
 *  - jamais pour un humain tue par un joueur, en JcJ libre (FFA), ou dont la resurrection est refusee par un module
 *    (OnPlayerCanResurrect) ; un bot en mission du Directeur ou cible de banc ne releve que son humain ; un sort a
 *    recharge de plus de 60 s (resurrection de combat) est reserve au compagnon ;
 *  - le sort est lu dans le livre du bot par ses EFFETS DBC (SPELL_EFFECT_RESURRECT 18, RESURRECT_NEW 113 ; tout sort
 *    portant SELF_RESURRECT 94 est ecarte), jamais par une liste d'ids ;
 *  - un humain LIBERE (fantome) est vise par son CADAVRE (SpellCastTargets::SetCorpseTarget) : seule voie du coeur qui
 *    le releve hors groupe (le fantome est invisible hors groupe : SpellInfo::CheckTarget -> CanSeeOrDetect). Seuls
 *    les sorts a effet 113 a cible explicite y servent : la ligne de vue vers le cadavre ne remplace celle du joueur
 *    que pour RESURRECT_NEW (Spell::CheckEffectTarget).
 *
 * FILS. Mort, Releve et HumainConnecte : n'importe quel fil (verrou). MettreAJour : FIL MONDE seulement (lit
 * CoaCompagnons::CompagnonEveille). L'action et le declencheur : fil de carte du bot ; ils ne touchent qu'un humain de
 * la MEME carte (ObjectAccessor::GetPlayer(*bot, ...)), donc du meme fil.
 *
 * Journal : « coa relever: bot= humain= sort= distance= resultat= ».
 */

#ifndef _PLAYERBOT_COARELEVER_H
#define _PLAYERBOT_COARELEVER_H

#include "Define.h"
#include "MovementActions.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Trigger.h"

class Player;
class PlayerbotAI;

namespace CoaRelever
{
// N'importe quel fil. Crochet OnPlayerJustDied : inscrit un humain mort au registre (option eteinte : rien).
void Mort(Player* humain);
// N'importe quel fil. Crochet OnPlayerResurrect : retire l'humain du registre, journal « releve ».
void Releve(Player* humain);
// N'importe quel fil. Crochet OnPlayerPVPKill : un humain tue par un joueur (ou un bot) n'est releve par personne
// (sinon : releve devant son tueur, retue, et ainsi de suite).
void TueParJoueur(Player* tueur, Player* tue);
// N'importe quel fil. Crochet OnPlayerLogin : un humain qui se connecte mort (fantome) est inscrit.
void HumainConnecte(Player* humain);
// FIL MONDE (PlayerbotsWorldScript::OnUpdate). Une fois par seconde : compagnon eveille de chaque mort, purge des
// deconnectes et des inscriptions trop vieilles.
void MettreAJour(uint32 diff);
}  // namespace CoaRelever

class CoaReleverStrategy : public Strategy
{
public:
    CoaReleverStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "coa relever"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

class CoaHumainAReleverTrigger : public Trigger
{
public:
    CoaHumainAReleverTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa humain a relever", 1000) {}

    bool IsActive() override;
};

class CoaReleverAction : public MovementAction
{
public:
    CoaReleverAction(PlayerbotAI* botAI) : MovementAction(botAI, "coa relever") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// Fabriques construites par defaut, donc SANS freres (P-130) : poser « coa relever » ne retire rien du moteur.
class CoaReleverStrategyContext : public NamedObjectContext<Strategy>
{
public:
    CoaReleverStrategyContext() { creators["coa relever"] = &CoaReleverStrategyContext::coa_relever; }

private:
    static Strategy* coa_relever(PlayerbotAI* botAI) { return new CoaReleverStrategy(botAI); }
};

class CoaReleverActionContext : public NamedObjectContext<Action>
{
public:
    CoaReleverActionContext() { creators["coa relever"] = &CoaReleverActionContext::coa_relever; }

private:
    static Action* coa_relever(PlayerbotAI* botAI) { return new CoaReleverAction(botAI); }
};

class CoaReleverTriggerContext : public NamedObjectContext<Trigger>
{
public:
    CoaReleverTriggerContext() { creators["coa humain a relever"] = &CoaReleverTriggerContext::coa_humain_a_relever; }

private:
    static Trigger* coa_humain_a_relever(PlayerbotAI* botAI) { return new CoaHumainAReleverTrigger(botAI); }
};

#endif
