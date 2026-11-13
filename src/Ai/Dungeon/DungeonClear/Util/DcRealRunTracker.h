/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DUNGEONCLEAR_DCREALRUNTRACKER_H
#define _DUNGEONCLEAR_DCREALRUNTRACKER_H

#include <string>

#include "Define.h"

class Player;
class PlayerbotAI;

// Engine side of the real-run log (DcRealRunLog): follows each player-started
// Dungeon Clear run from `dc on` to its end and writes one runs.jsonl line.
//
//   Begin      — DcOnAction, once the run is enabled. Skips `.dc test` / lab
//                parties (DcTestRunManager::IsRunMember) and anything that is
//                not inside a dungeon instance. A `dc on` on a run already
//                tracked in the same instance keeps it.
//   NoteWipe   — the death rung, on a full-party wipe (raid entrance regroup or
//                the Wipe disable).
//   NoteDeathEnd — the death rung, right before it disables the run: the end
//                is then a "wipe".
//   End        — DisableDungeonClear, before the run state is reset.
//   Tick       — global world tick: samples boss kills (encounter mask + cleared
//                anchors, like the bench) once a second, and closes runs whose
//                leader vanished or whose enabled flag dropped outside the
//                disable funnel (`dc skip` past the last boss).
//
// State is keyed by the run owner's GUID and guarded by one mutex; every entry
// point is safe from map threads and the world thread.
namespace DcRealRunTracker
{
    void Begin(PlayerbotAI* ownerAI);
    void NoteWipe(Player* owner);
    void NoteDeathEnd(Player* owner);
    void End(PlayerbotAI* ownerAI, std::string const& reason);
    void Tick(uint32 diff);
}

#endif  // _DUNGEONCLEAR_DCREALRUNTRACKER_H
