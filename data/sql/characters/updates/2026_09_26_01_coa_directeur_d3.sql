-- CoA -- le Directeur, lot D3 (verbes repondre, peupler, faire_parler, heure_du_jour, meteo).
-- Specification : coa-project/docs/SPEC-directeur-D3.md, § 3.1 (migration) et § 3.2 (garde).
-- A APPLIQUER A LA MAIN AVANT d'installer le binaire D3 (P-131). Idempotente : rejouee, elle
-- ne change rien.
--
-- SANS DANGER POUR UN BINAIRE D2 : il ne lit que ses colonnes, et une ligne d'un verbe neuf
-- lui arrive comme un verbe inconnu, refuse `verbe_eteint` (CoaDirecteur.cpp, VerbeDe).
--
-- UN PROPRIETAIRE PAR COLONNE (P-134), comme en D2 : les cinq colonnes neuves sont ecrites par
-- le SERVICE a l'INSERT, jamais modifiees ensuite ; le jeu les lit.
--
-- VERIFICATION apres application : la requete de la garde du jeu doit rendre 1 --
--   SELECT (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
--             AND TABLE_NAME = 'coa_dir_commande'
--             AND COLUMN_NAME IN ('texte','canal','valeur','intensite','delai_s')) = 5
--      AND (SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
--             AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'verbe')
--          = "enum('treve','reaffecter','alleger','suspendre_mercenaire','lever','peupler','faire_parler','repondre','heure_du_jour','meteo')";

-- 1. Verbes : on AJOUTE en fin d'ENUM, les valeurs existantes ne bougent pas. Rejouer le meme
--    MODIFY est sans effet.
ALTER TABLE `coa_dir_commande` MODIFY `verbe`
  ENUM('treve','reaffecter','alleger','suspendre_mercenaire','lever',
       'peupler','faire_parler','repondre','heure_du_jour','meteo') NOT NULL;

-- 2. Colonnes neuves, gardees par information_schema (MySQL 8.4 n'a pas ADD COLUMN IF NOT
--    EXISTS). Un bloc par colonne.
SET @a := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'texte');
SET @q := IF(@a = 0,
  'ALTER TABLE `coa_dir_commande` ADD COLUMN `texte` VARCHAR(255) CHARACTER SET utf8mb4 NULL COMMENT ''repondre, faire_parler : UTF-8, sans | ni controle''',
  'DO 0');
PREPARE coa_stmt FROM @q; EXECUTE coa_stmt; DEALLOCATE PREPARE coa_stmt;

SET @a := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'canal');
SET @q := IF(@a = 0,
  'ALTER TABLE `coa_dir_commande` ADD COLUMN `canal` ENUM(''systeme'',''dire'',''crier'',''chuchoter'',''zone'') NULL COMMENT ''faire_parler''',
  'DO 0');
PREPARE coa_stmt FROM @q; EXECUTE coa_stmt; DEALLOCATE PREPARE coa_stmt;

SET @a := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'valeur');
SET @q := IF(@a = 0,
  'ALTER TABLE `coa_dir_commande` ADD COLUMN `valeur` INT NULL COMMENT ''heure_du_jour : minute du ciel 0-1439 ; meteo : WeatherState''',
  'DO 0');
PREPARE coa_stmt FROM @q; EXECUTE coa_stmt; DEALLOCATE PREPARE coa_stmt;

SET @a := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'intensite');
SET @q := IF(@a = 0,
  'ALTER TABLE `coa_dir_commande` ADD COLUMN `intensite` FLOAT NULL COMMENT ''meteo : ]0, 1[''',
  'DO 0');
PREPARE coa_stmt FROM @q; EXECUTE coa_stmt; DEALLOCATE PREPARE coa_stmt;

SET @a := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'delai_s');
SET @q := IF(@a = 0,
  'ALTER TABLE `coa_dir_commande` ADD COLUMN `delai_s` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''faire_parler : livraison apres execution''',
  'DO 0');
PREPARE coa_stmt FROM @q; EXECUTE coa_stmt; DEALLOCATE PREPARE coa_stmt;
