/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "DcModuleEnable.h"

// The master switch's one load-bearing property is that it LATCHES: the answer
// to "is mod-dungeon-clear live in this process?" is resolved once and never
// moves again. Registration into mod-playerbots' shared contexts is a one-way
// append that already-built bots hold by reference, so a value that could change
// mid-process would produce a half-state — contexts registered but strategies
// refused, or the reverse. That is what these pin.
//
// The conf read itself (LatchFromConf -> DcSettings -> sConfigMgr) needs a live
// config manager, so it is exercised at runtime; LatchValue is the same latch
// with the conf read spliced out.

namespace
{
    struct DcModuleEnableTest : public ::testing::Test
    {
        void SetUp() override { DcModule::ResetLatchForTests(); }
        void TearDown() override { DcModule::ResetLatchForTests(); }
    };
}

TEST_F(DcModuleEnableTest, LatchedEnabledIsReportedEnabled)
{
    DcModule::LatchValue(true);
    EXPECT_TRUE(DcModule::IsEnabled());
}

TEST_F(DcModuleEnableTest, LatchedDisabledIsReportedDisabled)
{
    DcModule::LatchValue(false);
    EXPECT_FALSE(DcModule::IsEnabled());
}

TEST_F(DcModuleEnableTest, FirstLatchWins)
{
    // A later conf reload (or any stray second call) must not move the answer
    // out from under bots whose engines were built against the first one.
    DcModule::LatchValue(false);
    DcModule::LatchValue(true);
    EXPECT_FALSE(DcModule::IsEnabled());

    DcModule::ResetLatchForTests();

    DcModule::LatchValue(true);
    DcModule::LatchValue(false);
    EXPECT_TRUE(DcModule::IsEnabled());
}

TEST_F(DcModuleEnableTest, RepeatedReadsAreStable)
{
    DcModule::LatchValue(false);
    for (int i = 0; i < 5; ++i)
        EXPECT_FALSE(DcModule::IsEnabled());
}

// ---- startup-check mode and the bot-module guard ---------------------------------
// A regular server sets none of the extra keys, so the module stays exactly as
// DungeonClear.Enable says.

TEST(DcModuleDecideTest, RegularServerIsEnabled)
{
    EXPECT_EQ(DcModule::Decide({}), DcModule::Verdict::Enabled);
}

TEST(DcModuleDecideTest, EnableZeroSwitchesOff)
{
    DcModule::ConfInputs in;
    in.enable = false;
    EXPECT_EQ(DcModule::Decide(in), DcModule::Verdict::SwitchedOff);
}

TEST(DcModuleDecideTest, EachStartupCheckKeyMakesTheModuleInert)
{
    DcModule::ConfInputs own;
    own.startupCheck = true;
    DcModule::ConfInputs bots;
    bots.botsStartupCheck = true;
    DcModule::ConfInputs pack;
    pack.validationMode = true;
    EXPECT_EQ(DcModule::Decide(own), DcModule::Verdict::StartupCheck);
    EXPECT_EQ(DcModule::Decide(bots), DcModule::Verdict::StartupCheck);
    EXPECT_EQ(DcModule::Decide(pack), DcModule::Verdict::StartupCheck);
}

TEST(DcModuleDecideTest, DisabledBotsMakeTheModuleInert)
{
    DcModule::ConfInputs in;
    in.botsEnabled = false;
    EXPECT_EQ(DcModule::Decide(in), DcModule::Verdict::BotsDisabled);
}

TEST(DcModuleDecideTest, EveryInactiveVerdictIsDescribed)
{
    for (auto v : {DcModule::Verdict::SwitchedOff, DcModule::Verdict::StartupCheck,
                   DcModule::Verdict::BotsDisabled})
        EXPECT_STRNE(DcModule::Describe(v), "enabled");
}
