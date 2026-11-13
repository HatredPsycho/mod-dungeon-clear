#ifndef _PLAYERBOT_DC_TEST_RUN_STATS_H
#define _PLAYERBOT_DC_TEST_RUN_STATS_H

#include "Define.h"

#include <string>
#include <vector>

// Damage and healing done by the members of live test runs, for the per-member
// DPS / HPS of the run record. Fed by a UnitScript (DcTestRunStatsScript) on the
// map threads; only registered characters are counted, so the realm's other
// players and bots cost one locked set lookup per hit.
namespace DcTestRunStats
{
    struct Counters
    {
        uint64 damageDone = 0;
        uint64 healingDone = 0;  // effective: overhealing is left out
        uint64 damageTaken = 0;  // from creatures, before death
        uint64 absorbDone = 0;   // damage soaked by the member's absorb shields, on anyone
    };

    // One hit a tracked member took from a creature (schema 20): the record's
    // harm list is the harmful AURAS present at the last sample, which named a dummy
    // marker (Targorr's 2102735) as the killer. These are the actual damage events.
    struct Hit
    {
        uint32 msTime = 0;            // getMSTime() of the hit
        uint32 spellId = 0;           // 0 = melee swing
        std::string spellName;
        uint32 attackerEntry = 0;
        std::string attackerName;
        std::string attackerTarget;   // whom the attacker was fighting then ("" if none)
        uint32 amount = 0;            // damage at the Modify hook, BEFORE CoA scaling (coa_spell_damage_info)
        uint32 dealt = 0;             // what OnDamage then reported for it (after scaling); 0 = not matched
        uint32 hpBefore = 0;          // the member's health when it landed
        // Where it happened (map x/y), and the attacker's live ground markers from
        // kMarkerSpells (Targorr's Berserker Leap: the impact 2102737 killed
        // ranged bots the marker 2102735 never covered; Combat needs the three spots).
        float victimX = 0.0f, victimY = 0.0f;
        float attackerX = 0.0f, attackerY = 0.0f;
        std::vector<std::pair<float, float>> markers;
        // The attacker's last marker seen on ANY tracked hit, even if gone since:
        // 2102735 lives 3.5 s and the impact lands after a 3 s cast plus the
        // missile's flight, so the live list is often empty when it hits.
        bool hasLastMarker = false;
        float lastMarkerX = 0.0f, lastMarkerY = 0.0f;
        uint32 lastMarkerMs = 0;      // getMSTime() it was last seen
    };
    static constexpr std::size_t kHitLog = 3;

    // Boss casts in a watched instance (schema 20): finished casts and casts a
    // member interrupted, per (boss entry, spell). Combat asked how many of
    // Shazzrah's Arcane Force Novas went off against how many were kicked.
    struct BossCast
    {
        uint32 entry = 0;
        std::string boss;
        uint32 spellId = 0;
        std::string spellName;
        uint32 cast = 0;
        uint32 interrupted = 0;
    };
    void WatchInstance(uint32 instanceId);
    // Hands back the instance's boss casts and stops watching it.
    std::vector<BossCast> ReleaseInstance(uint32 instanceId);

    void Track(uint32 guidLow);
    // The last kHitLog creature hits on the member, oldest first.
    std::vector<Hit> LastHits(uint32 guidLow);
    // Hands back what the character did since Track and stops counting it.
    Counters Release(uint32 guidLow);
}

void AddSC_dc_test_run_stats();

#endif
