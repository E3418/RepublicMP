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
// worker_relocate, _educated, _inmate (2b8ce0, the button in a residential
// building's window selects it; marks 2b9263 allowed, 2b9065 refused) move people
// from the window's building to a picked one with the same kind of use: workers
// and educated workers: n = (int)(capacity - vacant) up to 10 (educated 5), n
// calls of 83c6a0(world, source, target, 0, educated); inmates: up to five of the
// source's residents +bd8 from a random place, 83ca50(world, citizen, target).
// vehicle_replace_auto (2b4680, mark 2b49df) queues the picked vehicle for
// auto-replacement. vehicle_addstop_copy (2b18c0, mark 2b2152) copies the window's
// vehicle's stops to the picked vehicle. vehicle_sendconstruction (1d88a0, marks
// 1d9188 and 1d8ea6 allowed, 1d8ecc refused) sends the window's vehicle, or another
// of its office's, to a building on the office's site list, or back to its office.
// In a session each tool runs with the click hidden; the mark tells the pick
// and whether it is allowed, and an allowed click becomes a command: every PC
// makes the tool's steps with the game's own routines and tests.
#pragma once

#include "host.h"

void LinkTools_Bind(const ModHost* host);

// The routines are the checked ones (the tools are put away otherwise):
// the link tools, and the fire and copy weights tools.
bool LinkTools_Ready();
bool LinkTools_PickReady();
bool LinkTools_RelocateReady();
bool LinkTools_VehicleReady();

// The tool stubs' targets: kind 0 station links (2b6f30), kind 1 distribution
// links (2b78b0), kind 2 the fire station's dispatch (1d48a0), kind 3 copy
// weights (1d71c0), kind 4 relocation (2b8ce0), 5 auto-replacement (2b4680), 6
// copied stops (2b18c0), 7 construction vehicles (1d88a0).
unsigned __int64 LinkTools_Station(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 LinkTools_Distribution(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 LinkTools_Fire(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 LinkTools_Weights(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 LinkTools_Relocate(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 LinkTools_AutoReplace(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 LinkTools_CopyStops(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 LinkTools_SendConstruction(unsigned __int64 world, unsigned __int64 a2);
// The tools' mark 40bd60 (2b75e9, 2b8480, 1d4d35, 1d74c4, 2b9263, 2b9065, 2b49df,
// 2b2152, 1d9188, 1d8ea6, 1d8ecc).
unsigned __int64 LinkTools_Mark(unsigned __int64 world, unsigned __int64 target, unsigned __int64 a3,
                                unsigned __int64 refused, unsigned __int64 a5);

// The command (lockstep/commands).
bool LinkTools_ApplyCommand(const void* data, size_t size);
