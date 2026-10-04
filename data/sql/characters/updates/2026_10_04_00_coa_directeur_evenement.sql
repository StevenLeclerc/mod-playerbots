-- CoA -- le Directeur, lot C des evenements (verbe `evenement` : ambiance, son, musique, effet, annoncer).
-- Conception : coa-project/docs/EVENEMENTS-grimoire-directeur.md, § 3.5 et § 5.1-5.2 (lot C).
-- A APPLIQUER A LA MAIN (P-131). Idempotente : rejouee, elle ne change rien.
--
-- ORDRE COMMUN DES CHANTIERS (decision K1 / T1, docs/ETUDE-directeur-proposition-C-D-E.md:36, 90, 181,
-- 251 ; decision n° 13 de docs/ETUDE-chasse-primes-bots.md) : apres les 13 valeurs R3, et en fin,
--   'pister', 'attirer', 'soigner', 'evenement'
-- « Une seule migration ENUM » (proposition C-D-E, l. 181) : celle-ci ecrit les QUATRE, dans cet
-- ordre. LU EN BASE le 04/10/2026 vers 22:30 (Paris) : l'ENUM en service s'arrete a 'recompenser'
-- (13 valeurs) ; aucune des quatre n'existe encore.
--
-- GARDE. La migration ne s'applique que si l'ENUM actuel est l'ENUM R3 (13 valeurs), ou l'ENUM R3
-- prolonge d'un DEBUT de la suite pister, attirer, soigner, evenement (une migration d'un autre
-- chantier passee avant). Tout autre ENUM (une valeur ajoutee hors de l'ordre commun) : ARRET, sans
-- rien modifier, sur l'erreur « Table '...refus_migration_evenement_enum_inattendu' doesn't exist » --
-- un MODIFY aveugle supprimerait cette valeur. (SIGNAL n'est pas permis dans une instruction
-- preparee : ERROR 1295, MESURE le 04/10 sur MySQL 8.4.11 ; d'ou la table absente.)
--
-- Sans danger pour le binaire en service et pour emetteur.py d'avant : tous deux acceptent un ENUM
-- qui PROLONGE l'ENUM R3 (CoaDirSourceMysql::CommencePar, emetteur.enum_prolonge, LUS le 04/10) ; un
-- binaire d'avant lit `evenement` comme un verbe inconnu et le refuse `verbe_eteint`.
-- Le binaire du lot C demarre apres elle : « colonnes evenement presentes, verbe evenement lisible ».
-- Sans elle : « colonnes evenement absentes, verbe evenement inerte », refus `colonnes_evenement`.
--
-- AUCUNE COLONNE NEUVE : `evenement` utilise cible et joueur (le joueur humain), texte (sous-action),
-- valeur (id resolu par le service), duree_s, carte/x/y/z (le point).
--
-- VERIFICATION apres application : doit rendre la chaine ci-dessous, exactement --
--   SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
--     AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'verbe';
--   enum('treve','reaffecter','alleger','suspendre_mercenaire','lever','peupler','faire_parler','repondre','heure_du_jour','meteo','prime','chasser','recompenser','pister','attirer','soigner','evenement')
--
-- RETOUR ARRIERE (ENUM remis a 13 valeurs), SEULEMENT si aucune ligne n'emploie les quatre valeurs
-- (sinon MySQL refuse : donnees tronquees) :
--   SELECT COUNT(*) FROM coa_dir_commande WHERE verbe IN ('pister','attirer','soigner','evenement');  -- 0
--   ALTER TABLE `coa_dir_commande` MODIFY `verbe`
--     ENUM('treve','reaffecter','alleger','suspendre_mercenaire','lever',
--          'peupler','faire_parler','repondre','heure_du_jour','meteo',
--          'prime','chasser','recompenser') NOT NULL;
-- Inutile en pratique : retirer `evenement` de Directeur.Verbes suffit a l'eteindre.

SET @coa_ev_r := 'enum(''treve'',''reaffecter'',''alleger'',''suspendre_mercenaire'',''lever'',''peupler'',''faire_parler'',''repondre'',''heure_du_jour'',''meteo'',''prime'',''chasser'',''recompenser''';
SET @coa_ev_type := (SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
                     AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'verbe');
SET @coa_ev_q := IF(@coa_ev_type IN (CONCAT(@coa_ev_r, ')'),
                                     CONCAT(@coa_ev_r, ',''pister'')'),
                                     CONCAT(@coa_ev_r, ',''pister'',''attirer'')'),
                                     CONCAT(@coa_ev_r, ',''pister'',''attirer'',''soigner'')'),
                                     CONCAT(@coa_ev_r, ',''pister'',''attirer'',''soigner'',''evenement'')')),
  'ALTER TABLE `coa_dir_commande` MODIFY `verbe` ENUM(''treve'',''reaffecter'',''alleger'',''suspendre_mercenaire'',''lever'',''peupler'',''faire_parler'',''repondre'',''heure_du_jour'',''meteo'',''prime'',''chasser'',''recompenser'',''pister'',''attirer'',''soigner'',''evenement'') NOT NULL',
  'SELECT 1 FROM refus_migration_evenement_enum_inattendu');
PREPARE coa_ev_s FROM @coa_ev_q;
EXECUTE coa_ev_s;
DEALLOCATE PREPARE coa_ev_s;
