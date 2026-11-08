/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_BREATH_GUARD_H
#define _DC_BREATH_GUARD_H

#include "Define.h"
#include "Ai/Dungeon/DungeonClear/Util/DcModuleCombat.h"

// The module's CoA Combat B2 ticks the guard itself when it carries it; DC's
// party gate names DcBreath, the module's CoaBreathGuard or DC's DcBreathGuard.
#if DC_MODULE_OWNS_BREATH_GUARD
#include "CoaBreathGuard.h"
namespace DcBreath = CoaBreathGuard;
#else

class Player;

// Breath. Nothing else in the module knows a bot can drown: the party gate,
// the rests and the "Holding near" parks all happily keep a party under water
// until the 180 s breath bar runs out, and on Blackfathom Deeps that was every
// run (members dying with no opponent every ~190 s in the flooded passage to
// Lady Sarevess, and in the pool under Kelris).
//
// Two halves:
//   * AnyMemberUnderWater: the gates ask it, so a party is never held under
//     water waiting for a straggler or a rest;
//   * Tick (world thread, every sweep): a bot of a running clear that has been
//     under water, out of combat, for longer than half its breath is brought
//     back up beside its leader, or to the leader's last dry spot when the
//     leader is under water too.
namespace DcBreathGuard
{
    bool AnyMemberUnderWater(Player* bot);
    void Tick(uint32 diffMs);
}

namespace DcBreath = DcBreathGuard;
#endif  // DC_MODULE_OWNS_BREATH_GUARD

#endif
