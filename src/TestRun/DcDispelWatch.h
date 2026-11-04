/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCDISPELWATCH_H
#define _PLAYERBOT_DCDISPELWATCH_H

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

// "Do the bots dispel what the encounter puts on them?" — the measure asked for
// Zul'Gurub (Mar'li's poison, Jeklik's curses and silences, Hakkar's Blood
// Siphon set-up), but filled on every run.
//
// Every monitor sample the job lists the dispellable HARMFUL auras a creature
// put on each living member, with the time each has left. This is the pure rule
// that turns those snapshots into per-spell counts: no engine types, so the
// counting is unit-tested.
namespace DcTestRun
{
    struct DebuffSample
    {
        std::uint64_t member = 0;  // player guid (raw)
        std::uint32_t spellId = 0;
        std::uint64_t caster = 0;  // creature guid (raw)
        std::uint8_t dispelType = 0;  // SpellInfo::Dispel (1 magic, 2 curse, 3 disease, 4 poison)
        std::int32_t remainingMs = -1;  // -1: no timer (permanent until removed)
        std::string name;
    };

    struct DispelStat
    {
        std::uint8_t dispelType = 0;
        std::string name;
        std::uint32_t seen = 0;     // applications seen (a new member/caster pair)
        std::uint32_t early = 0;    // gone while it still had time left: dispelled or cleansed
        std::uint32_t expired = 0;  // ran its timer out on the member
        std::uint64_t carriedMs = 0;  // member-milliseconds spent carrying it
    };

    using DebuffKey = std::tuple<std::uint64_t, std::uint32_t, std::uint64_t>;  // member, spell, caster

    // Fold one sample. `alive` lists the members standing this sample: a debuff
    // that vanishes with its carrier's death is neither dispelled nor expired and
    // is not counted either way. An aura gone with more than `graceMs` left on
    // its last reading (or with no timer at all) left early; one gone with less
    // ran out. A re-application on the same member by the same caster after a
    // removal is a new application.
    inline void FoldDispelWatch(std::map<DebuffKey, std::int32_t>& last, std::vector<DebuffSample> const& now,
                                std::unordered_set<std::uint64_t> const& alive, std::uint32_t dtMs,
                                std::uint32_t graceMs, std::map<std::uint32_t, DispelStat>& out)
    {
        std::map<DebuffKey, std::int32_t> next;
        for (DebuffSample const& s : now)
        {
            DebuffKey const key{s.member, s.spellId, s.caster};
            next[key] = s.remainingMs;
            DispelStat& st = out[s.spellId];
            if (st.name.empty())
            {
                st.name = s.name;
                st.dispelType = s.dispelType;
            }
            if (!last.count(key))
                ++st.seen;
            else
                st.carriedMs += dtMs;
        }
        for (auto const& [key, remaining] : last)
        {
            if (next.count(key) || !alive.count(std::get<0>(key)))
                continue;
            DispelStat& st = out[std::get<1>(key)];
            if (remaining < 0 || remaining > static_cast<std::int32_t>(dtMs + graceMs))
                ++st.early;
            else
                ++st.expired;
        }
        last.swap(next);
    }
}

#endif  // _PLAYERBOT_DCDISPELWATCH_H
