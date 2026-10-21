// ------------------------------------------------------------------ Sourcier, voie Directeur (21/10/2026, lot T2 MINAGE)
//
// FRAGMENT de CoaDirecteur.cpp : inclus par lui, HORS de ses espaces de noms anonymes, apres CoaDirecteurButin.inc (il
// appelle Enregistrer, definie dans le premier espace anonyme du fichier, visible dans toute l'unite). Il n'est pas une
// unite de compilation : aucun `cmake` a reconfigurer.
//
// Conception : coa-project docs/CONCEPTION-artisanat-T2-sourcier-60.md § 2.0 (3), « Voie Directeur, cote jeu ».
// Le module mod-ascension-compat tient tout ce qui se possede (garde de lancement, registre, objet pivot, victoire,
// recompenses) ; le Directeur ne fournit que les vagues. AUCUN LIEN DE CODE entre les deux modules : ce fichier voit
// LUI-MEME le lancement reussi d'un sort de Sourcier « Directeur » et ecrit la demande `sourcier` (proprietaire de
// coa_dir_demande : mod-playerbots, P-134). Les ids sont RECOPIES de AscensionArtisanatIds.h (SORT_OUVRIR_FAILLE_TELLURIQUE),
// comme TYPE_ARENE_1C1 l'est dans l'autre sens : a changer ensemble.
//
// Le module refuse le sort en amont (OnSpellCheckCast : combat, instance, registre, voisin...) : un sort refuse n'est
// jamais lance, donc aucune demande. Un lancement FORCE (skipCheck : `.cast ... triggered` d'un MJ) n'ecrit rien : il
// n'a pas passe la garde du module.
//
// Demande : type `sourcier`, texte = l'evenement (`faille_tellurique`), lieu du lanceur (colonnes carte/x/y/z de
// Enregistrer). Garde : option AiPlayerbot.Directeur.Sourcier (defaut 0, lue a chaque sort) ET l'ENUM `sourcier` de
// coa_dir_demande.type, lu au demarrage (migration data/sql/characters/updates/2026_10_21_00_coa_dir_demande_sourcier.sql).
// Sans l'une ou l'autre : rien n'est ecrit, une ligne de journal ; le module rend le lancement au bout de 180 s sans
// menace (ATTENTE_DEBUT_S).
// Journal : `coa directeur sourcier: ...` (CoaBots.log par le canal playerbots).

constexpr char const SOURCIER_OPTION[] = "AiPlayerbot.Directeur.Sourcier";

struct SourcierSortDirecteur
{
    uint32 sort;                // sort LANCE du module (AscensionArtisanatIds.h)
    char const* evenement;      // texte de la demande, lu par exploitation/directeur/sourcier.py (EVENEMENTS)
};

// Lot MINAGE : la Faille tellurique. Lots HERBORISTERIE et DEPECAGE : 9113901 (Jardin d'orage), 9113905 (Appel du
// Linceul) a ajouter ici avec leur evenement.
constexpr std::array<SourcierSortDirecteur, 1> SOURCIER_SORTS_DIRECTEUR =
{{
    { 9113900, "faille_tellurique" },
}};

std::atomic<bool> sourcierTypePresent{ false };   // ecrit une fois au demarrage (fil monde), lu des fils de carte

class coa_directeur_sourcier_spellscript : public AllSpellScript
{
public:
    coa_directeur_sourcier_spellscript() : AllSpellScript("coa_directeur_sourcier_spellscript", { ALLSPELLHOOK_ON_CAST }) { }

    void OnSpellCast(Spell* /*sort*/, Unit* lanceur, SpellInfo const* info, bool skipCheck) override
    {
        if (!info || skipCheck)
            return;
        char const* evenement = nullptr;
        for (SourcierSortDirecteur const& s : SOURCIER_SORTS_DIRECTEUR)
            if (s.sort == info->Id)
                evenement = s.evenement;
        if (!evenement)
            return;
        Player* joueur = lanceur ? lanceur->ToPlayer() : nullptr;
        if (!joueur || !joueur->GetSession() || joueur->GetSession()->IsBot())
            return;
        char const* raison = nullptr;
        if (!sConfigMgr->GetOption<bool>(SOURCIER_OPTION, false, false))
            raison = "option_eteinte";
        else if (!sourcierTypePresent.load(std::memory_order_acquire))
            raison = "enum_absente";
        if (raison)
        {
            LOG_INFO("playerbots", "coa directeur sourcier: demande non ecrite joueur={} evenement={} raison={}",
                     joueur->GetGUID().GetCounter(), evenement, raison);
            return;
        }
        Enregistrer(joueur, "sourcier", evenement);
        LOG_INFO("playerbots", "coa directeur sourcier: demande joueur={} evenement={} carte={} x={:.1f} y={:.1f} z={:.1f}",
                 joueur->GetGUID().GetCounter(), evenement, joueur->GetMapId(), joueur->GetPositionX(),
                 joueur->GetPositionY(), joueur->GetPositionZ());
    }
};

class coa_directeur_sourcier_worldscript : public WorldScript
{
public:
    coa_directeur_sourcier_worldscript() : WorldScript("coa_directeur_sourcier_worldscript", { WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override
    {
        bool present = false;
        std::string type;
        if (QueryResult r = CharacterDatabase.Query("SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE "
                "TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'coa_dir_demande' AND COLUMN_NAME = 'type'"))
        {
            type = r->Fetch()[0].Get<std::string>();
            present = type.find("'sourcier'") != std::string::npos;
        }
        sourcierTypePresent.store(present, std::memory_order_release);
        LOG_INFO("playerbots", "coa directeur sourcier: ENUM coa_dir_demande.type {} ({}) ; option {} = {} ; {} sort(s) Directeur",
                 present ? "porte `sourcier`" : "SANS `sourcier` (migration 2026_10_21_00 non appliquee) : aucune demande",
                 type.empty() ? "illisible" : type, SOURCIER_OPTION,
                 sConfigMgr->GetOption<bool>(SOURCIER_OPTION, false, false) ? 1 : 0, SOURCIER_SORTS_DIRECTEUR.size());
    }
};
