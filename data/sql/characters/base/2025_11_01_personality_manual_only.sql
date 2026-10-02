-- Add manual_only column to personality templates table (if not exists)
SET @col_exists = (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mod_ollama_chat_personality_templates' AND COLUMN_NAME = 'manual_only');
SET @q = IF(@col_exists = 0, 'ALTER TABLE `mod_ollama_chat_personality_templates` ADD COLUMN `manual_only` TINYINT(1) NOT NULL DEFAULT 0 AFTER `prompt`', 'SELECT 1');
PREPARE stmt FROM @q;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
