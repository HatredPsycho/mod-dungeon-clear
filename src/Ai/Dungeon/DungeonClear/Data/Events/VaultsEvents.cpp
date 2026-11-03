/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include <list>

#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Player.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"

// --- Vaults of the Inquisition (map 936, CoA) — the Deceiver's seals ------
// The Deceiver's Presence (162704, (-6658.4, -6104.3, 151.06)) starts SEALED:
// boss_coa_vaults_deceiver::Reset keeps him NOT_SELECTABLE | NON_ATTACKABLE,
// immune and passive (CoAVaults.cpp SetEngageable) until no Holy Crystal Seal
// (162705) is left alive within 40yd of him (UpdateSeal, the 1s check). Then a
// 17s awakening plays out and he becomes engageable for good: Reset keeps him
// awake through a wipe once _awakened is set, and the crystals respawn only
// after 7 days (RpRespawnDelay), so the seals are broken once per instance.
//
// The three crystals stand ~12yd round him, (-6646.3, -6104.5), (-6663.0,
// -6115.9) and (-6663.0, -6092.9). Nothing but a spell click breaks one:
// npc_spellclick_spells (162705, 47323), and npc_coa_vaults_holy_crystal's
// OnSpellClick makes the clicker its breaker, sets him channelling 47540 on
// it, and kills it 3s later (CrystalBreakMs). The crystal is passive, rooted
// and ignores attacks.
//
// mod-playerbots' coa-vaults strategy breaks them too ("vaults break seal"),
// but only out of combat, only for a bot within 40yd of a crystal, and not at
// all while a human master stands more than 30yd from every crystal. A DC run
// otherwise walks the tank to a boss it can never select and sits there. So
// the leader does it itself, the Zum'rah pattern: boss-nav parks the party
// in his room, the conditional below fires, and hook 64 walks the leader
// crystal to crystal. A crystal some other player is already channelling on
// is left to him, so the strategy's own clicks and this hook share the work.
namespace
{
    constexpr uint32 VAULTS_MAP = 936;
    constexpr uint32 VAULTS_EVENT_SEALS = 1;
    constexpr uint32 VAULTS_HOOK_BREAK_SEALS = 64;  // ObjectiveHookRegistry id

    constexpr uint32 VAULTS_DECEIVER = 162704;
    constexpr uint32 VAULTS_SEAL_CRYSTAL = 162705;
    constexpr uint32 VAULTS_SPELL_HOLY_CHANNEL = 47540;  // the breaker's channel visual

    // Room-sized, like Zum'rah's: the event fires as soon as boss-nav parks the
    // tank by him, never from the corridor before.
    constexpr float VAULTS_DECEIVER_SCAN = 60.0f;
    // The core's own SealRange: the crystals the boss counts.
    constexpr float VAULTS_SEAL_RANGE = 40.0f;
    // A player clicks from beside the crystal; the strategy clicks from 3yd.
    constexpr float VAULTS_SEAL_REACH = 3.0f;
    // Three crystals, 3s each, ~25yd of walking between them.
    constexpr uint32 VAULTS_SEALS_TIMEOUT = 60000;

    Creature* SealedDeceiver(Player* bot)
    {
        Creature* deceiver = bot->FindNearestCreature(VAULTS_DECEIVER, VAULTS_DECEIVER_SCAN, /*alive*/ true);
        if (!deceiver || !deceiver->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
            return nullptr;
        return deceiver;
    }

    bool VaultsDeceiverSealed(Player* bot, AiObjectContext* /*context*/)
    {
        return SealedDeceiver(bot) != nullptr;
    }

    // True when some player on the map is already channelling on `crystal`:
    // its breaker is set, a second click would be ignored.
    bool SealBeingBroken(Map* map, Creature* crystal)
    {
        for (auto const& ref : map->GetPlayers())
            if (Player* player = ref.GetSource())
                if (player->GetGuidValue(UNIT_FIELD_CHANNEL_OBJECT) == crystal->GetGUID())
                    return true;
        return false;
    }

    // Hook 64: break every seal crystal round the Deceiver (see above). Done
    // once none is left alive; the awakening then runs on the boss's own timer
    // and the clear engages him as soon as he is selectable.
    ObjectiveArriveResult BreakDeceiverSeals(Player* bot, AiObjectContext* /*context*/,
                                             DungeonBossInfo const& /*info*/)
    {
        Creature* deceiver = bot->FindNearestCreature(VAULTS_DECEIVER, VAULTS_DECEIVER_SCAN, /*alive*/ true);
        if (!deceiver)
            return ObjectiveArriveResult::Done;

        // Mid-break: the crystal dies on its own 3s timer, stand still for it.
        if (bot->GetUInt32Value(UNIT_CHANNEL_SPELL) == VAULTS_SPELL_HOLY_CHANNEL)
        {
            if (bot->isMoving())
                bot->StopMoving();
            return ObjectiveArriveResult::Running;
        }

        std::list<Creature*> crystals;
        deceiver->GetCreatureListWithEntryInGrid(crystals, VAULTS_SEAL_CRYSTAL, VAULTS_SEAL_RANGE);
        bool anyAlive = false;
        Creature* next = nullptr;
        for (Creature* crystal : crystals)
        {
            if (!crystal->IsAlive())
                continue;
            anyAlive = true;
            if (SealBeingBroken(bot->GetMap(), crystal))
                continue;
            if (!next || bot->GetExactDist2d(crystal) < bot->GetExactDist2d(next))
                next = crystal;
        }
        if (!anyAlive)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Vaults: the Deceiver's seals are broken, waiting for him to awaken",
                     bot->GetName());
            return ObjectiveArriveResult::Done;
        }
        // Every live crystal is mid-break by someone else.
        if (!next)
            return ObjectiveArriveResult::Running;

        if (bot->GetExactDist2d(next) > VAULTS_SEAL_REACH)
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
                 "[DC:{}] Vaults: breaking a seal of the Deceiver ({} at {:.1f}yd)",
                 bot->GetName(), next->GetGUID().ToString(), bot->GetExactDist2d(next));
        next->HandleSpellClick(bot);
        return ObjectiveArriveResult::Running;
    }
}

void RegisterVaultsHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, VAULTS_HOOK_BREAK_SEALS, &BreakDeceiverSeals);
}

void RegisterVaultsEvents(std::vector<DungeonEvent>& out)
{
    // Not Repeatable: the seals are broken once per instance (see above), and
    // the predicate reads false for good once he is awake. Folded under the
    // Deceiver in the panel — it is his pull, not a step of its own.
    out.push_back(EventBuilder(VAULTS_MAP, VAULTS_EVENT_SEALS, "Break the Deceiver's seals")
                      .Conditional(&VaultsDeceiverSealed)
                      .PanelBeforeBoss(VAULTS_DECEIVER)
                      .StepsOwnMovement()
                      .Custom(VAULTS_HOOK_BREAK_SEALS)
                          .Timeout(VAULTS_SEALS_TIMEOUT)
                      .Build());
}
