/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "RouteSweepRegistry.h"

namespace
{
    // The maps, each earned by its own run data. Measured off acore_world.creature.
    //
    //   34 — Stormwind Stockade (issue #17). The reference geometry, and about as
    //   pure a case as the failure has. The first boss by DBC order (Targorr) sits
    //   105yd dead ahead of the entrance up the central axis, so the opening leg of
    //   the run is a straight 105yd corridor. Its cells flank that axis at 10-21yd
    //   against ~29yd of real aggro reach, on BOTH sides — so every fight spot in
    //   the hall is inside two neighbours at once, and there is no line to detour
    //   to. Every trash mob is rank-1 elite at lvl 23-25, which is why the Dynamic
    //   ceiling (thirds-of-an-elite against PullDynamicMaxLeeroyMobs * 3) reads
    //   comfortably safe while the room is not: live run tr-20260809-201248-2
    //   classified 35 LEEROY to 4 ADVANCED, and its one sized pull predicted 3 mobs
    //   and fought 7.
    //
    //   532 — Karazhan (tr-20260927-003140-1 and both siblings of its plan). The
    //   Grand Ballroom is a field of Phantom Guest formations 30-40yd apart, each
    //   member with ~22yd of aggro reach and a 10yd assistance chain, and the
    //   Moroes route runs straight through the middle of it. The corridor band
    //   (half-width 18yd) walked the tank past a cluster 20yd beside route vertex
    //   23 without ever picking it; the cluster picked the tank at 18yd instead,
    //   with no verdict and no camp, and the walk-in engage fought it in place —
    //   which on that floor means the room. The Banquet Hall next door is the
    //   same geometry with Moroes' dinner guests in it, and both sibling runs
    //   lost him to a trash fight no pull was in flight for. 10-man normal only:
    //   EnRouteSweepApplies excludes heroic, and Karazhan has none.
    //   33 — Shadowfang Keep (3.0, 08/11). The courtyard: one pack fought in place
    //   recruits the next through the 3 s assistance re-call (Creature-
    //   FamilyAssistancePeriod), and the chain reaches the whole keep — ~1,650
    //   joins from 30-180yd over ~30 bench runs, Silverlaine and Razorclaw among
    //   them, the mythic wipes. One pack at a time, dragged to a camp, is the
    //   only shape that keeps a pull a single pack. On EVERY tier (see below).
    uint32 const kSweepMaps[] =
    {
        34,     // The Stockade
        532,    // Karazhan
        33,     // Shadowfang Keep
    };

    // Sweep maps where the heroic-tier exclusion does NOT apply. CoA Mythic on a
    // 5-man reads heroic tier (DcDifficulty::IsHeroicTier), and SFK's chain is
    // worst there: the walk detour (PullEnRouteAvoid) cannot stop a fight that
    // spreads by assistance once it starts.
    uint32 const kSweepEveryTierMaps[] =
    {
        33,     // Shadowfang Keep
    };

    // Stacked-floor maps (GuardsStackedFloors), measured off acore_world.creature.
    //
    //   33 — Shadowfang Keep. The climb from the entrance to the courtyard-door
    //   landing (-244,2141,87) runs under the gallery: Tormented Officers 3873 at
    //   z 97.4 over the ramp, Vile Bat 3866 / Blood Seeker 3868 / Odo the
    //   Blindwatcher 4279 at z 100.1 and Wailing Guardsmen 3877 at z 94-95 over
    //   the landing, and Rethilgore's hall (3914 at (-252.1,2123.1,81.2), its
    //   Whitescalps at z 81.3) 6yd below it. The classifier read every one of them
    //   as "another floor" and sized the landing's Whitescalp at 2 elites against
    //   a ceiling of 2 -> LEEROY, camp none. tr-20261104-152435-131 (mythic):
    //   fought on the landing, the gallery joined through the ceiling (LOS n,
    //   15-22yd in 3D from the nearest member), the Guardsmen chained the keep and
    //   Silverlaine and Razorclaw came with it — wipe on Rethilgore, 0/10 of that
    //   series; ~3/10 of the normal series die the same way.
    uint32 const kStackedFloorMaps[] =
    {
        33,     // Shadowfang Keep
    };
}

bool RouteSweepRegistry::GuardsStackedFloors(uint32 mapId)
{
    for (uint32 const id : kStackedFloorMaps)
        if (id == mapId)
            return true;
    return false;
}

bool RouteSweepRegistry::SweepsEveryTier(uint32 mapId)
{
    for (uint32 const id : kSweepEveryTierMaps)
        if (id == mapId)
            return true;
    return false;
}

bool RouteSweepRegistry::SweepsRoute(uint32 mapId)
{
    for (uint32 const id : kSweepMaps)
        if (id == mapId)
            return true;
    return false;
}
