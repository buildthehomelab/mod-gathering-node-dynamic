-- mod-gathering-node-dynamic: undo the module's world database changes. Run it by hand on the
-- world database after removing the module. It isn't applied automatically: AzerothCore only runs
-- the module's db-world folder.
--
-- Puts back the pool_template.max_limit values the module raised, then drops its backup table.
-- Setting GatheringNodeDynamic.Pool.Enable = 0 and restarting does the same restore, so this is
-- only needed when the module is removed while pool density was still on. It also forgets that the
-- module's base SQL was applied, so installing the module again applies it again. A second run
-- stops at the first statement, because the backup table is already gone, and changes nothing.

UPDATE `pool_template` pt
INNER JOIN `mod_gathering_node_dynamic_pools` b ON b.pool_entry = pt.entry
SET pt.max_limit = b.original_max_limit;

DROP TABLE IF EXISTS `mod_gathering_node_dynamic_pools`;

DELETE FROM `updates` WHERE `name` LIKE 'mod!_gathering!_node!_dynamic!_%' ESCAPE '!';
