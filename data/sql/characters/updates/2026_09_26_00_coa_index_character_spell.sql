-- P-146 (26/09/2026) : index secondaire sur character_spell(spell).
--
-- Pourquoi : a chaque sort invalide qu'un personnage tente d'apprendre,
-- SpellMgr::CheckSpellValid appelle DeleteSpellFromAllPlayers, qui execute
-- `DELETE FROM character_spell WHERE spell = ?` (CHAR_DEL_INVALID_SPELL_SPELLS).
-- La cle primaire est (guid, spell) : sans index sur `spell`, chaque DELETE
-- parcourt et verrouille les ~2,3 M lignes (1,65 s mesure avec 4 Go de buffer
-- pool, 2,6 s avec 128 Mo). CharacterDatabase n'a qu'un fil asynchrone : dix
-- DELETE a la suite bloquent la file ~26 s, bus du Directeur compris.
--
-- Idempotent : ne cree l'index que s'il manque. En ligne (INPLACE, LOCK=NONE).

SET @existe := (SELECT COUNT(*) FROM information_schema.statistics
                WHERE table_schema = DATABASE()
                  AND table_name = 'character_spell'
                  AND index_name = 'idx_coa_spell');
SET @sql := IF(@existe = 0,
    'ALTER TABLE `character_spell` ADD INDEX `idx_coa_spell` (`spell`), ALGORITHM=INPLACE, LOCK=NONE',
    'DO 0');
PREPARE coa_stmt FROM @sql;
EXECUTE coa_stmt;
DEALLOCATE PREPARE coa_stmt;
