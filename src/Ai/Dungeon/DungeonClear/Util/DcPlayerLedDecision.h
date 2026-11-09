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
        // The flagged player is alive. A dead Main Tank is no reference: healers
        // and regroup must not aim at a corpse (FindLeaderTank skips the dead too).
        bool playerAlive = false;
        // The flagged player is driven by a human, not by a PlayerbotAI (a player
        // who switches to self-bot leaves the mode; the server flag is not cleared
        // by that switch).
        bool playerHuman = false;
    };

    // The player leads: every DC member rung reads that player as the party tank.
    constexpr bool IsPlayerLed(Inputs const& in)
    {
        return in.inInstance && in.flaggedPlayerOnMap && !in.dcRunOwned && in.playerAlive && in.playerHuman;
    }

    // Where the party tank value comes from. A live, unpaused DC run always wins
    // (the On button: today's behaviour, untouched); player-led is only a
    // fallback when no run drives.
    enum class PartyTankSource
    {
        None,
        RunLeader,
        MainTankPlayer
    };
    constexpr PartyTankSource ChoosePartyTank(bool runLeaderActive, bool playerLedLeader)
    {
        return runLeaderActive ? PartyTankSource::RunLeader
             : playerLedLeader ? PartyTankSource::MainTankPlayer
                               : PartyTankSource::None;
    }

    // The DC rungs that DRIVE or read the tank's own AI, per party tank. Each one
    // is today's verdict for a bot tank (`tankIsHuman` false) and stands down for
    // a human Main Tank. Stock playerbots takes over: follow-master, rest, the
    // collision shuffle, and the player's own fights.
    struct TankRungs
    {
        bool dcFollowTank = false;        // DC follow-tank trails the tank
        bool suppressStockFollow = false; // stock follow-master is zeroed
        bool restCaps = false;            // Smart Rest latch / run rest caps
        bool collisionBan = false;        // stock "move out of collision" zeroed
        bool breakStuckCombat = false;    // phantom-combat breaker may clear combat
    };
    constexpr TankRungs RungsFor(bool tankPresent, bool tankIsSelf, bool tankIsHuman)
    {
        TankRungs r;
        bool const botTank = tankPresent && !tankIsHuman;
        r.dcFollowTank = botTank && !tankIsSelf;
        r.suppressStockFollow = botTank;
        r.restCaps = botTank;
        r.collisionBan = botTank;
        r.breakStuckCombat = botTank;
        return r;
    }

    // Member rungs that MOVE a bot toward the tank or put it on his fight (combat
    // regroup, DC assist, Lyceum close-up, Iron Hall hop, the room-boss chase
    // guard): a bot tank is the player's off-tank and is left to his orders.
    // With a bot tank driving (On), every member keeps today's verdict.
    constexpr bool MemberRungApplies(bool tankIsHuman, bool botIsTank)
    {
        return !(tankIsHuman && botIsTank);
    }

    // What DC hands the playerbots module as "the DC run leader of this member"
    // (CoaGroupTank provider), which decides whether the module's guards that DC
    // still runs itself stand down. DC disabled (DungeonClear.Enable = 0): never
    // anything — DC registers no values then and must not be read at all. A run
    // that exists counts even while PAUSED, so a pause hands the party back
    // exactly as in 2.9.4 instead of waking the module's copy of a guard DC
    // still runs (DC's own ground-hazard guard ignores pause).
    enum class ModuleProvide
    {
        Nothing,
        PartyTank,  // the live run's leader, or the player-led Main Tank
        RunOwner    // a run exists but drives nothing right now (paused / leader dead)
    };
    constexpr ModuleProvide ProvideToModule(bool dcEnabled, bool partyTankKnown, bool runExists)
    {
        return !dcEnabled ? ModuleProvide::Nothing
             : partyTankKnown ? ModuleProvide::PartyTank
             : runExists ? ModuleProvide::RunOwner
                         : ModuleProvide::Nothing;
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
