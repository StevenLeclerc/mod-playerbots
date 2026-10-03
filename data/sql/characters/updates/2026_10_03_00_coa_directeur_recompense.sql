-- CoA -- le Directeur, lot R3 (verbe `recompenser` : payer une prime tenue).
-- Conception : coa-project/docs/CONCEPTION-recompenses-primes.md, § 4 et § 5.
-- A APPLIQUER A LA MAIN (P-131). Idempotente : rejouee, elle ne change rien.
--
-- ORDRE (P-131) : CETTE MIGRATION AVANT LE BINAIRE R3. Sans danger pour le binaire en
-- service et pour emetteur.py d'avant : tous deux acceptent un ENUM qui PROLONGE l'ENUM D4
-- (CoaDirSourceMysql::CommencePar, emetteur.enum_prolonge, LUS le 03/10) ; un binaire
-- d'avant lit `recompenser` comme un verbe inconnu et le refuse `verbe_eteint` -- et le
-- service ne l'emet jamais tant qu'il n'est pas dans AiPlayerbot.Directeur.Verbes.
-- Le binaire R3 demarre apres elle : « colonnes recompense presentes, verbe recompenser lisible ».
-- Sans elle, le binaire R3 ecrit « colonnes recompense absentes, verbe recompenser inerte »
-- et refuse `colonnes_recompense` ; tous les autres verbes passent.
--
-- AUCUNE COLONNE NEUVE : `recompenser` utilise cible et joueur (le joueur paye),
-- valeur (cuivres), texte (objets « id:quantite », liste blanche du jeu), action (R-<id du
-- registre coa_dir_recompense>).
--
-- VERIFICATION apres application : doit rendre la chaine ci-dessous, exactement --
--   SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
--     AND TABLE_NAME = 'coa_dir_commande' AND COLUMN_NAME = 'verbe';
--   enum('treve','reaffecter','alleger','suspendre_mercenaire','lever','peupler','faire_parler','repondre','heure_du_jour','meteo','prime','chasser','recompenser')
--
-- RETOUR ARRIERE (ENUM remis a 12 valeurs), SEULEMENT si aucune ligne `recompenser` n'existe
-- (sinon MySQL refuse : donnees tronquees) :
--   SELECT COUNT(*) FROM coa_dir_commande WHERE verbe = 'recompenser';   -- doit rendre 0
--   ALTER TABLE `coa_dir_commande` MODIFY `verbe`
--     ENUM('treve','reaffecter','alleger','suspendre_mercenaire','lever',
--          'peupler','faire_parler','repondre','heure_du_jour','meteo',
--          'prime','chasser') NOT NULL;
-- Inutile en pratique : retirer `recompenser` de Directeur.Verbes suffit a l'eteindre.

-- On AJOUTE en fin d'ENUM : les valeurs existantes ne bougent pas. Rejouer le meme MODIFY est sans effet.
ALTER TABLE `coa_dir_commande` MODIFY `verbe`
  ENUM('treve','reaffecter','alleger','suspendre_mercenaire','lever',
       'peupler','faire_parler','repondre','heure_du_jour','meteo',
       'prime','chasser','recompenser') NOT NULL;
