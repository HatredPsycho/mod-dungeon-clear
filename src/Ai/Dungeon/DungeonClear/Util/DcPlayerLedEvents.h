/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPLAYERLEDEVENTS_H
#define _PLAYERBOT_DCPLAYERLEDEVENTS_H

#include "Action.h"
#include "Trigger.h"

class Player;
class PlayerbotAI;
class DcPlayerLedEventStore;

// PLAYER-LED v2: DC's scripted events while a human leads with the Main Tank bar
// (DcPlayerLed::Leader non-null: flagged player on the map, no DC run).
//
// One bot per group and instance is the EXECUTOR (lowest GUID among alive
// non-tank bots on the player's map, DcPlayerLedEventDecision::Elect). It runs
// the bot steps of the event the player has reached, reads the gates of the
// player's own steps, and holds every step that would start an encounter until
// the player gives the go ("dc go", or doing it himself). Its state is per
// instance (DcPlayerLedEventStore), so a re-elected executor takes over where
// the last one stopped.
//
// A DC run never reaches any of this: every entry point first requires
// DcPlayerLed::Leader, which is null whenever a run is owned.
namespace DcPlayerLedEvents
{
    DcPlayerLedEventStore& Store();

    // The elected executor for the group `mainTank` leads, or nullptr.
    Player* ElectExecutor(Player* mainTank);

    // The player's go: arm it for his instance. `mainTank` must be the flagged
    // player leading (DcPlayerLed::Leader of his own group). True when the order
    // was armed fresh; false when refused (not leading) or already pending.
    bool OrderGo(Player* mainTank);

    // Whether `mainTank` may give the go right now (he leads his group).
    bool CanOrderGo(Player* mainTank);

    // The executor's tick: drive the event the player has reached. False when
    // there is nothing to drive.
    bool Tick(PlayerbotAI* botAI);
}

// Fires on the elected executor, out of combat, while an event is due in Main
// Tank mode. Inert in a DC run and with nobody flagged Main Tank.
class DungeonClearPlayerLedEventTrigger : public Trigger
{
public:
    DungeonClearPlayerLedEventTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "dungeon clear player led event", 1) {}
    bool IsActive() override;
};

class DungeonClearPlayerLedEventAction : public Action
{
public:
    DungeonClearPlayerLedEventAction(PlayerbotAI* botAI)
        : Action(botAI, "dungeon clear player led event") {}
    bool Execute(Event event) override;
};

// "dc go" said in chat (whisper or party) by the Main Tank player: release the
// pending encounter-starting step. Does nothing unless the speaker leads in Main
// Tank mode, so in a DC run the chat line stays as inert as before.
class DcGoPlayerLedAction : public Action
{
public:
    DcGoPlayerLedAction(PlayerbotAI* botAI) : Action(botAI, "dc go player led") {}
    bool Execute(Event event) override;
};

#endif
