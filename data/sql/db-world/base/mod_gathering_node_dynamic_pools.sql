-- Backup of the pool_template.max_limit values raised by mod-gathering-node-dynamic.
-- The module creates this table on demand as well; it is shipped so the world DB
-- assembler keeps it in sync.
CREATE TABLE IF NOT EXISTS `mod_gathering_node_dynamic_pools` (
    `pool_entry` INT UNSIGNED NOT NULL,
    `original_max_limit` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`pool_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
