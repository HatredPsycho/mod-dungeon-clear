/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Util/DcBreathGuard.h"

#if !DC_MODULE_OWNS_BREATH_GUARD

#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"

#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Position.h"

#include <unordered_map>

namespace
{
    // Half the 180 s breath bar: enough to swim a flooded corridor, short of
    // the point where the bar is gone.
    constexpr uint32 UNDER_WATER_LIMIT_MS = 90 * 1000;

    // World-thread only (Tick runs from the world OnUpdate).
    std::unordered_map<ObjectGuid, uint32> g_underWaterMs;
    std::unordered_map<ObjectGuid, Position> g_lastDryLeaderPos;

    bool InRunningClear(Player* bot, Player*& leader)
    {
        leader = DcLeaderSignal::FindRunOwner(bot);
        PlayerbotAI* leaderAI = leader ? GET_PLAYERBOT_AI(leader) : nullptr;
        return leaderAI && DcRun::Of(leaderAI).enabled;
    }
}

namespace DcBreathGuard
{
    bool AnyMemberUnderWater(Player* bot)
    {
        if (!bot)
            return false;
        if (bot->IsUnderWater())
            return true;
        Group* group = bot->GetGroup();
        if (!group)
            return false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource();
                member && member != bot && member->IsInWorld() && member->IsAlive() &&
                member->GetMapId() == bot->GetMapId() && member->IsUnderWater())
                return true;
        return false;
    }

    void Tick(uint32 diffMs)
    {
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (!player || !player->IsInWorld() || !player->IsAlive() || !GET_PLAYERBOT_AI(player))
                continue;
            Map* map = player->GetMap();
            if (!map || !map->IsDungeon())
            {
                g_underWaterMs.erase(guid);
                continue;
            }

            Player* leader = nullptr;
            if (!InRunningClear(player, leader) || !leader)
            {
                g_underWaterMs.erase(guid);
                continue;
            }

            if (player == leader && !player->IsUnderWater())
                g_lastDryLeaderPos[leader->GetGUID()] = player->GetPosition();

            if (!player->IsUnderWater() || player->IsInCombat())
            {
                g_underWaterMs.erase(guid);
                continue;
            }

            uint32& ms = g_underWaterMs[guid];
            ms += diffMs;
            if (ms < UNDER_WATER_LIMIT_MS)
                continue;
            ms = 0;

            Position dest;
            if (leader != player && leader->IsInWorld() && leader->GetMapId() == player->GetMapId() &&
                !leader->IsUnderWater())
                dest = leader->GetPosition();
            else if (auto itr = g_lastDryLeaderPos.find(leader->GetGUID()); itr != g_lastDryLeaderPos.end())
                dest = itr->second;
            else
                continue;

            player->GetMotionMaster()->Clear();
            player->NearTeleportTo(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
                                   player->GetOrientation());
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] breath-guard: {}s under water out of combat -> brought up beside {}",
                     leader->GetName(), UNDER_WATER_LIMIT_MS / 1000, player->GetName());
        }
    }
}
#endif  // !DC_MODULE_OWNS_BREATH_GUARD
