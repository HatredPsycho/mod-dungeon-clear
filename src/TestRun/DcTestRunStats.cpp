#include "TestRun/DcTestRunStats.h"

#include "AllSpellScript.h"
#include "Creature.h"
#include "DynamicObject.h"
#include "Spell.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "Unit.h"
#include "UnitScript.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace
{
    std::mutex g_mutex;
    std::unordered_map<uint32, DcTestRunStats::Counters> g_tracked;
    std::unordered_map<uint32, std::deque<DcTestRunStats::Hit>> g_hits;  // tracked members only
    // instanceId -> (entry << 32 | spell) -> counts; only watched instances.
    std::unordered_map<uint32, std::unordered_map<uint64, DcTestRunStats::BossCast>> g_bossCasts;
    // Watched instances, readable without the lock: the cast hooks fire for every
    // spell of every unit (tens of thousands a second at 3000 bots) and must cost
    // one relaxed load when no test run is live.
    std::atomic<uint32> g_watchedInstances{0};

    void NoteBossCast(Unit* caster, SpellInfo const* spellInfo, bool interrupted)
    {
        if (g_watchedInstances.load(std::memory_order_relaxed) == 0 || !caster || !spellInfo)
            return;
        Creature* boss = caster->ToCreature();
        if (!boss || (!boss->IsDungeonBoss() && !boss->isWorldBoss()) || !boss->GetMap())
            return;
        uint32 const instanceId = boss->GetMap()->GetInstanceId();
        std::lock_guard<std::mutex> lock(g_mutex);
        auto inst = g_bossCasts.find(instanceId);
        if (inst == g_bossCasts.end())
            return;
        uint64 const key = (uint64(boss->GetEntry()) << 32) | spellInfo->Id;
        DcTestRunStats::BossCast& c = inst->second[key];
        if (!c.entry)
        {
            c.entry = boss->GetEntry();
            c.boss = boss->GetName();
            c.spellId = spellInfo->Id;
            c.spellName = spellInfo->SpellName[LOCALE_enUS];
        }
        ++(interrupted ? c.interrupted : c.cast);
    }
    struct SeenMarker { float x; float y; uint32 ms; };
    std::unordered_map<uint64, SeenMarker> g_lastMarker;  // attacker raw guid -> last marker seen

    // A creature's hit on a tracked member, kept as one of its last kHitLog.
    void NoteHit(Unit* target, Unit* attacker, uint32 damage, SpellInfo const* spellInfo)
    {
        if (!damage || !target || !target->IsPlayer() || !attacker ||
            attacker->GetCharmerOrOwnerPlayerOrPlayerItself())
            return;
        uint32 const low = target->GetGUID().GetCounter();
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_tracked.find(low) == g_tracked.end())
            return;
        DcTestRunStats::Hit hit;
        hit.msTime = getMSTime();
        hit.spellId = spellInfo ? spellInfo->Id : 0;
        hit.spellName = spellInfo ? spellInfo->SpellName[LOCALE_enUS] : "melee";
        hit.attackerEntry = attacker->GetEntry();
        hit.attackerName = attacker->GetName();
        if (Unit* victim = attacker->GetVictim())
            hit.attackerTarget = victim->GetName();
        hit.amount = damage;
        hit.hpBefore = target->GetHealth();
        hit.victimX = target->GetPositionX();
        hit.victimY = target->GetPositionY();
        hit.attackerX = attacker->GetPositionX();
        hit.attackerY = attacker->GetPositionY();
        // Ground markers the attacker owns right now. A short fixed list: the
        // core exposes a unit's dynamic objects by spell id only.
        static constexpr uint32 kMarkerSpells[] = { 2102735 };  // Berserker Leap Dest Area
        for (uint32 const markerSpell : kMarkerSpells)
            if (DynamicObject* marker = attacker->GetDynObject(markerSpell))
                hit.markers.emplace_back(marker->GetPositionX(), marker->GetPositionY());
        uint64 const attackerKey = attacker->GetGUID().GetRawValue();
        if (!hit.markers.empty())
            g_lastMarker[attackerKey] = SeenMarker{ hit.markers.back().first, hit.markers.back().second, hit.msTime };
        if (auto seen = g_lastMarker.find(attackerKey); seen != g_lastMarker.end())
        {
            if (getMSTimeDiff(seen->second.ms, hit.msTime) > 60000)
                g_lastMarker.erase(seen);  // stale: another pull, not this leap
            else
            {
                hit.hasLastMarker = true;
                hit.lastMarkerX = seen->second.x;
                hit.lastMarkerY = seen->second.y;
                hit.lastMarkerMs = seen->second.ms;
            }
        }
        std::deque<DcTestRunStats::Hit>& log = g_hits[low];
        log.push_back(std::move(hit));
        while (log.size() > DcTestRunStats::kHitLog)
            log.pop_front();
    }

    // Absorb shields cast by a tracked member: the core has no absorb hook, so
    // the soaked part is read off the shield's amount, which the core lowers as
    // it absorbs. Settled when the shield is recalculated (a refresh) or goes.
    // The amount hook runs before the core's last scaling (CoA absorb mods,
    // stacks), so the full amount is read when the shield lands and that scale
    // is kept for its refreshes.
    struct Shield
    {
        uint32 caster;
        uint32 spellId;
        int32 amount;
        int32 calculated;
        float scale = 1.0f;
    };
    std::unordered_map<AuraEffect const*, Shield> g_shields;

    void SettleShield(AuraEffect const* effect, bool forget)
    {
        auto itr = g_shields.find(effect);
        if (itr == g_shields.end())
            return;
        if (itr->second.spellId == effect->GetId())
        {
            int32 const soaked = itr->second.amount - std::max(effect->GetAmount(), 0);
            if (soaked > 0)
                if (auto member = g_tracked.find(itr->second.caster); member != g_tracked.end())
                    member->second.absorbDone += uint32(soaked);
        }
        if (forget)
            g_shields.erase(itr);
    }

    // A pet's or a totem's hits belong to the character that owns it.
    uint32 OwnerGuidLow(Unit* unit)
    {
        if (!unit)
            return 0;
        Player* player = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
        return player ? player->GetGUID().GetCounter() : 0;
    }
}

namespace DcTestRunStats
{
    void WatchInstance(uint32 instanceId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto [itr, added] = g_bossCasts.try_emplace(instanceId);
        itr->second.clear();
        if (added)
            g_watchedInstances.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<BossCast> ReleaseInstance(uint32 instanceId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<BossCast> out;
        if (auto itr = g_bossCasts.find(instanceId); itr != g_bossCasts.end())
        {
            for (auto const& [key, c] : itr->second)
                out.push_back(c);
            g_bossCasts.erase(itr);
            g_watchedInstances.fetch_sub(1, std::memory_order_relaxed);
        }
        return out;
    }

    void Track(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_tracked[guidLow] = Counters{};
        g_hits.erase(guidLow);
    }

    std::vector<Hit> LastHits(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto itr = g_hits.find(guidLow);
        if (itr == g_hits.end())
            return {};
        return std::vector<Hit>(itr->second.begin(), itr->second.end());
    }

    Counters Release(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto itr = g_tracked.find(guidLow);
        if (itr == g_tracked.end())
            return {};
        Counters const out = itr->second;
        g_tracked.erase(itr);
        g_hits.erase(guidLow);
        for (auto shield = g_shields.begin(); shield != g_shields.end();)
            shield = shield->second.caster == guidLow ? g_shields.erase(shield) : std::next(shield);
        return out;
    }
}

class DcTestRunStatsScript : public UnitScript
{
public:
    DcTestRunStatsScript() : UnitScript("DcTestRunStatsScript", true, { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL,
        UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
        UNITHOOK_ON_AFTER_AURA_EFFECT_CALCULATE_AMOUNT, UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE }) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!damage || !victim)
            return;
        uint32 const dealt = std::min<uint32>(damage, victim->GetHealth());
        if (victim->IsPlayer())
        {
            // A member taking a hit from a creature (not friendly fire).
            if (!attacker || attacker->GetCharmerOrOwnerPlayerOrPlayerItself())
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (auto itr = g_tracked.find(victim->GetGUID().GetCounter()); itr != g_tracked.end())
                itr->second.damageTaken += dealt;
            // The final amount of the hit just logged by a Modify hook: same
            // attacker, same moment. The Modify value is pre-CoA-scaling and
            // under-read the real blows (a "602" Burning Ground killed 5306 HP).
            if (auto log = g_hits.find(victim->GetGUID().GetCounter()); log != g_hits.end() && !log->second.empty())
            {
                DcTestRunStats::Hit& last = log->second.back();
                if (!last.dealt && last.attackerEntry == attacker->GetEntry() &&
                    getMSTimeDiff(last.msTime, getMSTime()) <= 50)
                    last.dealt = damage;
            }
            return;
        }
        uint32 const guid = OwnerGuidLow(attacker);
        if (!guid)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (auto itr = g_tracked.find(guid); itr != g_tracked.end())
            itr->second.damageDone += dealt;
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        NoteHit(target, attacker, damage, spellInfo);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        NoteHit(target, attacker, damage, nullptr);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (damage > 0)
            NoteHit(target, attacker, uint32(damage), spellInfo);
    }

    void OnHeal(Unit* healer, Unit* reciever, uint32& gain) override
    {
        if (!gain || !reciever)
            return;
        uint32 const guid = OwnerGuidLow(healer);
        if (!guid)
            return;
        uint32 const missing = reciever->GetMaxHealth() - std::min(reciever->GetHealth(), reciever->GetMaxHealth());
        std::lock_guard<std::mutex> lock(g_mutex);
        if (auto itr = g_tracked.find(guid); itr != g_tracked.end())
            itr->second.healingDone += std::min(gain, missing);
    }

    void OnAfterAuraEffectCalculateAmount(AuraEffect const* effect, Unit* caster, int32& amount) override
    {
        if (!effect || effect->GetAuraType() != SPELL_AURA_SCHOOL_ABSORB || amount <= 0)
            return;
        uint32 const guid = OwnerGuidLow(caster);
        if (!guid)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_tracked.find(guid) == g_tracked.end())
            return;
        float scale = 1.0f;
        if (auto itr = g_shields.find(effect); itr != g_shields.end() && itr->second.spellId == effect->GetId())
            scale = itr->second.scale;
        SettleShield(effect, true);
        g_shields[effect] = Shield{ guid, effect->GetId(), int32(float(amount) * scale), amount, scale };
    }

    void OnAuraApply(Unit* /*unit*/, Aura* aura) override
    {
        if (!aura || !aura->HasEffectType(SPELL_AURA_SCHOOL_ABSORB))
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_shields.empty())
            return;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* effect = aura->GetEffect(i))
                if (auto itr = g_shields.find(effect); itr != g_shields.end() && itr->second.spellId == effect->GetId() &&
                    itr->second.calculated > 0 && effect->GetAmount() > 0)
                {
                    itr->second.amount = effect->GetAmount();
                    itr->second.scale = float(effect->GetAmount()) / float(itr->second.calculated);
                }
    }

    void OnAuraRemove(Unit* /*unit*/, AuraApplication* aurApp, AuraRemoveMode /*mode*/) override
    {
        Aura const* aura = aurApp ? aurApp->GetBase() : nullptr;
        if (!aura || !aura->HasEffectType(SPELL_AURA_SCHOOL_ABSORB))
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_shields.empty())
            return;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* effect = aura->GetEffect(i))
                SettleShield(effect, true);
    }
};

// A boss's cast that finished, or that someone else cancelled (an interrupt):
// OnSpellCastCancel reports bySelf=false when the caster did not stop it itself.
class DcTestRunBossCastScript : public AllSpellScript
{
public:
    DcTestRunBossCastScript() : AllSpellScript("DcTestRunBossCastScript", { ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL }) { }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        NoteBossCast(caster, spellInfo, false);
    }

    void OnSpellCastCancel(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool bySelf) override
    {
        if (!bySelf)
            NoteBossCast(caster, spellInfo, true);
    }
};

void AddSC_dc_test_run_stats()
{
    new DcTestRunStatsScript();
    new DcTestRunBossCastScript();
}
