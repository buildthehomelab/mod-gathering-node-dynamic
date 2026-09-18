-- Backup of the pool_template.max_limit values raised by mod-mining-node-dynamic.
-- The module creates this table on demand as well; it is shipped so the world DB
-- assembler keeps it in sync.
CREATE TABLE IF NOT EXISTS `mod_mining_node_dynamic_pools` (
    `pool_entry` INT UNSIGNED NOT NULL,
    `original_max_limit` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`pool_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
