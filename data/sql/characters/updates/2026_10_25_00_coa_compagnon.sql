-- CoA -- bots compagnons du Directeur (« visages connus »), lot 1 (25/10/2026).
-- Conception : coa-project/docs/CONCEPTION-bots-compagnons-directeur.md § 4. Code : src/Ai/Coa/CoaCompagnons.cpp.
-- A APPLIQUER A LA MAIN (P-131), base des PERSONNAGES du royaume (acore_characters_ptr au PTR ; le Live seulement au
-- lot 6). Idempotente : rejouee, elle ne change rien.
--
-- UN SEUL AUTEUR (P-134) : le worldserver (CoaCompagnons.cpp). Le Directeur, le panneau et les outils la LISENT.
-- Une ligne par (humain, bot) ; un bot n'appartient qu'a un humain (u_bot). Heures en UTC (UTC_TIMESTAMP).
--
-- ORDRE : cette migration AVANT le demarrage d'un binaire qui porte CoaCompagnons.cpp AVEC
-- AiPlayerbot.CoaCompagnons.Enable = 1. Sans elle, le binaire le dit au journal (« table coa_compagnon absente ») et
-- les compagnons restent inertes. Option a 0 : la table n'est jamais lue, la migration est sans effet.
--
-- VERIFICATION apres application :
--   SHOW CREATE TABLE coa_compagnon;
--   SELECT COUNT(*) FROM coa_compagnon;   -- 0 avant la premiere connexion d'un humain au seuil
--
-- RETOUR ARRIERE, serveur ARRETE (en marche, ses ecritures sur la table echoueraient en silence) :
--   1. liberer les compagnons du tirage (sinon ils restent hors ligne pendant PermanentlyInWorldTime) :
--      DELETE FROM acore_playerbots_ptr.playerbots_random_bots
--       WHERE owner = 0 AND event = 'logout' AND bot IN (SELECT bot_guid FROM acore_characters_ptr.coa_compagnon);
--   2. DROP TABLE coa_compagnon;

CREATE TABLE IF NOT EXISTS `coa_compagnon` (
  `humain_guid`       INT UNSIGNED NOT NULL COMMENT 'characters.guid du personnage humain',
  `bot_guid`          INT UNSIGNED NOT NULL COMMENT 'characters.guid du bot attitre',
  `camp`              ENUM('allie','vis_a_vis') NOT NULL COMMENT 'allie : faction de l''humain ; vis_a_vis : l''autre',
  `rang`              TINYINT UNSIGNED NOT NULL COMMENT '1, 2, ... dans son camp',
  `etat`              ENUM('dort','eveille','retire') NOT NULL DEFAULT 'dort',
  `origine`           ENUM('auto','ami','guilde','panneau') NOT NULL DEFAULT 'auto',
  `cree`              DATETIME NOT NULL COMMENT 'UTC',
  `dernier_eveil`     DATETIME NULL COMMENT 'UTC',
  `secondes_ensemble` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'secondes eveille avec son humain en jeu',
  `niveau_reveil`     TINYINT UNSIGNED NULL COMMENT 'niveau du bot a son dernier reveil',
  `motif_retrait`     VARCHAR(64) NULL,
  PRIMARY KEY (`humain_guid`, `bot_guid`),
  UNIQUE KEY `u_bot` (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='CoA : bots compagnons des personnages humains (CoaCompagnons.cpp)';
