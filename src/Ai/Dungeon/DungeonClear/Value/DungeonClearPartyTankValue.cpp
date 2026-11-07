/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearPartyTankValue.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPlayerLed.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPlayerLedDecision.h"

#include "Player.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

Player* DungeonClearPartyTankValue::Calculate()
{
    if (!bot)
        return nullptr;

    // The single elected leader for the whole group (party or raid). For the
    // leader bot itself this resolves to `bot`; for every follower — non-tanks
    // and non-leader (off-)tanks alike — it is the tank they should trail.
    Player* leader = DcLeaderSignal::FindLeaderTank(bot);
    PlayerbotAI* leaderAI = leader ? GET_PLAYERBOT_AI(leader) : nullptr;

    // Expose the leader only while its clear is actively running and unpaused.
    // A paused/disabled leader returns null so followers stop following it and
    // revert to the player — matching the leader's own paused/off behavior.
    bool const runLeaderActive = leaderAI && DcRun::Of(leaderAI->GetAiObjectContext()).enabled &&
                                 !DcRun::Of(leaderAI->GetAiObjectContext()).paused;
    // No DC run drives the party (a bot tank of a raid is elected even with DC
    // off): a human tanking with the Main Tank bar is the party tank instead.
    // DcPlayerLed returns null while any run is on, paused included; it is only
    // asked when no run drives, so a DC-driven party pays nothing for it.
    Player* const human = runLeaderActive ? nullptr : DcPlayerLed::Leader(bot);
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
