# Gathering Node Dynamic

Increases how often mining and herbalism nodes are available on an AzerothCore server, without
editing `gameobject.spawntimesecs` by hand.

Two independent levers:

- **Respawn timer** — every gathering node gets its respawn delay rescaled as it spawns, optionally
  scaling with how many players are online.
- **Pool density** (optional) — raises `pool_template.max_limit` on gathering pools so more nodes
  are up at the same time.

Mining and herbalism are switched separately, so the module can run for one profession only.

---

## How it works

Gathering nodes are chests whose lock requires the Mining or Herbalism skill (`LOCKTYPE_MINING` /
`LOCKTYPE_HERBALISM` in `Lock.dbc`), so the module identifies them from lock data rather than from an
entry list — custom nodes are picked up automatically.

When a node enters the world, its respawn delay is recomputed from the `spawntimesecs` stored in its
spawn data and written back with `SetRespawnDelay`. Nothing in the database is modified, the value is
re-derived from the original on every spawn (so repeated respawns never compound), and it works for
pooled and unpooled spawns alike: a pooled node that respawns sooner simply makes the pool re-roll a
spawn point sooner.

Nodes that are already sitting in the world when you change the config keep their current timer until
the next time they are gathered.

Pool density is different: `PoolMgr` reads `pool_template` once during startup and has no runtime
setter, so that part is written to the world database while the server boots, before pools load. The
original limits are saved in `mod_gathering_node_dynamic_pools` and restored when the feature is
turned off or the multiplier changes.

---

## Installation

```bash
cd /path/to/azerothcore-wotlk/modules
git clone https://github.com/buildthehomelab/mod-gathering-node-dynamic.git
cd /path/to/build
cmake ../ -DCMAKE_INSTALL_PREFIX=/path/to/server
make -j$(nproc) install
```

Copy `mod_gathering_node_dynamic.conf.dist` to your server's `etc/modules` folder as
`mod_gathering_node_dynamic.conf` and edit it there.

---

## Configuration

| Setting | Default | Meaning |
| --- | --- | --- |
| `GatheringNodeDynamic.Enable` | `1` | Master switch. |
| `GatheringNodeDynamic.Debug` | `0` | Logs every rescaled node and multiplier change. |
| `GatheringNodeDynamic.Mining.Enable` | `1` | Include nodes requiring Mining. |
| `GatheringNodeDynamic.Herbalism.Enable` | `1` | Include nodes requiring Herbalism. |
| `GatheringNodeDynamic.Respawn.Multiplier` | `0.5` | Multiplies the database respawn time. |
| `GatheringNodeDynamic.Respawn.MinSeconds` | `30` | Floor for the scaled timer. |
| `GatheringNodeDynamic.Respawn.MaxSeconds` | `0` | Ceiling for the scaled timer, `0` = none. |
| `GatheringNodeDynamic.Population.Enable` | `0` | Scale the multiplier with the online population. |
| `GatheringNodeDynamic.Population.MinPlayers` | `10` | At or below this, `Respawn.Multiplier` is used. |
| `GatheringNodeDynamic.Population.MaxPlayers` | `100` | At or above this, `MultiplierAtMaxPlayers` is used. |
| `GatheringNodeDynamic.Population.MultiplierAtMaxPlayers` | `0.25` | Multiplier for a full server. |
| `GatheringNodeDynamic.Population.RefreshSeconds` | `60` | Player-count sampling interval. |
| `GatheringNodeDynamic.Pool.Enable` | `0` | Raise `pool_template.max_limit` on gathering pools. |
| `GatheringNodeDynamic.Pool.Multiplier` | `1.5` | How much to raise it by. |
| `GatheringNodeDynamic.Pool.MaxLimitCap` | `0` | Hard ceiling for `max_limit`, `0` = none. Never lowers a pool below its stock limit. |

`.reload config` applies everything except the pool settings, which need a worldserver restart.

### Examples

Nodes respawn twice as fast, nothing else changes (the default):

```
GatheringNodeDynamic.Respawn.Multiplier = 0.5
```

Ore only, herbs left Blizzlike:

```
GatheringNodeDynamic.Mining.Enable = 1
GatheringNodeDynamic.Herbalism.Enable = 0
```

Quiet server keeps Blizzlike timers, a busy one runs at 3x:

```
GatheringNodeDynamic.Respawn.Multiplier = 1.0
GatheringNodeDynamic.Population.Enable = 1
GatheringNodeDynamic.Population.MinPlayers = 20
GatheringNodeDynamic.Population.MaxPlayers = 150
GatheringNodeDynamic.Population.MultiplierAtMaxPlayers = 0.33
```

Same timers, but 50% more nodes up at once per pool:

```
GatheringNodeDynamic.Pool.Enable = 1
GatheringNodeDynamic.Pool.Multiplier = 1.5
```

---

## Notes

- Pool density is capped at the number of spawn points a pool has, so a pool never tries to spawn
  more nodes than the database defines positions for. Limits only ever go up: a pool already above
  `Pool.MaxLimitCap`, such as the Alterac Valley gathering pool, keeps its stock value.
- The pool pass runs in SQL, where lock data is not reachable, so it matches chest pools whose loot
  contains Metal & Stone (mining) or Herb (herbalism) trade goods. A treasure chest pool that happens
  to loot ore could in principle be matched too; `Pool.MaxLimitCap` keeps any such case bounded.
- To revert the pool changes, set `GatheringNodeDynamic.Pool.Enable = 0` and restart; the module puts
  the saved `max_limit` values back. The respawn side leaves no trace at all.

---

## License

Released under the GNU AGPL v3, matching AzerothCore.
