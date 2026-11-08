/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcPlayerLedEvents.h"

#include <string>
#include <vector>

#include "Creature.h"
#include "Event.h"
#include "GameObject.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcBossStandDown.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDifficulty.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPlayerLed.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPlayerLedEventDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPlayerLedEventStore.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPlayerbotCompat.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStatusPublisher.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

namespace
{
    using DcPlayerLedEventDecision::GoVerdict;
    using DcPlayerLedEventDecision::StepActor;

    // Wide enough to find a lever or an NPC from anywhere in its room.
    constexpr float kTargetScan = 100.0f;
    // A UseItemAt receipt this close to the step's point means "done here".
    constexpr float kReceiptLatch = 10.0f;
    // DropInHole: the player is on the deep floor.
    constexpr float kLandingRadius2d = 8.0f;
    constexpr float kLandingZSlack = 4.0f;
    // TeleportParty: the player made it down to the landing himself.
    constexpr float kRelocationLandingRadius = 12.0f;
    // ClearRadius: the executor must be this close for its reach probe to count.
    constexpr float kClearProbeRange = 30.0f;

    enum class Notice : uint8
    {
        None,
        WaitForGo,  // an encounter-starting step is ready
        LeftToYou,  // a step DC does not run in this mode, or the event ran out
    };

    uint32 NowNonZero()
    {
        uint32 const now = getMSTime();
        return now != 0 ? now : 1u;
    }

    bool HasPoint(EventStep const& step)
    {
        return step.x != 0.0f || step.y != 0.0f || step.z != 0.0f;
    }

    // Where the player has to stand to give the go, and whether he already did
    // the step himself. `alreadyDone` only ever reads a stable world latch.
    void GoTarget(Player* mt, EventStep const& step, float& dist, bool& alreadyDone)
    {
        dist = 0.0f;
        alreadyDone = false;
        switch (step.kind)
        {
            case EventStepKind::UseGameObject:
            {
                GameObject* go = mt->FindNearestGameObject(step.goEntry, kTargetScan);
                if (go)
                {
                    dist = mt->GetExactDist(go);
                    // A lever, gong or door already out of READY was used.
                    alreadyDone = go->GetGoState() != GO_STATE_READY;
                    return;
                }
                break;
            }
            case EventStepKind::Gossip:
            {
                if (Creature* npc = mt->FindNearestCreature(step.creatureEntry, kTargetScan, true))
                {
                    dist = mt->GetExactDist(npc);
                    return;
                }
                break;
            }
            case EventStepKind::UseItemAt:
            {
                if (GameObject* receipt = mt->FindNearestGameObject(step.goEntry, kTargetScan))
                    alreadyDone = receipt->GetExactDist(step.x, step.y, step.z) <= kReceiptLatch;
                break;
            }
            default:
                break;
        }
        if (HasPoint(step))
            dist = mt->GetExactDist(step.x, step.y, step.z);
        // No point and no object found: the step is done where the executor
        // stands, so only the order and the player's combat state gate it.
    }

    // The completion gate of one of the PLAYER's steps. Never moves the player;
    // the only thing it moves is bots, pulled across a gap he has crossed.
    StepResult PlayerGate(Player* bot, AiObjectContext* context, Player* mt, EventStep const& step,
                          DungeonEventProgress& prog, uint32 now)
    {
        switch (step.kind)
        {
            case EventStepKind::MoveTo:
                // A garrison: hold until its gate clears, wherever he holds it.
                // A plain MoveTo here is the tank's walk onto the lip of the
                // step after it, whose own gate checks that he made it.
                return DcPlayerLedEventDecision::IsGarrison(step)
                           ? DungeonEventExecutor::GarrisonGate(mt, step)
                           : StepResult::Done;
            case EventStepKind::Jump:
            {
                float const radius = step.radius > 0.0f ? step.radius : 4.0f;
                if (mt->GetExactDist(step.x, step.y, step.z) > radius)
                    return StepResult::Running;
                DungeonEventExecutor::PullBotsAcross(mt, step.x, step.y, step.z);
                return StepResult::Done;
            }
            case EventStepKind::DropInHole:
            {
                if (mt->IsFalling() || mt->GetExactDist2d(step.landX, step.landY) > kLandingRadius2d ||
                    mt->GetPositionZ() > step.landZ + kLandingZSlack)
                    return StepResult::Running;
                DungeonEventExecutor::PullBotsAcross(mt, step.landX, step.landY, step.landZ);
                return StepResult::Done;
            }
            case EventStepKind::TeleportParty:
            {
                // Never relocate the human: he walks or drops down himself, and
                // the bots follow him across once he is on the landing.
                if (mt->GetExactDist(step.landX, step.landY, step.landZ) > kRelocationLandingRadius)
                    return StepResult::Running;
                DungeonEventExecutor::PullBotsAcross(mt, step.landX, step.landY, step.landZ);
                return StepResult::Done;
            }
            case EventStepKind::KillCreature:
            case EventStepKind::EscortCreature:
                // Both kinds' RunStep is a pure gate (no movement, no engage):
                // read it around the player, who is where the fight is.
                return DungeonEventExecutor::RunStep(mt, context, step, prog, now);
            case EventStepKind::ClearRadius:
            {
                float const judge = DungeonEventExecutor::ClearJudgeRadius();
                if (mt->GetExactDist(step.x, step.y, step.z) > judge ||
                    bot->GetExactDist(step.x, step.y, step.z) > kClearProbeRange)
                    return StepResult::Running;
                float const r = step.radius > 0.0f ? step.radius : 50.0f;
                Unit* u = DcTargeting::NearestHostileNearPoint(bot, context, step.x, step.y, step.z, r,
                                                               step.zBand, &step.entryFilter);
                return u ? StepResult::Running : StepResult::Done;
            }
            default:
                return StepResult::Running;
        }
    }

    // The event the player has reached, or nullptr. An anchored event drives
    // once he is inside its objective's arrive radius (a persistent one keeps
    // driving after he leaves it); a conditional one while its predicate holds.
    DungeonEvent const* SelectEvent(Player* bot, AiObjectContext* context, Player* mt,
                                    DcPlayerLedInstanceEvents const& state)
    {
        Map* map = bot->GetMap();
        if (!map)
            return nullptr;
        uint32 const mapId = bot->GetMapId();
        if (!DungeonEventRegistry::HasEvents(mapId))
            return nullptr;
        DcDiffKey const key = DcDifficulty::Of(map);

        std::vector<DungeonBossInfo> const& roster =
            context->GetValue<std::vector<DungeonBossInfo>>(DcKey::DungeonBosses)->Get();
        float const defaultRadius = DcSettings::GetFloat(bot, "ObjectiveArriveRadius");
        for (DungeonBossInfo const& info : roster)
        {
            if (info.kind != DungeonAnchorKind::Objective || !info.eventId)
                continue;
            DungeonEvent const* ev = DungeonEventRegistry::Find(mapId, info.eventId);
            if (!ev || state.done.count(ev->id) || !DcGateMatches(ev->gate, key))
                continue;
            if (!DcPlayerLedEventDecision::MayDrive(*ev, DungeonEventRegistry::IsRoomAggroPreClear(*ev)))
                continue;
            bool const midway = ev->persistent && state.progress.eventId == ev->id &&
                                state.progress.stepIndex > 0;
            float const radius = info.arriveRadius > 0.0f ? info.arriveRadius : defaultRadius;
            if (midway || mt->GetExactDist(info.x, info.y, info.z) <= radius)
                return ev;
        }

        bool const standDown = DcBossStandDown::IsRaidStandDown(bot);
        for (DungeonEvent const* ev : DungeonEventRegistry::Conditional(mapId, key))
        {
            if (state.done.count(ev->id))
                continue;
            if (!DcPlayerLedEventDecision::MayDrive(*ev, DungeonEventRegistry::IsRoomAggroPreClear(*ev)))
                continue;
            if (standDown && !ev->encounterActive)
                continue;
            if (ev->condition && ev->condition(bot, context))
                return ev;
        }
        return nullptr;
    }

    // DungeonEventExecutor::Drive, split by who does each step. The progress
    // bookkeeping (new instance, new event, lapse rewind, completion GO) is the
    // same as a run's; the step itself goes to the executor, to the player, or
    // waits for the player's go.
    EventDriveOutcome DriveForPlayer(Player* bot, AiObjectContext* context, Player* mt,
                                     DungeonEvent const& ev, DcPlayerLedInstanceEvents& state,
                                     uint32 now, bool& consumedGo, Notice& notice)
    {
        DungeonEventProgress& prog = state.progress;
        if (!DcGateMatches(ev.gate, DcDifficulty::Of(bot->GetMap())))
            return EventDriveOutcome::Skipped;

        uint32 const instanceId = bot->GetInstanceId();
        if (instanceId != 0 && prog.instanceId != 0 && prog.instanceId != instanceId)
            prog.Reset();
        prog.instanceId = instanceId;
        if (prog.eventId != ev.id)
            prog.BeginEvent(ev.id, now);
        else if (getMSTimeDiff(prog.lastDriveMs, now) > DungeonEventExecutor::StaleGapMs())
        {
            prog.stepStartMs = now;
            prog.progressMs = now;
            if (!ev.persistent)
            {
                prog.stepIndex = 0;
                prog.attempts = 0;
                prog.maxStepIndex = 0;
            }
        }
        prog.lastDriveMs = now;

        if (prog.stepIndex >= ev.steps.size())
            return EventDriveOutcome::Completed;

        if (ev.completeGoEntry)
        {
            float const search = ev.completeGoRadius > 0.0f ? ev.completeGoRadius : kTargetScan;
            GameObject* go = mt->FindNearestGameObject(ev.completeGoEntry, search);
            GameObject* also = ev.completeAlsoGoEntry
                                   ? mt->FindNearestGameObject(ev.completeAlsoGoEntry, search)
                                   : nullptr;
            if (go && static_cast<uint32>(go->GetGoState()) == ev.completeGoState &&
                (!ev.completeAlsoGoEntry ||
                 (also && static_cast<uint32>(also->GetGoState()) == ev.completeAlsoGoState)))
            {
                prog.stepIndex = static_cast<uint32>(ev.steps.size());
                return EventDriveOutcome::Completed;
            }
        }

        EventStep const& step = ev.steps[prog.stepIndex];
        StepActor const actor = DcPlayerLedEventDecision::ClassifyInEvent(ev, prog.stepIndex);
        GoVerdict verdict = GoVerdict::Run;
        StepResult result = StepResult::Running;
        switch (actor)
        {
            case StepActor::Audit:
                // Custom hooks are not enabled in Main Tank mode (audit pending):
                // DC leaves the rest of the event to the player.
                notice = Notice::LeftToYou;
                result = StepResult::Blocked;
                break;
            case StepActor::BotGate:
                result = DungeonEventExecutor::RunStep(bot, context, step, prog, now);
                break;
            case StepActor::Bot:
            {
                DcPlayerLedEventDecision::GoFacts facts;
                facts.startsEncounter = DcPlayerLedEventDecision::StartsEncounter(step);
                float dist = 0.0f;
                GoTarget(mt, step, dist, facts.alreadyDone);
                facts.playerNear = dist <= DcPlayerLedEventDecision::kGoRange;
                facts.playerInCombat = mt->IsInCombat();
                facts.goOrdered = DcPlayerLedEventDecision::GoOrderLive(state.goOrderMs, now);
                verdict = DcPlayerLedEventDecision::DecideGo(facts);
                if (verdict == GoVerdict::AlreadyDone)
                    result = StepResult::Done;
                else if (verdict == GoVerdict::WaitForGo)
                    notice = Notice::WaitForGo;
                else
                {
                    result = DungeonEventExecutor::RunStep(bot, context, step, prog, now);
                    if (result == StepResult::Done && facts.startsEncounter)
                        consumedGo = true;
                }
                break;
            }
            case StepActor::Player:
                result = PlayerGate(bot, context, mt, step, prog, now);
                break;
        }

        if (DcPlayerLedEventDecision::HoldsClock(actor, verdict))
        {
            prog.stepStartMs = now;
            prog.progressMs = now;
        }

        uint32 const defaultTimeoutMs = DcSettings::GetUInt(bot, "EventStepTimeout") * 1000u;
        return DungeonEventExecutor::Advance(ev, prog, result, now, defaultTimeoutMs);
    }

    void Tell(Player* bot, Player* mt, std::string const& text)
    {
        if (mt && mt->IsInWorld())
            bot->Whisper(text, LANG_UNIVERSAL, mt);
    }
}

DcPlayerLedEventStore& DcPlayerLedEvents::Store()
{
    static DcPlayerLedEventStore store;
    return store;
}

Player* DcPlayerLedEvents::ElectExecutor(Player* mainTank)
{
    if (!mainTank)
        return nullptr;
    Group* group = mainTank->GetGroup();
    Map* map = mainTank->GetMap();
    if (!group || !map)
        return nullptr;

    std::vector<DcPlayerLedEventDecision::Candidate> candidates;
    std::vector<Player*> players;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;
        DcPlayerLedEventDecision::Candidate c;
        c.guid = member->GetGUID().GetRawValue();
        c.isBot = GET_PLAYERBOT_AI(member) != nullptr;
        c.alive = member->IsAlive();
        c.isTank = PlayerbotAI::IsTank(member);
        c.onPlayerMap = member->IsInWorld() && member->GetMap() == map;
        candidates.push_back(c);
        players.push_back(member);
    }
    uint64 const elected = DcPlayerLedEventDecision::Elect(candidates);
    for (size_t i = 0; i < candidates.size(); ++i)
        if (elected != 0 && candidates[i].guid == elected)
            return players[i];
    return nullptr;
}

bool DcPlayerLedEvents::CanOrderGo(Player* mainTank)
{
    return mainTank && mainTank->GetMap() && mainTank->GetMap()->IsDungeon() &&
           DcPlayerLed::Leader(mainTank) == mainTank;
}

bool DcPlayerLedEvents::OrderGo(Player* mainTank)
{
    if (!CanOrderGo(mainTank))
        return false;
    bool const fresh = Store().OrderGo(mainTank->GetInstanceId(), mainTank->GetMapId(), NowNonZero());
    if (fresh)
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Main Tank go received", mainTank->GetName());
    return fresh;
}

bool DcPlayerLedEvents::Tick(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return false;
    Player* mt = DcPlayerLed::Leader(bot);
    if (!mt || ElectExecutor(mt) != bot)
        return false;
    AiObjectContext* context = botAI->GetAiObjectContext();
    uint32 const instanceId = bot->GetInstanceId();
    uint32 const mapId = bot->GetMapId();
    uint32 const now = NowNonZero();
    uint64 const self = bot->GetGUID().GetRawValue();

    // Work on a copy so no engine call runs under the store's lock. Only the
    // elected executor writes an instance's state, so nothing races the copy
    // except a "dc go", which is merged back field by field below.
    DcPlayerLedInstanceEvents state = Store().With(instanceId, mapId, [&](DcPlayerLedInstanceEvents& s)
    {
        s.lastLedMs = now;
        if (s.executorGuid != self)
        {
            if (s.executorGuid != 0)
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] Main Tank events: executor handover (event {} step {})",
                         bot->GetName(), s.progress.eventId, s.progress.stepIndex);
            s.executorGuid = self;
        }
        return s;
    });

    DungeonEvent const* ev = SelectEvent(bot, context, mt, state);
    if (!ev)
        return false;

    bool consumedGo = false;
    Notice notice = Notice::None;
    uint32 const stepBefore = state.progress.stepIndex;
    EventDriveOutcome const outcome = DriveForPlayer(bot, context, mt, *ev, state, now, consumedGo, notice);
    bool const finished = outcome != EventDriveOutcome::Running;
    if (finished && outcome != EventDriveOutcome::Completed && notice == Notice::None)
        notice = Notice::LeftToYou;

    bool announce = false;
    Store().With(instanceId, mapId, [&](DcPlayerLedInstanceEvents& s)
    {
        s.progress = state.progress;
        if (consumedGo)
            s.goOrderMs = 0;
        if (notice != Notice::None &&
            (s.announcedEventId != ev->id || s.announcedStep != stepBefore))
        {
            s.announcedEventId = ev->id;
            s.announcedStep = stepBefore;
            announce = true;
        }
        if (finished)
        {
            if (ev->repeatable)
                s.progress.eventId = 0;  // re-arms with its condition
            else
                s.done.insert(ev->id);
        }
        return 0;
    });

    if (finished)
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Main Tank event '{}' {} at step {}", bot->GetName(),
                 ev->name, outcome == EventDriveOutcome::Completed ? "completed" : "left to the player",
                 stepBefore);

    // The addon's "Start event" button (SquidBots Lite 2.9.5) follows these
    // lines: MTEV\t<eventId>\t<ready|running|done|left>\t<name>. One event at a
    // time per instance; the addon shows the last one it heard.
    auto mtev = [&](char const* what)
    {
        DcStatusPublisher::SendAddonMessage(
            botAI, "MTEV\t" + std::to_string(ev->id) + "\t" + what + "\t" + ev->name);
    };

    if (announce)
    {
        if (notice == Notice::WaitForGo)
        {
            Tell(bot, mt,
                 "[DC] " + ev->name + ": ready. Stand near it, out of combat, and say \"dc go\" to start it.");
            mtev("ready");
        }
        else
        {
            Tell(bot, mt, "[DC] " + ev->name + ": DC leaves the rest of this event to you.");
            mtev("left");
        }
    }
    if (consumedGo && !finished)
        mtev("running");
    if (finished && outcome == EventDriveOutcome::Completed)
        mtev("done");
    return true;
}

bool DungeonClearPlayerLedEventTrigger::IsActive()
{
    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsInWorld())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    Player* mt = DcPlayerLed::Leader(bot);
    if (!mt)
    {
        // No Main Tank lead here (a DC run, or nobody flagged): drop the
        // instance's event state once the mode has really ended. One relaxed
        // atomic load on every realm where nobody ever used the bar.
        DcPlayerLedEventStore& store = DcPlayerLedEvents::Store();
        uint32 const instanceId = bot->GetInstanceId();
        if (store.Has(instanceId))
        {
            bool const runOwned = DcLeaderSignal::FindRunOwner(bot) != nullptr;
            uint32 const lastLed = store.With(instanceId, bot->GetMapId(),
                                              [](DcPlayerLedInstanceEvents& s) { return s.lastLedMs; });
            if (DcPlayerLedEventDecision::ShouldForget(runOwned, lastLed, NowNonZero()))
                store.Forget(instanceId);
        }
        return false;
    }

    bool const isExecutor = DcPlayerLedEvents::ElectExecutor(mt) == bot;
    if (!DcPlayerLedEventDecision::ShouldTick(true, isExecutor, bot->IsInCombat()) || !bot->IsAlive())
        return false;

    DcPlayerLedInstanceEvents const state = DcPlayerLedEvents::Store().With(
        bot->GetInstanceId(), bot->GetMapId(), [&](DcPlayerLedInstanceEvents& s)
        {
            s.lastLedMs = NowNonZero();
            return s;
        });
    return SelectEvent(bot, botAI->GetAiObjectContext(), mt, state) != nullptr;
}

bool DungeonClearPlayerLedEventAction::Execute(Event /*event*/)
{
    return DcPlayerLedEvents::Tick(botAI);
}

bool DcGoPlayerLedAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    Player* bot = botAI->GetBot();
    if (!owner || !bot)
        return false;
    if (GET_PLAYERBOT_AI(owner) && !DcPlayerbotCompat::IsSelfBot(owner))
        return false;
    // Only the player leading in Main Tank mode gives the go; in a DC run and for
    // anyone else this chat line does nothing, exactly as before it existed.
    if (DcPlayerLed::Leader(bot) != owner || !DcPlayerLedEvents::CanOrderGo(owner))
        return false;
    if (DcPlayerLedEvents::OrderGo(owner))
        bot->Whisper("[DC] Go: starting the event.", LANG_UNIVERSAL, owner);
    return true;
}
