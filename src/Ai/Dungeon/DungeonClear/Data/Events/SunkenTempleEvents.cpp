/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

#include <cmath>
#include <string>
#include <unordered_set>

#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Item.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Player.h"
#include "Playerbots.h"
#include "Spell.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFormGate.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

// --- Sunken Temple / Temple of Atal'Hakkar (map 109) ----------------------
// The most mechanically dense dungeon in the set: THREE independent scripted
// gates over four stacked Z-levels around a central pit. Full design in
// deployment-files/docs/mod-dungeon-clear_sunken-temple-events_plan.md.
//
// GATE 1 (required) — the FORCEFIELD to Jammal'an. Six Atal'ai "defender"
//   mini-bosses ring the upper level (Z ≈ −66.8); each death sets instance
//   data, and the 6th drops the forcefield (GO 149431). They are NOT
//   DungeonEncounters (the auto-roster treats them as trash) and sit ~186yd
//   from Jammal'an, so normal flow doesn't guarantee the gate opens.
//
//   CRITICAL navigation note (live-verified 2026-06-14): each defender stands on
//   its OWN upper BALCONY whose only walkable approach is a long, winding route
//   AROUND the room — the balcony sits high above a floor-level room that shares
//   its X,Y. A raw MoveTo/EngageDirect (base PathGenerator, 74-waypoint cap)
//   can't build that whole ascent: it overruns the cap, the path resolves
//   INCOMPLETE, and the tank walks to the wrong floor-level room directly under
//   the balcony instead of up to the defender. So the forcefield CANNOT be a
//   conditional KillCreatureEngage chain (that drives EngageDirect, which
//   bee-lines into the wrong room), and it CANNOT pair two defenders per anchor
//   (the second, far defender is reached via EngageDirect and gets skipped —
//   the reported bug). Each defender MUST be reached by BOSS-NAV: the
//   LongRangePathfinder (no hop cap, used only for roster anchors) resolves the
//   full multi-level route. Hence SIX OBJECTIVE anchors, one per defender
//   (events 1/12/13/14/15/16); boss-nav drives every approach, and EngageDirect
//   only closes the last few yards once the tank has arrived. Same lesson as the
//   statue rim walk (§5/§6 Watch B).
//   The CoA 2.8 core (#8527) opens the forcefield from the start; the six anchors
//   then read done from the instance data (ST_DATA_DEFENDER_KILLED) and the ring
//   is not walked at all.
//
// GATE 2 (optional pit wing) — the STATUE order puzzle -> Atal'alarion. Six
//   statues on the middle ring must be clicked IN ENTRY ORDER (148830..148835);
//   at statuePhase 6 Atal'alarion un-phases at the pit bottom and the Idol of
//   Hakkar is summoned. Each statue is its OWN objective anchor (boss-nav does
//   the long rim walk) carrying a one-step UseGO event (events 2..7).
//
// GATE 3 (optional, hardest) — the AVATAR of Hakkar. Clicking the idol is a
//   QUESTGIVER no-op; the Shade is summoned ONLY by player spell 12346 "Awaken
//   the Soulflayer" (normally fired via the quest item Yeh'kinya's Scroll). The
//   bot CASTS 12346 directly (event 8) — the spell script has no statue/boss/
//   quest gate, so the Shade spawns at the fixed north-room spot. It manifests
//   into the Avatar (event 9, Optional + Persistent) on its own via the OOC
//   suppressor counter while the bot walks north.
//
// ORDERING TRAP — Weaver/Dreamscythe spawn phaseMask 2 (invisible) until
//   Jammal'an dies, and Atal'alarion is bit 0 (visited first by default) but is
//   the optional, puzzle-gated pit boss. The DBC bit order is NOT a valid clear
//   order. BossRosterRegistry (map 109) removes those three from their low bits
//   and re-adds them as objective anchors ordered after their un-phase trigger,
//   each carrying a KillCreature(engage) event (events 10/11) so the real kill
//   still flips the real DBC bit. See that file + §9 of the plan. Since core
//   #11271 all four Sanctum dragons also stay hidden until the Avatar dies, so
//   they follow him in the clear order (ST_MORPHAZ).

namespace
{
    // GATE 1 — the six Atal'ai defenders (each on its own upper balcony,
    // Z ≈ −66.8). Killing all six drops the forcefield. Each is a mini-boss with
    // adds/totems; the kill step gets a generous timeout (approach + fight).
    constexpr uint32 ST_ZOLO = 5712;
    constexpr uint32 ST_GASHER = 5713;
    constexpr uint32 ST_LORO = 5714;
    constexpr uint32 ST_HUKKU = 5715;
    constexpr uint32 ST_ZULLOR = 5716;
    constexpr uint32 ST_MIJAN = 5717;
    constexpr uint32 ST_DEFENDER_TIMEOUT = 120000;  // 120s per defender
    // Boss-nav delivers the tank to the anchor (its own defender), so the engage
    // only needs a local search around the arrival spot to find that defender.
    constexpr float ST_DEFENDER_SEARCH = 60.0f;
    // sunken_temple.h DATA_DEFENDER_KILLED / DEFENDERS_COUNT. GetData of that slot
    // reads the six-kill counter on the stock script, and a constant 6 on the CoA
    // 2.8 core, whose forcefield stands open from the start (core #8527). The six
    // defender anchors are finished once it reads 6: on the stock script that is
    // the kill that drops the forcefield, on the 2.8 core it is every tick, so the
    // ring walk (and the "Stuck off the route to Hukku" stalls on it) is skipped.
    constexpr int32 ST_DATA_DEFENDER_KILLED = 11;
    constexpr uint32 ST_DEFENDERS_COUNT = 6;

    // The Shade of Eranikus, the last boss. The CoA 2.8 core (#8527) keeps him
    // NOT_SELECTABLE until the Avatar of Hakkar, Jammal'an and Ogom, Dreamscythe
    // and Weaver, and Hazzas and Morphaz are all dead, so the roster sends the
    // party to him only after the Avatar (order key below, past the pit wing).
    // Before that core he was reached right after Hazzas and Morphaz, and on the
    // 24/10 night every normal run engaged an untargetable Shade there and wiped
    // (13/21, 4 runs of 4); the 23/10 binary, without that gate, killed him in
    // 36-63s. The later order is valid on both cores. Between the Avatar (18)
    // and him come the four Sanctum dragons (19-21, see ST_MORPHAZ_ORDER), the
    // pre-clear (22) and the muster (23).
    constexpr uint32 ST_ERANIKUS = 5709;
    constexpr int32 ST_ERANIKUS_ORDER = 24;

    // The four Sanctum dragons. Since core #11271 (2.9.4) the instance keeps
    // Weaver, Dreamscythe, Morphaz and Hazzas invisible, passive and
    // unattackable until BOTH the Avatar of Hakkar and Jammal'an are dead.
    // Ordered right after Jammal'an, every 2.9.4 RC1 normal run walked to an
    // invisible Weaver and idled there until the 3h limit (3 runs of 3), so
    // all four now follow the Avatar (18): Weaver & Dreamscythe (19), Morphaz
    // (20), Hazzas (21). They keep their real kill-bits. On an older core they
    // are already visible there, so the later order is valid on both.
    constexpr uint32 ST_MORPHAZ = 5719;
    constexpr uint32 ST_HAZZAS = 5722;
    constexpr int32 ST_DRAGONS_ORDER = 19;
    constexpr int32 ST_MORPHAZ_ORDER = 20;
    constexpr int32 ST_HAZZAS_ORDER = 21;
    constexpr float ST_ERANIKUS_X = -658.4f;  // his spawn
    constexpr float ST_ERANIKUS_Y = -35.8f;
    constexpr float ST_ERANIKUS_Z = -90.8f;

    // His pull wakes the dragonkin. Shade of Eranikus smart_scripts "On Aggro -
    // Set Instance Data 12" -> instance_sunken_temple DATA_ERANIKUS_FIGHT, which
    // runs SetInCombatWithZone on EVERY living dragonkin in a loaded grid. The
    // nearest are the whelp/wyrmkin/scalebane pack 102-121yd east of him
    // (centre below); they reach the fight in seconds and land on the healer.
    // Event 18 kills that pack first, dragonkin only.
    constexpr uint32 ST_NIGHTMARE_WHELP = 8319;
    constexpr uint32 ST_NIGHTMARE_SCALEBANE = 5277;
    constexpr uint32 ST_NIGHTMARE_WYRMKIN = 5280;
    constexpr uint32 ST_NIGHTMARE_WANDERER = 5283;
    constexpr float ST_EAST_PACK_X = -553.0f;
    constexpr float ST_EAST_PACK_Y = -68.0f;
    constexpr float ST_EAST_PACK_Z = -90.8f;
    constexpr float ST_EAST_PACK_RADIUS = 25.0f;  // the six spawns sit within 13yd
    constexpr float ST_EAST_PACK_ZBAND = 10.0f;
    constexpr uint32 ST_EAST_PACK_TIMEOUT = 120000;

    // Event 19: the muster before the Shade (hook below). The anchor stops the
    // tank 40yd short of him on the way in (well outside his aggro); the hook
    // holds there until the party stands together and is topped off, for at most
    // ST_MUSTER_CAP_MS, then lets the boss engage go. The step timeout is a
    // backstop above the cap: the hook always ends the step itself, so the rest
    // override it raised is always taken back.
    constexpr uint32 HOOK_ST_ERANIKUS_MUSTER = 63;
    constexpr float ST_MUSTER_ARRIVE = 40.0f;
    constexpr float ST_MUSTER_SHADE_SCAN = 120.0f;
    constexpr uint32 ST_MUSTER_CAP_MS = 120000;
    constexpr uint32 ST_MUSTER_TIMEOUT = 180000;
    // Bars, bots: health full but for a sliver, mana at the Smart Rest release
    // bar. Humans are only held to looser bars (they may not drink on cue). The
    // tank runs this hook and owns the tick, so it cannot eat or drink here: it
    // is held to health only, which the healers restore out of combat.
    constexpr float ST_MUSTER_BOT_HP = 95.0f;
    constexpr float ST_MUSTER_BOT_MANA = 90.0f;
    constexpr float ST_MUSTER_HUMAN_HP = 90.0f;
    constexpr float ST_MUSTER_HUMAN_MANA = 80.0f;
    constexpr float ST_MUSTER_TANK_HP = 90.0f;

    // Central-circle pre-clear (event 17, before Jammal'an). Centre = the pit
    // centre (matches the defender-ring centre). Radius covers the circular floor;
    // the floor z-band keeps the defender balconies (Z ≈ −66.8, above) and the
    // statue/Atal'alarion pit (Z ≤ −148, below) out of a floor-level sweep.
    constexpr float ST_CIRCLE_RADIUS = 60.0f;
    constexpr float ST_CIRCLE_ZBAND = 20.0f;
    constexpr uint32 ST_CIRCLE_TIMEOUT = 120000;  // give-up valve -> pull anyway

    // GATE 2 — statues (middle ring, Z -148.7), in correct click order.
    constexpr uint32 ST_STATUE_1 = 148830;
    constexpr uint32 ST_STATUE_2 = 148831;
    constexpr uint32 ST_STATUE_3 = 148832;
    constexpr uint32 ST_STATUE_4 = 148833;
    constexpr uint32 ST_STATUE_5 = 148834;
    constexpr uint32 ST_STATUE_6 = 148835;
    // Comfortably covers each statue objective's 8yd arrive radius.
    constexpr float ST_STATUE_SEARCH = 12.0f;

    // Reordered real bosses (see BossRosterRegistry map 109).
    constexpr uint32 ST_WEAVER = 5720;
    constexpr uint32 ST_DREAMSCYTHE = 5721;
    constexpr uint32 ST_ATALALARION = 8580;
    constexpr float ST_BOSS_SEARCH = 250.0f;
    // A real boss fight (approach + kill) far exceeds the default step timeout.
    constexpr uint32 ST_BOSS_TIMEOUT = 180000;  // 3 min

    // GATE 3 — Avatar of Hakkar (the Sanctum of the Fallen, the north flame room).
    // FULL mechanic (live + DB verified). The summon trigger is NOT the idol:
    // GO 148838 "Idol of Hakkar" is a QUESTGIVER (Use() just opens a gossip
    // greeting), and a bare CastSpell(12346) is rejected before its SEND_EVENT
    // fires. The encounter starts ONLY when a player USES the quest item
    //   Egg of Hakkar (10465)  [casts 12346 "Awaken the Soulflayer"]
    // standing AT THE CENTRE of the Sanctum (the Shade-spawn spot below). On use:
    //   - the Shade of Hakkar (8440) spawns; the room gates (149432/149433) shut;
    //     waves spawn.
    //   - The Shade tracks two counters (its smart_scripts):
    //       counter 1 = Eternal Flames doused. Each of the FOUR corner flame GOs
    //         (148418-148421), when USED, does SET_COUNTER 1 +1 on the Shade
    //         (their event 64 GOSSIP_HELLO -> action 63). At counter 1 == 4 the
    //         Shade casts 12639 and SUMMONS the Avatar (8443). <- the win path.
    //       counter 2 = suppressor drain. Nightmare Suppressors (8497) channel and
    //         SET_COUNTER 2 +1; at counter 2 == 25 the Shade DESPAWNS and the
    //         event RESETS (fail). So the suppressors MUST be interrupted/killed.
    //   - Using a flame needs Hakkari Blood (item 10460) — the flame's lock (520)
    //     consumes it (dropped by Bloodkeepers 8438 during the waves).
    // OUR CHEAT (phase 2, the in-combat orchestration — TODO): after the egg use,
    // grant the tank Hakkari Blood and walk it corner-to-corner USING each flame
    // (counter 1 -> 4) while the party fights the waves and interrupts the
    // suppressors; then kill the Avatar. PHASE 1 (this file) only fires the egg
    // so the encounter actually starts.
    constexpr uint32 ST_EGG_OF_HAKKAR = 10465;   // used at the room centre
    // The Sanctum centre / Shade-spawn spot (mirrors the OBJ(9) anchor in
    // BossRosterRegistry). The egg summon FAILS unless it is used FROM this
    // centre, so the egg-use event walks the tank precisely here first.
    constexpr float ST_SANCTUM_CX = -466.8f;
    constexpr float ST_SANCTUM_CY = 272.9f;
    constexpr float ST_SANCTUM_CZ = -90.4f;
    // Tight tolerance for the centre approach — the objective arrive radius (8yd)
    // is too loose; an off-centre egg use deadlocks the run (live-verified
    // 2026-06-15: tank parked short of centre, summon silently failed).
    constexpr float ST_SANCTUM_CENTRE_RADIUS = 3.0f;
    constexpr uint32 ST_AVATAR = 8443;
    constexpr float ST_NORTH_SEARCH = 80.0f;
    // Generous wait for the Avatar to manifest once the flames are doused.
    constexpr uint32 ST_AVATAR_SPAWN_WAIT = 150000;

    // Awakening the Soulflayer (hook 68, event 8). The one-shot MoveTo(3yd) +
    // UseItem pair it replaces reported Done without ever checking the encounter
    // had started, and when the 3yd approach did not land inside the default 30s
    // step timeout the Optional event was skipped before the egg was used at
    // all: no "event-step UseItem 10465" line for the tank, no wave, the Avatar
    // wait ran out 150s later, and on the CoA 2.8 core the Shade of Eranikus
    // then stayed NOT_SELECTABLE and killed the party (tr-20261025-161455-156,
    // -145609-105, -141955-80, PC -180819-23: 19/23, the Avatar's encounter bit
    // never set). The hook holds until the instance reads the Hakkar event
    // started (instance_sunken_temple TYPE_HAKKAR_EVENT, slot 2) and retries the
    // trigger until it does.
    //
    // The 2.8 core adds the Hakkar Worship Altar (GO 2310500, 7.8yd west of the
    // centre), whose gossip starts the same encounter without the egg; its
    // summon spot is fixed, so where the altar is used from does not matter.
    // The stock script has no altar and needs the egg used from the centre.
    constexpr uint32 HOOK_ST_AWAKEN_SOULFLAYER = 68;
    constexpr uint32 ST_TYPE_HAKKAR_EVENT = 2;
    constexpr uint32 ST_WORSHIP_ALTAR = 2310500;
    constexpr uint32 ST_SHADE_OF_HAKKAR = 8440;
    constexpr float ST_ALTAR_REACH = 5.0f;
    // Past this on the step, trigger from where the tank stands: the altar from
    // anywhere in the room (GameObject::Use has no range check of its own), the
    // egg after a near-teleport to the centre, which is what it needs.
    constexpr uint32 ST_AWAKEN_WALK_MS = 20000;
    constexpr float ST_AWAKEN_FORCE_RANGE = 40.0f;
    // One trigger per this many ms on the step, so a refused use is retried
    // without spamming.
    constexpr uint32 ST_AWAKEN_RETRY_MS = 4000;
    constexpr uint32 ST_AWAKEN_TIMEOUT = 90000;

    // DungeonEncounter.dbc bit the Avatar of Hakkar's kill sets ("encounter #7"
    // in every run record that killed him). Read by the muster before the Shade.
    constexpr uint32 ST_AVATAR_ENCOUNTER_BIT = 7;
    // How many times the muster sends the party back to the Sanctum when the
    // Shade still slumbers because the Avatar is not dead.
    constexpr uint8 ST_AVATAR_RETRIES = 2;
}

namespace
{
    // --- the Shade of Eranikus muster (hook 63) ----------------------------
    // 5-man counterpart of the raid muster (DcRaidMuster, raid maps only): raise
    // the run's RestHealthPct/RestManaPct to 100 so the bots eat and drink all the
    // way up, and hold the tank until every living member stands within
    // PartyMaxSpread of it and is over the bars above. The 24/10 night showed the
    // party walking onto the Shade 20-40s after Hazzas and Morphaz, healer first
    // to die, with "Waiting on ... (out of range)" just before the pull.
    //
    // The override is flagged with DcRunState::musterRestOverride, the raid
    // muster's own flag (DcRaidMuster never runs on a 5-man map): with it set,
    // DcPartyState::GetRestGate does not read the 100/100 back as a readiness
    // floor, so this hook, bounded by ST_MUSTER_CAP_MS, is the only thing that
    // waits on it. Retracted on every Done; never set over a value the player
    // pinned for the run.
    void ApplyMusterRest(Player* bot, DcRunState& run)
    {
        if (run.musterRestOverride)
            return;
        ObjectGuid const owner = bot->GetGUID();
        if (DcSettings::HasOverride(owner, "RestHealthPct") ||
            DcSettings::HasOverride(owner, "RestManaPct"))
            return;
        DcSettings::SetOverride(owner, "RestHealthPct", 100.0, nullptr);
        DcSettings::SetOverride(owner, "RestManaPct", 100.0, nullptr);
        run.musterRestOverride = true;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] Shade of Eranikus — mustering: regroup and rest to full before the pull",
                 bot->GetName());
    }

    void RetractMusterRest(Player* bot, DcRunState& run)
    {
        if (!run.musterRestOverride)
            return;
        DcSettings::ResetOverride(bot->GetGUID(), "RestHealthPct");
        DcSettings::ResetOverride(bot->GetGUID(), "RestManaPct");
        run.musterRestOverride = false;
    }

    // The Shade of Eranikus still slumbers (CoA 2.8 core: NOT_SELECTABLE until
    // the Avatar, Jammal'an and Ogom, Dreamscythe and Weaver, Hazzas and Morphaz
    // are dead). Pulling him then is a wipe: he hits the tank, nobody can hit
    // him. If the Avatar is what is missing, reopen the two Sanctum anchors and
    // send the party back there (at most ST_AVATAR_RETRIES times); otherwise, or
    // once the retries are spent, stall rather than pull.
    ObjectiveArriveResult HoldSlumberingShade(Player* bot, AiObjectContext* context, DcRunState& run)
    {
        RetractMusterRest(bot, run);

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        uint32 const mask = inst ? inst->GetCompletedEncounterMask() : 0u;
        uint32 const hakkar = inst ? inst->GetData(ST_TYPE_HAKKAR_EVENT) : 0u;
        bool const avatarDead = (mask & (1u << ST_AVATAR_ENCOUNTER_BIT)) != 0;

        if (!avatarDead && run.stAvatarRetries < ST_AVATAR_RETRIES)
        {
            ++run.stAvatarRetries;
            auto& cleared =
                context->GetValue<std::unordered_set<uint32>&>(DcKey::ClearedAnchors)->Get();
            cleared.erase(DcRoster::OBJ(9));
            cleared.erase(DcRoster::OBJ(10));
            run.selectedBossEntry = DcRoster::OBJ(9);
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] Shade of Eranikus still slumbers and the Avatar of Hakkar is not "
                     "dead (mask=0x{:x}, Hakkar event {}) -> back to the Sanctum to awaken the "
                     "Soulflayer (retry {}/{})",
                     bot->GetName(), mask, hakkar, run.stAvatarRetries, ST_AVATAR_RETRIES);
            return ObjectiveArriveResult::Running;
        }

        LOG_WARN("playerbots.dungeonclear",
                 "[DC:{}] Shade of Eranikus still slumbers (mask=0x{:x}, Avatar dead {}, Hakkar "
                 "event {}, retries {}) -> not pulling a boss nobody can hit",
                 bot->GetName(), mask, avatarDead, hakkar, run.stAvatarRetries);
        return ObjectiveArriveResult::Blocked;
    }

    // Hook 68: start the Avatar of Hakkar encounter and confirm it started (see
    // HOOK_ST_AWAKEN_SOULFLAYER above).
    ObjectiveArriveResult AwakenSoulflayer(Player* bot, AiObjectContext* context,
                                           DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        uint32 const hakkar = inst ? inst->GetData(ST_TYPE_HAKKAR_EVENT) : 0u;
        if (hakkar != NOT_STARTED ||
            bot->FindNearestCreature(ST_SHADE_OF_HAKKAR, ST_NORTH_SEARCH, /*alive*/ true) ||
            bot->FindNearestCreature(ST_AVATAR, ST_NORTH_SEARCH, /*alive*/ true) ||
            bot->FindNearestCreature(ST_AVATAR, ST_NORTH_SEARCH, /*alive*/ false))
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Awaken the Soulflayer: encounter started (Hakkar event {})",
                     bot->GetName(), hakkar);
            return ObjectiveArriveResult::Done;
        }

        DungeonEventProgress& prog =
            context->GetValue<DungeonEventProgress&>(DcKey::EventProgress)->Get();
        uint32 const elapsed = getMSTimeDiff(prog.stepStartMs, getMSTime());
        bool const late = elapsed >= ST_AWAKEN_WALK_MS;

        GameObject* altar = bot->FindNearestGameObject(ST_WORSHIP_ALTAR, ST_NORTH_SEARCH);
        if (altar && altar->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
            altar = nullptr;

        bool inPlace = false;
        if (altar)
        {
            float const d = bot->GetExactDist2d(altar);
            inPlace = d <= ST_ALTAR_REACH || (late && d <= ST_AWAKEN_FORCE_RANGE);
            if (!inPlace)
            {
                // Halfway between the altar and the centre: on the floor the tanks
                // already reach, inside the altar's reach.
                float const x = (altar->GetPositionX() + ST_SANCTUM_CX) * 0.5f;
                float const y = (altar->GetPositionY() + ST_SANCTUM_CY) * 0.5f;
                if (!bot->isMoving())
                    bot->GetMotionMaster()->MovePoint(0, x, y, ST_SANCTUM_CZ,
                                                      FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                                      /*generatePath*/ true, false);
                return ObjectiveArriveResult::Running;
            }
        }
        else
        {
            inPlace = bot->GetExactDist2d(ST_SANCTUM_CX, ST_SANCTUM_CY) <= ST_SANCTUM_CENTRE_RADIUS &&
                      std::fabs(bot->GetPositionZ() - ST_SANCTUM_CZ) <= ST_SANCTUM_CENTRE_RADIUS;
            if (!inPlace)
            {
                if (late)
                {
                    LOG_WARN("playerbots.dungeonclear",
                             "[DC:{}] Awaken the Soulflayer: {}s without reaching the Sanctum "
                             "centre ({:.1f}yd off) -> near-teleport onto it",
                             bot->GetName(), elapsed / IN_MILLISECONDS,
                             bot->GetExactDist2d(ST_SANCTUM_CX, ST_SANCTUM_CY));
                    bot->NearTeleportTo(ST_SANCTUM_CX, ST_SANCTUM_CY, ST_SANCTUM_CZ,
                                        bot->GetOrientation());
                }
                else if (!bot->isMoving())
                    bot->GetMotionMaster()->MovePoint(0, ST_SANCTUM_CX, ST_SANCTUM_CY,
                                                      ST_SANCTUM_CZ, FORCED_MOVEMENT_NONE, 0.0f,
                                                      0.0f, /*generatePath*/ true, false);
                return ObjectiveArriveResult::Running;
            }
        }

        // In place: one trigger per ST_AWAKEN_RETRY_MS, confirmed next tick by the
        // instance data read above.
        if (elapsed < prog.attempts * ST_AWAKEN_RETRY_MS)
            return ObjectiveArriveResult::Running;
        ++prog.attempts;
        if (bot->isMoving())
            bot->StopMoving();

        if (altar)
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Awaken the Soulflayer: using the Hakkar Worship Altar at {:.1f}yd "
                     "(attempt {}, Hakkar event {})",
                     bot->GetName(), bot->GetExactDist2d(altar), prog.attempts, hakkar);
            altar->Use(bot);
            return ObjectiveArriveResult::Running;
        }

        Item* egg = bot->GetItemByEntry(ST_EGG_OF_HAKKAR);
        if (!egg)
        {
            bot->AddItem(ST_EGG_OF_HAKKAR, 1);
            egg = bot->GetItemByEntry(ST_EGG_OF_HAKKAR);
            if (!egg)
                return ObjectiveArriveResult::Running;  // bags full this tick — retry
        }
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] Awaken the Soulflayer: using the Egg of Hakkar at the centre "
                 "(attempt {}, Hakkar event {})",
                 bot->GetName(), prog.attempts, hakkar);
        DcFormGate::DropBlockingForm(bot, egg);
        SpellCastTargets targets;
        targets.SetUnitTarget(bot);
        bot->CastItemUseSpell(egg, targets, 0, 0);
        return ObjectiveArriveResult::Running;
    }

    ObjectiveArriveResult MusterBeforeEranikus(Player* bot, AiObjectContext* context,
                                               DungeonBossInfo const& /*info*/)
    {
        DcRunState& run = DcRun::Of(context);

        // Already fighting him (or he is gone): nothing left to prepare.
        Creature* shade = bot->FindNearestCreature(ST_ERANIKUS, ST_MUSTER_SHADE_SCAN, /*alive*/ true);
        if (!shade || shade->IsInCombat())
        {
            RetractMusterRest(bot, run);
            return ObjectiveArriveResult::Done;
        }

        if (shade->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
            return HoldSlumberingShade(bot, context, run);

        Group* group = bot->GetGroup();
        if (!group)
        {
            RetractMusterRest(bot, run);
            return ObjectiveArriveResult::Done;
        }

        DungeonEventProgress const& prog =
            context->GetValue<DungeonEventProgress&>(DcKey::EventProgress)->Get();
        uint32 const elapsed = getMSTimeDiff(prog.stepStartMs, getMSTime());

        ApplyMusterRest(bot, run);

        float const spread = DcSettings::GetFloat(bot, "PartyMaxSpread");
        std::string waiting;
        uint32 blockers = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInWorld() || !member->IsAlive() ||
                member->GetMapId() != bot->GetMapId())
                continue;

            bool const isBot = GET_PLAYERBOT_AI(member) != nullptr;
            bool const isTank = member == bot;
            float const hpBar = isTank ? ST_MUSTER_TANK_HP
                                       : (isBot ? ST_MUSTER_BOT_HP : ST_MUSTER_HUMAN_HP);
            float const manaBar = isTank ? 0.0f
                                         : (isBot ? ST_MUSTER_BOT_MANA : ST_MUSTER_HUMAN_MANA);

            char const* why = nullptr;
            if (!isTank && bot->GetDistance(member) > spread)
                why = "out of range";
            else if (member->GetHealthPct() < hpBar)
                why = "health";
            else if (manaBar > 0.0f && member->GetMaxPower(POWER_MANA) > 0 &&
                     member->GetPowerPct(POWER_MANA) < manaBar)
                why = "mana";
            if (!why)
                continue;

            ++blockers;
            if (!waiting.empty())
                waiting += ", ";
            waiting += member->GetName() + " (" + why + ")";
        }

        if (!blockers)
        {
            RetractMusterRest(bot, run);
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Shade of Eranikus — party together and topped off after {}s, pulling",
                     bot->GetName(), elapsed / IN_MILLISECONDS);
            return ObjectiveArriveResult::Done;
        }

        if (elapsed >= ST_MUSTER_CAP_MS)
        {
            RetractMusterRest(bot, run);
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] Shade of Eranikus — muster cap ({}s) reached, still waiting on {}; "
                     "pulling anyway", bot->GetName(), ST_MUSTER_CAP_MS / IN_MILLISECONDS, waiting);
            return ObjectiveArriveResult::Done;
        }

        return ObjectiveArriveResult::Running;
    }
}

void RegisterSunkenTempleHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, HOOK_ST_ERANIKUS_MUSTER, &MusterBeforeEranikus);
    ObjectiveHookRegistry::AddHook(out, HOOK_ST_AWAKEN_SOULFLAYER, &AwakenSoulflayer);
}

void RegisterSunkenTempleEvents(std::vector<DungeonEvent>& out)
{
    // --- Events 1/12/13/14/15/16: GATE 1 — drop the forcefield (6 anchors) --
    // ONE Anchored event per Atal'ai defender, one per objective anchor — each
    // defender stands on its OWN balcony reachable only by a long winding route
    // (BossRosterRegistry gives all six index 0, before Jammal'an, visited as one
    // gate). Boss-nav (LongRangePathfinder, no 74-cap) walks the tank all the way
    // to each defender; the event then engage-kills just that one (a short hop now
    // that the tank has arrived). Pairing two defenders per anchor was the bug:
    // the second, far defender was reached via EngageDirect's capped MoveTo, which
    // could not path the long inter-balcony trip and SKIPPED it.
    // Required: if the gate won't open the run should stall for the human rather
    // than skip a wall. A defender already swept by trash no-ops instantly (its
    // KillCreature gate is Done with no live target). The instance drops GO 149431
    // when all six die — no WaitForGOState confirm (the tank ends on a far balcony
    // where that GO's grid may be cold, so a GO scan would false-timeout).
    struct DefenderAnchor { uint32 eventId; uint32 entry; char const* name; };
    for (DefenderAnchor const& d : {
             DefenderAnchor{1, ST_MIJAN, "Atal'ai Defender (Mijan)"},
             DefenderAnchor{12, ST_ZULLOR, "Atal'ai Defender (Zul'Lor)"},
             DefenderAnchor{13, ST_ZOLO, "Atal'ai Defender (Zolo)"},
             DefenderAnchor{14, ST_GASHER, "Atal'ai Defender (Gasher)"},
             DefenderAnchor{15, ST_LORO, "Atal'ai Defender (Loro)"},
             DefenderAnchor{16, ST_HUKKU, "Atal'ai Defender (Hukku)"} })
    {
        out.push_back(EventBuilder(109, d.eventId, d.name)
                          .Anchored(0)
                          .KillCreatureEngage(d.entry, 1, ST_DEFENDER_SEARCH)
                          .Timeout(ST_DEFENDER_TIMEOUT)
                          .Build());
    }

    // --- Event 17: central-circle pre-clear (orderIndex 1, before Jammal'an) -
    // The wide circular FLOOR around the central pit is patrolled by dangerous
    // packs. Weaver & Dreamscythe un-phase and circle this floor only AFTER
    // Jammal'an dies, so the party must clear it FIRST or it walks into the whole
    // pile mid-Weaver/Dreamscythe fight. ClearRadius engages every reachable
    // hostile within ST_CIRCLE_RADIUS (2D) and ST_CIRCLE_ZBAND of the pit centre
    // — position-based, so it sweeps whatever patrols regardless of entry; the
    // floor band excludes the upper defender balconies and the deep statue/
    // Atal'alarion pit directly below. Persistent so chasing a patrolling pack to
    // the ring edge doesn't drop the objective; Optional + a generous timeout so a
    // respawn-churn or an unreachable straggler degrades to "pull anyway / dc
    // skip" rather than livelocking the required spine.
    out.push_back(EventBuilder(109, 17, "Central Circle (pre-clear)")
                      .Anchored(1)
                      .Persistent()
                      .Optional()
                      .ClearRadius(-467.0f, 95.0f, -91.0f, ST_CIRCLE_RADIUS, ST_CIRCLE_ZBAND)
                      .Timeout(ST_CIRCLE_TIMEOUT)
                      .Build());

    // --- Events 2-7: GATE 2 — the six statue clicks ------------------------
    // One Anchored event per statue: the objective anchor (BossRosterRegistry)
    // sits AT the statue so boss-nav does the long rim walk; this event just
    // clicks once the tank has arrived. Optional — the whole pit wing is bonus,
    // so a failed click skips rather than stalling the (already-finished)
    // required spine. Out-of-order is impossible (anchors are visited in roster
    // order) and a mis-click is harmless anyway (the statue phase never regresses).
    out.push_back(EventBuilder(109, 2, "Atal'ai Statue 1")
                      .Anchored(10).UseGO(ST_STATUE_1, ST_STATUE_SEARCH).Optional().Build());
    out.push_back(EventBuilder(109, 3, "Atal'ai Statue 2")
                      .Anchored(11).UseGO(ST_STATUE_2, ST_STATUE_SEARCH).Optional().Build());
    out.push_back(EventBuilder(109, 4, "Atal'ai Statue 3")
                      .Anchored(12).UseGO(ST_STATUE_3, ST_STATUE_SEARCH).Optional().Build());
    out.push_back(EventBuilder(109, 5, "Atal'ai Statue 4")
                      .Anchored(13).UseGO(ST_STATUE_4, ST_STATUE_SEARCH).Optional().Build());
    out.push_back(EventBuilder(109, 6, "Atal'ai Statue 5")
                      .Anchored(14).UseGO(ST_STATUE_5, ST_STATUE_SEARCH).Optional().Build());
    out.push_back(EventBuilder(109, 7, "Atal'ai Statue 6")
                      .Anchored(15).UseGO(ST_STATUE_6, ST_STATUE_SEARCH).Optional().Build());

    // --- Event 8: GATE 3 beat 1 — start the encounter (use the Egg) -------
    // Anchored at the CENTRE of the Sanctum of the Fallen (the Shade-spawn spot),
    // ordered after Atal'alarion. The bot USES the Egg of Hakkar (10465) — granted
    // on the fly — which is the only thing that actually summons the Shade and
    // starts the encounter (a bare spell cast / idol click does not). Optional —
    // pit-wing bonus; degrades to `dc skip` if the room can't be reached.
    // The egg summon FAILS unless used from dead centre, and the objective arrive
    // radius (8yd) lets boss-nav park the tank short of it — so a tight MoveTo
    // walks the tank precisely to the centre BEFORE the egg fires (live bug
    // 2026-06-15: tank stopped short of centre, summon failed, run deadlocked).
    // Hook 68 now owns the whole beat: the altar on the CoA 2.8 core, the egg
    // from dead centre on the stock script, retried until the instance reads the
    // encounter started (see HOOK_ST_AWAKEN_SOULFLAYER). It walks the tank
    // itself, hence StepsOwnMovement.
    out.push_back(EventBuilder(109, 8, "Awaken the Soulflayer (Egg of Hakkar)")
                      .Anchored(17)
                      .StepsOwnMovement()
                      .Optional()
                      .Custom(HOOK_ST_AWAKEN_SOULFLAYER)
                      .Timeout(ST_AWAKEN_TIMEOUT)
                      .Build());

    // --- Event 9: GATE 3 beat 2 — the Avatar of Hakkar fight ---------------
    // Anchored at the Sanctum centre, Optional + Persistent. The Avatar (8443)
    // only manifests once the FOUR Eternal Flames are doused (Shade counter 1 ->
    // 4) while the suppressors are kept interrupted (counter 2 < 25). That flame/
    // suppressor orchestration is PHASE 2 (not yet built); until then this event
    // waits for 8443 and degrades to `dc skip` on timeout. When phase 2 lands it
    // drives the douse loop, then this waits for the Avatar and kills it.
    // Persistent keeps progress across the long fight; Optional degrades a hang to
    // `dc skip`. On the CoA 2.8 core the Avatar is one of the kills that wake the
    // Shade of Eranikus, so a skipped Avatar leaves the last boss untargetable.
    out.push_back(EventBuilder(109, 9, "Avatar of Hakkar")
                      .Anchored(18)
                      .Persistent()
                      .Optional()
                      .WaitForSpawn(ST_AVATAR, /*wantAlive*/ true, ST_AVATAR_SPAWN_WAIT)
                      .KillCreatureEngage(ST_AVATAR, 1, ST_NORTH_SEARCH).Timeout(ST_BOSS_TIMEOUT)
                      .Build());

    // --- Event 10: Weaver & Dreamscythe (required spine) -------------------
    // Both spawn phaseMask 2 until Jammal'an dies, and the 2.9.4 core also hides
    // them until the Avatar of Hakkar dies; this objective is ordered after the
    // Avatar (ST_DRAGONS_ORDER) so they are visible on arrival. They sit ~10yd apart and un-phase
    // together, so ONE objective kills both (two same-index objectives would be
    // skipped by NextDungeonBossValue's strictly-greater advance-forward). Two
    // engage steps; the real kills flip their real DBC bits. Persistent so the
    // back-to-back fights don't rewind the chain; Required (spine boss).
    out.push_back(EventBuilder(109, 10, "Weaver & Dreamscythe")
                      .Anchored(ST_DRAGONS_ORDER)
                      .Persistent()
                      .KillCreatureEngage(ST_WEAVER, 1, ST_BOSS_SEARCH).Timeout(ST_BOSS_TIMEOUT)
                      .KillCreatureEngage(ST_DREAMSCYTHE, 1, ST_BOSS_SEARCH).Timeout(ST_BOSS_TIMEOUT)
                      .Build());

    // --- Event 11: Atal'alarion (optional pit wing) ------------------------
    // Un-phases at the pit bottom once statuePhase hits 6 (after statue 6). One
    // engage step kills him (flipping his real bit); his death makes the idol
    // usable (event 8). Optional + Persistent — pit-wing bonus, and the deep pit
    // may be unmeshed (water-surface navmesh), in which case an unreachable boss
    // degrades to `dc skip` after the kill step times out.
    out.push_back(EventBuilder(109, 11, "Atal'alarion")
                      .Anchored(16)
                      .Persistent()
                      .Optional()
                      .KillCreatureEngage(ST_ATALALARION, 1, ST_BOSS_SEARCH).Timeout(ST_BOSS_TIMEOUT)
                      .Build());

    // --- Event 18: the dragonkin pack east of the Shade (orderIndex 22) ------
    // Killed BEFORE the muster and the pull: the Shade's aggro zone-pulls every
    // living dragonkin in a loaded grid (see ST_EAST_PACK_*), and this pack is the
    // nearest. Dragonkin only (OnlyEntries), so the sweep never wanders onto
    // anything his pull would leave alone. Persistent so a chase does not drop the
    // objective; Optional + timeout so an unreachable straggler degrades to "go on"
    // rather than stalling the last boss.
    out.push_back(EventBuilder(109, 18, "Dragonkin before the Shade")
                      .Anchored(22)
                      .Persistent()
                      .Optional()
                      .ClearRadius(ST_EAST_PACK_X, ST_EAST_PACK_Y, ST_EAST_PACK_Z,
                                   ST_EAST_PACK_RADIUS, ST_EAST_PACK_ZBAND)
                      .OnlyEntries({ ST_NIGHTMARE_WHELP, ST_NIGHTMARE_SCALEBANE,
                                     ST_NIGHTMARE_WYRMKIN, ST_NIGHTMARE_WANDERER })
                      .Timeout(ST_EAST_PACK_TIMEOUT)
                      .Build());

    // --- Event 19: muster before the Shade of Eranikus (orderIndex 23) -------
    // Regroup and rest to full 40yd short of him (hook 63), then the ordinary
    // boss engage takes over. Optional: a muster that cannot finish only costs the
    // top-off, never the run.
    out.push_back(EventBuilder(109, 19, "Muster before the Shade of Eranikus")
                      .Anchored(23)
                      .Optional()
                      .Custom(HOOK_ST_ERANIKUS_MUSTER)
                      .Timeout(ST_MUSTER_TIMEOUT)
                      .Build());
}

// --- roster patch (relocated from BossRosterRegistry) --------------------
void RegisterSunkenTempleRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    // --- Sunken Temple (map 109) — full dungeon-events restructure
    // Three scripted gates + a phase-ordering trap (see
    // deployment-files/docs/mod-dungeon-clear_sunken-temple-events_plan.md
    // and SunkenTempleEvents.cpp). The DBC bit order is NOT a valid clear
    // order: Atal'alarion is bit 0 but is the OPTIONAL, puzzle-gated pit
    // boss, and Weaver/Dreamscythe are early bits but spawn phaseMask 2
    // (invisible) until Jammal'an dies. Played straight the tank stalls on
    // an invisible boss at #2 or wastes #0 on the deep optional pit.
    //
    // Fix (the established remove/re-add-as-objective pattern): drop the
    // three phase/puzzle-gated bosses from their low bits and re-add them
    // as OBJECTIVE anchors ordered AFTER their un-phase trigger, each
    // carrying a KillCreature(engage) event so the real kill still flips
    // the real DBC bit (objectives skip the completion-mask check, so
    // their orderIndex is a pure ordering hint that can't collide). The
    // remaining auto bosses — Jammal'an, Morphaz, Hazzas, Eranikus — keep
    // their real bits; Morphaz, Hazzas and Eranikus are moved past the
    // Avatar (p.reorder), who gates all of them on the 2.9.4 core.
    //
    // FORCEFIELD via OBJECTIVE anchors (not a conditional event). The six
    // Atal'ai defenders each stand on their OWN separate upper BALCONY
    // whose only walkable approach is a long multi-level route around the
    // room — a raw MoveTo/EngageDirect (74-cap PathGenerator) overruns the
    // cap, resolves INCOMPLETE, and walks the tank to the wrong floor-level
    // room under the balcony (live-verified). Only boss-nav's
    // LongRangePathfinder (no cap) resolves each ring approach, so EACH
    // defender gets its OWN anchor (six total). An earlier design paired
    // two defenders per anchor and engaged the second via EngageDirect;
    // that second, far defender could not be pathed and was skipped — the
    // reported "it skips the second boss of the group" bug.
    //
    // ORDER INDICES (real DBC bits, read from DungeonEncounter.dbc:
    // Jammal'an 3, Morphaz 5, Hazzas 6, Eranikus 8; Atal'alarion 0,
    // Dreamscythe 1, Weaver 2 are removed). Objective indices below:
    //   0      all six forcefield defenders (before Jammal'an 3) — they
    //          share index 0 and are visited as one gate by
    //          NextDungeonBossValue::PickTarget's same-index-first advance
    //          (only the spawned auto boss at bit 0, Atal'alarion, used
    //          this slot, and it is removed). Order among them is the
    //          ring-sweep insertion order; the forcefield drops once all
    //          six die regardless of which order they fall in.
    //   10..15 six statues in click order } the pit wing, after Jammal'an.
    //   16     Atal'alarion                } Optional, but the Avatar now
    //   17     Idol of Hakkar               } gates the four dragons and the
    //   18     Avatar of Hakkar             } Shade (see ST_MORPHAZ).
    //   19     Weaver & Dreamscythe — ONE merged objective (they are ~10yd
    //          apart and appear together).
    //   20     Morphaz (reordered, real bit 5 kept)
    //   21     Hazzas (reordered, real bit 6 kept)
    //   22     dragonkin pack east of the Shade (pre-clear, event 18)
    //   23     muster before the Shade (event 19, hook 63)
    //   24     Shade of Eranikus (reordered, real bit 8 kept) — LAST: on the
    //          CoA 2.8 core he cannot be targeted until the Avatar is dead
    //          (see ST_ERANIKUS_ORDER).
    {
        BossRosterPatch p;
        p.mapId = 109;
        // Remove the three phase/puzzle-gated bosses; re-added as
        // objectives below. (Weaver 5720, Dreamscythe 5721, Atal'alarion
        // 8580 are auto-derived bosses at bits 2/1/0 respectively.)
        p.remove = { 5720, 5721, 8580 };
        p.reorder = { { ST_MORPHAZ, ST_MORPHAZ_ORDER },
                      { ST_HAZZAS, ST_HAZZAS_ORDER },
                      { ST_ERANIKUS, ST_ERANIKUS_ORDER } };

        // A forcefield defender anchor is finished once the instance reads all
        // six defenders dead (see ST_DATA_DEFENDER_KILLED): immediately on the
        // CoA 2.8 core, where the forcefield is open from the start.
        auto const defender = [](DungeonBossInfo o)
        {
            o.doneInstanceDataId = ST_DATA_DEFENDER_KILLED;
            o.doneInstanceDataValue = ST_DEFENDERS_COUNT;
            return o;
        };
        p.add = {
            // GATE 1 forcefield — SIX ring anchors, one per Atal'ai
            // defender, in a monotonic sweep around the balcony. Each
            // defender stands on its OWN separate balcony reachable only
            // by a long, winding multi-level route, so each needs its own
            // boss-nav anchor (LongRangePathfinder, no 74-cap) to be
            // travelled to — a single anchor that then tried to engage a
            // second, far defender via EngageDirect (raw capped MoveTo)
            // could not path the trip and SKIPPED that second defender
            // (the live "it skips the second boss of the group" bug). All
            // six share encounterIndex 0 so they sort before Jammal'an
            // (bit 3, the gate they unlock) and are visited as one gate by
            // NextDungeonBossValue::PickTarget's same-index-first advance;
            // each carries a one-kill event (1/12/13/14/15/16). Coords are
            // each defender's own spawn (Z ≈ −66.8). Ring-sweep order
            // (≈30°→90°→…→330° around centre −467,95).
            defender(MakeObjective(OBJ(11), /*orderIndex*/ 0, 109, "Atal'ai Defender (Mijan)",
                                   -406.2f, 131.1f, -66.9f, /*arriveRadius*/ 15.0f,
                                   /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1)),
            defender(MakeObjective(OBJ(12), /*orderIndex*/ 0, 109, "Atal'ai Defender (Zul'Lor)",
                                   -467.4f, 166.0f, -66.7f, /*arriveRadius*/ 15.0f,
                                   /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 12)),
            defender(MakeObjective(OBJ(13), /*orderIndex*/ 0, 109, "Atal'ai Defender (Zolo)",
                                   -528.6f, 130.2f, -66.8f, /*arriveRadius*/ 15.0f,
                                   /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 13)),
            defender(MakeObjective(OBJ(14), /*orderIndex*/ 0, 109, "Atal'ai Defender (Gasher)",
                                   -528.0f, 59.5f, -66.7f, /*arriveRadius*/ 15.0f,
                                   /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 14)),
            defender(MakeObjective(OBJ(15), /*orderIndex*/ 0, 109, "Atal'ai Defender (Loro)",
                                   -466.7f, 24.4f, -66.8f, /*arriveRadius*/ 15.0f,
                                   /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 15)),
            defender(MakeObjective(OBJ(16), /*orderIndex*/ 0, 109, "Atal'ai Defender (Hukku)",
                                   -405.5f, 60.5f, -67.1f, /*arriveRadius*/ 15.0f,
                                   /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 16)),

            // Central circle PRE-CLEAR (orderIndex 1: after the six
            // defenders at 0, before Jammal'an at 3). The big circular
            // floor around the central pit is patrolled by dangerous packs
            // (whelps/wanderers/scalebanes); Weaver & Dreamscythe un-phase
            // and circle this floor AFTER Jammal'an dies, so it must be
            // swept FIRST. A ClearRadius event (17) clears every reachable
            // hostile within 60yd (2D) and a 20yd floor band of the pit
            // centre — z-banded so the upper defender balconies and the
            // deep statue/Atal'alarion pit are excluded. The anchor hangs
            // over the open pit: the walkable rim is ~20-27yd from it, so
            // arriveRadius 20 left the tank pacing the rim ("dist=20.2 >
            // arriveRadius=20.0") until no_progress (31 runs stalled there,
            // 0 kills). 32 lands anywhere on the rim floor; the clear itself
            // still sweeps the full 60yd band.
            MakeObjective(OBJ(17), /*orderIndex*/ 1, 109, "Central Circle (pre-clear)",
                          -467.0f, 95.0f, -91.0f, /*arriveRadius*/ 32.0f,
                          /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 17),

            // Required spine: Weaver + Dreamscythe (shown once Jammal'an and
            // the Avatar are dead, see ST_MORPHAZ). One objective at their
            // midpoint kills both via event 10.
            MakeObjective(OBJ(1), ST_DRAGONS_ORDER, 109, "Weaver & Dreamscythe",
                          -456.2f, 132.5f, -91.2f, /*arriveRadius*/ 30.0f,
                          /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 10),

            // Optional pit wing — six statues in click order (each at its
            // statue so boss-nav does the long rim walk), Atal'alarion, the
            // idol, the Avatar. eventIds 2..9 (see SunkenTempleEvents.cpp).
            MakeObjective(OBJ(2), 10, 109, "Atal'ai Statue 1",
                          -515.6f, 95.3f, -148.7f, 8.0f, 0, 0, /*eventId*/ 2),
            MakeObjective(OBJ(3), 11, 109, "Atal'ai Statue 2",
                          -419.8f, 94.5f, -148.7f, 8.0f, 0, 0, /*eventId*/ 3),
            MakeObjective(OBJ(4), 12, 109, "Atal'ai Statue 3",
                          -491.4f, 136.0f, -148.7f, 8.0f, 0, 0, /*eventId*/ 4),
            MakeObjective(OBJ(5), 13, 109, "Atal'ai Statue 4",
                          -491.5f, 53.5f, -148.7f, 8.0f, 0, 0, /*eventId*/ 5),
            MakeObjective(OBJ(6), 14, 109, "Atal'ai Statue 5",
                          -443.9f, 136.1f, -148.7f, 8.0f, 0, 0, /*eventId*/ 6),
            MakeObjective(OBJ(7), 15, 109, "Atal'ai Statue 6",
                          -443.4f, 53.8f, -148.7f, 8.0f, 0, 0, /*eventId*/ 7),
            // Atal'alarion un-phases at statuePhase 6; killed via event 11.
            MakeObjective(OBJ(8), 16, 109, "Atal'alarion",
                          -480.4f, 96.6f, -189.7f, 30.0f, 0, 0, /*eventId*/ 11),
            // Start the Avatar encounter: USE the Egg of Hakkar at the
            // CENTRE of the Sanctum of the Fallen (the Shade-spawn spot),
            // not down at the pit idol (a QUESTGIVER no-op). Anchored at
            // the room centre so boss-nav stands the tank there before the
            // egg fires (event 8). Same spot as the Avatar anchor below.
            MakeObjective(OBJ(9), 17, 109, "Awaken the Soulflayer",
                          -466.8f, 272.9f, -90.4f, 8.0f, 0, 0, /*eventId*/ 8),
            // Avatar manifests in the north flame room (event 9). NO
            // gateEntry: a gate firing the Persistent event from afar (the
            // tank still in the pit) would run its KillCreature gates with
            // the channelers/Avatar beyond the 80yd search and falsely
            // complete the fight. arriveRadius-only makes boss-nav travel
            // the tank INTO the room before the event starts.
            MakeObjective(OBJ(10), 18, 109, "Avatar of Hakkar",
                          -466.8f, 272.9f, -90.4f, 15.0f, /*gateEntry*/ 0,
                          0, /*eventId*/ 9),

            // Before the Shade of Eranikus (reordered to 24, last): kill the
            // dragonkin pack his pull would bring first (event 18, anchored on
            // the pack), then muster 40yd short of him (event 19, anchored on
            // his spawn so boss-nav walks the tank in and stops outside his
            // aggro).
            MakeObjective(OBJ(18), 22, 109, "Dragonkin before the Shade",
                          ST_EAST_PACK_X, ST_EAST_PACK_Y, ST_EAST_PACK_Z,
                          ST_EAST_PACK_RADIUS, 0, 0, /*eventId*/ 18),
            MakeObjective(OBJ(19), 23, 109, "Muster before the Shade of Eranikus",
                          ST_ERANIKUS_X, ST_ERANIKUS_Y, ST_ERANIKUS_Z,
                          ST_MUSTER_ARRIVE, 0, 0, /*eventId*/ 19),
        };
        t.push_back(std::move(p));
    }
}
