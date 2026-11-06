/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "TestRun/DcOffTankAggro.h"

using DcTestRun::AggroRole;
using DcTestRun::FoldOffTankAggro;
using DcTestRun::MobOnMember;
using DcTestRun::OffTankAggro;

namespace
{
    constexpr std::uint64_t TANK = 1, HEALER = 2, MAGE = 3;
    constexpr std::uint64_t MOB_A = 100, MOB_B = 101;
}

// An opener on the healer is a switch, and staying there is not a second one.
TEST(DcOffTankAggroTest, AMobStayingOnTheHealerCountsOnce)
{
    std::unordered_map<std::uint64_t, std::uint64_t> last;
    OffTankAggro out;
    FoldOffTankAggro(last, {{MOB_A, HEALER, AggroRole::Healer}}, true, 500, out);
    FoldOffTankAggro(last, {{MOB_A, HEALER, AggroRole::Healer}}, true, 500, out);
    EXPECT_EQ(out.healerSwitches, 1u);
    EXPECT_EQ(out.healerMobMs, 1000u);
    EXPECT_EQ(out.dpsSwitches, 0u);
}

// Tank -> healer -> tank -> healer is the #11173 shape: two switches.
TEST(DcOffTankAggroTest, AMobBouncingOffTheTankCountsEachTime)
{
    std::unordered_map<std::uint64_t, std::uint64_t> last;
    OffTankAggro out;
    FoldOffTankAggro(last, {{MOB_A, TANK, AggroRole::Tank}}, true, 500, out);
    FoldOffTankAggro(last, {{MOB_A, HEALER, AggroRole::Healer}}, true, 500, out);
    FoldOffTankAggro(last, {{MOB_A, TANK, AggroRole::Tank}}, true, 500, out);
    FoldOffTankAggro(last, {{MOB_A, HEALER, AggroRole::Healer}}, true, 500, out);
    EXPECT_EQ(out.healerSwitches, 2u);
    EXPECT_EQ(out.healerMobMs, 1000u);
}

// With the tank down every mob is on somebody else: that is the wipe, not a
// threat problem, and it must not be counted.
TEST(DcOffTankAggroTest, NothingCountsWhileTheTankIsDead)
{
    std::unordered_map<std::uint64_t, std::uint64_t> last;
    OffTankAggro out;
    FoldOffTankAggro(last, {{MOB_A, HEALER, AggroRole::Healer}, {MOB_B, MAGE, AggroRole::Dps}}, false, 500, out);
    EXPECT_EQ(out.healerSwitches, 0u);
    EXPECT_EQ(out.dpsSwitches, 0u);
    EXPECT_EQ(out.healerMobMs, 0u);
}

// Healer to mage is a new victim, so it is a dps switch.
TEST(DcOffTankAggroTest, HoppingBetweenNonTanksCountsForTheNewVictim)
{
    std::unordered_map<std::uint64_t, std::uint64_t> last;
    OffTankAggro out;
    FoldOffTankAggro(last, {{MOB_A, HEALER, AggroRole::Healer}}, true, 500, out);
    FoldOffTankAggro(last, {{MOB_A, MAGE, AggroRole::Dps}}, true, 500, out);
    EXPECT_EQ(out.healerSwitches, 1u);
    EXPECT_EQ(out.dpsSwitches, 1u);
}
