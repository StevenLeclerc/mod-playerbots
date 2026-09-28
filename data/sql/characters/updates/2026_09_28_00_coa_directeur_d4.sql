-- CoA -- le Directeur, lot D4c (verbes prime et chasser).
-- Specification : coa-project/docs/SPEC-directeur-D4.md, § 3.1 (migration), § 3.2 (garde), § 11 (ordre).
-- A APPLIQUER A LA MAIN (P-131). Idempotente : rejouee, elle ne change rien.
--
-- LE PIEGE DE L'EGALITE STRICTE (§ 3.1). Le binaire D3 et un emetteur.py non relache exigent
-- l'ENUM D3 EXACT (10 valeurs) : apres cette migration, un binaire D3 qui redemarre -- plantage
-- compris -- refuse TOUS ses verbes D3 `colonnes_d3`. D'ou l'ordre impose :
--   1. emetteur.py relache (colonnes_d3 accepte D3 ou D4) deploye ET les trois services relances ;
--   2. binaire D4 COPIE en place (il accepte l'ENUM D3 comme l'ENUM D4) ;
--   3. cette migration ;
--   4. redemarrage du monde : `coa directeur : colonnes D4 presentes, verbes D4 lisibles`.
--
-- SANS DANGER POUR UN BINAIRE D4 NON MIGRE : il ecrit `colonnes D4 absentes, verbes D4 inertes`
-- et refuse tout verbe D4 `colonnes_d4` ; les verbes D2 et D3 passent.
--
-- AUCUNE COLONNE NEUVE : `prime` utilise cible, joueur, duree_s, valeur (plafond de chasseurs) ;
-- `chasser` utilise le disque (carte, x, y, z, rayon) comme toute affectation.
--
-- VERIFICATION apres application : doit rendre la chaine ci-dessous, exactement --
--   SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
--     AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'verbe';
--   enum('treve','reaffecter','alleger','suspendre_mercenaire','lever','peupler','faire_parler','repondre','heure_du_jour','meteo','prime','chasser')
--
-- RETOUR ARRIERE (ENUM remis a 10 valeurs), SEULEMENT si aucune ligne `prime`/`chasser` n'existe
-- (sinon MySQL refuse : donnees tronquees ; les supprimer d'abord, historique perdu, a noter) :
--   SELECT COUNT(*) FROM coa_dir_commande WHERE verbe IN ('prime','chasser');   -- doit rendre 0
--   ALTER TABLE `coa_dir_commande` MODIFY `verbe`
--     ENUM('treve','reaffecter','alleger','suspendre_mercenaire','lever',
--          'peupler','faire_parler','repondre','heure_du_jour','meteo') NOT NULL;
-- puis remettre le binaire D3 AVANT tout redemarrage (§ 11).

-- On AJOUTE en fin d'ENUM : les valeurs existantes ne bougent pas. Rejouer le meme MODIFY est sans effet.
ALTER TABLE `coa_dir_commande` MODIFY `verbe`
  ENUM('treve','reaffecter','alleger','suspendre_mercenaire','lever',
       'peupler','faire_parler','repondre','heure_du_jour','meteo',
       'prime','chasser') NOT NULL;
