/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BattleGroundTactics.h"
#include "Chat.h"
#include "CoaSpecialization.h"
#include "Group.h"
#include "GuildTaskMgr.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringConvert.h"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{

// A legacy `char const*` handler receives every argument as one string (ChatCommand.h l. 141-150).
// Split it on blanks, dropping empty runs so that a trailing or doubled space makes no token.
std::vector<std::string> SplitCoaArguments(char const* args)
{
    std::string const line = args ? args : "";
    std::vector<std::string> tokens;
    for (std::string::size_type at = 0; at < line.size();)
    {
        std::string::size_type const start = line.find_first_not_of(" \t", at);
        if (start == std::string::npos)
            break;

        std::string::size_type const end = line.find_first_of(" \t", start);
        tokens.emplace_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            break;

        at = end;
    }

    return tokens;
}

// The words accepted in game before the console form existed; they stay exactly those.
bool ParseCoaRole(std::string const& wanted, CoaRole& role)
{
    if (wanted == "tank")
        role = CoaRole::Tank;
    else if (wanted == "heal" || wanted == "healer")
        role = CoaRole::Heal;
    else if (wanted == "dps")
        role = CoaRole::Dps;
    else
        return false;

    return true;
}

// The same three words as RoleName in CoaSpecialization.cpp, which sits in an anonymous namespace
// there and cannot be reached from here: the summary line has to read like the per-bot lines.
char const* CoaRoleLabel(CoaRole role)
{
    switch (role)
    {
        case CoaRole::Tank: return "tank";
        case CoaRole::Heal: return "heal";
        default:            return "dps";
    }
}

// Le mot rendu a l'exploitant pour un palier. Les memes que ceux qu'il saisit, pour qu'il
// puisse recopier la reponse dans la commande suivante.
char const* CoaTierLabel(CoaGearTier tier)
{
    switch (tier)
    {
        case CoaGearTier::Median: return "moyen";
        case CoaGearTier::Top:    return "max";
        default:                  return "bas";
    }
}

// Les bornes de delta telles qu'on les rend. Non bornees, on ecrit « any » plutot que les
// deux extremes d'un int32, qui ne veulent rien dire pour un lecteur.
std::string CoaDeltaLabel(CoaRecruitOptions const& options)
{
    if (!options.HasLevelDelta())
        return "any";

    std::string texte;
    if (options.levelDeltaLow == std::numeric_limits<int32>::min())
        texte = "any";
    else
        texte = (options.levelDeltaLow > 0 ? "+" : "") + std::to_string(options.levelDeltaLow);

    texte += "/";
    if (options.levelDeltaHigh == std::numeric_limits<int32>::max())
        texte += "any";
    else
        texte += (options.levelDeltaHigh > 0 ? "+" : "") + std::to_string(options.levelDeltaHigh);

    return texte;
}

// Une borne de delta : un entier relatif, avec ou sans signe.
//
// LE PIEGE, LU ET NON SUPPOSE. Acore::StringTo repose sur std::from_chars
// (common/Utilities/StringConvert.h l. 69), et std::from_chars REFUSE le '+' de tete pour un
// type entier : il n'accepte que le '-'. La forme « +3 » - celle qu'on documente, celle que
// l'exploitant ecrira - serait donc rejetee en bloc, et « delta=-1/+3 » avec elle. Le '+' est
// retire avant lecture.
//
// Le signe n'est pas exige : la barre separe deja les deux bornes sans ambiguite possible,
// donc « delta=-1/3 » et « delta=0/3 » se lisent aussi. Une paire inversee est refusee plus
// haut, ce qui rattrape la seule erreur de saisie que l'absence de signe pourrait produire.
bool ParseCoaDeltaBound(std::string const& piece, int32& out)
{
    if (piece.empty())
        return false;

    std::string const lu = piece[0] == '+' ? piece.substr(1) : piece;

    // StringTo<int32> refuse le vide, les caracteres en trop et le debordement.
    Optional<int32> const value = Acore::StringTo<int32>(lu);
    if (!value)
        return false;

    out = *value;
    return true;
}

// `delta=-1/+3` : deux bornes signees separees par une barre. Les extremes d'un int32 sont
// reserves a « non borne », donc refuses en saisie : les accepter rendrait HasLevelDelta()
// menteur.
bool ParseCoaDelta(std::string const& value, CoaRecruitOptions& options, std::string& error)
{
    std::string::size_type const cut = value.find('/');
    if (cut == std::string::npos || cut == 0 || cut + 1 >= value.size())
    {
        error = "coa: '" + value + "' is not a level delta, expected a form like -1/+3.";
        return false;
    }

    int32 low = 0;
    int32 high = 0;
    if (!ParseCoaDeltaBound(value.substr(0, cut), low) || !ParseCoaDeltaBound(value.substr(cut + 1), high) ||
        low == std::numeric_limits<int32>::min() || high == std::numeric_limits<int32>::max())
    {
        error = "coa: '" + value + "' is not a level delta, expected a form like -1/+3.";
        return false;
    }

    // Sans ce refus le vivier serait vide et personne ne saurait pourquoi : aucun ecart ne
    // peut etre a la fois au-dessus de `high` et au-dessous de `low`.
    if (low > high)
    {
        error = "coa: delta low " + std::to_string(low) + " is above delta high " + std::to_string(high) + ".";
        return false;
    }

    options.levelDeltaLow = low;
    options.levelDeltaHigh = high;
    return true;
}

// Les options en suffixe, a partir du jeton `from`. Elles portent toutes un `=`, ce qui les
// rend impossibles a confondre avec les jetons positionnels d'avant ce lot. Un mot-cle
// inconnu est un REFUS explicite : un silence ferait croire l'option prise en compte.
bool ParseCoaOptions(std::vector<std::string> const& tokens, size_t from, CoaRecruitOptions& options,
                     std::string& error)
{
    for (size_t at = from; at < tokens.size(); ++at)
    {
        std::string const& token = tokens[at];
        std::string::size_type const eq = token.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            error = "coa: unknown option '" + token + "', expected delta=, palier= or refab=.";
            return false;
        }

        std::string const key = token.substr(0, eq);
        std::string const value = token.substr(eq + 1);

        if (key == "delta")
        {
            if (!ParseCoaDelta(value, options, error))
                return false;
        }
        else if (key == "palier")
        {
            if (value == "bas")
                options.tier = CoaGearTier::Any;
            else if (value == "moyen")
                options.tier = CoaGearTier::Median;
            else if (value == "max" || value == "maximum")
                options.tier = CoaGearTier::Top;
            else
            {
                error = "coa: '" + value + "' is not a tier, expected bas, moyen or max.";
                return false;
            }
        }
        else if (key == "refab")
        {
            if (value == "oui")
                options.rebuild = true;
            else if (value == "non")
                options.rebuild = false;
            else
            {
                error = "coa: '" + value + "' is not a yes or no, expected oui or non.";
                return false;
            }
        }
        else
        {
            error = "coa: unknown option '" + key + "', expected delta=, palier= or refab=.";
            return false;
        }
    }

    return true;
}

}  // namespace

class playerbots_commandscript : public CommandScript
{
public:
    playerbots_commandscript() : CommandScript("playerbots_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable playerbotsDebugCommandTable = {
            {"bg", HandleDebugBGCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsAccountCommandTable = {
            {"setKey", HandleSetSecurityKeyCommand, SEC_PLAYER, Console::No},
            {"link", HandleLinkAccountCommand, SEC_PLAYER, Console::No},
            {"linkedAccounts", HandleViewLinkedAccountsCommand, SEC_PLAYER, Console::No},
            {"unlink", HandleUnlinkAccountCommand, SEC_PLAYER, Console::No},
        };

        static ChatCommandTable playerbotsCommandTable = {
            {"bot", HandlePlayerbotCommand, SEC_PLAYER, Console::No},
            {"gtask", HandleGuildTaskCommand, SEC_GAMEMASTER, Console::Yes},
            {"pmon", HandlePerfMonCommand, SEC_GAMEMASTER, Console::Yes},
            {"rndbot", HandleRandomPlayerbotCommand, SEC_GAMEMASTER, Console::Yes},
            {"coa", HandleCoaRecruitCommand, SEC_GAMEMASTER, Console::Yes},
            {"debug", playerbotsDebugCommandTable},
            {"account", playerbotsAccountCommandTable},
        };

        static ChatCommandTable commandTable = {
            {"playerbots", playerbotsCommandTable},
        };

        return commandTable;
    }

    static bool HandlePlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return PlayerbotMgr::HandlePlayerbotMgrCommand(handler, args);
    }

    // LES DEUX LIGNES AJOUTEES PAR CE LOT, ET LEUR CONTRAT AU CARACTERE PRES.
    //
    // Le panneau les analyse avec deux expressions ancrees (phase8-panel/panel.py,
    // _RE_GROUPE_GEAR et _RE_GROUPE_SHORT) :
    //   coa gear: <Nom> <indice> (pool max <max>, tier <bas|moyen|max>)
    //   coa short: <place>/<demande> <role> for <maitre>; <quoi>
    // Pas de point final sur la ligne d'indice : l'expression est ancree par $ et n'en
    // attend aucun. Le nom du maitre y est capture par [^;] : rien ne doit s'inserer entre
    // « for » et le premier point-virgule, d'ou le detail qui passe apres celui-ci.
    //
    // CE LOT N'EMET AUCUNE AUTRE LIGNE NOUVELLE, ET C'EST UNE CONTRAINTE, PAS UN GOUT. La
    // retombee de fin de _groupe_lire_sortie transforme la PREMIERE ligne non reconnue en
    // motif d'arret des que le lot est servi court. Une troisieme ligne, si utile soit-elle,
    // s'afficherait donc comme une panne. Tout ce qu'on a a dire en plus tient dans le champ
    // libre <quoi>, que le panneau rend tel quel.
    //
    // ET RIEN N'EST EMIS DU TOUT quand aucune option n'est donnee - la forme que le panneau
    // EN SERVICE joue aujourd'hui, et qui ne connait ni l'une ni l'autre de ces expressions.
    // La sortie reste alors identique au caractere pres a celle d'avant ce lot. C'est le
    // premier critere d'acceptation de ce lot.
    static void SendCoaGearLine(ChatHandler* handler, CoaRecruitOptions const& options,
                                CoaRecruitReport const& report)
    {
        if (options.IsDefault() || !report.gearKnown || !report.picked)
            return;

        // L'indice ABSOLU du bot retenu ET le maximum du vivier. Les deux, parce qu'aucun des
        // deux ne dit rien seul : 9 est mediocre a cote d'un vivier qui monte a 20, excellent
        // a cote d'un vivier qui plafonne a 9, et un palier relatif sur un vivier d'un seul
        // bot le declare « maximum » quel que soit son denuement.
        handler->SendSysMessage("coa gear: " + report.pickedName + " " + std::to_string(report.pickedGear) +
                                " (pool max " + std::to_string(report.gearMax) + ", tier " +
                                CoaTierLabel(options.tier) + ")");
    }

    // La ligne de negociation. « 2 sur 4 » ne renseigne personne : le champ libre porte la
    // ventilation par critere, pour qu'on sache s'il manque des soigneurs, si c'est le delta
    // de niveau qui vide le vivier, ou le palier d'equipement.
    static void SendCoaShortLine(ChatHandler* handler, CoaRecruitOptions const& options,
                                 CoaRecruitReport const& report, uint32 placed, uint32 count, uint32 rebuilt,
                                 CoaRole role, std::string const& masterName)
    {
        // `seen == 0` veut dire que le balayage n'a jamais eu lieu : le refus est tombe avant
        // (groupe plein, maitre parti). Ventiler un vivier qu'on n'a pas regarde afficherait
        // « 0 seen, 0 unavailable » a cote de 500 bots en ligne, ce qui se lit comme une
        // panne. La raison, elle, est deja rendue par la ligne de bilan.
        if (options.IsDefault() || placed >= count || !report.seen)
            return;

        std::string const roleLabel = CoaRoleLabel(role);
        std::string quoi = std::to_string(report.seen) + " seen, " + std::to_string(report.unavailable) +
                           " unavailable, " + std::to_string(report.wrongRole) + " cannot " + roleLabel + ", " +
                           std::to_string(report.outsideDelta) + " outside level " + CoaDeltaLabel(options) + ", " +
                           std::to_string(report.belowTier) + " below tier " + CoaTierLabel(options.tier) + ", " +
                           std::to_string(report.eligible) + " eligible";

        if (report.gearKnown)
            quoi += ", gear pool " + std::to_string(report.gearMin) + "/" + std::to_string(report.gearMedian) + "/" +
                    std::to_string(report.gearMax) + " floor " + std::to_string(report.gearFloor);

        // Dit a voix haute parce que c'est irreversible : la refabrication vide les sacs du
        // bot (PlayerbotFactory::ClearInventory, appelee sans condition). Ces bots-la ont
        // change de vie ce jour-la, et rien ne les y ramenera.
        if (rebuilt)
            quoi += ", " + std::to_string(rebuilt) + " already rebuilt (bags emptied)";

        quoi += "; widen delta= or palier=, or rebuild " + std::to_string(count - placed) + " with refab=oui";

        handler->SendSysMessage("coa short: " + std::to_string(placed) + "/" + std::to_string(count) + " " +
                                roleLabel + " for " + masterName + "; " + quoi);
    }

    // Recruit Conquest of Azeroth bots for a role, in the master's group, at his position.
    //   .playerbots coa tank|heal|dps [options]                    in game: the caller is the master.
    //   .playerbots coa <player> tank|heal|dps [count] [options]   from the console: the master is named.
    //
    // Options, toutes en suffixe et toutes porteuses d'un `=` :
    //   delta=-1/+3        n'accepter qu'un bot dont le niveau est dans cet ecart du maitre
    //   palier=bas|moyen|max  palier d'equipement, relatif au vivier retenu
    //   refab=oui|non      autoriser, ou non, le repli qui refabrique le bot au niveau du maitre
    //
    // POURQUOI DES MOTS-CLES ET NON DES POSITIONS. Le quatrieme jeton de la forme console est
    // deja `count`, et il est optionnel : un cinquieme jeton positionnel serait ambigu des que
    // `count` est omis. Un jeton contenant `=` ne peut se confondre avec aucune forme existante.
    //
    // POURQUOI LES DEFAUTS REPRODUISENT LE COMPORTEMENT D'AVANT, ET NON LA NOUVELLE REGLE.
    // C'est le choix de compatibilite de ce lot, et il est deliberé. Le panneau en service
    // joue encore `.playerbots coa <joueur> <role> [nombre]` sans option, et son analyseur ne
    // sait pas encore negocier. Or `refab=non` n'a de sens qu'accompagne d'un `delta=` : sans
    // borne, on servirait un bot de niveau 1 a un joueur de niveau 40. Poser le couple par
    // defaut ferait donc echouer en penurie - « 2 sur 4 » - toutes les demandes au-dessus du
    // niveau 25 (mesure : plus aucun bot libre non mercenaire au-dela du niveau 29), sur un
    // panneau qui ne peut ni l'expliquer ni proposer d'elargir. Ce serait remplacer une
    // surprise silencieuse par une autre. La nouvelle regle est donc a demander, jusqu'a ce
    // que le panneau soit mis a jour et joue `delta=-1/+3 refab=non`.
    static bool HandleCoaRecruitCommand(ChatHandler* handler, char const* args)
    {
        // A CliHandler carries no session but overrides HasSession() to return true, so that its
        // output reaches the console (Chat.h l. 255-277). The console is therefore recognised by
        // GetSession() alone: HasSession() would send every console call down the in-game branch.
        WorldSession* session = handler->GetSession();
        std::vector<std::string> const tokens = SplitCoaArguments(args);

        CoaRecruitOptions options;
        std::string optionError;

        if (session)
        {
            CoaRole role;
            // Les options sont acceptees ici aussi : le premier jeton reste le role, et tout
            // le reste porte un `=`, donc aucune ambiguite de position n'apparait. C'est la
            // forme qu'un MJ tape reellement en jeu.
            if (tokens.empty() || !ParseCoaRole(tokens[0], role))
            {
                handler->SendSysMessage(
                    "Usage: .playerbots coa tank|heal|dps [delta=-1/+3] [palier=bas|moyen|max] [refab=oui|non]");
                return true;
            }

            if (!ParseCoaOptions(tokens, 1, options, optionError))
            {
                handler->SendSysMessage(optionError);
                return true;
            }

            Player* master = session->GetPlayer();
            if (!master)
                return false;

            // Handled either way: returning false would add the generic usage text to our message.
            std::string message;
            CoaRecruitReport report;
            std::string const inGameMasterName = master->GetName();
            bool const ok = RecruitCoaBot(master, role, options, report, message);
            handler->SendSysMessage(message);
            SendCoaGearLine(handler, options, report);
            SendCoaShortLine(handler, options, report, ok ? 1 : 0, 1, report.rebuilt ? 1 : 0, role, inGameMasterName);
            return true;
        }

        CoaRole role;
        if (tokens.size() < 2 || !ParseCoaRole(tokens[1], role))
        {
            handler->SendSysMessage("Usage: .playerbots coa <player> tank|heal|dps [count] [delta=-1/+3] "
                                    "[palier=bas|moyen|max] [refab=oui|non]");
            return true;
        }

        // Le troisieme jeton reste le nombre, et seulement s'il ne porte pas de `=` : c'est ce
        // qui laisse intacte la forme que le panneau joue aujourd'hui.
        size_t firstOption = 2;
        uint32 count = 1;
        if (tokens.size() > 2 && tokens[2].find('=') == std::string::npos)
        {
            // Refuses an empty string, a sign, trailing characters and anything past 2^32 - 1;
            // std::from_chars rejects the '-' of a negative number for an unsigned type.
            Optional<uint32> const asked = Acore::StringTo<uint32>(tokens[2]);
            if (!asked || !*asked)
            {
                handler->PSendSysMessage("coa: '{}' is not a count of bots, expected 1 to {}.", tokens[2],
                                         uint32(MAXRAIDSIZE));
                return true;
            }

            if (*asked > uint32(MAXRAIDSIZE))
            {
                handler->PSendSysMessage("coa: {} bots asked, {} at most - a full raid.", *asked, uint32(MAXRAIDSIZE));
                return true;
            }

            count = *asked;
            firstOption = 3;
        }

        if (!ParseCoaOptions(tokens, firstOption, options, optionError))
        {
            handler->SendSysMessage(optionError);
            return true;
        }

        // RecruitCoaBot reads the master's map, level and position, so nothing but a character
        // currently in the world will do. FindPlayerByName normalises the case itself and answers
        // nullptr when the character is not connected (ObjectAccessor.cpp l. 127-135 and 295-302).
        Player* master = ObjectAccessor::FindPlayerByName(tokens[0], true);
        if (!master)
        {
            handler->PSendSysMessage("coa: no connected character named '{}'.", tokens[0]);
            return true;
        }

        // Not a refusal: filling a bot's group is a legitimate test. Said out loud because the
        // name of a bot is easy to reach by mistake, and the group that fills is then not a
        // player's.
        if (PlayerbotsMgr::instance().GetPlayerbotAI(master))
            handler->PSendSysMessage("coa: warning, {} is itself a bot.", master->GetName());

        // Kept by value: the summary line below is written after the last recruitment, and the
        // name outlives the pointer.
        std::string const masterName = master->GetName();

        uint32 placed = 0;
        uint32 rebuilt = 0;
        bool stopped = false;
        std::string reason;
        // Le rapport du DERNIER essai. En echec c'est celui qui explique la penurie ; en
        // succes complet, celui du vivier tel qu'il restait a la fin.
        CoaRecruitReport report;
        for (uint32 done = 0; done < count; ++done)
        {
            // Looked up again on every turn rather than held: one recruitment rebuilds a whole
            // bot, and a Player* is only good for as long as that character is in the world.
            // Belt and braces - the command runs on the world thread, which logs nobody out
            // while it runs (RASession.cpp l. 200-201, World::ProcessCliCommands).
            master = ObjectAccessor::FindPlayerByName(masterName, true);
            if (!master)
            {
                stopped = true;
                reason = masterName + " left the world.";
                break;
            }

            // One line per bot, the message RecruitCoaBot already writes. It answers false with
            // the reason - group full, no free bot able to play the role - and nothing else was
            // going to succeed either, so the run stops there rather than repeating the refusal.
            //
            // ON S'ARRETE TOUJOURS AU PREMIER REFUS, ET LE COMPTE DE MANQUANTS N'EN SOUFFRE
            // PAS : un essai refuse ne consomme aucun bot et ne change rien au vivier, donc
            // l'essai suivant echouerait a l'identique. Le nombre reel de manquants est
            // count - placed, quel que soit le rang de l'echec. Continuer coutait un balayage
            // complet du vivier par essai restant, pour la meme reponse.
            std::string message;
            CoaRecruitReport attempt;
            if (!RecruitCoaBot(master, role, options, attempt, message))
            {
                report = attempt;
                stopped = true;
                // Every refusal in RecruitCoaBot fills `message`; a silent one would otherwise
                // read as a success in the summary below.
                reason = message.empty() ? std::string("refused without a reason.") : message;
                break;
            }

            report = attempt;
            if (attempt.rebuilt)
                ++rebuilt;
            ++placed;
            handler->SendSysMessage(message);
            // Une ligne d'indice PAR recrue : le panneau les range par nom de bot.
            SendCoaGearLine(handler, options, attempt);
        }

        if (count == 1)
        {
            if (stopped)
                handler->SendSysMessage(reason);
            SendCoaShortLine(handler, options, report, placed, count, rebuilt, role, masterName);
            return true;
        }

        // Avant le bilan : le bilan est la derniere ligne du lot depuis toujours, et la
        // negociation se lit mieux juste apres les recrues qu'elle commente.
        SendCoaShortLine(handler, options, report, placed, count, rebuilt, role, masterName);

        if (!stopped)
            handler->PSendSysMessage("coa: {}/{} {} recruited for {}.", placed, count, CoaRoleLabel(role), masterName);
        else
            handler->PSendSysMessage("coa: {}/{} {} recruited for {}, stopped: {}", placed, count, CoaRoleLabel(role),
                                     masterName, reason);

        return true;
    }

    static bool HandleRandomPlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(handler, args);
    }

    static bool HandleGuildTaskCommand(ChatHandler* handler, char const* args)
    {
        return GuildTaskMgr::HandleConsoleCommand(handler, args);
    }

    static bool HandlePerfMonCommand(ChatHandler* /*handler*/, char const* args)
    {
        if (!strcmp(args, "reset"))
        {
            sPerfMonitor.Reset();
            return true;
        }

        if (!strcmp(args, "tick"))
        {
            sPerfMonitor.PrintStats(true, false);
            return true;
        }

        if (!strcmp(args, "stack"))
        {
            sPerfMonitor.PrintStats(false, true);
            return true;
        }

        if (!strcmp(args, "toggle"))
        {
            sPlayerbotAIConfig.perfMonEnabled = !sPlayerbotAIConfig.perfMonEnabled;
            if (sPlayerbotAIConfig.perfMonEnabled)
                LOG_INFO("playerbots", "Performance monitor enabled");
            else
                LOG_INFO("playerbots", "Performance monitor disabled");
            return true;
        }

        sPerfMonitor.PrintStats();
        return true;
    }

    static bool HandleDebugBGCommand(ChatHandler* handler, char const* args)
    {
        return BGTactics::HandleConsoleCommand(handler, args);
    }

    static bool HandleSetSecurityKeyCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->PSendSysMessage("Usage: .playerbots account setKey <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();
        std::string key = args;

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleSetSecurityKeyCommand(player, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleLinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        char* key = strtok(nullptr, " ");

        if (!accountName || !key)
        {
            handler->PSendSysMessage("Usage: .playerbots account link <accountName> <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleLinkAccountCommand(player, accountName, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleViewLinkedAccountsCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleViewLinkedAccountsCommand(player);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleUnlinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        if (!accountName)
        {
            handler->PSendSysMessage("Usage: .playerbots account unlink <accountName>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleUnlinkAccountCommand(player, accountName);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }
};

void AddPlayerbotsCommandscripts() { new playerbots_commandscript(); }
