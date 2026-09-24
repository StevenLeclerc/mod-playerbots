/*
 * Conquest of Azeroth — la description de situation envoyee a l'oracle Laya.
 * Voir l'en-tete pour le pourquoi et la regle de redaction.
 */

#include "CoaLayaEtat.h"

#include "CoaRegistreCombat.h"
#include "CoaSpecLookup.h"

#include "AiObjectContext.h"
#include "Creature.h"
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Timer.h"
#include "Unit.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace
{
    // La ressource principale, nommee. Ecrire « mana » a une classe qui carbure
    // a la rage, c'est lui faire lire « a sec » en permanence : la plupart des
    // 21 classes CoA n'ont pas de mana.
    char const* NomRessource(Powers type)
    {
        switch (type)
        {
            case POWER_MANA:         return "mana";
            case POWER_RAGE:         return "rage";
            case POWER_FOCUS:        return "focus";
            case POWER_ENERGY:       return "energie";
            case POWER_RUNIC_POWER:  return "puissance runique";
            default:                 return "ressource";
        }
    }

    char const* NomRole(CoaSpecRole role)
    {
        switch (role)
        {
            case CoaSpecRole::Tank: return "tank";
            case CoaSpecRole::Heal: return "soigneur";
            default:                return "degats";
        }
    }

    // « corps a corps » ou « a distance » : la table des specs CoA porte
    // "close" ou "ranged". On ne devine pas, on lit.
    char const* NomPosition(char const* position)
    {
        if (!position)
            return "corps a corps";
        return position[0] == 'r' ? "a distance" : "corps a corps";
    }

    void Ajouter(std::string& sortie, char const* format, ...)
    {
        char tampon[320];
        va_list args;
        va_start(args, format);
        vsnprintf(tampon, sizeof(tampon), format, args);
        va_end(args);
        sortie += tampon;
    }
}

std::string CoaDecrireEtatLaya(PlayerbotAI* botAI, Player* bot, Unit* cible)
{
    if (!bot)
        return "Etat inconnu.";

    std::string etat;
    etat.reserve(640);

    AiObjectContext* contexte = botAI ? botAI->GetAiObjectContext() : nullptr;

    /* ---------------------------------------------------------------- */
    /* 1. Moi                                                            */
    /* ---------------------------------------------------------------- */

    CoaSpecStrategy const* spec = GetCoaSpecStrategyFor(bot);
    Powers const typeRessource = bot->getPowerType();

    if (spec)
        Ajouter(etat, "Moi: %s, %s, %s, niveau %u.", spec->specName, NomRole(spec->role),
                NomPosition(spec->position), static_cast<unsigned>(bot->GetLevel()));
    else
        Ajouter(etat, "Moi: niveau %u.", static_cast<unsigned>(bot->GetLevel()));

    Ajouter(etat, " Vie %d%%. %s %d%%.", static_cast<int>(bot->GetHealthPct()),
            NomRessource(typeRessource), static_cast<int>(bot->GetPowerPct(typeRessource)));

    // Ressource secondaire. On ne l'ecrit que si elle existe reellement :
    // annoncer « 0 point de combo » a une classe qui n'en a pas, c'est un fait
    // faux de plus. La regle de decision de beaucoup de finisseurs tient
    // entierement dans ce chiffre.
    if (uint8 const combo = bot->GetComboPoints())
        Ajouter(etat, " %u point%s de combo.", static_cast<unsigned>(combo), combo > 1 ? "s" : "");

    // Un sort a temps d'incantation ne part pas d'un bot en mouvement : c'est
    // le premier filtre de toute rotation a distance.
    if (bot->isMoving())
        etat += " Je me deplace.";

    /* ---------------------------------------------------------------- */
    /* 2. Le combat                                                      */
    /* ---------------------------------------------------------------- */

    if (bot->IsInCombat())
    {
        uint32 const debut = CoaRegistreCombat::Instance().DebutMs(bot->GetGUID().GetRawValue());
        if (debut)
            Ajouter(etat, " En combat depuis %u s.", GetMSTimeDiffToNow(debut) / 1000u);
        else
            etat += " En combat.";
    }
    else
        etat += " Hors combat.";

    size_t assaillants = 0;
    if (contexte)
        if (Value<GuidVector>* v = contexte->GetValue<GuidVector>("attackers"))
            assaillants = v->Get().size();
    Ajouter(etat, " %zu ennemi%s sur moi.", assaillants, assaillants > 1 ? "s" : "");

    /* ---------------------------------------------------------------- */
    /* 3. La cible                                                       */
    /* ---------------------------------------------------------------- */

    if (!cible && contexte)
        if (Value<Unit*>* v = contexte->GetValue<Unit*>("current target"))
            cible = v->Get();

    if (!cible || !cible->IsAlive())
    {
        etat += " Aucune cible.";
    }
    else
    {
        // Ce que la cible EST. Un joueur ne se traite pas comme un mob, et un
        // soigneur adverse ne se traite pas comme un autre joueur.
        char const* nature = "monstre";
        if (Player* joueur = cible->ToPlayer())
        {
            CoaSpecStrategy const* specCible = GetCoaSpecStrategyFor(joueur);
            nature = specCible ? NomRole(specCible->role) : "joueur";
            if (specCible)
                Ajouter(etat, " Cible: joueur %s %s, niveau %u,", specCible->specName, nature,
                        static_cast<unsigned>(cible->GetLevel()));
            else
                Ajouter(etat, " Cible: joueur, niveau %u,", static_cast<unsigned>(cible->GetLevel()));
        }
        else
        {
            if (Creature* creature = cible->ToCreature())
            {
                if (creature->isWorldBoss())
                    nature = "boss";
                else if (creature->isElite())
                    nature = "monstre d'elite";
            }
            Ajouter(etat, " Cible: %s, niveau %u,", nature, static_cast<unsigned>(cible->GetLevel()));
        }

        Ajouter(etat, " vie %d%%, a %d metres.", static_cast<int>(cible->GetHealthPct()),
                static_cast<int>(bot->GetExactDist2d(cible)));

        // Une incantation adverse est la seule situation ou une interruption
        // sert a quelque chose.
        if (cible->IsNonMeleeSpellCast(false))
            etat += " Elle incante.";

        // Qui la cible frappe : c'est ce qui distingue « je tiens l'aggro » de
        // « elle tape quelqu'un d'autre », et donc une provocation utile d'une
        // provocation gaspillee.
        Unit* victime = cible->GetVictim();
        if (victime == bot)
            etat += " Elle me frappe.";
        else if (victime)
            etat += " Elle frappe un autre.";
    }

    /* ---------------------------------------------------------------- */
    /* 4. La densite d'ennemis — la question du sort de zone             */
    /* ---------------------------------------------------------------- */

    // Sans ce chiffre, aucun arbitrage entre mono-cible et zone n'est possible.
    // On compte autour de la CIBLE, parce que c'est la que le sort de zone
    // tombera, et non autour du bot.
    if (contexte && cible)
    {
        if (Value<GuidVector>* v = contexte->GetValue<GuidVector>("possible targets"))
        {
            GuidVector const proies = v->Get();
            size_t groupes = 0;
            size_t joueurs = 0;
            float const rayon = sPlayerbotAIConfig.aoeRadius;
            for (ObjectGuid const& guid : proies)
            {
                Unit* unite = botAI->GetUnit(guid);
                if (!unite || !unite->IsAlive())
                    continue;
                if (cible->GetExactDist2d(unite) > rayon)
                    continue;
                ++groupes;
                if (unite->GetTypeId() == TYPEID_PLAYER)
                    ++joueurs;
            }
            Ajouter(etat, " %zu ennemi%s groupe%s autour de la cible", groupes,
                    groupes > 1 ? "s" : "", groupes > 1 ? "s" : "");
            if (joueurs)
                Ajouter(etat, ", dont %zu joueur%s", joueurs, joueurs > 1 ? "s" : "");
            etat += ".";
        }
    }

    /* ---------------------------------------------------------------- */
    /* 5. Le groupe — la question du soin                                */
    /* ---------------------------------------------------------------- */

    if (Group* groupe = bot->GetGroup())
    {
        size_t allies = 0;
        size_t blesses = 0;
        int pireVie = 100;
        for (GroupReference* ref = groupe->GetFirstMember(); ref; ref = ref->next())
        {
            Player* membre = ref->GetSource();
            if (!membre || membre == bot || !membre->IsAlive() || membre->GetMapId() != bot->GetMapId())
                continue;
            if (bot->GetExactDist2d(membre) > sPlayerbotAIConfig.sightDistance)
                continue;
            ++allies;
            int const vie = static_cast<int>(membre->GetHealthPct());
            if (vie < 90)
                ++blesses;
            if (vie < pireVie)
                pireVie = vie;
        }
        if (allies)
            Ajouter(etat, " Groupe: %zu allie%s proche%s, %zu blesse%s, le plus bas a %d%% de vie.",
                    allies, allies > 1 ? "s" : "", allies > 1 ? "s" : "",
                    blesses, blesses > 1 ? "s" : "", pireVie);
        else
            etat += " Groupe: personne a portee.";
    }
    else
        etat += " Seul, sans groupe.";

    return etat;
}
