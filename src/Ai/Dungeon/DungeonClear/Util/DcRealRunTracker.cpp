/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Util/DcRealRunTracker.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "AiFactory.h"
#include "Config.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPlayerbotCompat.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRealRunLog.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRunWing.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "TestRun/DcTestDungeonRegistry.h"
#include "TestRun/DcTestRunManager.h"
#include "Util/DcBotProvisioning.h"

namespace DcRealRunTracker
{
    namespace
    {
        struct RosterRef
        {
            uint32 entry = 0;
            uint32 encounterIndex = 0;
            std::string name;
            bool isBoss = false;
        };

        struct Live
        {
            DcRealRunLog::Record rec;
            std::vector<RosterRef> roster;
            uint32 rosterBits = 0;   // encounter bits some roster entry owns
            uint32 lastMask = 0;     // baseline: kills before `dc on` are not this run's
            std::size_t lastAnchors = 0;
            bool endedByDeath = false;
        };

        std::mutex g_mutex;
        std::unordered_map<ObjectGuid, Live> g_live;
        uint32 g_tickAccumMs = 0;
        constexpr uint32 kSampleMs = 1000;

        uint64 NowUnixMs()
        {
            using namespace std::chrono;
            return static_cast<uint64>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
        }

        // Same row choice as DcRezRecovery::RegroupAtEntrance: the run's wing row
        // on a split map, else the first non-scenario row for the map.
        DcTestDungeonRegistry::Row const* RowFor(Player* owner, uint32 mapId)
        {
            std::string const wingToken = DcRunWing::ActiveWingToken(owner);
            DcTestDungeonRegistry::Row const* row = nullptr;
            for (DcTestDungeonRegistry::Row const& r : DcTestDungeonRegistry::All())
            {
                if (r.mapId != mapId || DcTestDungeonRegistry::IsScenario(r))
                    continue;
                if (!wingToken.empty() && wingToken == r.token)
                    return &r;
                if (!row)
                    row = &r;
            }
            return row;
        }

        DcRealRunLog::CompEntry CompOf(Player* m)
        {
            DcRealRunLog::CompEntry c;
            c.name = m->GetName();
            c.className = DcBotProvisioning::ClassToken(m->getClass());
            c.spec = AiFactory::GetPlayerSpecName(m);
            c.role = PlayerbotAI::IsTank(m, /*bySpec*/ true)   ? "tank"
                     : PlayerbotAI::IsHeal(m, /*bySpec*/ true) ? "heal"
                                                              : "dps";
            return c;
        }

        // Fold the kills credited since the last sample into the timeline — the
        // bench's two sources (DcTestRunJob::TickMonitoring): encounter-mask bits,
        // named after the roster entry owning them (boss first), and cleared
        // anchors. The mask is read only while the owner stands in the run's
        // instance; outside it reads 0 and the last value is kept.
        void Sample(Player* owner, PlayerbotAI* ai, Live& live, uint64 nowMs)
        {
            uint32 const t = nowMs > live.rec.startedAtMs
                                 ? static_cast<uint32>((nowMs - live.rec.startedAtMs) / 1000)
                                 : 0u;
            bool progressed = false;
            Map* map = owner->GetMap();
            if (map && map->IsDungeon() && map->GetInstanceId() == live.rec.instance)
            {
                uint32 const mask = DcTargeting::CompletedEncounterMask(owner);
                if (uint32 const fresh = mask & ~live.lastMask & live.rosterBits)
                {
                    for (uint32 bit = 0; bit < 32; ++bit)
                    {
                        if (!(fresh & (1u << bit)))
                            continue;
                        DcTestRunRecord::BossKill kill;
                        kill.t = t;
                        kill.via = "mask";
                        for (RosterRef const& ref : live.roster)
                            if (ref.encounterIndex == bit && (ref.isBoss || kill.name.empty()))
                            {
                                kill.entry = ref.entry;
                                kill.name = ref.name;
                                if (ref.isBoss)
                                    break;
                            }
                        if (kill.name.empty())
                            kill.name = "encounter #" + std::to_string(bit);
                        live.rec.bossTimeline.push_back(kill);
                        progressed = true;
                    }
                }
                live.lastMask = mask;
            }
            if (ai)
            {
                std::size_t const anchors = ai->GetAiObjectContext()
                                                ->GetValue<std::unordered_set<uint32>&>(DcKey::ClearedAnchors)
                                                ->Get()
                                                .size();
                if (anchors > live.lastAnchors)
                {
                    for (std::size_t i = live.lastAnchors; i < anchors; ++i)
                    {
                        DcTestRunRecord::BossKill kill;
                        kill.t = t;
                        kill.name = "objective";
                        kill.via = "anchor";
                        live.rec.bossTimeline.push_back(kill);
                    }
                    live.lastAnchors = anchors;
                    progressed = true;
                }
            }
            if (progressed)
                live.rec.bossesKilled = std::min<uint32>(static_cast<uint32>(live.rec.bossTimeline.size()),
                                                         live.rec.bossesTotal);
        }

        DcRealRunLog::Record Close(Live& live, bool allCleared, uint64 nowMs)
        {
            DcRealRunLog::Record rec = std::move(live.rec);
            rec.endedAtMs = nowMs;
            rec.durationS = nowMs > rec.startedAtMs ? static_cast<uint32>((nowMs - rec.startedAtMs) / 1000) : 0u;
            rec.result = DcRealRunLog::ResultFor(allCleared, live.endedByDeath, rec.bossesKilled, rec.bossesTotal);
            return rec;
        }

        // File I/O outside g_mutex. AiPlayerbot.CoaDataFiles = 0 turns every
        // dashboard file off; AiPlayerbot.CoaDataDir "" = <LogsDir>/squidbot.
        void Write(DcRealRunLog::Record const& rec)
        {
            if (!sConfigMgr->GetOption<bool>("AiPlayerbot.CoaDataFiles", true, false))
                return;
            std::string const dir = DcRealRunLog::ResolveDir(
                sConfigMgr->GetOption<std::string>("AiPlayerbot.CoaDataDir", "", false), sLog->GetLogsDir());
            if (!DcRealRunLog::AppendLine(dir, DcRealRunLog::ToJsonl(rec)))
                LOG_WARN("playerbots.dungeonclear", "[DC] real run {}: could not write {}/runs.jsonl", rec.runId,
                         dir);
            else
                LOG_INFO("playerbots.dungeonclear", "[DC] real run {} {} ({}): {} {}/{} in {}s", rec.runId,
                         rec.dungeon, rec.instance, rec.result, rec.bossesKilled, rec.bossesTotal, rec.durationS);
        }
    }

    void Begin(PlayerbotAI* ownerAI)
    {
        Player* owner = ownerAI ? ownerAI->GetBot() : nullptr;
        if (!owner)
            return;
        // A `.dc test` or lab party is the bench's, which writes its own record.
        if (DcTestRunManager::Instance().IsRunMember(owner->GetGUID()))
            return;
        Map* map = owner->GetMap();
        if (!map || !map->IsDungeon() || !map->GetInstanceId())
            return;

        uint64 const nowMs = NowUnixMs();
        Live live;
        live.rec.instance = map->GetInstanceId();
        live.rec.runId = DcRealRunLog::MakeRunId(static_cast<std::time_t>(nowMs / 1000), live.rec.instance);
        if (DcTestDungeonRegistry::Row const* row = RowFor(owner, map->GetId()))
        {
            live.rec.dungeon = row->token;
            live.rec.dungeonName = row->name;
        }
        else
        {
            live.rec.dungeon = "map" + std::to_string(map->GetId());
            live.rec.dungeonName = map->GetMapName();
        }
        // The bench stamps the group's dungeon or raid difficulty; the instance
        // carries the same value as its spawn mode (0 normal, 1 heroic, 2 mythic).
        live.rec.difficulty = static_cast<uint32>(map->GetDifficulty());
        live.rec.startedAtMs = nowMs;

        std::vector<DungeonBossInfo> const& bosses =
            ownerAI->GetAiObjectContext()->GetValue<std::vector<DungeonBossInfo>>(DcKey::DungeonBosses)->Get();
        for (DungeonBossInfo const& b : bosses)
        {
            RosterRef ref;
            ref.entry = b.entry;
            ref.encounterIndex = b.encounterIndex;
            ref.name = b.name;
            ref.isBoss = b.kind == DungeonAnchorKind::Boss;
            if (ref.encounterIndex < 32)
                live.rosterBits |= 1u << ref.encounterIndex;
            live.roster.push_back(std::move(ref));
        }
        live.rec.bossesTotal = static_cast<uint32>(live.roster.size());
        live.lastMask = DcTargeting::CompletedEncounterMask(owner);

        auto addMember = [&live](Player* m)
        {
            live.rec.comp.push_back(CompOf(m));
            if (DcPlayerbotCompat::IsHumanControlled(m))
                live.rec.players.push_back(m->GetName());
        };
        if (Group* group = owner->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource(); m && m->IsInWorld() && m->GetMap() == map)
                    addMember(m);
        }
        else
            addMember(owner);

        std::vector<DcRealRunLog::Record> done;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_live.find(owner->GetGUID());
            if (it != g_live.end())
            {
                // `dc on` again on the run already followed: keep it.
                if (it->second.rec.instance == live.rec.instance)
                    return;
                done.push_back(Close(it->second, false, nowMs));
                g_live.erase(it);
            }
            g_live.emplace(owner->GetGUID(), std::move(live));
        }
        for (DcRealRunLog::Record const& rec : done)
            Write(rec);
    }

    void NoteWipe(Player* owner)
    {
        if (!owner)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_live.find(owner->GetGUID());
        if (it != g_live.end())
            ++it->second.rec.wipes;
    }

    void NoteDeathEnd(Player* owner)
    {
        if (!owner)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_live.find(owner->GetGUID());
        if (it != g_live.end())
            it->second.endedByDeath = true;
    }

    void End(PlayerbotAI* ownerAI, std::string const& reason)
    {
        Player* owner = ownerAI ? ownerAI->GetBot() : nullptr;
        if (!owner)
            return;
        uint64 const nowMs = NowUnixMs();
        DcRealRunLog::Record rec;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_live.find(owner->GetGUID());
            if (it == g_live.end())
                return;
            // Last sample first: the kill that triggered the all-cleared rung can
            // land inside the same second.
            Sample(owner, ownerAI, it->second, nowMs);
            rec = Close(it->second, reason == DcActionShared::kReasonAllCleared, nowMs);
            g_live.erase(it);
        }
        Write(rec);
    }

    void Tick(uint32 diff)
    {
        g_tickAccumMs += diff;
        if (g_tickAccumMs < kSampleMs)
            return;
        g_tickAccumMs = 0;

        uint64 const nowMs = NowUnixMs();
        std::vector<DcRealRunLog::Record> done;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto it = g_live.begin(); it != g_live.end();)
            {
                Player* owner = ObjectAccessor::FindPlayer(it->first);
                PlayerbotAI* ai = owner && owner->IsInWorld() ? GET_PLAYERBOT_AI(owner) : nullptr;
                // Leader gone (logout, instance reset) or the run switched off
                // without the disable funnel: close it as it stands.
                if (!ai || !DcRun::Of(ai).enabled)
                {
                    done.push_back(Close(it->second, false, nowMs));
                    it = g_live.erase(it);
                    continue;
                }
                Sample(owner, ai, it->second, nowMs);
                ++it;
            }
        }
        for (DcRealRunLog::Record const& rec : done)
            Write(rec);
    }
}
