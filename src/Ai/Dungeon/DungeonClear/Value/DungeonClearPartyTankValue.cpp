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

    // See DcPlayerLed::PartyTankFor: the run's elected leader while it runs
    // unpaused, else the player-led Main Tank, else nobody.
    return DcPlayerLed::PartyTankFor(bot);
}
