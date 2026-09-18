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

namespace
{
constexpr char const* POOL_BACKUP_TABLE = "mod_gathering_node_dynamic_pools";
constexpr uint32 MIN_RESPAWN_FLOOR = 1; // a delay of 0 means "never respawns"

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

void ApplyPoolLimits()
{
    std::string const subclasses = BuildLootSubclassList();
    if (subclasses.empty())
        return;

    WorldDatabase.DirectExecute(
        "INSERT INTO `{}` (pool_entry, original_max_limit) "
        "SELECT DISTINCT pt.entry, pt.max_limit FROM `pool_template` pt "
        "INNER JOIN `pool_gameobject` pg ON pg.pool_entry = pt.entry "
        "INNER JOIN `gameobject` g ON g.guid = pg.guid "
        "INNER JOIN `gameobject_template` gt ON gt.entry = g.id "
        "WHERE pt.max_limit > 0 AND gt.type = {} AND gt.data0 > 0 AND ("
        "EXISTS (SELECT 1 FROM `gameobject_loot_template` glt "
        "INNER JOIN `item_template` it ON it.entry = glt.Item "
        "WHERE glt.Entry = gt.data1 AND glt.Reference = 0 AND it.class = {} AND it.subclass IN ({})) "
        "OR EXISTS (SELECT 1 FROM `gameobject_loot_template` glt "
        "INNER JOIN `reference_loot_template` rlt ON rlt.Entry = glt.Reference "
        "INNER JOIN `item_template` it ON it.entry = rlt.Item "
        "WHERE glt.Entry = gt.data1 AND glt.Reference > 0 AND it.class = {} AND it.subclass IN ({})))",
        POOL_BACKUP_TABLE, uint32(GAMEOBJECT_TYPE_CHEST),
        uint32(ITEM_CLASS_TRADE_GOODS), subclasses,
        uint32(ITEM_CLASS_TRADE_GOODS), subclasses);

    // Never go past the number of spawn points the pool actually has.
    std::string limitExpression =
        "LEAST(c.spawn_points, ROUND(b.original_max_limit * " + std::to_string(g_poolMultiplier) + "))";

    if (g_poolMaxLimitCap)
        limitExpression = "LEAST(" + std::to_string(g_poolMaxLimitCap) + ", " + limitExpression + ")";

    // A pool that already spawns more than the cap, such as the Alterac Valley
    // one, keeps its stock limit: this feature only ever adds nodes.
    limitExpression = "GREATEST(b.original_max_limit, " + limitExpression + ")";

    WorldDatabase.DirectExecute(
        "UPDATE `pool_template` pt "
        "INNER JOIN `{}` b ON b.pool_entry = pt.entry "
        "INNER JOIN (SELECT pool_entry, COUNT(*) AS spawn_points FROM `pool_gameobject` GROUP BY pool_entry) c "
        "ON c.pool_entry = pt.entry "
        "SET pt.max_limit = {}", POOL_BACKUP_TABLE, limitExpression);

    LOG_INFO("module", "GatheringNodeDynamic: raised max_limit on {} gathering pools (x{:.2f})", CountBackedUpPools(), g_poolMultiplier);
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
