-- CoA -- souvenirs du compagnon (27/10/2026). Code : src/Ai/Coa/CoaSouvenirs.cpp.
-- A APPLIQUER A LA MAIN (P-131), base des PERSONNAGES du royaume. Idempotente : rejouee, elle ne change rien.
--
-- UN SEUL AUTEUR (P-134) : le worldserver (CoaSouvenirs.cpp). Le Directeur, le panneau et les outils la LISENT.
-- Une ligne par souvenir d'un humain : genre, objet (titre de quete, nom de creature, competence de recolte, niveau),
-- zone, vecu ou non avec son compagnon, et quand le compagnon l'a dit. Heures en UTC. Les lignes de plus de 30 jours
-- sont purgees au demarrage du worldserver.
--
-- ORDRE : cette migration AVANT le demarrage d'un binaire qui porte CoaSouvenirs.cpp AVEC
-- AiPlayerbot.CoaCompagnons.Souvenirs.Enable = 1. Sans elle, le binaire le dit au journal (« souvenirs table=absente »)
-- et reste inerte.
--
-- VERIFICATION : SHOW CREATE TABLE coa_compagnon_souvenir;
-- RETOUR ARRIERE : Souvenirs.Enable = 0, puis DROP TABLE coa_compagnon_souvenir;

CREATE TABLE IF NOT EXISTS `coa_compagnon_souvenir` (
  `id`          INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `humain_guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid du personnage humain',
  `bot_guid`    INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'compagnon present (ensemble = 1), 0 sinon',
  `genre`       ENUM('mort','quete','danger','recolte','exploit','niveau') NOT NULL,
  `objet`       VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'quete, creature, competence (minage, herboristerie, depecage) ou niveau',
  `zone`        INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'AreaTable.dbc',
  `ensemble`    TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 : compagnon eveille a moins de 80 yd',
  `quand`       DATETIME NOT NULL COMMENT 'UTC',
  `dit_le`      DATETIME NULL COMMENT 'UTC : chuchote par le compagnon',
  PRIMARY KEY (`id`),
  KEY `k_humain` (`humain_guid`, `dit_le`, `quand`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='CoA : souvenirs des humains pour leur compagnon (CoaSouvenirs.cpp)';
