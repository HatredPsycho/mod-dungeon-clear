/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "DcPlayerLedEventDecision.h"
#include "DcPlayerLedEventStore.h"
#include "Ai/Dungeon/DungeonClear/Strategy/DcRelevance.h"

using namespace DcPlayerLedEventDecision;

namespace
{
    Candidate Bot(std::uint64_t guid, bool tank = false)
    {
        Candidate c;
        c.guid = guid;
        c.isBot = true;
        c.alive = true;
        c.isTank = tank;
        c.onPlayerMap = true;
        return c;
    }

    // The first (only) step of a one-step event built the authoring way.
    template <typename Fn>
    EventStep OneStep(Fn&& add)
    {
        EventBuilder b(1, 1, "test");
        add(b);
        return b.Build().steps.front();
    }
}

// --- Executor election ------------------------------------------------------

TEST(PlayerLedEventsTest, ElectsLowestGuidNonTankBot)
{
    Candidate human = Bot(1);
    human.isBot = false;
    std::vector<Candidate> party = {human, Bot(2, /*tank*/ true), Bot(9), Bot(5), Bot(7)};
    EXPECT_EQ(Elect(party), 5u);
}

TEST(PlayerLedEventsTest, NeverElectsATankOrAHuman)
{
    Candidate human = Bot(1);
    human.isBot = false;
    std::vector<Candidate> party = {human, Bot(2, true), Bot(3, true)};
    EXPECT_EQ(Elect(party), 0u);
}

TEST(PlayerLedEventsTest, ReelectsWhenTheExecutorDiesOrLeavesTheMap)
{
    std::vector<Candidate> party = {Bot(4), Bot(6), Bot(8)};
    EXPECT_EQ(Elect(party), 4u);
    party[0].alive = false;
    EXPECT_EQ(Elect(party), 6u);
    party[1].onPlayerMap = false;
    EXPECT_EQ(Elect(party), 8u);
    party[2].alive = false;
    EXPECT_EQ(Elect(party), 0u);
}

TEST(PlayerLedEventsTest, NothingTicksWithoutAMainTankLead)
{
    // Non-regression: in a DC run, or with nobody flagged, DcPlayerLed::Leader is
    // null and nothing of v2 is ever selected, executor or not, in combat or not.
    EXPECT_FALSE(ShouldTick(false, true, false));
    EXPECT_FALSE(ShouldTick(false, true, true));
    EXPECT_FALSE(ShouldTick(false, false, false));
    EXPECT_TRUE(ShouldTick(true, true, false));
    EXPECT_FALSE(ShouldTick(true, false, false));
    EXPECT_FALSE(ShouldTick(true, true, true));
}

TEST(PlayerLedEventsTest, ForgetsOnARunOrAfterALongAbsence)
{
    EXPECT_TRUE(ShouldForget(true, 1000, 1000));
    EXPECT_FALSE(ShouldForget(false, 1000, 1000 + kUnledForgetMs - 1));
    EXPECT_TRUE(ShouldForget(false, 1000, 1000 + kUnledForgetMs));
    EXPECT_FALSE(ShouldForget(false, 0, 999999));  // never led: nothing to time out
    // Wrap-safe across the 49-day getMSTime rollover.
    EXPECT_FALSE(ShouldForget(false, 0xFFFFFF00u, 0x100u));
}

// --- Step classification: one test per kind ----------------------------------

TEST(PlayerLedEventsTest, ClassifiesBotSteps)
{
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.MoveTo(1, 2, 3, 4); })), StepActor::Bot);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.UseGO(100, 10); })), StepActor::Bot);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.Gossip(200, 0); })), StepActor::Bot);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.CastSpell(300); })), StepActor::Bot);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.UseItem(400); })), StepActor::Bot);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.UseItemOnGO(1, 2, 3, 0, 0, 0); })), StepActor::Bot);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.UseItemAt(1, 2, 3, 0, 0, 0); })), StepActor::Bot);
}

TEST(PlayerLedEventsTest, ClassifiesGateSteps)
{
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.Wait(1000); })), StepActor::BotGate);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.WaitForSpawn(5); })), StepActor::BotGate);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.WaitForGOState(5, 0); })), StepActor::BotGate);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.KillCreature(5); })), StepActor::BotGate);
}

TEST(PlayerLedEventsTest, ClassifiesPlayerSteps)
{
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.Jump(1, 2, 3); })), StepActor::Player);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.DropInHole(1, 2, 3, 4, 5, 6); })), StepActor::Player);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.TeleportParty(1, 2, 3, 4, 5, 6); })), StepActor::Player);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.KillCreatureEngage(5); })), StepActor::Player);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.ClearRadius(1, 2, 3, 30); })), StepActor::Player);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.EscortCreature(5, 0, 6, -1); })), StepActor::Player);
    // Every garrison shape is the tank holding a point.
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.MoveToHoldUntilSpawn(1, 2, 3, 5, 77); })),
              StepActor::Player);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.MoveToHoldUntilInstanceData(1, 2, 3, 5, 1, 2); })),
              StepActor::Player);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.MoveToHoldUntilPersistentData(1, 2, 3, 5, 1, 2); })),
              StepActor::Player);
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.MoveToHoldUntilBossState(1, 2, 3, 5, 1, 2); })),
              StepActor::Player);
}

TEST(PlayerLedEventsTest, CustomHooksNeedAnAudit)
{
    EXPECT_EQ(Classify(OneStep([](EventBuilder& b) { b.Custom(12); })), StepActor::Audit);
}

TEST(PlayerLedEventsTest, TheWalkOntoALipBelongsToThePlayer)
{
    DungeonEvent const jump = EventBuilder(1, 1, "j").MoveTo(1, 2, 3, 3).Jump(4, 5, 6).Build();
    EXPECT_EQ(ClassifyInEvent(jump, 0), StepActor::Player);
    DungeonEvent const drop = EventBuilder(1, 2, "d").MoveTo(1, 2, 3, 3).DropInHole(1, 2, 3, 4, 5, 6).Build();
    EXPECT_EQ(ClassifyInEvent(drop, 0), StepActor::Player);
    DungeonEvent const lever = EventBuilder(1, 3, "l").MoveTo(1, 2, 3, 3).UseGO(9, 10).Build();
    EXPECT_EQ(ClassifyInEvent(lever, 0), StepActor::Bot);
    EXPECT_EQ(ClassifyInEvent(lever, 1), StepActor::Bot);
    EXPECT_EQ(ClassifyInEvent(lever, 2), StepActor::Audit);  // out of range
}

// --- Encounter start ----------------------------------------------------------

TEST(PlayerLedEventsTest, ActionStepsStartAnEncounterByDefault)
{
    EXPECT_TRUE(StartsEncounter(OneStep([](EventBuilder& b) { b.UseGO(100, 10); })));
    EXPECT_TRUE(StartsEncounter(OneStep([](EventBuilder& b) { b.Gossip(200, 0); })));
    EXPECT_TRUE(StartsEncounter(OneStep([](EventBuilder& b) { b.CastSpell(300); })));
    EXPECT_TRUE(StartsEncounter(OneStep([](EventBuilder& b) { b.UseItem(400); })));
    EXPECT_TRUE(StartsEncounter(OneStep([](EventBuilder& b) { b.MoveTo(1, 2, 3); })));
}

TEST(PlayerLedEventsTest, GatesAndPlayerStepsNeverWaitForTheGo)
{
    EXPECT_FALSE(StartsEncounter(OneStep([](EventBuilder& b) { b.Wait(1000); })));
    EXPECT_FALSE(StartsEncounter(OneStep([](EventBuilder& b) { b.WaitForSpawn(5); })));
    EXPECT_FALSE(StartsEncounter(OneStep([](EventBuilder& b) { b.WaitForGOState(5, 0); })));
    EXPECT_FALSE(StartsEncounter(OneStep([](EventBuilder& b) { b.KillCreature(5); })));
    EXPECT_FALSE(StartsEncounter(OneStep([](EventBuilder& b) { b.Jump(1, 2, 3); })));
}

TEST(PlayerLedEventsTest, AuthoredFlagWins)
{
    EXPECT_FALSE(StartsEncounter(OneStep([](EventBuilder& b) { b.UseGO(100, 10).NoEncounterStart(); })));
    EXPECT_TRUE(StartsEncounter(OneStep([](EventBuilder& b) { b.Wait(10).StartsEncounter(); })));
}

TEST(PlayerLedEventsTest, ClassicGongsWaitForTheGoAndDoorLeversDoNot)
{
    auto stepOf = [](std::uint32_t mapId, std::uint32_t goEntry) -> EventStep const*
    {
        for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
            if (ev.mapId == mapId)
                for (EventStep const& s : ev.steps)
                    if (s.kind == EventStepKind::UseGameObject && s.goEntry == goEntry)
                        return &s;
        return nullptr;
    };
    EventStep const* rfdGong = stepOf(129, 148917);
    ASSERT_NE(rfdGong, nullptr);
    EXPECT_TRUE(StartsEncounter(*rfdGong));
    EventStep const* gordokDoor = stepOf(429, 177219);
    ASSERT_NE(gordokDoor, nullptr);
    EXPECT_FALSE(StartsEncounter(*gordokDoor));
    EventStep const* sfkLever = stepOf(33, 18901);
    ASSERT_NE(sfkLever, nullptr);
    EXPECT_FALSE(StartsEncounter(*sfkLever));
}

TEST(PlayerLedEventsTest, EveryRegisteredStepHasAnActor)
{
    // Only Custom hooks are left out of Main Tank mode in this pass.
    for (DungeonEvent const& ev : DungeonEventRegistry::AllEvents())
        for (std::size_t i = 0; i < ev.steps.size(); ++i)
            if (ev.steps[i].kind != EventStepKind::Custom)
                EXPECT_NE(ClassifyInEvent(ev, i), StepActor::Audit) << ev.name << " step " << i;
}

// --- The player's go ------------------------------------------------------------

TEST(PlayerLedEventsTest, SafeStepsRunWithoutTheGo)
{
    GoFacts f;
    f.startsEncounter = false;
    EXPECT_EQ(DecideGo(f), GoVerdict::Run);
}

TEST(PlayerLedEventsTest, EncounterStartWaitsForEveryCondition)
{
    GoFacts f;
    f.startsEncounter = true;
    EXPECT_EQ(DecideGo(f), GoVerdict::WaitForGo);
    f.goOrdered = true;
    EXPECT_EQ(DecideGo(f), GoVerdict::WaitForGo);  // not near
    f.playerNear = true;
    EXPECT_EQ(DecideGo(f), GoVerdict::Run);
    f.playerInCombat = true;
    EXPECT_EQ(DecideGo(f), GoVerdict::WaitForGo);
    f.playerInCombat = false;
    f.goOrdered = false;
    EXPECT_EQ(DecideGo(f), GoVerdict::WaitForGo);  // near but no order
}

TEST(PlayerLedEventsTest, PlayerDoingItHimselfCountsAsDone)
{
    GoFacts f;
    f.startsEncounter = true;
    f.alreadyDone = true;
    EXPECT_EQ(DecideGo(f), GoVerdict::AlreadyDone);
}

TEST(PlayerLedEventsTest, GoOrderExpires)
{
    EXPECT_FALSE(GoOrderLive(0, 5000));
    EXPECT_TRUE(GoOrderLive(1000, 1000 + kGoOrderTtlMs));
    EXPECT_FALSE(GoOrderLive(1000, 1001 + kGoOrderTtlMs));
    EXPECT_TRUE(GoOrderLive(0xFFFFFF00u, 0x100u));  // wrap-safe
}

TEST(PlayerLedEventsTest, ThePlayerIsNeverRushed)
{
    EXPECT_TRUE(HoldsClock(StepActor::Player, GoVerdict::Run));
    EXPECT_TRUE(HoldsClock(StepActor::Bot, GoVerdict::WaitForGo));
    EXPECT_FALSE(HoldsClock(StepActor::Bot, GoVerdict::Run));
    EXPECT_FALSE(HoldsClock(StepActor::BotGate, GoVerdict::Run));
}

TEST(PlayerLedEventsTest, RoomPreClearsStayThePlayersPull)
{
    DungeonEvent const ev = EventBuilder(1, 1, "x").KillCreature(0).Build();
    EXPECT_FALSE(MayDrive(ev, true));
    EXPECT_TRUE(MayDrive(ev, false));
    EXPECT_FALSE(MayDrive(EventBuilder(1, 2, "empty").Build(), false));
}

TEST(PlayerLedEventsTest, RelevanceSitsBetweenRezAndTheFollowerRungs)
{
    EXPECT_LT(DcRel::PlayerLedEvent, DcRel::RezParty);
    EXPECT_GT(DcRel::PlayerLedEvent, DcRel::AssistCamp);
    EXPECT_GT(DcRel::PlayerLedEvent, DcRel::FollowTank);
}

// --- Per-instance store ---------------------------------------------------------

TEST(PlayerLedEventsTest, StoreKeepsStatePerInstance)
{
    DcPlayerLedEventStore store;
    EXPECT_FALSE(store.Any());
    store.With(10, 129, [](DcPlayerLedInstanceEvents& s) { s.done.insert(1); return 0; });
    store.With(11, 129, [](DcPlayerLedInstanceEvents& s) { s.done.insert(2); return 0; });
    EXPECT_TRUE(store.Any());
    EXPECT_EQ(store.Size(), 2u);
    EXPECT_TRUE(store.With(10, 129, [](DcPlayerLedInstanceEvents& s) { return s.done.count(1) == 1; }));
    EXPECT_FALSE(store.With(10, 129, [](DcPlayerLedInstanceEvents& s) { return s.done.count(2) == 1; }));
}

TEST(PlayerLedEventsTest, StoreForgetsAndRestartsARecycledInstance)
{
    DcPlayerLedEventStore store;
    store.With(10, 129, [](DcPlayerLedInstanceEvents& s) { s.done.insert(1); return 0; });
    // Same instance id on another map: a recycled id, it starts over.
    EXPECT_TRUE(store.With(10, 209, [](DcPlayerLedInstanceEvents& s) { return s.done.empty(); }));
    store.Forget(10);
    EXPECT_FALSE(store.Has(10));
    EXPECT_FALSE(store.Any());
}

TEST(PlayerLedEventsTest, GoOrderArmsOnceUntilItExpires)
{
    DcPlayerLedEventStore store;
    EXPECT_TRUE(store.OrderGo(10, 129, 1000));
    EXPECT_FALSE(store.OrderGo(10, 129, 2000));  // party chat: every bot hears it
    EXPECT_TRUE(store.OrderGo(10, 129, 2000 + kGoOrderTtlMs + 1));
    EXPECT_TRUE(store.OrderGo(11, 129, 2000));   // another instance is separate
}
