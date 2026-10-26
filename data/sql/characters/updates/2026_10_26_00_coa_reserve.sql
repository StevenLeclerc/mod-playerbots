-- CoA -- reserves de bots par tranche de niveau (revision du 26/10/2026 des bots compagnons).
-- Conception : coa-project/docs/CONCEPTION-bots-compagnons-directeur.md (« RÉVISION du 26/10/2026 », § 15 bis).
-- Code : src/Ai/Coa/CoaCompagnons.cpp (partie « reserve »).
-- A APPLIQUER A LA MAIN (P-131), base des PERSONNAGES du royaume (acore_characters_ptr au PTR ; le Live seulement au
-- lot 6). Idempotente : rejouee, elle ne change rien.
--
-- UN SEUL AUTEUR (P-134) : le worldserver. Le Directeur, le panneau et les outils la LISENT.
-- Une ligne par bot (cle primaire bot_guid) : un bot n'appartient qu'a une reserve, et jamais en meme temps au
-- registre des compagnons (`coa_compagnon`) -- regle tenue par le C++ (vivier et chargement), pas par une contrainte.
-- tranche = (niveau - 1) / 10, de 0 (1-10) a 5 (51-60). Heures en UTC (UTC_TIMESTAMP).
--
-- ORDRE : cette migration AVANT le demarrage d'un binaire qui porte la reserve AVEC
-- AiPlayerbot.CoaCompagnons.Reserve.Enable = 1. Sans elle, le journal dit « coa reserve: inerte raison=table_absente ».
--
-- VERIFICATION apres application :
--   SHOW CREATE TABLE coa_reserve;
--   SELECT tranche, faction, etat, origine, COUNT(*) FROM coa_reserve GROUP BY 1, 2, 3, 4;
--
-- RETOUR ARRIERE, serveur ARRETE :
--   1. liberer les bots de reserve du tirage (sinon ils restent hors ligne pendant PermanentlyInWorldTime) :
--      DELETE FROM acore_playerbots_ptr.playerbots_random_bots
--       WHERE owner = 0 AND event = 'logout' AND bot IN (SELECT bot_guid FROM acore_characters_ptr.coa_reserve);
--      (PLAYER_FLAGS_NO_XP_GAIN tombe seul a leur prochaine connexion : PlayerbotHolder::OnBotLogin le retire.)
--   2. DROP TABLE coa_reserve;

CREATE TABLE IF NOT EXISTS `coa_reserve` (
  `bot_guid`      INT UNSIGNED NOT NULL COMMENT 'characters.guid du bot',
  `tranche`       TINYINT UNSIGNED NOT NULL COMMENT '(niveau - 1) / 10 : 0 = 1-10 ... 5 = 51-60',
  `faction`       ENUM('alliance','horde') NOT NULL COMMENT 'a la constitution : ChrRaces.dbc ; verifiee par GetTeamId au reveil',
  `rang`          TINYINT UNSIGNED NOT NULL COMMENT '1, 2, ... dans sa reserve',
  `etat`          ENUM('dort','eveille','retire') NOT NULL DEFAULT 'dort',
  `origine`       ENUM('naturel','montee') NOT NULL COMMENT 'naturel : deja dans la tranche ; montee : monte a niveau_cible au premier reveil',
  `niveau_cible`  TINYINT UNSIGNED NOT NULL COMMENT 'niveau fige du bot dans sa tranche',
  `cree`          DATETIME NOT NULL COMMENT 'UTC',
  `dernier_eveil` DATETIME NULL COMMENT 'UTC',
  `niveau_reveil` TINYINT UNSIGNED NULL COMMENT 'niveau du bot a son dernier reveil (avant montee)',
  `monte_le`      DATETIME NULL COMMENT 'UTC, montee faite',
  `motif_retrait` VARCHAR(64) NULL,
  PRIMARY KEY (`bot_guid`),
  KEY `k_tranche` (`tranche`, `faction`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='CoA : reserves de bots par tranche de niveau (CoaCompagnons.cpp)';
