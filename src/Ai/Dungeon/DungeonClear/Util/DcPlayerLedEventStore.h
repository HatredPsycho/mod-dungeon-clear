/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPLAYERLEDEVENTSTORE_H
#define _PLAYERBOT_DCPLAYERLEDEVENTSTORE_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "Ai/Dungeon/DungeonClear/Util/DcPlayerLedEventDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

// PLAYER-LED v2: the event state of Main Tank mode, kept PER INSTANCE.
//
// A DC run keeps its event state in the leader bot's context (EventProgress,
// ClearedAnchors, SeenDueEvents). Main Tank mode has no run and its executor
// changes hands whenever the elected bot dies or leaves, so its state lives
// here instead, keyed by instance id: a DUPLICATE of those three values, never
// a move of them, so a DC run's path is untouched. Forgotten when the instance
// has no Main Tank player any more, or when he leaves it.
//
// Engine-free (GUIDs and ids only) so the gtest suite drives it directly.
struct DcPlayerLedInstanceEvents
{
    std::uint32_t mapId = 0;
    DungeonEventProgress progress;           // the active event (EventProgress twin)
    std::unordered_set<std::uint32_t> done;  // event ids finished, skipped or stalled
    std::unordered_set<std::uint32_t> seenDue;
    std::uint32_t goOrderMs = 0;             // last "dc go" (0 = none)
    std::uint64_t executorGuid = 0;          // last elected executor (handover log)
    std::uint32_t lastLedMs = 0;             // last tick that saw the player leading
    std::uint32_t announcedEventId = 0;      // "waiting for your go" told for this
    std::uint32_t announcedStep = UINT32_MAX;
};

class DcPlayerLedEventStore
{
public:
    // Run `fn(state)` under the lock on `instanceId`'s state, creating it when
    // missing. A state found under another map id is a recycled instance id:
    // it starts over.
    template <typename Fn>
    auto With(std::uint32_t instanceId, std::uint32_t mapId, Fn&& fn)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _states.find(instanceId);
        if (it == _states.end() || it->second.mapId != mapId)
        {
            DcPlayerLedInstanceEvents fresh;
            fresh.mapId = mapId;
            it = _states.insert_or_assign(instanceId, std::move(fresh)).first;
            _count.store(_states.size(), std::memory_order_relaxed);
        }
        return fn(it->second);
    }

    // Lock-free first look: false whenever nothing is stored at all, which is
    // every realm where nobody uses the Main Tank bar.
    bool Any() const { return _count.load(std::memory_order_relaxed) != 0; }

    bool Has(std::uint32_t instanceId) const
    {
        if (!Any())
            return false;
        std::lock_guard<std::mutex> lock(_mutex);
        return _states.count(instanceId) != 0;
    }

    void Forget(std::uint32_t instanceId)
    {
        if (!Any())
            return;
        std::lock_guard<std::mutex> lock(_mutex);
        _states.erase(instanceId);
        _count.store(_states.size(), std::memory_order_relaxed);
    }

    // Arm the player's go. True when no live go was pending (the first "dc go"
    // of a burst: party chat reaches every bot).
    bool OrderGo(std::uint32_t instanceId, std::uint32_t mapId, std::uint32_t nowMs)
    {
        std::uint32_t const stamp = nowMs != 0 ? nowMs : 1u;
        return With(instanceId, mapId, [&](DcPlayerLedInstanceEvents& s)
        {
            bool const fresh = !DcPlayerLedEventDecision::GoOrderLive(s.goOrderMs, nowMs);
            s.goOrderMs = stamp;
            return fresh;
        });
    }

    std::size_t Size() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _states.size();
    }

private:
    mutable std::mutex _mutex;
    std::unordered_map<std::uint32_t, DcPlayerLedInstanceEvents> _states;
    std::atomic<std::size_t> _count{0};
};

#endif
