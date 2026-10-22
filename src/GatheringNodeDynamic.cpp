/*
 * Speeds up gathering node respawns without editing gameobject.spawntimesecs.
 *
 * Every mining or herbalism node that enters the world has its respawn delay
 * rescaled from the value stored in its spawn data, so the change applies to
 * pooled and unpooled spawns alike and is re-applied on every respawn.
 * Optionally the multiplier follows the online population, and pool density
 * (how many nodes of a pool are up at the same time) can be raised at startup.
 */

#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
constexpr char const* POOL_BACKUP_TABLE = "mod_gathering_node_dynamic_pools";
constexpr uint32 MIN_RESPAWN_FLOOR = 1; // a delay of 0 means "never respawns"

// Spawn points closer than this count as one spot. Alternative node types at a
// spot share its exact position, and two nodes this close visibly overlap.
constexpr float STACK_DISTANCE = 3.0f;
constexpr uint32 MAX_POOL_DEPTH = 16;    // guards against pool_pool loops, which the core only removes later
constexpr size_t WRITE_BATCH_SIZE = 500; // pools per INSERT/UPDATE statement

// Read from map update threads, written from the world thread.
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_debug{false};
std::atomic<bool> g_mining{true};
std::atomic<bool> g_herbalism{true};
std::atomic<uint32> g_minRespawnSeconds{30};
std::atomic<uint32> g_maxRespawnSeconds{0};
std::atomic<float> g_activeMultiplier{1.0f};

// World thread only.
float g_baseMultiplier = 0.5f;
bool g_populationEnabled = false;
uint32 g_populationMinPlayers = 10;
uint32 g_populationMaxPlayers = 100;
float g_populationMultiplier = 0.25f;
uint32 g_populationRefreshSeconds = 60;
bool g_poolEnabled = false;
float g_poolMultiplier = 1.5f;
uint32 g_poolMaxLimitCap = 0;

bool IsGatheringNodeLock(uint32 lockId, bool mining, bool herbalism)
{
    LockEntry const* lock = sLockStore.LookupEntry(lockId);
    if (!lock)
        return false;

    for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
    {
        if (lock->Type[i] != LOCK_KEY_SKILL)
            continue;

        if (mining && lock->Index[i] == LOCKTYPE_MINING)
            return true;

        if (herbalism && lock->Index[i] == LOCKTYPE_HERBALISM)
            return true;
    }

    return false;
}

uint32 ScaleRespawnDelay(uint32 baseSeconds)
{
    float const multiplier = g_activeMultiplier.load(std::memory_order_relaxed);
    float const scaled = std::round(static_cast<float>(baseSeconds) * multiplier);

    uint32 result = scaled < 0.0f ? 0 : static_cast<uint32>(scaled);

    if (uint32 const maxSeconds = g_maxRespawnSeconds.load(std::memory_order_relaxed))
        result = std::min(result, maxSeconds);

    uint32 const minSeconds = std::max(MIN_RESPAWN_FLOOR, g_minRespawnSeconds.load(std::memory_order_relaxed));
    return std::max(result, minSeconds);
}

// Nodes are chests looting trade goods: ore and stone for mining, herbs for
// herbalism. Lock data lives in the DBCs and cannot be reached from SQL, so the
// pool query identifies gathering pools through their loot instead.
std::string BuildLootSubclassList()
{
    std::string subclasses;

    if (g_mining.load())
        subclasses += std::to_string(uint32(ITEM_SUBCLASS_METAL_STONE));

    if (g_herbalism.load())
    {
        if (!subclasses.empty())
            subclasses += ",";

        subclasses += std::to_string(uint32(ITEM_SUBCLASS_HERB));
    }

    return subclasses;
}

bool PoolBackupTableExists()
{
    QueryResult result = WorldDatabase.Query("SHOW TABLES LIKE '{}'", POOL_BACKUP_TABLE);
    return result != nullptr;
}

void CreatePoolBackupTable()
{
    WorldDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS `{}` ("
        "`pool_entry` INT UNSIGNED NOT NULL,"
        "`original_max_limit` INT UNSIGNED NOT NULL,"
        "PRIMARY KEY (`pool_entry`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4", POOL_BACKUP_TABLE);
}

uint64 CountBackedUpPools()
{
    QueryResult result = WorldDatabase.Query("SELECT COUNT(*) FROM `{}`", POOL_BACKUP_TABLE);
    if (!result)
        return 0;

    return result->Fetch()[0].Get<uint64>();
}

// Puts max_limit back to the values captured the last time the module raised
// them, so a changed multiplier never compounds and disabling really reverts.
void RestorePoolLimits()
{
    WorldDatabase.DirectExecute(
        "UPDATE `pool_template` pt "
        "INNER JOIN `{}` b ON b.pool_entry = pt.entry "
        "SET pt.max_limit = b.original_max_limit", POOL_BACKUP_TABLE);

    WorldDatabase.DirectExecute("DELETE FROM `{}`", POOL_BACKUP_TABLE);
}

struct SpawnPoint
{
    uint32 map;
    float x;
    float y;
    float z;
};

using SpawnPoints = std::vector<SpawnPoint>;

// A spawn point that belongs to spot `spot` of pool `pool`.
struct OwnedPoint
{
    SpawnPoint point;
    uint32 pool;
    uint32 spot;
};

// Returns the pools in which a spawn point sits within STACK_DISTANCE of a
// point on another spot, of the same pool or of any other one. Points are
// bucketed into a STACK_DISTANCE grid, so only neighbouring cells are compared.
std::unordered_set<uint32> FindPoolsSharingASpot(std::vector<OwnedPoint> const& points)
{
    auto cellOf = [](float value) { return static_cast<int32>(std::floor(value / STACK_DISTANCE)); };
    auto keyOf = [](uint32 map, int32 cx, int32 cy)
    {
        return (uint64(map) << 48) ^ (uint64(uint32(cx) & 0xFFFFFF) << 24) ^ uint64(uint32(cy) & 0xFFFFFF);
    };

    std::unordered_map<uint64, std::vector<size_t>> grid;
    for (size_t i = 0; i < points.size(); ++i)
        grid[keyOf(points[i].point.map, cellOf(points[i].point.x), cellOf(points[i].point.y))].push_back(i);

    std::unordered_set<uint32> shared;
    for (size_t i = 0; i < points.size(); ++i)
    {
        OwnedPoint const& p = points[i];
        int32 const cx = cellOf(p.point.x);
        int32 const cy = cellOf(p.point.y);

        for (int32 dx = -1; dx <= 1; ++dx)
            for (int32 dy = -1; dy <= 1; ++dy)
            {
                auto cell = grid.find(keyOf(p.point.map, cx + dx, cy + dy));
                if (cell == grid.end())
                    continue;

                for (size_t j : cell->second)
                {
                    OwnedPoint const& q = points[j];
                    if (j <= i || q.point.map != p.point.map || (q.pool == p.pool && q.spot == p.spot))
                        continue;

                    float const ox = p.point.x - q.point.x;
                    float const oy = p.point.y - q.point.y;
                    float const oz = p.point.z - q.point.z;

                    if (ox * ox + oy * oy + oz * oz < STACK_DISTANCE * STACK_DISTANCE)
                    {
                        shared.insert(p.pool);
                        shared.insert(q.pool);
                    }
                }
            }
    }

    return shared;
}

// What the pool tables say about every pool, read once at startup.
struct PoolTables
{
    std::unordered_set<uint32> gathering;                    // pools that directly own gathering nodes
    std::unordered_map<uint32, uint32> maxLimit;             // pool_template
    std::unordered_map<uint32, uint32> motherOf;             // pool_pool child -> mother
    std::unordered_map<uint32, std::vector<uint32>> children; // pool_pool mother -> children
    std::unordered_map<uint32, SpawnPoints> points;          // pool_gameobject positions
};

PoolTables LoadPoolTables(std::string const& subclasses)
{
    PoolTables tables;

    if (QueryResult result = WorldDatabase.Query(
        "SELECT DISTINCT pg.pool_entry FROM `pool_gameobject` pg "
        "INNER JOIN `gameobject` g ON g.guid = pg.guid "
        "INNER JOIN `gameobject_template` gt ON gt.entry = g.id "
        "WHERE gt.type = {} AND gt.data0 > 0 AND ("
        "EXISTS (SELECT 1 FROM `gameobject_loot_template` glt "
        "INNER JOIN `item_template` it ON it.entry = glt.Item "
        "WHERE glt.Entry = gt.data1 AND glt.Reference = 0 AND it.class = {} AND it.subclass IN ({})) "
        "OR EXISTS (SELECT 1 FROM `gameobject_loot_template` glt "
        "INNER JOIN `reference_loot_template` rlt ON rlt.Entry = glt.Reference "
        "INNER JOIN `item_template` it ON it.entry = rlt.Item "
        "WHERE glt.Entry = gt.data1 AND glt.Reference > 0 AND it.class = {} AND it.subclass IN ({}))) "
        // Treasure chests can loot ore or herbs too, but nodes never loot gear.
        "AND NOT EXISTS (SELECT 1 FROM `gameobject_loot_template` glt "
        "INNER JOIN `item_template` it ON it.entry = glt.Item "
        "WHERE glt.Entry = gt.data1 AND glt.Reference = 0 AND it.class IN ({}, {})) "
        "AND NOT EXISTS (SELECT 1 FROM `gameobject_loot_template` glt "
        "INNER JOIN `reference_loot_template` rlt ON rlt.Entry = glt.Reference "
        "INNER JOIN `item_template` it ON it.entry = rlt.Item "
        "WHERE glt.Entry = gt.data1 AND glt.Reference > 0 AND it.class IN ({}, {}))",
        uint32(GAMEOBJECT_TYPE_CHEST),
        uint32(ITEM_CLASS_TRADE_GOODS), subclasses,
        uint32(ITEM_CLASS_TRADE_GOODS), subclasses,
        uint32(ITEM_CLASS_WEAPON), uint32(ITEM_CLASS_ARMOR),
        uint32(ITEM_CLASS_WEAPON), uint32(ITEM_CLASS_ARMOR)))
    {
        do
            tables.gathering.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    }

    if (tables.gathering.empty())
        return tables;

    if (QueryResult result = WorldDatabase.Query("SELECT entry, max_limit FROM `pool_template`"))
    {
        do
        {
            Field* fields = result->Fetch();
            tables.maxLimit[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
        } while (result->NextRow());
    }

    if (QueryResult result = WorldDatabase.Query("SELECT pool_id, mother_pool FROM `pool_pool`"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const child = fields[0].Get<uint32>();
            uint32 const mother = fields[1].Get<uint32>();

            tables.motherOf[child] = mother;
            tables.children[mother].push_back(child);
        } while (result->NextRow());
    }

    if (QueryResult result = WorldDatabase.Query(
        "SELECT pg.pool_entry, g.map, g.position_x, g.position_y, g.position_z FROM `pool_gameobject` pg "
        "INNER JOIN `gameobject` g ON g.guid = pg.guid"))
    {
        do
        {
            Field* fields = result->Fetch();
            tables.points[fields[0].Get<uint32>()].push_back(
                { fields[1].Get<uint16>(), fields[2].Get<float>(), fields[3].Get<float>(), fields[4].Get<float>() });
        } while (result->NextRow());
    }

    return tables;
}

// The pool whose max_limit decides how many nodes of this group are up: the
// outermost mother pool, or the pool itself when it has none. 0 on a loop.
uint32 FindTopPool(PoolTables const& tables, uint32 poolId)
{
    for (uint32 depth = 0; depth < MAX_POOL_DEPTH; ++depth)
    {
        auto itr = tables.motherOf.find(poolId);
        if (itr == tables.motherOf.end())
            return poolId;

        poolId = itr->second;
    }

    return 0;
}

// Adds every spawn point below poolId to points, so the shared-spot check sees
// them all. Returns false when the tree holds a pool that is not a limited
// gathering pool, or is deeper than expected.
bool CollectSpawnPoints(PoolTables const& tables, uint32 poolId, SpawnPoints& points, uint32 depth = 0)
{
    if (depth >= MAX_POOL_DEPTH)
        return false;

    auto childItr = tables.children.find(poolId);
    if (childItr == tables.children.end())
    {
        auto pointItr = tables.points.find(poolId);
        if (pointItr != tables.points.end())
            points.insert(points.end(), pointItr->second.begin(), pointItr->second.end());

        // A child pool without a limit spawns every node type on its spot at once.
        auto limitItr = tables.maxLimit.find(poolId);
        return tables.gathering.count(poolId) && limitItr != tables.maxLimit.end() && limitItr->second;
    }

    bool gatheringOnly = true;
    for (uint32 child : childItr->second)
        if (!CollectSpawnPoints(tables, child, points, depth + 1))
            gatheringOnly = false;

    return gatheringOnly;
}

// Pools hold the alternatives for their spawn points in two ways. A plain pool
// lists one node per spawn point and its max_limit says how many are up. A
// mother pool (pool_pool) has one child pool per spawn point, holding every node
// type that can appear there with max_limit 1; the mother's max_limit says how
// many spots are up. Raising a child pool would put two node types on one spot,
// so only the top pool of each group is raised, and only up to its number of
// spots. A pool with a spot that another spot also uses, in the same pool or
// in another one, is left alone, since raising it could still stack nodes.
void ApplyPoolLimits()
{
    std::string const subclasses = BuildLootSubclassList();
    if (subclasses.empty())
        return;

    PoolTables const tables = LoadPoolTables(subclasses);

    std::vector<uint32> topPools;
    for (uint32 poolId : tables.gathering)
        if (uint32 topPool = FindTopPool(tables, poolId))
            topPools.push_back(topPool);

    std::sort(topPools.begin(), topPools.end());
    topPools.erase(std::unique(topPools.begin(), topPools.end()), topPools.end());

    struct LimitChange
    {
        uint32 pool;
        uint32 original;
        uint32 raised;
    };

    struct Candidate
    {
        uint32 pool;
        uint32 original;
        uint32 spots;
    };

    std::vector<Candidate> candidates;
    std::vector<OwnedPoint> points;
    uint32 skippedMixed = 0;

    for (uint32 poolId : topPools)
    {
        // Every top pool adds its spawn points, raised or not, so a raised pool
        // never stacks onto a spot of one that is left alone.
        uint32 spots = 0;
        bool gatheringOnly = true;

        // A mother pool has one spot per child pool, a plain pool one per node.
        auto childItr = tables.children.find(poolId);
        if (childItr != tables.children.end())
        {
            for (uint32 child : childItr->second)
            {
                SpawnPoints childPoints;
                if (!CollectSpawnPoints(tables, child, childPoints))
                    gatheringOnly = false;

                if (childPoints.empty())
                    continue;

                for (SpawnPoint const& point : childPoints)
                    points.push_back({ point, poolId, spots });

                ++spots;
            }
        }
        else if (auto pointItr = tables.points.find(poolId); pointItr != tables.points.end())
        {
            for (SpawnPoint const& point : pointItr->second)
                points.push_back({ point, poolId, spots++ });
        }

        auto limitItr = tables.maxLimit.find(poolId);
        if (limitItr == tables.maxLimit.end() || !limitItr->second)
            continue; // 0 means no limit

        uint32 const original = limitItr->second;

        if (!gatheringOnly)
        {
            ++skippedMixed;
            if (g_debug.load())
                LOG_INFO("module", "GatheringNodeDynamic: pool {} also holds non-gathering or unlimited pools, left at max_limit {}", poolId, original);
            continue;
        }

        candidates.push_back({ poolId, original, spots });
    }

    // Raising a pool whose spot is also used by another spot, in the same pool
    // or a different one, would let two nodes come up there at once.
    std::unordered_set<uint32> const sharedPools = FindPoolsSharingASpot(points);

    std::vector<LimitChange> changes;
    uint32 skippedShared = 0;

    for (Candidate const& candidate : candidates)
    {
        if (sharedPools.count(candidate.pool))
        {
            ++skippedShared;
            if (g_debug.load())
                LOG_INFO("module", "GatheringNodeDynamic: pool {} has a spawn point on a spot another spawn point also uses, left at max_limit {}", candidate.pool, candidate.original);
            continue;
        }

        // Never go past the number of spots the pool actually has.
        uint32 raised = std::min(candidate.spots, static_cast<uint32>(std::lround(candidate.original * g_poolMultiplier)));

        if (g_poolMaxLimitCap)
            raised = std::min(raised, g_poolMaxLimitCap);

        // A pool that already spawns more than the cap, such as the Alterac Valley
        // one, keeps its stock limit: this feature only ever adds nodes.
        if (raised > candidate.original)
            changes.push_back({ candidate.pool, candidate.original, raised });
    }

    WorldDatabaseTransaction trans = WorldDatabase.BeginTransaction();

    for (size_t first = 0; first < changes.size(); first += WRITE_BATCH_SIZE)
    {
        size_t const last = std::min(changes.size(), first + WRITE_BATCH_SIZE);

        std::string backupValues;
        std::string limitCases;
        std::string entries;

        for (size_t i = first; i < last; ++i)
        {
            LimitChange const& change = changes[i];
            std::string const pool = std::to_string(change.pool);

            if (i != first)
            {
                backupValues += ",";
                entries += ",";
            }

            backupValues += "(" + pool + "," + std::to_string(change.original) + ")";
            limitCases += " WHEN " + pool + " THEN " + std::to_string(change.raised);
            entries += pool;
        }

        trans->Append("INSERT INTO `{}` (pool_entry, original_max_limit) VALUES {}", POOL_BACKUP_TABLE, backupValues);
        trans->Append("UPDATE `pool_template` SET max_limit = CASE entry{} END WHERE entry IN ({})", limitCases, entries);
    }

    if (!changes.empty())
        WorldDatabase.DirectCommitTransaction(trans);

    LOG_INFO("module", "GatheringNodeDynamic: raised max_limit on {} gathering pools (x{:.2f}), left {} alone because a spot is shared and {} because they hold other pools",
        changes.size(), g_poolMultiplier, skippedShared, skippedMixed);
}
}

class GatheringNodeDynamicWorldScript : public WorldScript
{
public:
    GatheringNodeDynamicWorldScript() : WorldScript("GatheringNodeDynamicWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_UPDATE
    })
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig();
        RefreshMultiplier();
        _refreshTimerMs = 0;

        // Runs before PoolMgr reads pool_template on startup, so the new limits
        // are picked up by this boot. On a config reload the pools are already
        // in memory and only the respawn side takes effect immediately.
        if (!reload)
            SyncPoolLimits();
        else if (g_poolEnabled)
            LOG_INFO("module", "GatheringNodeDynamic: pool density is only applied during startup, restart the worldserver to change it");
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_enabled.load(std::memory_order_relaxed) || !g_populationEnabled)
            return;

        if (_refreshTimerMs > diff)
        {
            _refreshTimerMs -= diff;
            return;
        }

        _refreshTimerMs = g_populationRefreshSeconds * IN_MILLISECONDS;
        RefreshMultiplier();
    }

private:
    void LoadConfig()
    {
        g_enabled.store(sConfigMgr->GetOption<bool>("GatheringNodeDynamic.Enable", true));
        g_debug.store(sConfigMgr->GetOption<bool>("GatheringNodeDynamic.Debug", false));
        g_mining.store(sConfigMgr->GetOption<bool>("GatheringNodeDynamic.Mining.Enable", true));
        g_herbalism.store(sConfigMgr->GetOption<bool>("GatheringNodeDynamic.Herbalism.Enable", true));

        if (g_enabled.load() && !g_mining.load() && !g_herbalism.load())
            LOG_ERROR("module", "GatheringNodeDynamic: both Mining.Enable and Herbalism.Enable are off, nothing will be rescaled");

        g_baseMultiplier = std::max(0.01f, sConfigMgr->GetOption<float>("GatheringNodeDynamic.Respawn.Multiplier", 0.5f));

        uint32 minSeconds = sConfigMgr->GetOption<uint32>("GatheringNodeDynamic.Respawn.MinSeconds", 30);
        uint32 maxSeconds = sConfigMgr->GetOption<uint32>("GatheringNodeDynamic.Respawn.MaxSeconds", 0);

        if (maxSeconds && maxSeconds < minSeconds)
        {
            LOG_ERROR("module", "GatheringNodeDynamic: Respawn.MaxSeconds ({}) is below Respawn.MinSeconds ({}), ignoring the maximum", maxSeconds, minSeconds);
            maxSeconds = 0;
        }

        g_minRespawnSeconds.store(minSeconds);
        g_maxRespawnSeconds.store(maxSeconds);

        g_populationEnabled = sConfigMgr->GetOption<bool>("GatheringNodeDynamic.Population.Enable", false);
        g_populationMinPlayers = sConfigMgr->GetOption<uint32>("GatheringNodeDynamic.Population.MinPlayers", 10);
        g_populationMaxPlayers = sConfigMgr->GetOption<uint32>("GatheringNodeDynamic.Population.MaxPlayers", 100);
        g_populationMultiplier = std::max(0.01f, sConfigMgr->GetOption<float>("GatheringNodeDynamic.Population.MultiplierAtMaxPlayers", 0.25f));
        g_populationRefreshSeconds = std::max<uint32>(5, sConfigMgr->GetOption<uint32>("GatheringNodeDynamic.Population.RefreshSeconds", 60));

        if (g_populationEnabled && g_populationMaxPlayers <= g_populationMinPlayers)
        {
            LOG_ERROR("module", "GatheringNodeDynamic: Population.MaxPlayers ({}) must be above Population.MinPlayers ({}), population scaling disabled",
                g_populationMaxPlayers, g_populationMinPlayers);
            g_populationEnabled = false;
        }

        g_poolEnabled = sConfigMgr->GetOption<bool>("GatheringNodeDynamic.Pool.Enable", false);
        g_poolMultiplier = std::max(1.0f, sConfigMgr->GetOption<float>("GatheringNodeDynamic.Pool.Multiplier", 1.5f));
        g_poolMaxLimitCap = sConfigMgr->GetOption<uint32>("GatheringNodeDynamic.Pool.MaxLimitCap", 0);
    }

    void RefreshMultiplier()
    {
        float multiplier = g_baseMultiplier;

        if (g_populationEnabled)
        {
            uint32 const players = sWorldSessionMgr->GetPlayerCount();

            if (players >= g_populationMaxPlayers)
            {
                multiplier = g_populationMultiplier;
            }
            else if (players > g_populationMinPlayers)
            {
                float const progress = static_cast<float>(players - g_populationMinPlayers) /
                    static_cast<float>(g_populationMaxPlayers - g_populationMinPlayers);
                multiplier = g_baseMultiplier + (g_populationMultiplier - g_baseMultiplier) * progress;
            }
        }

        float const previous = g_activeMultiplier.exchange(multiplier);

        if (g_debug.load() && std::fabs(previous - multiplier) > 0.001f)
            LOG_INFO("module", "GatheringNodeDynamic: respawn multiplier is now {:.2f} ({} players online)", multiplier, sWorldSessionMgr->GetPlayerCount());
    }

    void SyncPoolLimits()
    {
        if (!g_poolEnabled || !g_enabled.load())
        {
            // Only touch the DB when a previous run left raised limits behind.
            if (PoolBackupTableExists() && CountBackedUpPools())
            {
                RestorePoolLimits();
                LOG_INFO("module", "GatheringNodeDynamic: pool density disabled, original pool_template.max_limit values restored");
            }

            return;
        }

        CreatePoolBackupTable();
        RestorePoolLimits();
        ApplyPoolLimits();
    }

    uint32 _refreshTimerMs = 0;
};

class GatheringNodeDynamicGameObjectScript : public AllGameObjectScript
{
public:
    GatheringNodeDynamicGameObjectScript() : AllGameObjectScript("GatheringNodeDynamicGameObjectScript") { }

    void OnGameObjectAddWorld(GameObject* go) override
    {
        if (!g_enabled.load(std::memory_order_relaxed))
            return;

        if (!go || !go->GetSpawnId() || !go->isSpawnedByDefault())
            return;

        if (go->GetGoType() != GAMEOBJECT_TYPE_CHEST)
            return;

        GameObjectTemplate const* goInfo = go->GetGOInfo();
        if (!goInfo)
            return;

        if (!IsGatheringNodeLock(goInfo->GetLockId(), g_mining.load(std::memory_order_relaxed), g_herbalism.load(std::memory_order_relaxed)))
            return;

        // Scale from the spawn data rather than the live delay: the same object
        // is re-added to the map after an unpooled respawn, and reading back our
        // own value there would shrink the timer a little further every cycle.
        GameObjectData const* data = go->GetGameObjectData();
        if (!data || data->spawntimesecs <= 0)
            return;

        // The core zeroes the delay for nodes flagged GO_FLAG_NODESPAWN; those
        // are not meant to cycle at all, so leave them alone.
        if (!go->GetRespawnDelay())
            return;

        uint32 const baseDelay = static_cast<uint32>(data->spawntimesecs);
        uint32 const newDelay = ScaleRespawnDelay(baseDelay);

        if (newDelay == go->GetRespawnDelay())
            return;

        go->SetRespawnDelay(static_cast<int32>(newDelay));

        if (g_debug.load(std::memory_order_relaxed))
            LOG_INFO("module", "GatheringNodeDynamic: node {} (guid {}) respawn {}s -> {}s", goInfo->entry, go->GetSpawnId(), baseDelay, newDelay);
    }
};

void AddGatheringNodeDynamicScripts()
{
    new GatheringNodeDynamicWorldScript();
    new GatheringNodeDynamicGameObjectScript();
}
