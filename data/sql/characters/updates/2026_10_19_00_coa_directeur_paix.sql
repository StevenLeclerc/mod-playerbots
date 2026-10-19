-- CoA -- le Directeur, PAIX DE ZONE (19/10/2026) : verbe `paix` du bus (CoaDirecteur.cpp, ExecuterPaix ; regle
-- coa-project exploitation/directeur/paix_zone.py). Base : acore_characters (PTR : acore_characters_ptr).
-- A APPLIQUER A LA MAIN (P-131), sur CHAQUE royaume qui doit pouvoir poser une paix. Idempotente : rejouee, elle ne
-- change rien.
--
-- AJOUT EN FIN D'ENUM : 'paix' apres les 17 valeurs en service. LU EN BASE le 19/10/2026 (Live, PTR et Armageddon) :
--   enum('treve','reaffecter','alleger','suspendre_mercenaire','lever','peupler','faire_parler','repondre',
--        'heure_du_jour','meteo','prime','chasser','recompenser','pister','attirer','soigner','evenement')
--
-- GARDE (P-276 : un MODIFY ENUM rejoue hors de son ordre retire en silence une valeur posee apres lui). La migration
-- ne s'applique que si l'ENUM actuel est EXACTEMENT celui ci-dessus (17 valeurs) ; s'il porte deja 'paix' en fin
-- (rejouee), elle ne fait rien. Tout autre ENUM : ARRET sans rien modifier, sur l'erreur
-- « Table '...refus_migration_paix_enum_inattendu' doesn't exist » (SIGNAL n'est pas permis dans une instruction
-- preparee : ERROR 1295, mesure le 04/10 sur MySQL 8.4.11, d'ou la table absente) -- relire alors l'ENUM et ecrire la
-- liste a la main.
--
-- Sans danger pour le binaire en service et pour emetteur.py d'avant : tous deux acceptent un ENUM qui PROLONGE
-- l'ENUM R3 (CoaDirSourceMysql::CommencePar, « contient » pour evenement / attirer / soigner ; emetteur.enum_prolonge).
-- Un binaire d'avant lit `paix` comme un verbe inconnu et le refuse `verbe_eteint`.
-- Le binaire de la paix demarre apres elle : « colonnes paix presentes, verbe paix lisible ». Sans elle : « colonnes
-- paix absentes, verbe paix inerte », refus `colonnes_paix`.
--
-- AUCUNE COLONNE NEUVE : `paix` utilise cible (l'id de ZONE), carte, duree_s ; joueur NULL.
--
-- VERIFICATION apres application : doit rendre la chaine ci-dessous, exactement --
--   SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
--     AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'verbe';
--   enum('treve','reaffecter','alleger','suspendre_mercenaire','lever','peupler','faire_parler','repondre','heure_du_jour','meteo','prime','chasser','recompenser','pister','attirer','soigner','evenement','paix')
--
-- RETOUR ARRIERE (ENUM remis a 17 valeurs), SEULEMENT si aucune ligne n'emploie 'paix' (sinon MySQL refuse) :
--   SELECT COUNT(*) FROM coa_dir_commande WHERE verbe = 'paix';  -- 0
--   ALTER TABLE `coa_dir_commande` MODIFY `verbe`
--     ENUM('treve','reaffecter','alleger','suspendre_mercenaire','lever','peupler','faire_parler','repondre',
--          'heure_du_jour','meteo','prime','chasser','recompenser','pister','attirer','soigner','evenement') NOT NULL;
-- Inutile en pratique : retirer `paix` de Directeur.Verbes (ou mettre paix_zone = 0 dans recit.conf) empeche toute
-- NOUVELLE paix ; celles en cours courent jusqu'a leur terme (10 min par defaut) ou se levent par
-- `emettre.py lever --cible <id>` (0 = TOUTES les consignes du bus, pas seulement les paix).

SET @coa_paix_17 := 'enum(''treve'',''reaffecter'',''alleger'',''suspendre_mercenaire'',''lever'',''peupler'',''faire_parler'',''repondre'',''heure_du_jour'',''meteo'',''prime'',''chasser'',''recompenser'',''pister'',''attirer'',''soigner'',''evenement''';
SET @coa_paix_type := (SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
                       AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'verbe');
SET @coa_paix_q := IF(@coa_paix_type = CONCAT(@coa_paix_17, ')'),
  'ALTER TABLE `coa_dir_commande` MODIFY `verbe` ENUM(''treve'',''reaffecter'',''alleger'',''suspendre_mercenaire'',''lever'',''peupler'',''faire_parler'',''repondre'',''heure_du_jour'',''meteo'',''prime'',''chasser'',''recompenser'',''pister'',''attirer'',''soigner'',''evenement'',''paix'') NOT NULL',
  IF(@coa_paix_type = CONCAT(@coa_paix_17, ',''paix'')'),
     'SELECT 1',
     'SELECT 1 FROM refus_migration_paix_enum_inattendu'));
PREPARE coa_paix_s FROM @coa_paix_q;
EXECUTE coa_paix_s;
DEALLOCATE PREPARE coa_paix_s;
