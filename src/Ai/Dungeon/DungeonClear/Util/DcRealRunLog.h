/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DUNGEONCLEAR_DCREALRUNLOG_H
#define _DUNGEONCLEAR_DCREALRUNLOG_H

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "TestRun/DcTestRunRecord.h"

// One JSON line per REAL Dungeon Clear run (started by a player's `dc on`, never
// a `.dc test` / lab run), appended to <dir>/runs.jsonl when the run ends, for
// the SquidBots dashboard. The engine side (who started, what died, when it
// ended) lives in DcRealRunTracker; this unit is the pure part: the record, its
// serialisation, the result rule, the run id and the rotating append.
//
// The field names are the bench record's (DcTestRunRecord) wherever the two
// overlap — dungeon token, difficulty, bossTimeline, comp — so the dashboard
// reads both with the same code. bossTimeline entries ARE DcTestRunRecord::
// BossKill, and comp entries carry DcTestRunRecord::CompEntry's name / class /
// spec / role only.

namespace DcRealRunLog
{
    struct CompEntry
    {
        std::string name;
        std::string className;  // "warrior", ... (DcBotProvisioning::ClassToken)
        std::string spec;       // AiFactory::GetPlayerSpecName
        std::string role;       // "tank" | "heal" | "dps", read from the spec
    };

    struct Record
    {
        std::string runId;        // rr-<yyyymmdd-hhmmss>-<instance>
        std::string dungeon;      // DcTestDungeonRegistry token ("map<id>" if none)
        std::string dungeonName;
        std::uint32_t difficulty = 0;  // 0 normal, 1 heroic, 2 mythic (map difficulty)
        std::uint32_t instance = 0;
        std::uint64_t startedAtMs = 0; // unix ms
        std::uint64_t endedAtMs = 0;
        std::uint32_t durationS = 0;
        std::string result;       // "clear" | "wipe" | "aborted" | "setup_failed"
        std::uint32_t bossesKilled = 0;
        std::uint32_t bossesTotal = 0;
        std::vector<DcTestRunRecord::BossKill> bossTimeline;
        std::uint32_t wipes = 0;
        std::vector<CompEntry> comp;
        std::vector<std::string> players;  // human-controlled members
    };

    // How the run ended. `allCleared`: the disable came from the all-cleared rung
    // (or every roster entry was credited); `partyDied`: from the death/wipe
    // rung. "setup_failed" is part of the vocabulary the dashboard shares with
    // the bench, but a real run that never set up is simply not written.
    char const* ResultFor(bool allCleared, bool partyDied, std::uint32_t bossesKilled,
                          std::uint32_t bossesTotal);

    // rr-<yyyymmdd-hhmmss>-<instance>, local time like the bench's tr- ids.
    std::string MakeRunId(std::time_t startedAt, std::uint32_t instance);

    std::string ToJsonl(Record const& rec);

    // The folder: `configured` when set, else <logsDir>/squidbot. logsDir is
    // the core's LogsDir ("" = the working directory).
    std::string ResolveDir(std::string const& configured, std::string const& logsDir);

    inline constexpr std::uint64_t kRotateBytes = 5ull * 1024 * 1024;

    // Append `line` + '\n' to <dir>/runs.jsonl, creating the folder. Past
    // `rotateBytes` the file becomes runs.jsonl.1 (replacing the old one) and a
    // fresh file starts. Serialised by a process-wide mutex (one writer per map
    // thread). False when the file could not be written.
    bool AppendLine(std::string const& dir, std::string const& line,
                    std::uint64_t rotateBytes = kRotateBytes);
}

#endif  // _DUNGEONCLEAR_DCREALRUNLOG_H
