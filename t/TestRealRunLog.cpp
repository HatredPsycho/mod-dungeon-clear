/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "Ai/Dungeon/DungeonClear/Util/DcRealRunLog.h"

using DcRealRunLog::Record;

namespace
{
    Record Sample()
    {
        Record r;
        r.runId = "rr-20261109-120000-42";
        r.dungeon = "sfk";
        r.dungeonName = "Shadowfang Keep";
        r.difficulty = 1;
        r.instance = 42;
        r.startedAtMs = 1000;
        r.endedAtMs = 61000;
        r.durationS = 60;
        r.result = "clear";
        r.bossesKilled = 1;
        r.bossesTotal = 2;
        DcTestRunRecord::BossKill k;
        k.t = 30;
        k.entry = 3914;
        k.name = "Rethilgore";
        k.via = "mask";
        r.bossTimeline.push_back(k);
        r.wipes = 2;
        r.comp.push_back({"Tanky", "warrior", "prot", "tank"});
        r.players.push_back("Hu\"man");
        return r;
    }

    std::string ReadAll(std::filesystem::path const& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
}

TEST(DcRealRunLogTest, SerialisesTheDashboardLine)
{
    EXPECT_EQ(DcRealRunLog::ToJsonl(Sample()),
              "{\"runId\":\"rr-20261109-120000-42\",\"dungeon\":\"sfk\",\"dungeonName\":\"Shadowfang Keep\","
              "\"difficulty\":1,\"instance\":42,\"startedAtMs\":1000,\"endedAtMs\":61000,\"durationS\":60,"
              "\"result\":\"clear\",\"bossesKilled\":1,\"bossesTotal\":2,"
              "\"bossTimeline\":[{\"t\":30,\"entry\":3914,\"name\":\"Rethilgore\",\"via\":\"mask\"}],"
              "\"wipes\":2,\"comp\":[{\"name\":\"Tanky\",\"class\":\"warrior\",\"spec\":\"prot\",\"role\":\"tank\"}],"
              "\"players\":[\"Hu\\\"man\"]}");
}

TEST(DcRealRunLogTest, EmptyListsStayValidJson)
{
    Record r;
    std::string const line = DcRealRunLog::ToJsonl(r);
    EXPECT_NE(line.find("\"bossTimeline\":[]"), std::string::npos);
    EXPECT_NE(line.find("\"comp\":[]"), std::string::npos);
    EXPECT_NE(line.find("\"players\":[]}"), std::string::npos);
    EXPECT_EQ(line.find('\n'), std::string::npos);
}

TEST(DcRealRunLogTest, ResultRule)
{
    EXPECT_STREQ(DcRealRunLog::ResultFor(true, false, 0, 5), "clear");
    EXPECT_STREQ(DcRealRunLog::ResultFor(true, true, 0, 5), "clear");
    EXPECT_STREQ(DcRealRunLog::ResultFor(false, true, 4, 5), "wipe");
    EXPECT_STREQ(DcRealRunLog::ResultFor(false, false, 2, 5), "aborted");
    EXPECT_STREQ(DcRealRunLog::ResultFor(false, false, 5, 5), "clear");
    EXPECT_STREQ(DcRealRunLog::ResultFor(false, false, 0, 0), "aborted");
}

TEST(DcRealRunLogTest, RunIdCarriesTheInstance)
{
    std::string const id = DcRealRunLog::MakeRunId(1700000000, 77);
    ASSERT_EQ(id.size(), std::string("rr-yyyymmdd-hhmmss-77").size());
    EXPECT_EQ(id.rfind("rr-", 0), 0u);
    EXPECT_EQ(id.substr(id.size() - 3), "-77");
}

TEST(DcRealRunLogTest, ResolveDirDefaultsUnderLogsDir)
{
    EXPECT_EQ(DcRealRunLog::ResolveDir("/data/coa", "/logs/"), "/data/coa");
    EXPECT_EQ(DcRealRunLog::ResolveDir("", ""), "squidbot");
    EXPECT_EQ(std::filesystem::path(DcRealRunLog::ResolveDir("", "logs/")),
              std::filesystem::path("logs") / "squidbot");
}

TEST(DcRealRunLogTest, AppendsAndRotates)
{
    namespace fs = std::filesystem;
    fs::path const dir = fs::temp_directory_path() / "dc_real_run_log_test" / "squidbot";
    std::error_code ec;
    fs::remove_all(dir.parent_path(), ec);

    ASSERT_TRUE(DcRealRunLog::AppendLine(dir.string(), "a", 4));
    ASSERT_TRUE(DcRealRunLog::AppendLine(dir.string(), "b", 4));
    EXPECT_EQ(ReadAll(dir / "runs.jsonl"), "a\nb\n");

    // 4 bytes reached: the next line rotates first.
    ASSERT_TRUE(DcRealRunLog::AppendLine(dir.string(), "c", 4));
    EXPECT_EQ(ReadAll(dir / "runs.jsonl.1"), "a\nb\n");
    EXPECT_EQ(ReadAll(dir / "runs.jsonl"), "c\n");

    // A second rotation replaces the old .1.
    ASSERT_TRUE(DcRealRunLog::AppendLine(dir.string(), "dd", 4));
    ASSERT_TRUE(DcRealRunLog::AppendLine(dir.string(), "e", 4));
    EXPECT_EQ(ReadAll(dir / "runs.jsonl.1"), "c\ndd\n");
    EXPECT_EQ(ReadAll(dir / "runs.jsonl"), "e\n");

    fs::remove_all(dir.parent_path(), ec);
}
