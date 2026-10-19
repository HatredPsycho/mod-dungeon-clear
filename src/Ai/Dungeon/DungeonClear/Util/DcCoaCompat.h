/*
 * mod-dungeon-clear — Conquest of Azeroth (CoA) compatibility.
 *
 * CoA's 21 custom classes (ids 12-32) are not in the module's per-class tables:
 * their tank specializations have no stock pull opener, their healers no stock
 * resurrect class, and CoA reset many vanilla instance doors to lock 0. These
 * helpers derive the same answers from the spells a character actually knows and
 * from the realm configuration, so the stock behaviour stays untouched.
 */

#pragma once

#include "AscensionSpecialization.h"
#include "Config.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>

namespace DcCoaCompat
{
    inline bool IsCoaClass(Player const* player)
    {
        return player && IsAscensionCustomClassId(player->getClass());
    }

    struct PullOpener
    {
        uint32 spellId;
        float minRange;
        float maxRange;
    };

    inline bool Taunts(SpellInfo const* info)
    {
        return info->HasEffect(SPELL_EFFECT_ATTACK_ME) || info->HasAura(SPELL_AURA_MOD_TAUNT);
    }

    inline bool Damages(SpellInfo const* info)
    {
        return info->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE) || info->HasEffect(SPELL_EFFECT_WEAPON_DAMAGE) ||
               info->HasEffect(SPELL_EFFECT_WEAPON_PERCENT_DAMAGE) ||
               info->HasEffect(SPELL_EFFECT_NORMALIZED_WEAPON_DMG) || info->HasEffect(SPELL_EFFECT_HEALTH_LEECH) ||
               info->HasAura(SPELL_AURA_PERIODIC_DAMAGE);
    }

    // The best ranged single-target opener a CoA tank knows: an instant taunt first,
    // then an instant damaging spell, the longer reach and the shorter cooldown
    // breaking ties. Spells on cooldown are skipped so the next pack still gets one.
    inline std::optional<PullOpener> ResolvePullOpener(Player* bot)
    {
        if (!IsCoaClass(bot))
            return std::nullopt;

        constexpr float MinimumReach = 20.0f;
        constexpr uint32 LongestCooldownMs = 30000;

        std::optional<PullOpener> best;
        int bestScore = 0;
        uint32 bestCooldown = 0;
        for (auto const& [spellId, state] : bot->GetSpellMap())
        {
            if (state->State == PLAYERSPELL_REMOVED || !state->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || info->IsPassive() || info->IsPositive() || !info->NeedsExplicitUnitTarget() ||
                info->IsAffectingArea() || info->IsChanneled() || info->CalcCastTime() > 0)
                continue;
            uint32 const cooldown = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
            if (cooldown > LongestCooldownMs || bot->HasSpellCooldown(spellId))
                continue;
            float const maxRange = info->GetMaxRange(false);
            if (maxRange < MinimumReach)
                continue;
            int const score = Taunts(info) ? 2 : Damages(info) ? 1 : 0;
            if (!score)
                continue;
            bool const better = !best || score > bestScore ||
                (score == bestScore && (maxRange > best->maxRange ||
                                        (maxRange == best->maxRange && cooldown < bestCooldown)));
            if (better)
            {
                best = PullOpener{spellId, std::max(info->GetMinRange(false), 8.0f), maxRange};
                bestScore = score;
                bestCooldown = cooldown;
            }
        }
        return best;
    }

    // Whether a CoA character can bring a dead ally back: it knows a resurrect.
    inline bool CanResurrect(Player const* player)
    {
        if (!IsCoaClass(player))
            return false;
        for (auto const& [spellId, state] : player->GetSpellMap())
        {
            if (state->State == PLAYERSPELL_REMOVED || !state->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (info && !info->IsPassive() &&
                (info->HasEffect(SPELL_EFFECT_RESURRECT) || info->HasEffect(SPELL_EFFECT_RESURRECT_NEW)))
                return true;
        }
        return false;
    }

    // DungeonClear.LockFreeClickableDoors: comma-separated gameobject entries the
    // realm vouches for as plain click doors although their lock is 0 (CoA PR #6477
    // reset 74 vanilla instance doors that way). Re-read when the value changes, so
    // .reload config applies it.
    inline bool IsConfiguredLockFreeDoor(uint32 goEntry)
    {
        static std::mutex lock;
        static std::string parsedFrom;
        static std::unordered_set<uint32> entries;

        std::string const value =
            sConfigMgr->GetOption<std::string>("DungeonClear.LockFreeClickableDoors", "17153,17154", false);
        std::lock_guard<std::mutex> guard(lock);
        if (value != parsedFrom)
        {
            entries.clear();
            std::stringstream stream(value);
            std::string item;
            while (std::getline(stream, item, ','))
            {
                try
                {
                    if (item.find_first_not_of(" \t") != std::string::npos)
                        entries.insert(static_cast<uint32>(std::stoul(item)));
                }
                catch (...)
                {
                }
            }
            parsedFrom = value;
        }
        return entries.count(goEntry) != 0;
    }
}
