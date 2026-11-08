/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_PET_STANCE_H
#define _DC_PET_STANCE_H

class Player;

// PETS DO NOT OPEN FIGHTS ON A DC RUN (3.0, Shadowfang Keep courtyard).
//
// Between fights a run member's aggressive pets and guardians (CoA summons such as
// Lesser Zombie, Air Elemental, Hellfire Imp, Hallucination, and hunter pets left
// aggressive) engage whatever comes into their own aggro range, 20-60yd ahead of
// the party. In the SFK courtyard that opens a second front that the assistance
// chain then spreads through the keep (72 of ~1,650 far joins, 07-08/11).
//
// While the owner is out of combat, every aggressive unit it controls is held
// DEFENSIVE and called back to follow if it strays past kRecallYards. The moment
// the owner enters combat, the units this module changed are set back to
// AGGRESSIVE, so the fight itself is unchanged. Throttled to once a second per
// bot; only bots with the "dungeon clear" non-combat strategy, inside a dungeon.
namespace DcPetStance
{
    void Tick(Player* player);
}

#endif  // _DC_PET_STANCE_H
