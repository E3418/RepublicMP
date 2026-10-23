// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the city tool (namepoint) making a new city as a command.
//
// The tool's handler 32d240 (Late, through the tool stubs) takes a click on
// the terrain under the cursor (world +f6c), draws a free name (32d150 checks
// it against the cities), then makes the city with 32f450(world, &position,
// name) at 32f3e3 - a 0x1c0-byte record pushed on world +10348 - and gives it
// the buildings around with 32ff40(world, &position) at 32f403. In a session
// the tool runs in a private scope and both were undone on every PC alike, so
// the tool did nothing. game_calls takes both calls: the first becomes a
// command with the position and the name, the second is left to it, and
// every PC makes both at the boundary. The name is drawn on the clicking PC.
#pragma once

#include "tesmio_plugin.h"

void NewCity_Bind(const TsmHost* host);

// At the 32f450 call (game_calls): true when the city became a command (the
// tool's call is skipped, and so is its 32ff40 that follows).
bool NewCity_Capture(unsigned __int64 position, unsigned __int64 name);
// At the 32ff40 call: true when it belongs to a captured city (skipped).
bool NewCity_TakeAssign();

// The command (lockstep/commands).
bool NewCity_ApplyCommand(const void* data, size_t size);
