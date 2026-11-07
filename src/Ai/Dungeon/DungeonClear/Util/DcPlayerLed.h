/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPLAYERLED_H
#define _PLAYERBOT_DCPLAYERLED_H

#include "Define.h"

class Player;

// PLAYER-LED mode (2.9.4): a human tanks with the SquidBots "Main Tank" bar and
// leads. There is no DC run — the addon switches DC off when the bar opens — so
// DC drives nothing: no route, no pull, no advance, no assist or objective
// target, no watchdog, and nothing overrides the player's off-tank orders
// (CoaTankAction). What DC keeps is its MEMBER side, which reads the party tank
// value: that value resolves to the player here, so healers place on him, DPS
// wait out his threat lead, followers out of range regroup on him in combat,
// raid encounters stand DC down, and encounter-gated mechanics keep running.
//
// The signal is the server's own Main Tank flag (CoaPlayerMainTank), not a new
// command: "tank main on" enters the mode, "tank main off" or `dc on` leaves it.
// See DcPlayerLedDecision for the rule.
namespace DcPlayerLed
{
    // The Main Tank player `bot` follows, or nullptr when the group is not
    // player-led (no flagged player on the map, not an instance, or a DC run on).
    Player* Leader(Player* bot);

    // True when `tank` (a party-tank value) is a human leading in this mode.
    // Every DC rung that would drive or read the tank's own AI checks this.
    bool IsHumanTank(Player const* tank);

    // Threat lead, the player's side (DcLeaderSignal feeds and reads these).
    // NoteEnterCombat: the player's 0->1 combat edge. NoteHit: his damage on a
    // non-player. Both are no-ops unless `player` has the Main Tank flag.
    void NoteEnterCombat(Player* player);
    void NoteHit(Player* player);
    // The player's current fight: combat start and first hit (0 = none yet).
    // Both 0 out of combat. Stale stamps from an earlier fight read as 0.
    void CombatStamps(Player const* player, uint32& combatSinceMs, uint32& firstHitMs);

    // Drop everything held for this player (logout, map change). A Main Tank
    // who turns the flag off is dropped on the next read (CombatStamps).
    void Forget(Player const* player);
}

#endif
