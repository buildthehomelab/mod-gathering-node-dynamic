# Mining Node Dynamic

Increases how often mining nodes are available on an AzerothCore server, without editing
`gameobject.spawntimesecs` by hand.

Two independent levers:

- **Respawn timer** — every mining node gets its respawn delay rescaled as it spawns, optionally
  scaling with how many players are online.
- **Pool density** (optional) — raises `pool_template.max_limit` on gathering pools so more nodes
  are up at the same time.

---

## How it works

Mining nodes are chests whose lock requires the Mining skill (`LOCKTYPE_MINING` in `Lock.dbc`), so
the module identifies them from lock data rather than from an entry list — custom nodes are picked
up automatically.

When a node enters the world, its respawn delay is recomputed from the `spawntimesecs` stored in its
spawn data and written back with `SetRespawnDelay`. Nothing in the database is modified, the value is
re-derived from the original on every spawn (so repeated respawns never compound), and it works for
pooled and unpooled spawns alike: a pooled node that respawns sooner simply makes the pool re-roll a
spawn point sooner.

Nodes that are already sitting in the world when you change the config keep their current timer until
the next time they are mined.

Pool density is different: `PoolMgr` reads `pool_template` once during startup and has no runtime
setter, so that part is written to the world database while the server boots, before pools load. The
original limits are saved in `mod_mining_node_dynamic_pools` and restored when the feature is turned
off or the multiplier changes.

---

## Installation

```bash
cd /path/to/azerothcore-wotlk/modules
git clone <this-repo> mod-mining-node-dynamic
cd /path/to/build
cmake ../ -DCMAKE_INSTALL_PREFIX=/path/to/server
make -j$(nproc) install
```

Copy `mod_mining_node_dynamic.conf.dist` to your server's `etc/modules` folder as
`mod_mining_node_dynamic.conf` and edit it there.

---

## Configuration

| Setting | Default | Meaning |
| --- | --- | --- |
| `MiningNodeDynamic.Enable` | `1` | Master switch. |
| `MiningNodeDynamic.Debug` | `0` | Logs every rescaled node and multiplier change. |
| `MiningNodeDynamic.IncludeHerbalism` | `0` | Also rescale herbalism nodes. |
| `MiningNodeDynamic.Respawn.Multiplier` | `0.5` | Multiplies the database respawn time. |
| `MiningNodeDynamic.Respawn.MinSeconds` | `30` | Floor for the scaled timer. |
| `MiningNodeDynamic.Respawn.MaxSeconds` | `0` | Ceiling for the scaled timer, `0` = none. |
| `MiningNodeDynamic.Population.Enable` | `0` | Scale the multiplier with the online population. |
| `MiningNodeDynamic.Population.MinPlayers` | `10` | At or below this, `Respawn.Multiplier` is used. |
| `MiningNodeDynamic.Population.MaxPlayers` | `100` | At or above this, `MultiplierAtMaxPlayers` is used. |
| `MiningNodeDynamic.Population.MultiplierAtMaxPlayers` | `0.25` | Multiplier for a full server. |
| `MiningNodeDynamic.Population.RefreshSeconds` | `60` | Player-count sampling interval. |
| `MiningNodeDynamic.Pool.Enable` | `0` | Raise `pool_template.max_limit` on gathering pools. |
| `MiningNodeDynamic.Pool.Multiplier` | `1.5` | How much to raise it by. |
| `MiningNodeDynamic.Pool.MaxLimitCap` | `0` | Hard ceiling for `max_limit`, `0` = none. |
| `MiningNodeDynamic.Pool.NamePatterns` | `%Vein%,%Deposit%,%Obsidian Chunk%` | Which chest pools count as gathering pools. |

`.reload config` applies everything except the pool settings, which need a worldserver restart.

### Examples

Nodes respawn twice as fast, nothing else changes (the default):

```
MiningNodeDynamic.Respawn.Multiplier = 0.5
```

Quiet server keeps Blizzlike timers, a busy one runs at 3x:

```
MiningNodeDynamic.Respawn.Multiplier = 1.0
MiningNodeDynamic.Population.Enable = 1
MiningNodeDynamic.Population.MinPlayers = 20
MiningNodeDynamic.Population.MaxPlayers = 150
MiningNodeDynamic.Population.MultiplierAtMaxPlayers = 0.33
```

Same timers, but 50% more nodes up at once per pool:

```
MiningNodeDynamic.Pool.Enable = 1
MiningNodeDynamic.Pool.Multiplier = 1.5
```

---

## Notes

- Pool density is capped at the number of spawn points a pool has, so a pool never tries to spawn
  more nodes than the database defines positions for.
- The pool query matches `gameobject_template.name`, because lock data lives in the DBCs and is not
  reachable from SQL. Adjust `Pool.NamePatterns` if your world DB uses localized or custom node names.
- To revert the pool changes, set `MiningNodeDynamic.Pool.Enable = 0` and restart; the module puts the
  saved `max_limit` values back. The respawn side leaves no trace at all.

---

## License

Released under the GNU AGPL v3, matching AzerothCore.
