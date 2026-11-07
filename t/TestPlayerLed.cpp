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
using DcPlayerLedDecision::MayRegroupOnPlayer;

namespace
{
    Inputs Make(bool inInstance, bool flagged, bool runOwned)
    {
        Inputs in;
        in.inInstance = inInstance;
        in.flaggedPlayerOnMap = flagged;
        in.dcRunOwned = runOwned;
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

// Bot tanks are the player's off-tanks: DC never regroups them on him.
TEST(DcPlayerLed, OffTanksKeepTheirPlacement)
{
    EXPECT_FALSE(MayRegroupOnPlayer(true));
    EXPECT_TRUE(MayRegroupOnPlayer(false));
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
