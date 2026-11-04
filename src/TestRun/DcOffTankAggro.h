/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCOFFTANKAGGRO_H
#define _PLAYERBOT_DCOFFTANKAGGRO_H

#include <cstdint>
#include <unordered_map>
#include <vector>

// "Do the mobs run for the healer?" (Jealous #11173) — measured, not felt.
//
// Every monitor sample the job lists which mob is beating on which member, and
// the role that member holds in the comp. This is the pure rule that turns
// those snapshots into counts: no engine types, so the counting is unit-tested.
namespace DcTestRun
{
    enum class AggroRole : std::uint8_t
    {
        Tank,
        Healer,
        Dps
    };

    struct MobOnMember
    {
        std::uint64_t mob = 0;     // creature guid (raw)
        std::uint64_t victim = 0;  // member guid (raw)
        AggroRole role = AggroRole::Dps;
    };

    struct OffTankAggro
    {
        std::uint32_t healerSwitches = 0;
        std::uint32_t dpsSwitches = 0;
        std::uint32_t healerMobMs = 0;
        std::uint32_t dpsMobMs = 0;
    };

    // Fold one sample. A SWITCH is a mob whose victim is a non-tank and was not
    // that member at the previous sample: an opener on the healer counts, so
    // does a mob bouncing tank -> healer -> tank -> healer (twice). Nothing
    // counts while the tank is down — then every mob is on a non-tank by
    // necessity, which is the wipe, not the threat bug. `last` is replaced by
    // this sample's picture either way, so a mob that leaves and comes back
    // after a tank rez counts again.
    inline void FoldOffTankAggro(std::unordered_map<std::uint64_t, std::uint64_t>& last,
                                 std::vector<MobOnMember> const& now, bool tankAlive, std::uint32_t dtMs,
                                 OffTankAggro& out)
    {
        std::unordered_map<std::uint64_t, std::uint64_t> next;
        next.reserve(now.size());
        for (MobOnMember const& m : now)
        {
            next[m.mob] = m.victim;
            if (!tankAlive || m.role == AggroRole::Tank)
                continue;
            auto const it = last.find(m.mob);
            bool const switched = it == last.end() || it->second != m.victim;
            if (m.role == AggroRole::Healer)
            {
                out.healerSwitches += switched ? 1u : 0u;
                out.healerMobMs += dtMs;
            }
            else
            {
                out.dpsSwitches += switched ? 1u : 0u;
                out.dpsMobMs += dtMs;
            }
        }
        last.swap(next);
    }
}

#endif  // _PLAYERBOT_DCOFFTANKAGGRO_H
