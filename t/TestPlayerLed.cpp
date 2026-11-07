/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "DcPlayerLedDecision.h"
#include "DungeonClearMath.h"

using DcPlayerLedDecision::CurrentFirstHit;
using DcPlayerLedDecision::Inputs;
using DcPlayerLedDecision::IsPlayerLed;
using DcPlayerLedDecision::ChoosePartyTank;
using DcPlayerLedDecision::MemberRungApplies;
using DcPlayerLedDecision::ModuleProvide;
using DcPlayerLedDecision::ProvideToModule;
using DcPlayerLedDecision::PartyTankSource;
using DcPlayerLedDecision::RungsFor;

namespace
{
    Inputs Make(bool inInstance, bool flagged, bool runOwned, bool alive = true, bool human = true)
    {
        Inputs in;
        in.inInstance = inInstance;
        in.flaggedPlayerOnMap = flagged;
        in.dcRunOwned = runOwned;
        in.playerAlive = alive;
        in.playerHuman = human;
        return in;
    }
}

// The Main Tank bar is up in a dungeon and DC is off: the player leads.
TEST(DcPlayerLed, MainTankInInstanceWithoutRunLeads)
{
    EXPECT_TRUE(IsPlayerLed(Make(true, true, false)));
}

// Non-regression: with nobody flagged Main Tank, the mode never applies, so the
// party tank value and every rung that reads it behave exactly as before.
TEST(DcPlayerLed, NoFlaggedPlayerIsNeverPlayerLed)
{
    for (bool inInstance : {false, true})
        for (bool runOwned : {false, true})
            EXPECT_FALSE(IsPlayerLed(Make(inInstance, false, runOwned)))
                << "inInstance=" << inInstance << " runOwned=" << runOwned;
}

// DC driving always wins: `dc on` (a run, paused or not) ends player-led mode,
// as the addon's Main Tank / DC exclusion expects.
TEST(DcPlayerLed, AnyDcRunWins)
{
    EXPECT_FALSE(IsPlayerLed(Make(true, true, true)));
}

// Out in the world the flag means nothing to DC.
TEST(DcPlayerLed, OutsideInstancesNothingApplies)
{
    EXPECT_FALSE(IsPlayerLed(Make(false, true, false)));
}

// A dead Main Tank is no reference (healers and regroup must not aim at a
// corpse), and a flagged player who switched to self-bot has left the mode.
TEST(DcPlayerLed, DeadOrSelfBotMainTankIsNoLeader)
{
    EXPECT_FALSE(IsPlayerLed(Make(true, true, false, /*alive*/ false, /*human*/ true)));
    EXPECT_FALSE(IsPlayerLed(Make(true, true, false, /*alive*/ true, /*human*/ false)));
}

// Non-regression, party tank value: a live run's leader is ALWAYS the answer
// (the On button), whatever the Main Tank flag says; player-led is a fallback.
TEST(DcPlayerLed, PartyTankRunLeaderAlwaysWins)
{
    EXPECT_EQ(ChoosePartyTank(true, false), PartyTankSource::RunLeader);
    EXPECT_EQ(ChoosePartyTank(true, true), PartyTankSource::RunLeader);
    EXPECT_EQ(ChoosePartyTank(false, true), PartyTankSource::MainTankPlayer);
    EXPECT_EQ(ChoosePartyTank(false, false), PartyTankSource::None);
}

// Non-regression, driving rungs: with a bot tank (On) every rung keeps the
// pre-player-led verdict — the old `tank && tank != bot` / `tank` gates.
TEST(DcPlayerLed, BotTankRungsAreUnchanged)
{
    for (bool present : {false, true})
        for (bool self : {false, true})
        {
            if (self && !present)
                continue;
            auto const r = RungsFor(present, self, /*human*/ false);
            EXPECT_EQ(r.dcFollowTank, present && !self);
            EXPECT_EQ(r.suppressStockFollow, present);
            EXPECT_EQ(r.restCaps, present);
            EXPECT_EQ(r.collisionBan, present);
            EXPECT_EQ(r.breakStuckCombat, present);
        }
}

// A human Main Tank: DC never follows him (stock follow-master does, and the
// off-tanks' stay / placement holds), never caps rest, never breaks his combat.
TEST(DcPlayerLed, HumanTankStandsDcDrivingDown)
{
    auto const r = RungsFor(true, false, /*human*/ true);
    EXPECT_FALSE(r.dcFollowTank);
    EXPECT_FALSE(r.suppressStockFollow);
    EXPECT_FALSE(r.restCaps);
    EXPECT_FALSE(r.collisionBan);
    EXPECT_FALSE(r.breakStuckCombat);
}

// Off-tanks: never regrouped, assisted, pulled in (Lyceum, Iron Hall) or
// chase-guarded when a human leads; with a bot tank (On) every member keeps
// today's verdict, off-tanks included.
TEST(DcPlayerLed, OffTanksKeepTheirOrders)
{
    EXPECT_FALSE(MemberRungApplies(/*human*/ true, /*botIsTank*/ true));
    EXPECT_TRUE(MemberRungApplies(true, false));
    EXPECT_TRUE(MemberRungApplies(false, true));
    EXPECT_TRUE(MemberRungApplies(false, false));
}

// v1.5 assist on the player's clock: no stamp (flag set mid-fight) releases at
// once instead of a window that restarts every call; a stamp holds a fresh DPS
// for the lead, never a follower already in combat or a healer.
TEST(DcPlayerLed, AssistReleaseOnThePlayersClock)
{
    uint32_t const lead = 1500, since = 10000;
    EXPECT_TRUE(DungeonClearMath::ShouldReleaseFollower(false, false, 0, since + 10, lead, 100.f, 30.f));
    EXPECT_FALSE(DungeonClearMath::ShouldReleaseFollower(false, false, since, since + 500, lead, 100.f, 30.f));
    EXPECT_TRUE(DungeonClearMath::ShouldReleaseFollower(false, false, since, since + lead, lead, 100.f, 30.f));
    EXPECT_TRUE(DungeonClearMath::ShouldReleaseFollower(false, true, since, since + 10, lead, 100.f, 30.f));
    EXPECT_TRUE(DungeonClearMath::ShouldReleaseFollower(true, false, since, since + 10, lead, 100.f, 30.f));
}

TEST(DcPlayerLed, FirstHitFromAnEarlierFightIsStale)
{
    EXPECT_EQ(CurrentFirstHit(1000, 0), 0u);
    EXPECT_EQ(CurrentFirstHit(1000, 900), 0u);
    EXPECT_EQ(CurrentFirstHit(1000, 1000), 1000u);
    EXPECT_EQ(CurrentFirstHit(1000, 1500), 1500u);
}

// Threat lead on the player's clock: a DPS waits for his first hit plus the lead,
// and is never stuck when he does not hit (an off-tank pulled, or he waits).
TEST(DcPlayerLed, ThreatLeadReleasesWithoutAPlayerHit)
{
    uint32_t const lead = 1500, cap = 2500;
    uint32_t const since = 10000;
    // Fight just started, no hit yet: hold.
    EXPECT_TRUE(DungeonClearMath::ShouldHoldThreatLead(false, false, since, 0, since + 500, lead, cap, 100.f, 30.f));
    // No hit at all: released once lead + cap has run out.
    EXPECT_FALSE(DungeonClearMath::ShouldHoldThreatLead(false, false, since, 0, since + lead + cap, lead, cap, 100.f, 30.f));
    // He hit: hold for the lead after that hit only.
    EXPECT_TRUE(DungeonClearMath::ShouldHoldThreatLead(false, false, since, since + 200, since + 1000, lead, cap, 100.f, 30.f));
    EXPECT_FALSE(DungeonClearMath::ShouldHoldThreatLead(false, false, since, since + 200, since + 200 + lead, lead, cap, 100.f, 30.f));
    // Out of combat (no stamp): never held.
    EXPECT_FALSE(DungeonClearMath::ShouldHoldThreatLead(false, false, 0, 0, since, lead, cap, 100.f, 30.f));
    // A DPS something is hitting fights back at once.
    EXPECT_FALSE(DungeonClearMath::ShouldHoldThreatLead(false, true, since, 0, since + 100, lead, cap, 100.f, 30.f));
    // The player in danger: released.
    EXPECT_FALSE(DungeonClearMath::ShouldHoldThreatLead(false, false, since, 0, since + 100, lead, cap, 20.f, 30.f));
}

// The module's provider (CoaGroupTank): DC disabled hands nothing and reads no
// DC state at all (no values are registered then: a read would crash); a paused
// run still counts as "in a DC run", so the module's copies of the guards DC
// still runs stay quiet exactly as DC's own do in 2.9.4.
TEST(DcPlayerLed, ModuleProviderNeverReadsADisabledDc)
{
    for (bool tank : {false, true})
        for (bool run : {false, true})
            EXPECT_EQ(ProvideToModule(false, tank, run), ModuleProvide::Nothing);
    EXPECT_EQ(ProvideToModule(true, true, true), ModuleProvide::PartyTank);
    EXPECT_EQ(ProvideToModule(true, true, false), ModuleProvide::PartyTank);
    EXPECT_EQ(ProvideToModule(true, false, true), ModuleProvide::RunOwner);   // paused run
    EXPECT_EQ(ProvideToModule(true, false, false), ModuleProvide::Nothing);
}
