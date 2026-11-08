/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcPetStance.h"

#include "DungeonClearUtil.h"   // DC_PULL_* log macros
#include "CharmInfo.h"
#include "Creature.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PetDefines.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "Timer.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr uint32 kTickMs = 1000;
    constexpr float kRecallYards = 30.0f;

    // Map threads update players; each bot is always updated by the same map's
    // thread while it stays on that map, so thread_local state is per-bot safe.
    thread_local std::unordered_map<ObjectGuid::LowType, uint32> lastTick;
    thread_local std::unordered_set<ObjectGuid::LowType> heldDefensive;

    void SetReact(Creature* c, ReactStates react)
    {
        c->SetReactState(react);
        if (CharmInfo* ci = c->GetCharmInfo())
            ci->SetPlayerReactState(react);
    }
}

void DcPetStance::Tick(Player* player)
{
    if (!player || player->m_Controlled.empty())
        return;

    uint32 const now = getMSTime();
    uint32& last = lastTick[player->GetGUID().GetCounter()];
    if (last && getMSTimeDiff(last, now) < kTickMs)
        return;
    last = now;
    // Bounded: ids of despawned summons pile up. A live pet dropped by the clear stays
    // defensive, which still assists its owner under PetAI.
    if (heldDefensive.size() > 4096)
        heldDefensive.clear();

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI || !botAI->HasStrategy("dungeon clear", BOT_STATE_NON_COMBAT))
        return;
    Map* map = player->GetMap();
    if (!map || !map->IsDungeon())
        return;

    std::vector<Creature*> units;
    for (Unit* u : player->m_Controlled)
        if (Creature* c = u ? u->ToCreature() : nullptr)
            if (c->IsAlive() && c->IsInWorld() && !c->IsTotem() && c->GetMapId() == player->GetMapId())
                units.push_back(c);

    bool const ownerFighting = player->IsInCombat();
    for (Creature* c : units)
    {
        ObjectGuid::LowType const id = c->GetGUID().GetCounter();
        if (ownerFighting)
        {
            if (heldDefensive.erase(id) && c->HasReactState(REACT_DEFENSIVE))
                SetReact(c, REACT_AGGRESSIVE);
            continue;
        }

        if (c->HasReactState(REACT_AGGRESSIVE))
        {
            SetReact(c, REACT_DEFENSIVE);
            heldDefensive.insert(id);
            DC_PULL_DEBUG("[DC:{}] pet stance: {} (entry {}) held defensive out of combat",
                          player->GetName(), c->GetName(), c->GetEntry());
        }

        if (!c->IsInCombat() && c->GetExactDist(player) > kRecallYards)
        {
            if (CharmInfo* ci = c->GetCharmInfo())
                ci->SetCommandState(COMMAND_FOLLOW);
            c->AttackStop();
            c->GetMotionMaster()->MoveFollow(player, PET_FOLLOW_DIST, c->GetFollowAngle());
            DC_PULL_DEBUG("[DC:{}] pet stance: {} (entry {}) recalled from {:.1f}yd",
                          player->GetName(), c->GetName(), c->GetEntry(), c->GetExactDist(player));
        }
    }
}
