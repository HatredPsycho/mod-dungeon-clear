/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

#include <list>

#include "Creature.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Player.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

// --- Road to De Other Side (map 937, CoA) — the Quimera Portal ritual --------
// The final boss, the Chimeric Embryo (162716, DungeonEncounter 15007/15013/
// 15019, bit 2 on every difficulty), has NO `creature` row: the Quimera Portal
// (162719, guid 9370160 at (296.6, -222.9, 341.8)) summons him, all in SmartAI.
// Nothing but the party starts it — no kill, area trigger or timer:
//
//   * Three Body of Offering (162972, guids 9370535-9370537) lie ~10yd from the
//     portal, by the arena middle. npc_spellclick_spells (162972, 256920, 3) makes
//     each clickable; its SmartAI on SMART_EVENT_ON_SPELLCLICK stores the
//     clicker, strips its own UNIT_NPC_FLAG_SPELLCLICK (one click per body),
//     and 2.5s later swings, ends the clicker's 256920 pose, bleeds, sets data
//     1 1 on the portal (SetData, 80yd) and dies.
//   * The portal counts those (event 38 -> counter 1); at 3 (event 77) the
//     ritual's timed events run, and step 9, 22.5s after the third body,
//     summons the Embryo on itself (SMART_ACTION_SUMMON_CREATURE, type 8 =
//     manual despawn).
//   * boss_coa_chimeric_embryo::IsSummonedBy keeps him NOT_SELECTABLE and
//     passive while he walks to the arena controller (162731) and for 1s after
//     (CoAOtherSideEmbryo.cpp), then he is a normal boss.
//
// BossSpawnIndex emits no row for him (no spawn), so the run used to end
// "allCleared" after Drak'math. The roster patch below adds him with his real
// bit 2, anchored on the arena middle (the bodies lie round it), so boss-nav
// parks the party there — the Razorfen Downs gong pattern. The conditional
// event then has the leader click the bodies (hook 69, the Vaults pattern),
// and stays Repeatable so it is never latched: DcTargeting::
// HasPendingSummonEvent keeps Advance from stalling "not spawned" through the
// 22.5s ritual and the Embryo's walk. His kill is read off the latched
// encounter credit (DcTargeting::NoteScriptlessCredit): the instance has no
// InstanceScript and he has no spawn to read.
namespace
{
    constexpr uint32 ROAD_MAP = 937;
    constexpr uint32 ROAD_EVENT_OFFER_BODIES = 1;
    constexpr uint32 ROAD_HOOK_OFFER_BODIES = 69;  // ObjectiveHookRegistry id

    constexpr uint32 ROAD_EMBRYO = 162716;
    constexpr uint32 ROAD_EMBRYO_BIT = 2;
    constexpr uint32 ROAD_BODY_OF_OFFERING = 162972;
    constexpr uint32 ROAD_SPELL_SACRIFICE_POSE = 256920;  // the clicker's pose, until the strike

    // ArenaMiddle in CoAOtherSideEmbryo.cpp, where the Embryo settles: the
    // three bodies lie 3-8yd from it.
    constexpr float ROAD_ARENA_X = 304.429f;
    constexpr float ROAD_ARENA_Y = -206.081f;
    constexpr float ROAD_ARENA_Z = 341.841f;

    // The arena platform stands ~90yd above the rest of the road, so a bot this
    // close to a body is on it: the event fires once boss-nav has parked the
    // party, never from below.
    constexpr float ROAD_BODY_SCAN = 40.0f;
    // He is summoned on the portal, ~18yd off the arena middle.
    constexpr float ROAD_EMBRYO_SCAN = 100.0f;
    // A player clicks from beside the body.
    constexpr float ROAD_BODY_REACH = 3.0f;
    // Three bodies, 2.5s each, a few yards apart.
    constexpr uint32 ROAD_OFFER_TIMEOUT = 60000;

    // Not yet offered: alive and still clickable (the click strips the flag).
    bool BodyOfferable(Creature* body)
    {
        return body->IsAlive() && body->HasNpcFlag(UNIT_NPC_FLAG_SPELLCLICK);
    }

    bool RoadBodiesAwaitOffering(Player* bot, AiObjectContext* /*context*/)
    {
        if (DcTargeting::CompletedEncounterMask(bot) & (1u << ROAD_EMBRYO_BIT))
            return false;
        if (bot->FindNearestCreature(ROAD_EMBRYO, ROAD_EMBRYO_SCAN, /*alive*/ true))
            return false;
        std::list<Creature*> bodies;
        bot->GetCreatureListWithEntryInGrid(bodies, ROAD_BODY_OF_OFFERING, ROAD_BODY_SCAN);
        for (Creature* body : bodies)
            if (BodyOfferable(body))
                return true;
        return false;
    }

    // Hook 69: offer every body still clickable (see above). Done once none is
    // left; the portal's ritual then runs on its own timers and the clear
    // engages the Embryo as soon as he is selectable.
    ObjectiveArriveResult OfferTheBodies(Player* bot, AiObjectContext* /*context*/,
                                         DungeonBossInfo const& /*info*/)
    {
        // Mid-offering: the pose holds until the body's 2.5s strike, stand still.
        if (bot->HasAura(ROAD_SPELL_SACRIFICE_POSE))
        {
            if (bot->isMoving())
                bot->StopMoving();
            return ObjectiveArriveResult::Running;
        }

        std::list<Creature*> bodies;
        bot->GetCreatureListWithEntryInGrid(bodies, ROAD_BODY_OF_OFFERING, ROAD_BODY_SCAN);
        Creature* next = nullptr;
        for (Creature* body : bodies)
        {
            if (!BodyOfferable(body))
                continue;
            if (!next || bot->GetExactDist2d(body) < bot->GetExactDist2d(next))
                next = body;
        }
        if (!next)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Road: the bodies are offered, waiting for the Chimeric Embryo",
                     bot->GetName());
            return ObjectiveArriveResult::Done;
        }

        if (bot->GetExactDist2d(next) > ROAD_BODY_REACH)
        {
            if (!bot->isMoving())
                bot->GetMotionMaster()->MovePoint(0, next->GetPositionX(), next->GetPositionY(),
                                                  next->GetPositionZ(), FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                                  /*generatePath*/ true, false);
            return ObjectiveArriveResult::Running;
        }

        if (bot->isMoving())
            bot->StopMoving();
        bot->SetFacingToObject(next);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] Road: offering a body to the Quimera Portal ({} at {:.1f}yd)",
                 bot->GetName(), next->GetGUID().ToString(), bot->GetExactDist2d(next));
        next->HandleSpellClick(bot);
        return ObjectiveArriveResult::Running;
    }
}

void RegisterRoadToDeOtherSideHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, ROAD_HOOK_OFFER_BODIES, &OfferTheBodies);
}

void RegisterRoadToDeOtherSideEvents(std::vector<DungeonEvent>& out)
{
    // Repeatable, like the Razorfen Downs gong: never latched, so it stays the
    // Embryo's pending summon event until he is up (see above). The predicate
    // reads false for good once every body is offered. Folded under the Embryo
    // in the panel — it is his summon, not a step of its own.
    out.push_back(EventBuilder(ROAD_MAP, ROAD_EVENT_OFFER_BODIES, "Offer the bodies to the Quimera Portal")
                      .Conditional(&RoadBodiesAwaitOffering)
                      .Repeatable()
                      .PanelBeforeBoss(ROAD_EMBRYO)
                      .StepsOwnMovement()
                      .Custom(ROAD_HOOK_OFFER_BODIES)
                          .Timeout(ROAD_OFFER_TIMEOUT)
                      .Build());
}

void RegisterRoadToDeOtherSideRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    // The Chimeric Embryo has a real DungeonEncounter bit (2) but no spawn, so
    // BossSpawnIndex never lists him: add him with that bit, last after Muzah
    // (0) and Drak'math (1), anchored on the arena middle where the bodies lie.
    BossRosterPatch p;
    p.mapId = ROAD_MAP;
    p.add = {
        MakeBossWithBit(ROAD_EMBRYO, ROAD_MAP, "Chimeric Embryo",
                        ROAD_ARENA_X, ROAD_ARENA_Y, ROAD_ARENA_Z, ROAD_EMBRYO_BIT),
    };
    t.push_back(std::move(p));
}
