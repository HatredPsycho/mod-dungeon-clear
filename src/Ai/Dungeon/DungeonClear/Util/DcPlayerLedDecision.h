/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPLAYERLEDDECISION_H
#define _PLAYERBOT_DCPLAYERLEDDECISION_H

#include <cstdint>

// Pure decision kernel for PLAYER-LED mode: a human tanks with the SquidBots
// "Main Tank" bar ("tank main on", CoaPlayerMainTank) and leads the party
// himself, while the bots keep DC's member-side behaviour (healer placement,
// threat lead, combat regroup, rest, encounter mechanics). DcPlayerLed (the
// glue) gathers the live facts and asks this; nothing here touches the engine.
namespace DcPlayerLedDecision
{
    struct Inputs
    {
        // The bot stands in a dungeon or raid instance.
        bool inInstance = false;
        // A group member on the bot's map has the Main Tank flag.
        bool flaggedPlayerOnMap = false;
        // A DC run is switched on in the group (a bot tank owns it, paused or
        // not). DC driving always wins: the Main Tank bar and DC exclude each
        // other, and a run means the party follows its elected bot tank.
        bool dcRunOwned = false;
    };

    // The player leads: every DC member rung reads that player as the party tank.
    constexpr bool IsPlayerLed(Inputs const& in)
    {
        return in.inInstance && in.flaggedPlayerOnMap && !in.dcRunOwned;
    }

    // Whether a DC member rung that trails or regroups on the party tank applies
    // to this bot when the tank is a human. Bot tanks are the player's off-tanks:
    // their place is the one the player gave them (CoaTankAction), never the
    // player's side, so DC must not move them.
    constexpr bool MayRegroupOnPlayer(bool botIsTank)
    {
        return !botIsTank;
    }

    // Threat-lead clock on the player's side, kept per player. `combatSinceMs`
    // is the player's 0->1 combat stamp, `firstHitMs` his first damage on a
    // non-player since then (0 = none yet). Returns the first-hit stamp the
    // threat-lead check must use: a stamp older than the current fight is stale.
    constexpr std::uint32_t CurrentFirstHit(std::uint32_t combatSinceMs, std::uint32_t firstHitMs)
    {
        return (firstHitMs != 0 && firstHitMs < combatSinceMs) ? 0u : firstHitMs;
    }
}

#endif
