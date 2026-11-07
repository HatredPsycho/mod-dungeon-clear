/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcPlayerLed.h"
#include "DcPlayerLedDecision.h"
#include "DcLeaderSignal.h"
#include "DcRun.h"

#include <mutex>
#include <unordered_map>

#include "Group.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

// The Main Tank bar ships with the CoA playerbots module (2.9.4). On a module
// without it nobody can be flagged, and player-led mode simply never applies.
#if __has_include("CoaPlayerMainTank.h")
#include "CoaPlayerMainTank.h"
#define DC_HAS_PLAYER_MAIN_TANK 1
#else
#define DC_HAS_PLAYER_MAIN_TANK 0
#endif

namespace
{
    struct Stamps
    {
        uint32 combatSinceMs = 0;
        uint32 firstHitMs = 0;
    };

    // Per flagged player, keyed by raw GUID. Written from the map threads (combat
    // and damage hooks), read from the bots' ticks on the same map: guarded. A
    // few entries at most (one per human using the bar); Forget drops them.
    std::mutex g_stampsMutex;
    std::unordered_map<uint64, Stamps> g_stamps;

    bool IsFlagged(Player const* player)
    {
#if DC_HAS_PLAYER_MAIN_TANK
        return player && CoaPlayerMainTank::IsSet(player);
#else
        (void)player;
        return false;
#endif
    }

    uint32 NowNonZero()
    {
        uint32 const now = getMSTime();
        return now != 0 ? now : 1u;
    }
}

Player* DcPlayerLed::Leader(Player* bot)
{
#if DC_HAS_PLAYER_MAIN_TANK
    if (!bot)
        return nullptr;
    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    if (!group || !map)
        return nullptr;

    DcPlayerLedDecision::Inputs in;
    in.inInstance = map->IsDungeon();
    if (!in.inInstance)
        return nullptr;
    // Cheap first: the flag set carries an atomic count, so a realm where nobody
    // uses the bar pays one load here and never reaches the run-owner scan.
    Player* flagged = CoaPlayerMainTank::FlaggedInGroup(group, bot);
    in.flaggedPlayerOnMap = flagged != nullptr;
    if (!in.flaggedPlayerOnMap)
        return nullptr;
    // `flagged` is on the bot's own Map (FlaggedInGroup compares Map pointers),
    // so these reads stay on this map's thread.
    in.playerAlive = flagged->IsAlive();
    in.playerHuman = IsHumanTank(flagged);
    // Only the run owner IN THIS INSTANCE is seen (FindRunOwner reads members on
    // this very Map object only): a run whose bot tank left the instance drives
    // nothing here, which is what the followers already fall back from today.
    // No other map's state is read for it.
    in.dcRunOwned = DcLeaderSignal::FindRunOwner(bot) != nullptr;
    return DcPlayerLedDecision::IsPlayerLed(in) ? flagged : nullptr;
#else
    (void)bot;
    return nullptr;
#endif
}

Player* DcPlayerLed::FlaggedHuman(Player* bot)
{
#if DC_HAS_PLAYER_MAIN_TANK
    if (!bot || !bot->GetGroup() || !bot->GetMap())
        return nullptr;
    Player* flagged = CoaPlayerMainTank::FlaggedInGroup(bot->GetGroup(), bot);
    return IsHumanTank(flagged) ? flagged : nullptr;
#else
    (void)bot;
    return nullptr;
#endif
}

Player* DcPlayerLed::PartyTankFor(Player* member)
{
    if (!member)
        return nullptr;
    // The single elected leader for the whole group (party or raid): the bot
    // itself for the leader, the tank to trail for everyone else.
    Player* leader = DcLeaderSignal::FindLeaderTank(member);
    PlayerbotAI* leaderAI = leader ? GET_PLAYERBOT_AI(leader) : nullptr;
    // Expose the leader only while its clear is actively running and unpaused.
    // A paused/disabled leader returns null so followers stop following it and
    // revert to the player — matching the leader's own paused/off behavior.
    bool const runLeaderActive = leaderAI && DcRun::Of(leaderAI->GetAiObjectContext()).enabled &&
                                 !DcRun::Of(leaderAI->GetAiObjectContext()).paused;
    // No DC run drives the party (a bot tank of a raid is elected even with DC
    // off): a human tanking with the Main Tank bar is the party tank instead.
    // Leader() is null while any run is on, paused included; it is only asked
    // when no run drives, so a DC-driven party pays nothing for it.
    Player* const human = runLeaderActive ? nullptr : Leader(member);
    switch (DcPlayerLedDecision::ChoosePartyTank(runLeaderActive, human != nullptr))
    {
        case DcPlayerLedDecision::PartyTankSource::RunLeader:
            return leader;
        case DcPlayerLedDecision::PartyTankSource::MainTankPlayer:
            return human;
        default:
            return nullptr;
    }
}

bool DcPlayerLed::IsHumanTank(Player const* tank)
{
    return tank && !GET_PLAYERBOT_AI(const_cast<Player*>(tank));
}

void DcPlayerLed::NoteEnterCombat(Player* player)
{
    if (!IsFlagged(player))
        return;
    std::lock_guard<std::mutex> lock(g_stampsMutex);
    Stamps& s = g_stamps[player->GetGUID().GetRawValue()];
    s.combatSinceMs = NowNonZero();
    s.firstHitMs = 0;
}

void DcPlayerLed::NoteHit(Player* player)
{
    if (!player || !player->IsInCombat() || !IsFlagged(player))
        return;
    std::lock_guard<std::mutex> lock(g_stampsMutex);
    Stamps& s = g_stamps[player->GetGUID().GetRawValue()];
    if (s.combatSinceMs == 0)
        s.combatSinceMs = NowNonZero();  // the enter-combat edge was missed
    if (s.firstHitMs == 0 || s.firstHitMs < s.combatSinceMs)
        s.firstHitMs = NowNonZero();
}

void DcPlayerLed::CombatStamps(Player const* player, uint32& combatSinceMs, uint32& firstHitMs)
{
    combatSinceMs = 0;
    firstHitMs = 0;
    if (!player)
        return;
    uint64 const key = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_stampsMutex);
    auto it = g_stamps.find(key);
    if (it == g_stamps.end())
        return;
    // Out of combat or no longer flagged: the fight is over, nothing to hold on.
    if (!player->IsInCombat() || !IsFlagged(player))
    {
        g_stamps.erase(it);
        return;
    }
    combatSinceMs = it->second.combatSinceMs;
    firstHitMs = DcPlayerLedDecision::CurrentFirstHit(it->second.combatSinceMs, it->second.firstHitMs);
}

void DcPlayerLed::Forget(Player const* player)
{
    if (!player)
        return;
    std::lock_guard<std::mutex> lock(g_stampsMutex);
    g_stamps.erase(player->GetGUID().GetRawValue());
}
