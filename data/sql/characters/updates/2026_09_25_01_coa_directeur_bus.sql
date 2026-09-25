-- CoA -- le Directeur, lot D2 (bus de commandes). Idempotent : peut etre rejoue sans effet.
-- A APPLIQUER A LA MAIN AVANT d'installer le binaire qui l'utilise (P-131).
-- Specification : coa-project/docs/SPEC-directeur-D2.md, § 2.4 et § 4.1.
--
-- UN PROPRIETAIRE PAR COLONNE (P-134) : le service INSERE une ligne et ne la modifie
-- jamais ; le jeu n'ecrit que `statut`, `raison`, `vu_le`. Une annulation est une
-- NOUVELLE ligne `lever`.

CREATE TABLE IF NOT EXISTS `coa_dir_commande` (
  `id`              INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `verbe`           ENUM('treve','reaffecter','alleger','suspendre_mercenaire','lever') NOT NULL,
  `cible`           INT UNSIGNED NOT NULL COMMENT 'guid bas vise ; pour lever : id de commande, 0 = tout',
  `joueur`          INT UNSIGNED NULL     COMMENT 'humain autour duquel on agit (levee sur stop)',
  `duree_s`         INT UNSIGNED NOT NULL DEFAULT 0,
  `carte`           SMALLINT UNSIGNED NULL,
  `x`               FLOAT NULL,
  `y`               FLOAT NULL,
  `z`               FLOAT NULL,
  `rayon`           FLOAT NULL            COMMENT 'disque assigne (reaffecter, alleger)',
  `epoque_monde`    INT UNSIGNED NOT NULL COMMENT 'coa_dir_instance.epoque lue par le service',
  `epoque_candidat` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'generation de la liste de destinations (service)',
  `source_dest`     ENUM('noeud','ville','creature','quete','') NOT NULL DEFAULT '' COMMENT 'source de la destination',
  `action`          VARCHAR(16) NULL      COMMENT 'A-xxxx du service',
  `origine`         ENUM('regle','ask','arc','manuel') NOT NULL,
  `cree_le`         DATETIME(3) NOT NULL  COMMENT 'NOW(3) du service ; au millieme pour la latence d accuse (SPEC 4.9)',
  `expire_le`       DATETIME NOT NULL     COMMENT 'au-dela, la commande n est plus executee',
  -- colonnes du JEU, seul ecrivain :
  `statut`          ENUM('en_attente','execute','refuse','expire','leve','remplace') NOT NULL DEFAULT 'en_attente',
  `raison`          VARCHAR(64) NULL,
  `vu_le`           DATETIME(3) NULL      COMMENT 'NOW(3) du jeu',
  PRIMARY KEY (`id`),
  KEY `k_statut` (`statut`, `id`),
  KEY `k_cible` (`cible`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Ecrite par le jeu a OnStartup (et a chaque bascule de l'interrupteur Bus).
CREATE TABLE IF NOT EXISTS `coa_dir_instance` (
  `id`         TINYINT UNSIGNED NOT NULL COMMENT 'toujours 1',
  `epoque`     INT UNSIGNED NOT NULL     COMMENT 'GameTime::GetStartTime(), secondes unix',
  `demarre_le` DATETIME NOT NULL,
  `bus`        TINYINT UNSIGNED NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Ecrite par les sessions qui lancent un banc (outils/declarer-banc.py de
-- preference), lue par le service seul ; le jeu ne la lit pas.
-- FUSEAU : debut/fin sont lus par UNIX_TIMESTAMP(), donc dans le fuseau de la
-- session MySQL, qui est SYSTEM = UTC (mesure le 25/09). Ecrire TOUJOURS
-- NOW() + INTERVAL n SECOND, JAMAIS une heure en clair : une heure de Paris
-- tapee a la main decale le banc de 1 ou 2 h.
CREATE TABLE IF NOT EXISTS `coa_dir_banc` (
  `id`      INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `carte`   SMALLINT UNSIGNED NULL COMMENT 'NULL = tout le monde',
  `x`       FLOAT NULL,
  `y`       FLOAT NULL,
  `rayon`   FLOAT NULL,
  `debut`   DATETIME NOT NULL,
  `fin`     DATETIME NOT NULL,
  `session` VARCHAR(64) NOT NULL,
  `motif`   VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
