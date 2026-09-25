-- CoA -- le Directeur, lot D1. Idempotent : peut etre rejoue sans effet.
-- A APPLIQUER AVANT d'installer le binaire qui l'utilise (P-131).

-- Ce que le joueur dit au Directeur : .directeur note|ask|stop|reprise.
-- Ecrit par le jeu (CoaDirecteur.cpp), lu et marque par le service externe.
CREATE TABLE IF NOT EXISTS `coa_dir_demande` (
  `id`        INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `guid`      INT UNSIGNED NOT NULL COMMENT 'personnage',
  `compte`    INT UNSIGNED NOT NULL,
  `type`      ENUM('note','ask','stop','reprise') NOT NULL,
  `texte`     VARCHAR(500) NOT NULL DEFAULT '',
  `carte`     SMALLINT UNSIGNED NOT NULL,
  `zone`      INT UNSIGNED NOT NULL,
  `aire`      INT UNSIGNED NOT NULL,
  `x`         FLOAT NOT NULL,
  `y`         FLOAT NOT NULL,
  `z`         FLOAT NOT NULL,
  `niveau`    TINYINT UNSIGNED NOT NULL,
  `statut`    ENUM('nouveau','lu','traite','refuse') NOT NULL DEFAULT 'nouveau',
  `reponse`   VARCHAR(500) NULL,
  `cree_le`   DATETIME NOT NULL,
  `traite_le` DATETIME NULL,
  PRIMARY KEY (`id`),
  KEY `k_statut` (`statut`, `id`),
  KEY `k_guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Le drapeau d'arret, par personnage. Proprietaire : le jeu (CoaDirecteur.cpp).
-- Relu au demarrage du worldserver.
CREATE TABLE IF NOT EXISTS `coa_dir_arret` (
  `guid`   INT UNSIGNED NOT NULL,
  `depuis` DATETIME NOT NULL,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
