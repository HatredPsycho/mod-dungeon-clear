/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_MODULE_COMBAT_H
#define _DC_MODULE_COMBAT_H

// The combat guards the playerbots module (SquidBot 3.0, CoA Combat) has rewritten.
// For each one, DC stops running its own copy when the module carries it, and keeps
// it on an older module, so the same DC builds against both.

// The reference-tank hook the CoA Combat guards read (DC names the run's tank).
#if __has_include("CoaCombatGroupTank.h")
#define DC_HAS_COA_COMBAT 1
#else
#define DC_HAS_COA_COMBAT 0
#endif

// The module's own list of the guards it owns (one marker per guard). Before it
// existed (A1 only, coa-3.0 b9dd6106), the rewritten A1 header was the sign.
#if __has_include("CoaCombatOwnedGuards.h")
#include "CoaCombatOwnedGuards.h"
#endif

// A1, escape leap ban (Blink / Disengage in a group in an instance).
#if defined(COA_COMBAT_OWNS_ESCAPE_LEAP_BAN) || __has_include("CoaEscapeLeapRule.h")
#define DC_MODULE_OWNS_ESCAPE_LEAP 1
#else
#define DC_MODULE_OWNS_ESCAPE_LEAP 0
#endif

// A2, room boss chase (no chase or leap that ends inside a sleeping room-aggro
// boss's sphere). DC hands the module the boss through SetRoomClearBoss.
#if defined(COA_COMBAT_OWNS_ROOM_BOSS_CHASE)
#define DC_MODULE_OWNS_ROOM_BOSS_CHASE 1
#else
#define DC_MODULE_OWNS_ROOM_BOSS_CHASE 0
#endif

// A3, BRD Lyceum close-up (every member but the tank stays near the tank in
// Lyceum fights, and no stock chase away from it).
#if defined(COA_COMBAT_OWNS_LYCEUM_CLOSE_UP)
#define DC_MODULE_OWNS_LYCEUM_CLOSE_UP 1
#else
#define DC_MODULE_OWNS_LYCEUM_CLOSE_UP 0
#endif

// A4, BRD Iron Hall (never stop in the Ironhand Guardians' fire: hop to the
// next fire-free stop).
#if defined(COA_COMBAT_OWNS_IRON_HALL)
#define DC_MODULE_OWNS_IRON_HALL 1
#else
#define DC_MODULE_OWNS_IRON_HALL 0
#endif

// A5, ground hazards (walk out of an unfightable hazard's pulse). The route
// half (camps and regroup points kept off the hazards) stays in DC.
#if defined(COA_COMBAT_OWNS_GROUND_HAZARDS)
#define DC_MODULE_OWNS_GROUND_HAZARDS 1
#else
#define DC_MODULE_OWNS_GROUND_HAZARDS 0
#endif

// A7, threat lead (no DPS or healer damage before the lead after the tank's
// first hit).
#if defined(COA_COMBAT_OWNS_THREAT_LEAD)
#define DC_MODULE_OWNS_THREAT_LEAD 1
#else
#define DC_MODULE_OWNS_THREAT_LEAD 0
#endif

// A8, tank fight assist (a DPS the tank's fight never reached is put on it).
// Only the ordinary fight leaves DC: the scripted camp fight's seed and the
// leader's LOS-break drag guard stay here.
#if defined(COA_COMBAT_OWNS_TANK_FIGHT_ASSIST)
#define DC_MODULE_OWNS_TANK_FIGHT_ASSIST 1
#else
#define DC_MODULE_OWNS_TANK_FIGHT_ASSIST 0
#endif

// A11, top off party (a healer tops the party off between pulls instead of
// waiting for food).
#if defined(COA_COMBAT_OWNS_TOP_OFF_PARTY)
#define DC_MODULE_OWNS_TOP_OFF_PARTY 1
#else
#define DC_MODULE_OWNS_TOP_OFF_PARTY 0
#endif

// A9, combat regroup (a follower that cannot contribute from where it stands
// walks to a role standoff point). DC gives the module its passive camp hold
// through SetCampHoldPassive.
#if defined(COA_COMBAT_OWNS_COMBAT_REGROUP)
#define DC_MODULE_OWNS_COMBAT_REGROUP 1
#else
#define DC_MODULE_OWNS_COMBAT_REGROUP 0
#endif

// A6, target exclusion and hold fire (BWL's out-of-order bosses and
// Razorgore). DC gives the module its combat purge bars and `dc skip` through
// CoaTargetExclusion::SetRunHooks.
#if defined(COA_COMBAT_OWNS_TARGET_EXCLUSION)
#define DC_MODULE_OWNS_TARGET_EXCLUSION 1
#else
#define DC_MODULE_OWNS_TARGET_EXCLUSION 0
#endif

// A10, heal reposition (a healer walks to see a hurt member out of its LOS)
// and the escortee healed like a member. DC's escort provider
// (CoaHealEscort::SetProvider) is wired with the module's guards.
#if defined(COA_COMBAT_OWNS_HEAL_REPOSITION)
#define DC_MODULE_OWNS_HEAL_REPOSITION 1
#else
#define DC_MODULE_OWNS_HEAL_REPOSITION 0
#endif

// A12, raid boss stand-down (the raid strategy owns the encounter). DC reads
// the module's verdict (CoaBossStandDown::IsActive) and gives it the run's boss
// roster through SetRosterBossProvider.
#if defined(COA_COMBAT_OWNS_BOSS_STAND_DOWN)
#define DC_MODULE_OWNS_BOSS_STAND_DOWN 1
#else
#define DC_MODULE_OWNS_BOSS_STAND_DOWN 0
#endif

#endif  // _DC_MODULE_COMBAT_H
