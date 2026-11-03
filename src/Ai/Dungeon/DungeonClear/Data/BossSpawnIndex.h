/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_BOSSSPAWNINDEX_H
#define _PLAYERBOT_BOSSSPAWNINDEX_H

#include <mutex>
#include <unordered_map>
#include <vector>

#include "Common.h"
#include "DBCEnums.h"
#include "DungeonBossInfo.h"
#include "ObjectGuid.h"

class BossSpawnIndex
{
public:
    static std::vector<DungeonBossInfo> const& Get(uint32 mapId, Difficulty difficulty);

    // Every DB spawn id of boss `entry` on `mapId`, all difficulties (Get keeps
    // one spawn per boss). The kill read for an instance with no InstanceScript
    // needs them: its respawn timers are keyed by spawn id, and a boss whose
    // corpse is gone or whose grid unloaded is only visible there.
    static std::vector<ObjectGuid::LowType> const& SpawnIds(uint32 mapId, uint32 entry);

private:
    static void EnsureBuilt();
    static void Build();

    static std::unordered_map<uint64, std::vector<DungeonBossInfo>> _store;
    // Keyed MakeKey(mapId, entry): the same packing, an entry in the low word.
    static std::unordered_map<uint64, std::vector<ObjectGuid::LowType>> _spawnIds;
    // Built on first Get(), which arrives from bot AI on map-update worker
    // threads: with MapUpdate.Threads > 1, bots in two instances can both be
    // first. call_once runs Build() exactly once and blocks the others until it
    // finishes; a bare bool let both threads fill _store at once (issue #34).
    static std::once_flag _once;

    static uint64 MakeKey(uint32 mapId, Difficulty difficulty)
    {
        return (static_cast<uint64>(mapId) << 32) | static_cast<uint32>(difficulty);
    }
};

#endif
