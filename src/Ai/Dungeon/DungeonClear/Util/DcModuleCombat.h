/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_MODULE_COMBAT_H
#define _DC_MODULE_COMBAT_H

// The combat guards the playerbots module (SquidBot 3.0, CoA Combat) has rewritten.
// For each one, DC stops running its own copy when the module carries it, and keeps
// it on an older module, so the same DC builds against both.

// The reference-tank hook the CoA Combat guards read (DC names the run's tank).
#if __has_include("CoaCombatGroupTank.h")
#define DC_HAS_COA_COMBAT 1
#else
#define DC_HAS_COA_COMBAT 0
#endif

// A1, escape leap ban (Blink / Disengage in a group in an instance).
#if __has_include("CoaEscapeLeapRule.h")
#define DC_MODULE_OWNS_ESCAPE_LEAP 1
#else
#define DC_MODULE_OWNS_ESCAPE_LEAP 0
#endif

#endif  // _DC_MODULE_COMBAT_H
