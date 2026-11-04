/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "TestRun/DcDispelWatch.h"

using DcTestRun::DebuffKey;
using DcTestRun::DebuffSample;
using DcTestRun::DispelStat;
using DcTestRun::FoldDispelWatch;

namespace
{
    constexpr std::uint64_t HEALER = 2, MAGE = 3, MARLI = 900;
    constexpr std::uint32_t POISON_BOLT = 24099;
    constexpr std::uint8_t DISPEL_POISON = 4;

    DebuffSample Poison(std::uint64_t member, std::int32_t leftMs)
    {
        return {member, POISON_BOLT, MARLI, DISPEL_POISON, leftMs, "Poison Bolt Volley"};
    }

    struct Watch
    {
        std::map<DebuffKey, std::int32_t> last;
        std::map<std::uint32_t, DispelStat> out;
        std::unordered_set<std::uint64_t> alive{HEALER, MAGE};
        void Step(std::vector<DebuffSample> const& now) { FoldDispelWatch(last, now, alive, 1000, 500, out); }
    };
}

// Taken off with seconds left: an early removal, i.e. a dispel or a cure.
TEST(DcDispelWatchTest, ADebuffGoneWithTimeLeftIsRemovedEarly)
{
    Watch w;
    w.Step({Poison(MAGE, 10000)});
    w.Step({Poison(MAGE, 9000)});
    w.Step({});
    DispelStat const& st = w.out[POISON_BOLT];
    EXPECT_EQ(st.seen, 1u);
    EXPECT_EQ(st.early, 1u);
    EXPECT_EQ(st.expired, 0u);
    EXPECT_EQ(st.carriedMs, 1000u);
    EXPECT_EQ(st.name, "Poison Bolt Volley");
}

// Its last reading had under a sample (plus grace) left: it ran out.
TEST(DcDispelWatchTest, ADebuffGoneAtTheEndOfItsTimerExpired)
{
    Watch w;
    w.Step({Poison(MAGE, 1200)});
    w.Step({});
    EXPECT_EQ(w.out[POISON_BOLT].early, 0u);
    EXPECT_EQ(w.out[POISON_BOLT].expired, 1u);
}

// A carrier who died lost the debuff to death, not to a healer.
TEST(DcDispelWatchTest, ADebuffLostWithItsCarrierIsNotCounted)
{
    Watch w;
    w.Step({Poison(MAGE, 10000)});
    w.alive.erase(MAGE);
    w.Step({});
    EXPECT_EQ(w.out[POISON_BOLT].early, 0u);
    EXPECT_EQ(w.out[POISON_BOLT].expired, 0u);
}

// One volley on two members is two applications; a permanent aura that goes
// away was removed.
TEST(DcDispelWatchTest, EachCarrierIsItsOwnApplication)
{
    Watch w;
    w.Step({Poison(MAGE, 10000), Poison(HEALER, -1)});
    w.Step({Poison(MAGE, 9000)});
    EXPECT_EQ(w.out[POISON_BOLT].seen, 2u);
    EXPECT_EQ(w.out[POISON_BOLT].early, 1u);
}
