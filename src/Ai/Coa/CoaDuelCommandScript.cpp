/*
 * Conquest of Azeroth — remettre un bot a plein, depuis la console.
 *
 * POURQUOI CETTE COMMANDE EXISTE
 *
 * Le banc de duels appaires oppose deux bots de meme classe et meme niveau,
 * l'un pilote par l'oracle Laya, l'autre laisse au tourniquet. Pour que les
 * manches se comparent, chacune doit partir a egalite.
 *
 * `.revive` ne suffit PAS, et c'est une mesure, pas une supposition. Le code
 * promet pourtant 100 % : HandleReviveCommand appelle
 * `ResurrectPlayer(1.0f)` des lors que l'appelant n'est pas un compte joueur,
 * et ResurrectPlayer fait `SetHealth(GetMaxHealth() * 1.0f)`. Mesure du
 * 2026-09-23 sur trois bots ressuscites depuis la console RA :
 *
 *     Vythan   classe 26 niveau 21    367 pv   contre 483 chez un pair
 *     Irnim    classe 16 niveau 17    211 pv   contre 416
 *     Fermilla classe 15 niveau 25    332 pv   contre 505
 *
 * Entre 51 et 76 % de la reference, et pas le meme ratio d'un cas a l'autre.
 * L'explication tient dans l'ordre des operations : a l'instant de la
 * resurrection, la vie maximale n'a pas encore ete recalculee — les auras
 * d'equipement et les bonus de caracteristiques ne sont pas tous reappliques.
 * `GetMaxHealth()` rend donc une valeur intermediaire, et 100 % de cette
 * valeur-la ne vaut pas 100 % de la vraie.
 *
 * D'ou cette commande, qui force le recalcul AVANT de remplir.
 *
 * CE QU'ELLE FAIT DE LA RESSOURCE, ET POURQUOI
 *
 * Elle suit la convention du coeur (Player::ResurrectPlayer) : mana, energie
 * et focus a plein, rage et puissance runique a zero. Ce sont des ressources
 * qui se GAGNENT au combat ; les donner pleines fausserait l'ouverture. Les
 * deux adversaires d'une paire etant de meme classe, la convention est
 * symetrique quelle qu'elle soit — mais autant qu'elle soit realiste.
 *
 *     .coa plein <nom>     remet ce bot a plein, le ressuscite s'il est mort
 *
 * ET POURQUOI IL Y A MAINTENANT UNE SECONDE COMMANDE
 *
 *     .coa niveau <nom> <niveau> [qualite]
 *
 * La premiere campagne de duels a oppose des bots de niveau 14 a 31. A ce
 * niveau-la un bot CoA dispose de trois a cinq sorts d'attaque : il n'y a
 * presque rien a ordonner, et un oracle qui ordonne trois sorts ne peut pas se
 * distinguer d'un tourniquet qui en fait autant. C'est la limite la plus
 * plausible du resultat negatif mesure (947 manches, -0,7 point +/- 3,9).
 *
 * Or aucun bot du serveur ne depasse le niveau 37, et la montee automatique ne
 * peut pas y remedier : `AiPlayerbot.RandomBotMaxLevel` vaut 1 dans la conf en
 * service, donc `IncreaseLevel` plafonne a 1 et ne fait rien. Les niveaux
 * observes viennent de l'experience gagnee en jeu.
 *
 * Cette commande monte donc un bot au niveau demande et lui refait son kit
 * avec la recette EXACTE que le module applique deja a chaque montee de niveau
 * (AutoMaintenanceOnLevelupAction::LearnTrainerSpells) : competences, sorts de
 * classe, sorts disponibles, familier — puis l'equipement.
 *
 * LE PIEGE QU'ELLE DESAMORCE. `RandomPlayerbotMgr::Randomize` rappelle
 * `PlayerbotFactory::Randomize(false)` avec un niveau tire entre
 * RandomBotMinLevel et RandomBotMaxLevel, tous deux a 1 : un bot monte a 60
 * serait donc RAMENE AU NIVEAU 1 a la prochaine echeance de l'evenement
 * « randomize », entre 2 heures et 14 jours plus tard. La commande repousse
 * cette echeance de `AiPlayerbot.MaxRandomBotInWorldTime` secondes (8 h dans
 * la conf en service) en posant l'evenement a la main. Une campagne plus
 * longue doit donc rejouer la commande.
 *
 * LA QUALITE D'EQUIPEMENT EST UN ARGUMENT, ET CE N'EST PAS UN DETAIL. Quatre
 * « resultats significatifs » de la campagne precedente se sont reveles etre
 * des artefacts d'equipement. Passer la meme qualite aux deux bots d'une paire
 * retire cette variable du plan d'experience au lieu d'esperer qu'elle
 * s'annule.
 */

#include "Chat.h"
#include "CharacterCache.h"
#include "Group.h"
#include "GroupMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "World.h"

#include <cstdlib>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

class coa_duel_commandscript : public CommandScript
{
public:
    coa_duel_commandscript() : CommandScript("coa_duel_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable coaTable =
        {
            // Console::Yes : la console RA n'a ni personnage ni cible
            // selectionnee, le nom est donc le seul moyen de designer un bot.
            { "plein",  HandleCoaPleinCommand,  SEC_GAMEMASTER, Console::Yes },
            { "niveau", HandleCoaNiveauCommand, SEC_GAMEMASTER, Console::Yes },
            { "enrole", HandleCoaEnroleCommand, SEC_GAMEMASTER, Console::Yes },
            { "groupe", HandleCoaGroupeCommand, SEC_GAMEMASTER, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "coa", coaTable },
        };
        return commandTable;
    }

    static bool HandleCoaPleinCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            // ChatHandler::PSendSysMessage passe par Acore::StringFormat (fmt) :
            // les marqueurs sont {}, jamais %u ni %s.
            handler->PSendSysMessage("Usage: .coa plein <nom>");
            return false;
        }

        std::string nom(args);
        // Un nom colle a une espace de fin arrive tel quel ; le laisser ferait
        // echouer la recherche sans le dire.
        while (!nom.empty() && (nom.back() == ' ' || nom.back() == '\r' || nom.back() == '\n'))
            nom.pop_back();
        while (!nom.empty() && nom.front() == ' ')
            nom.erase(nom.begin());
        if (nom.empty())
        {
            handler->PSendSysMessage("Usage: .coa plein <nom>");
            return false;
        }

        Player* cible = ObjectAccessor::FindPlayerByName(nom, true);
        if (!cible)
        {
            handler->PSendSysMessage("coa plein: {} est introuvable ou hors du monde.", nom);
            return false;
        }

        bool const etaitMort = !cible->IsAlive();

        // L'ORDRE COMPTE, et c'est tout l'objet de cette commande : recalculer
        // la vie maximale AVANT de remplir. Sans cela on remplit une valeur
        // intermediaire, ce que fait `.revive` et ce qui l'a rendu inutilisable
        // pour un banc de duels. Le detail est dans RemettreAPlein, partage
        // avec `.coa niveau` pour que les deux commandes ne divergent pas.
        RemettreAPlein(cible);

        Powers const type = cible->getPowerType();
        cible->SaveToDB(false, false);

        handler->PSendSysMessage(
            "coa plein: {} niveau {} — pv {}/{}, ressource {} {}/{}{}",
            cible->GetName(), uint32(cible->GetLevel()),
            cible->GetHealth(), cible->GetMaxHealth(),
            uint32(type), cible->GetPower(type), cible->GetMaxPower(type),
            etaitMort ? " (ressuscite)" : "");
        return true;
    }

    // ------------------------------------------------------------------
    // .coa groupe <chef> <membre> [membre ...]     ou     .coa groupe dissous <nom>
    //
    // POURQUOI ELLE EXISTE. La description de situation envoyee a l'oracle
    // porte l'etat du groupe : combien d'allies a portee, combien de blesses,
    // le plus bas en vie. Sur un bot sans groupe, ces trois faits valent
    // toujours « seul, sans groupe » — ils ne portent aucune information, et
    // on ne peut donc pas mesurer ce qu'ils apportent.
    //
    // Le coeur sait former un groupe, mais `.group join` exige que le chef
    // soit DEJA dans un groupe, et toutes les sous-commandes de `.group` sont
    // en `Console::No` : elles veulent un joueur a l'autre bout. Depuis la
    // console RA il n'y a donc aucun moyen de creer un groupe de bots.
    // ------------------------------------------------------------------
    static bool HandleCoaGroupeCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> mots = Decouper(args);
        if (mots.size() < 2)
        {
            handler->PSendSysMessage(
                "Usage: .coa groupe <chef> <membre> [membre ...]  |  .coa groupe dissous <nom>");
            return false;
        }

        if (mots[0] == "dissous")
        {
            Player* qui = ObjectAccessor::FindPlayerByName(mots[1], true);
            Group* groupe = qui ? qui->GetGroup() : nullptr;
            if (!groupe)
            {
                handler->PSendSysMessage("coa groupe: {} n'est dans aucun groupe.", mots[1]);
                return false;
            }
            groupe->Disband();
            handler->PSendSysMessage("coa groupe: groupe de {} dissous.", mots[1]);
            return true;
        }

        Player* chef = ObjectAccessor::FindPlayerByName(mots[0], true);
        if (!chef)
        {
            handler->PSendSysMessage("coa groupe: chef {} introuvable ou hors du monde.", mots[0]);
            return false;
        }

        Group* groupe = chef->GetGroup();
        if (!groupe)
        {
            // Le groupe s'enregistre AUPRES DU GESTIONNAIRE, sinon il n'a pas
            // d'identifiant de stockage et disparait au redemarrage en laissant
            // des lignes orphelines dans `group_member`.
            groupe = new Group();
            if (!groupe->Create(chef))
            {
                delete groupe;
                handler->PSendSysMessage("coa groupe: creation refusee pour {}.", mots[0]);
                return false;
            }
            sGroupMgr->AddGroup(groupe);
        }

        uint32 ajoutes = 0;
        std::string refuses;
        for (size_t i = 1; i < mots.size(); ++i)
        {
            Player* membre = ObjectAccessor::FindPlayerByName(mots[i], true);
            if (!membre || membre == chef || membre->GetGroup() || groupe->IsFull())
            {
                refuses += (refuses.empty() ? "" : ", ") + mots[i];
                continue;
            }
            if (groupe->AddMember(membre))
                ++ajoutes;
            else
                refuses += (refuses.empty() ? "" : ", ") + mots[i];
        }
        groupe->BroadcastGroupUpdate();

        handler->PSendSysMessage("coa groupe: {} mene {} membre(s), {} au total{}{}",
                                 chef->GetName(), ajoutes, groupe->GetMembersCount(),
                                 refuses.empty() ? "" : " — refuses: ", refuses);
        return ajoutes > 0 || groupe->GetMembersCount() > 1;
    }

    // Decoupage d'une ligne d'arguments en mots. La console RA passe une chaine
    // brute ; un argument manquant doit se dire, pas se deviner.
    static std::vector<std::string> Decouper(char const* args)
    {
        std::vector<std::string> mots;
        std::string mot;
        for (char c : std::string(args ? args : ""))
        {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                if (!mot.empty()) { mots.push_back(mot); mot.clear(); }
            }
            else
                mot.push_back(c);
        }
        if (!mot.empty())
            mots.push_back(mot);
        return mots;
    }

    // ------------------------------------------------------------------
    // .coa enrole <nom>
    //
    // POURQUOI ELLE EXISTE. Un banc de duels a besoin que ses 28 duellistes
    // soient en jeu EN MEME TEMPS, manche apres manche. Mesure du 2026-09-23,
    // vingt minutes apres le lancement d'une campagne : 17 des 28 etaient
    // deconnectes, et `.teleport name` echouait en silence — sept manches sur
    // quatorze comptees « mal_place ».
    //
    // Ce qui les sort du monde : `RandomPlayerbotMgr::ProcessBot` lit
    // l'evenement « add » du bot ; a zero, il le deconnecte et SUPPRIME la
    // ligne. Releve en base sur quatre duellistes : celui qui etait encore en
    // jeu avait sa ligne `add`, les trois autres n'en avaient plus aucune.
    //
    // La meme fonction fait l'inverse quand « add » vaut 1 et que le bot n'est
    // pas en jeu : elle appelle `AddPlayerBot` et le reconnecte. Poser
    // l'evenement suffit donc, et c'est tout ce que fait cette commande.
    //
    // ELLE TRAVAILLE PAR GUID, PAS PAR Player*. C'est le point : le bot visé
    // est justement celui qui n'est PAS en jeu, donc `FindPlayerByName` ne le
    // rend pas. `sCharacterCache` resout le nom hors connexion, et
    // `RandomPlayerbotMgr::SetValue(uint32, ...)` prend un GUID.
    //
    //     .coa enrole <nom>
    // ------------------------------------------------------------------
    static bool HandleCoaEnroleCommand(ChatHandler* handler, char const* args)
    {
        std::string nom = args ? args : "";
        while (!nom.empty() && (nom.back() == ' ' || nom.back() == '\r' || nom.back() == '\n'))
            nom.pop_back();
        while (!nom.empty() && nom.front() == ' ')
            nom.erase(nom.begin());
        if (nom.empty())
        {
            handler->PSendSysMessage("Usage: .coa enrole <nom>");
            return false;
        }

        ObjectGuid const guid = sCharacterCache->GetCharacterGuidByName(nom);
        if (!guid)
        {
            handler->PSendSysMessage("coa enrole: aucun personnage nomme {}.", nom);
            return false;
        }

        uint32 const bas = guid.GetCounter();

        // POSER L'EVENEMENT NE SUFFIT PAS, ET LE CROIRE COUTE UNE CAMPAGNE.
        // Mesure du 2026-09-23 : les evenements ecrits, 18 duellistes sur 28
        // sont restes hors jeu. Deux raisons, toutes deux dans
        // RandomPlayerbotMgr :
        //
        //  - la boucle de connexion (`tryLoginBot`) ECARTE tout bot dont
        //    l'evenement « add » est pose — elle en deduit qu'il est deja en
        //    jeu. Poser « add » sur un bot hors jeu le bloque donc dehors ;
        //  - `ProcessBot(uint32)`, qui saurait le reconnecter, n'est appelee
        //    que pour les bots de `currentBots`, dont il vient precisement
        //    d'etre retire.
        //
        // Et la boucle de connexion ne s'ouvre de toute facon qu'a
        // `MaxRandomBots` - `currentBots.size()` places, soit deux sur ce
        // serveur, disputees par cinq cents candidats.
        //
        // On appelle donc `AddPlayerBot` nous-memes : c'est exactement ce que
        // ferait `ProcessBot`, sans dependre de la file.
        // ET CONNECTER NE SUFFIT PAS NON PLUS. Un bot connecte par
        // `AddPlayerBot` seul n'entre pas dans `currentBots`, donc
        // `IsRandomBot` le renie, donc `MercenaryRewards::EstMercenaire` aussi,
        // donc le choix de cible PvP sauvage l'ecarte : il se tient au bon
        // endroit et ne se bat jamais. Mesure du 2026-09-23 : sept paires sur
        // quatorze, au meme metre, points de vie pleins, cinq manches sans un
        // coup. `CoaEnroler` fait les quatre gestes dans le bon ordre.
        Player* deja = ObjectAccessor::FindPlayerByName(nom, true);
        bool const en_jeu = sRandomPlayerbotMgr.CoaEnroler(guid);

        handler->PSendSysMessage(
            "coa enrole: {} (guid {}) — {}, maintenu en jeu {} s",
            nom, bas,
            en_jeu ? (deja ? "deja en jeu, reinscrit" : "connecte a l'instant")
                   : "connexion refusee",
            sPlayerbotAIConfig.maxRandomBotInWorldTime);
        return en_jeu;
    }

    // ------------------------------------------------------------------
    // .coa niveau <nom> <niveau> [qualite]
    // ------------------------------------------------------------------
    static bool HandleCoaNiveauCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> mots = Decouper(args);
        if (mots.size() < 2)
        {
            handler->PSendSysMessage("Usage: .coa niveau <nom> <niveau> [qualite 0-5]");
            return false;
        }

        std::string const& nom = mots[0];
        int const demande = std::atoi(mots[1].c_str());
        uint32 const plafond = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        if (demande < 1 || static_cast<uint32>(demande) > plafond)
        {
            handler->PSendSysMessage("coa niveau: niveau hors bornes (1 a {}).", plafond);
            return false;
        }
        uint32 const niveau = static_cast<uint32>(demande);

        // 0 = la factory choisit elle-meme. Toute autre valeur impose la meme
        // qualite aux deux bots d'une paire, ce qui retire l'equipement du plan
        // d'experience.
        uint32 qualite = 0;
        if (mots.size() >= 3)
        {
            int const q = std::atoi(mots[2].c_str());
            if (q < 0 || q > 5)
            {
                handler->PSendSysMessage("coa niveau: qualite hors bornes (0 a 5).");
                return false;
            }
            qualite = static_cast<uint32>(q);
        }

        Player* cible = ObjectAccessor::FindPlayerByName(nom, true);
        if (!cible)
        {
            handler->PSendSysMessage("coa niveau: {} est introuvable ou hors du monde.", nom);
            return false;
        }

        uint32 const avant = cible->GetLevel();

        // GELER LA REMISE A ZERO AVANT DE MONTER, pas apres : entre les deux,
        // le fil monde peut passer par ProcessBot et defaire le travail.
        // `SetValue` pose l'evenement pour MaxRandomBotInWorldTime secondes.
        sRandomPlayerbotMgr.SetValue(cible, "randomize", 1);
        // Meme raison que `.coa enrole` : un duelliste deconnecte ne duelle pas.
        sRandomPlayerbotMgr.SetValue(cible, "add", 1);
        sRandomPlayerbotMgr.SetValue(cible, "level", niveau);

        if (avant != niveau)
            cible->GiveLevel(static_cast<uint8>(niveau));

        // La recette du module lui-meme, a chaque montee de niveau :
        // AutoMaintenanceOnLevelupAction::LearnTrainerSpells. On ne touche PAS
        // aux talents ni a la specialisation : InitTalentsTree reroulerait la
        // spec, et le banc de duels appaire justement sur elle.
        PlayerbotFactory factory(cible, niveau, qualite);
        factory.InitSkills();
        factory.InitClassSpells();
        factory.InitAvailableSpells();
        factory.InitEquipment(false);
        factory.InitAmmo();
        factory.InitPet();

        RemettreAPlein(cible);
        cible->SaveToDB(false, false);

        handler->PSendSysMessage(
            "coa niveau: {} {} -> {} (qualite {}) — pv {}/{}, {} sorts connus",
            cible->GetName(), avant, uint32(cible->GetLevel()), qualite,
            cible->GetHealth(), cible->GetMaxHealth(),
            uint32(cible->GetSpellMap().size()));
        return true;
    }

    // Le remplissage, partage par les deux commandes. L'ordre compte : voir
    // l'en-tete de ce fichier.
    static void RemettreAPlein(Player* cible)
    {
        if (!cible->IsAlive())
        {
            cible->ResurrectPlayer(1.0f);
            cible->SpawnCorpseBones();
        }
        cible->UpdateAllStats();
        cible->SetFullHealth();

        Powers const type = cible->getPowerType();
        if (type == POWER_RAGE || type == POWER_RUNIC_POWER)
            cible->SetPower(type, 0);
        else
            cible->SetPower(type, cible->GetMaxPower(type));

        if (type != POWER_MANA && cible->GetMaxPower(POWER_MANA) > 0)
            cible->SetPower(POWER_MANA, cible->GetMaxPower(POWER_MANA));
    }
};

void AddSC_coa_duel_commandscript()
{
    new coa_duel_commandscript();
}
