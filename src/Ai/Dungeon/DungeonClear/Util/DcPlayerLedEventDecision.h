/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPLAYERLEDEVENTDECISION_H
#define _PLAYERBOT_DCPLAYERLEDEVENTDECISION_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"

// Pure decision kernel for PLAYER-LED v2: DC's scripted EVENTS while a human
// leads with the Main Tank bar (DcPlayerLed::Leader non-null, no DC run).
//
// In a DC run the leader bot tank drives every event step. Here the human tanks,
// so the steps split in two:
//   - BOT steps (click a lever, talk to an NPC, use an item, wait on a gate) are
//     done by ONE bot executor, elected per group and instance, never a tank;
//   - PLAYER steps (jump a gap, drop in a hole, hold a garrison point, engage a
//     target, escort an NPC) are the player's: the executor only reads their
//     completion gate and, where the step pulls stragglers across, pulls the bots.
// A BOT step that STARTS an encounter waits for the player's go.
//
// DcPlayerLedEvents (the glue) gathers the live facts and asks this; nothing
// here touches the engine. Nothing here is ever reached by a DC run: every glue
// entry point first requires DcPlayerLed::Leader(bot).
namespace DcPlayerLedEventDecision
{
    // ------------------------------------------------------------------
    // Executor election
    // ------------------------------------------------------------------

    // One group member as the election sees it.
    struct Candidate
    {
        std::uint64_t guid = 0;
        bool isBot = false;        // has a PlayerbotAI (a human never executes)
        bool alive = false;
        bool isTank = false;       // bot tanks are the player's off-tanks: never
        bool onPlayerMap = false;  // same map object as the Main Tank player
    };

    constexpr bool Eligible(Candidate const& c)
    {
        return c.guid != 0 && c.isBot && c.alive && !c.isTank && c.onPlayerMap;
    }

    // The executor: lowest GUID among eligible members; 0 when nobody qualifies.
    // Deterministic, so every bot agrees without talking, and re-election after a
    // death or a leave is simply the next call.
    inline std::uint64_t Elect(std::vector<Candidate> const& members)
    {
        std::uint64_t best = 0;
        for (Candidate const& c : members)
            if (Eligible(c) && (best == 0 || c.guid < best))
                best = c.guid;
        return best;
    }

    // Whether the event rung may tick at all. `playerLed` is
    // DcPlayerLed::Leader(bot) != nullptr: false in every DC run and with nobody
    // flagged, which keeps the whole v2 path dark there.
    constexpr bool ShouldTick(bool playerLed, bool isExecutor, bool executorInCombat)
    {
        return playerLed && isExecutor && !executorInCombat;
    }

    // When an instance's event state is dropped. A DC run taking over (`dc on`)
    // ends Main Tank mode at once. Otherwise the player has to be gone for a
    // while: a death, a release and a corpse run must not wipe the events he had
    // already done.
    constexpr std::uint32_t kUnledForgetMs = 300000;

    constexpr bool ShouldForget(bool dcRunOwned, std::uint32_t lastLedMs, std::uint32_t nowMs)
    {
        return dcRunOwned ||
               (lastLedMs != 0 && static_cast<std::uint32_t>(nowMs - lastLedMs) >= kUnledForgetMs);
    }

    // ------------------------------------------------------------------
    // Step classification
    // ------------------------------------------------------------------

    enum class StepActor : std::uint8_t
    {
        Bot,      // the executor performs the step (may move to it)
        BotGate,  // the executor only reads a condition, standing where it is
        Player,   // the player's gesture: the executor reads the gate, pulls bots
        Audit,    // Custom hook: not enabled in Main Tank mode until audited
    };

    // A MoveTo that holds a point until a gate clears (a garrison): the tank's
    // job, unlike a plain MoveTo onto a zone trigger.
    inline bool IsGarrison(EventStep const& step)
    {
        return step.kind == EventStepKind::MoveTo &&
               (step.instanceDataId >= 0 || step.persistentDataId >= 0 || step.bossStateId >= 0 ||
                step.creatureEntry != 0 || !step.orEntries.empty() || step.hookId != 0);
    }

    inline StepActor Classify(EventStep const& step)
    {
        switch (step.kind)
        {
            case EventStepKind::MoveTo:
                return IsGarrison(step) ? StepActor::Player : StepActor::Bot;
            case EventStepKind::UseGameObject:
            case EventStepKind::Gossip:
            case EventStepKind::CastSpell:
            case EventStepKind::UseItem:
            case EventStepKind::UseItemOnGO:
            case EventStepKind::UseItemAt:
                return StepActor::Bot;
            case EventStepKind::Wait:
            case EventStepKind::WaitForSpawn:
            case EventStepKind::WaitForGameObjectState:
                return StepActor::BotGate;
            case EventStepKind::KillCreature:
                return step.engage ? StepActor::Player : StepActor::BotGate;
            case EventStepKind::Jump:
            case EventStepKind::ClearRadius:
            case EventStepKind::EscortCreature:
            case EventStepKind::DropInHole:
            case EventStepKind::TeleportParty:
                return StepActor::Player;
            case EventStepKind::Custom:
                return StepActor::Audit;
        }
        return StepActor::Audit;  // an unknown kind is never run blind
    }

    // A step in its event: a plain MoveTo right before a Jump, a DropInHole or a
    // TeleportParty walks the TANK onto the lip or the checkpoint, so it belongs
    // to the player like the step it leads into.
    inline StepActor ClassifyInEvent(DungeonEvent const& ev, std::size_t index)
    {
        if (index >= ev.steps.size())
            return StepActor::Audit;
        EventStep const& step = ev.steps[index];
        StepActor const actor = Classify(step);
        if (actor == StepActor::Bot && step.kind == EventStepKind::MoveTo &&
            index + 1 < ev.steps.size())
        {
            EventStepKind const next = ev.steps[index + 1].kind;
            if (next == EventStepKind::Jump || next == EventStepKind::DropInHole ||
                next == EventStepKind::TeleportParty)
                return StepActor::Player;
        }
        return actor;
    }

    // Whether a step starts an encounter or brings mobs. The authored flag wins;
    // otherwise every BOT action step counts as one (when unsure, wait for the
    // go), and gates, waits and the player's own steps never do.
    inline bool StartsEncounter(EventStep const& step)
    {
        if (step.startsEncounter == EncounterStartFlag::Yes)
            return true;
        if (step.startsEncounter == EncounterStartFlag::No)
            return false;
        return Classify(step) == StepActor::Bot;
    }

    // ------------------------------------------------------------------
    // The player's go
    // ------------------------------------------------------------------

    // The player must stand this close to the step's target to give the go.
    constexpr float kGoRange = 30.0f;
    // A go order is good for this long: a "dc go" said for one encounter must
    // not start the next one ten minutes later.
    constexpr std::uint32_t kGoOrderTtlMs = 120000;

    constexpr bool GoOrderLive(std::uint32_t orderMs, std::uint32_t nowMs)
    {
        return orderMs != 0 && static_cast<std::uint32_t>(nowMs - orderMs) <= kGoOrderTtlMs;
    }

    struct GoFacts
    {
        bool startsEncounter = false;
        bool alreadyDone = false;     // the player did it himself (GO used, receipt there)
        bool playerNear = false;      // within kGoRange of the step's target
        bool playerInCombat = false;
        bool goOrdered = false;       // a live "dc go"
    };

    enum class GoVerdict : std::uint8_t
    {
        Run,          // perform the step now
        AlreadyDone,  // count the step done without touching it
        WaitForGo,    // hold: the player has not given the go
    };

    constexpr GoVerdict DecideGo(GoFacts const& f)
    {
        if (f.alreadyDone)
            return GoVerdict::AlreadyDone;
        if (!f.startsEncounter)
            return GoVerdict::Run;
        return (f.goOrdered && f.playerNear && !f.playerInCombat) ? GoVerdict::Run
                                                                   : GoVerdict::WaitForGo;
    }

    // Whether the event clock (step timeout and forward-progress watchdog) must
    // be held on this tick. The player has all the time he wants for his own
    // steps and for the go: neither is a wedge of the executor's.
    constexpr bool HoldsClock(StepActor actor, GoVerdict verdict)
    {
        return actor == StepActor::Player || actor == StepActor::Audit ||
               verdict == GoVerdict::WaitForGo;
    }

    // ------------------------------------------------------------------
    // Which events Main Tank mode drives
    // ------------------------------------------------------------------

    // A room-aggro pre-clear (KillCreature entry 0) is a pull the run's tank
    // makes; with a human tanking it is his room to clear.
    inline bool MayDrive(DungeonEvent const& ev, bool isRoomAggroPreClear)
    {
        return !isRoomAggroPreClear && !ev.steps.empty();
    }
}

#endif
