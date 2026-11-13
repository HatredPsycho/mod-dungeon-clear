/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Util/DcRealRunLog.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

#include "Timer.h"  // localtime_r on Windows

namespace DcRealRunLog
{
    namespace
    {
        void AppendEscaped(std::ostringstream& s, std::string_view v)
        {
            s << '"' << DcTestRunRecord::EscapeJson(v) << '"';
        }
    }

    char const* ResultFor(bool allCleared, bool partyDied, std::uint32_t bossesKilled,
                          std::uint32_t bossesTotal)
    {
        if (allCleared)
            return "clear";
        if (partyDied)
            return "wipe";
        // A run that ended some other way (`dc off`, the last boss skipped, the
        // leader gone) still counts as a clear when every roster entry was
        // credited along the way.
        if (bossesTotal > 0 && bossesKilled >= bossesTotal)
            return "clear";
        return "aborted";
    }

    std::string MakeRunId(std::time_t startedAt, std::uint32_t instance)
    {
        std::tm tmBuf{};
        localtime_r(&startedAt, &tmBuf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "rr-%Y%m%d-%H%M%S", &tmBuf);
        return std::string(buf) + "-" + std::to_string(instance);
    }

    std::string ToJsonl(Record const& rec)
    {
        std::ostringstream s;
        s << "{\"runId\":";
        AppendEscaped(s, rec.runId);
        s << ",\"dungeon\":";
        AppendEscaped(s, rec.dungeon);
        s << ",\"dungeonName\":";
        AppendEscaped(s, rec.dungeonName);
        s << ",\"difficulty\":" << rec.difficulty
          << ",\"instance\":" << rec.instance
          << ",\"startedAtMs\":" << rec.startedAtMs
          << ",\"endedAtMs\":" << rec.endedAtMs
          << ",\"durationS\":" << rec.durationS
          << ",\"result\":";
        AppendEscaped(s, rec.result);
        s << ",\"bossesKilled\":" << rec.bossesKilled
          << ",\"bossesTotal\":" << rec.bossesTotal
          << ",\"bossTimeline\":[";
        for (std::size_t i = 0; i < rec.bossTimeline.size(); ++i)
        {
            DcTestRunRecord::BossKill const& b = rec.bossTimeline[i];
            if (i)
                s << ',';
            s << "{\"t\":" << b.t << ",\"entry\":" << b.entry << ",\"name\":";
            AppendEscaped(s, b.name);
            s << ",\"via\":";
            AppendEscaped(s, b.via);
            s << '}';
        }
        s << "],\"wipes\":" << rec.wipes << ",\"comp\":[";
        for (std::size_t i = 0; i < rec.comp.size(); ++i)
        {
            CompEntry const& c = rec.comp[i];
            if (i)
                s << ',';
            s << "{\"name\":";
            AppendEscaped(s, c.name);
            s << ",\"class\":";
            AppendEscaped(s, c.className);
            s << ",\"spec\":";
            AppendEscaped(s, c.spec);
            s << ",\"role\":";
            AppendEscaped(s, c.role);
            s << '}';
        }
        s << "],\"players\":[";
        for (std::size_t i = 0; i < rec.players.size(); ++i)
        {
            if (i)
                s << ',';
            AppendEscaped(s, rec.players[i]);
        }
        s << "]}";
        return s.str();
    }

    std::string ResolveDir(std::string const& configured, std::string const& logsDir)
    {
        if (!configured.empty())
            return configured;
        if (logsDir.empty())
            return "squidbot";
        std::filesystem::path p(logsDir);
        p /= "squidbot";
        return p.string();
    }

    bool AppendLine(std::string const& dir, std::string const& line, std::uint64_t rotateBytes)
    {
        static std::mutex mtx;
        std::lock_guard<std::mutex> lock(mtx);

        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path const folder(dir);
        fs::create_directories(folder, ec);  // ec set (and ignored) when it exists
        fs::path const file = folder / "runs.jsonl";

        std::uintmax_t const size = fs::file_size(file, ec);
        if (!ec && size >= rotateBytes)
        {
            fs::path const old = folder / "runs.jsonl.1";
            std::error_code rmEc;
            fs::remove(old, rmEc);  // rename does not replace on Windows
            std::error_code mvEc;
            fs::rename(file, old, mvEc);
        }

        std::ofstream out(file, std::ios::out | std::ios::app | std::ios::binary);
        if (!out.is_open())
            return false;
        out << line << '\n';
        out.flush();
        return static_cast<bool>(out);
    }
}
