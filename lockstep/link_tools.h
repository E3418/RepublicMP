// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the link pick tools as commands (by SoylentGreed).
//
// building_addstation (2b6f30) and building_addstation_dist (2b78b0) link the
// window's building (world +d298) to a picked building. Each frame the tool
// picks the building under the cursor and marks it with 40bd60(world, target,
// 0, refused, 1) (at 2b75e9 / 2b8480); on a click an allowed pick is linked
// inline: a 0x18-byte link object pushed on the source's +d38 with a 0x198-byte
// record holding the load/unload shares (76a3a0), the source's +d50 and +fc0
// set, and the source pushed on the target's partner list (+d78 station, +d90
// distribution). It all lives on the heap: the link was made on the clicking
// PC only, and that PC's later load/unload settings of it were refused
// everywhere else (its link count differed).
// The same way two more pick tools act inline: firestation_addfire (1d48a0,
// mark 1d4d35) sends the fire station's vehicles (+d298) to a picked burning
// building (2582e0 per vehicle), and building_copyweights (1d71c0, mark
// 1d74c4) copies the nine words +11f4..+1214 of the window's building to the
// picked one.
// In a session each tool runs with the click hidden; the mark tells the pick
// and whether it is allowed, and an allowed click becomes a command: every PC
// makes the tool's steps with the game's own routines and tests.
#pragma once

#include "tesmio_plugin.h"

void LinkTools_Bind(const TsmHost* host);

// The routines are the checked ones (the tools are put away otherwise):
// the link tools, and the fire and copy weights tools.
bool LinkTools_Ready();
bool LinkTools_PickReady();

// The tool stubs' targets: kind 0 station links (2b6f30), kind 1 distribution
// links (2b78b0), kind 2 the fire station's dispatch (1d48a0), kind 3 copy
// weights (1d71c0).
unsigned __int64 LinkTools_Station(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 LinkTools_Distribution(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 LinkTools_Fire(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 LinkTools_Weights(unsigned __int64 world, unsigned __int64 a2);
// The tools' mark 40bd60 (2b75e9, 2b8480, 1d4d35, 1d74c4).
unsigned __int64 LinkTools_Mark(unsigned __int64 world, unsigned __int64 target, unsigned __int64 a3,
                                unsigned __int64 refused, unsigned __int64 a5);

// The command (lockstep/commands).
bool LinkTools_ApplyCommand(const void* data, size_t size);
