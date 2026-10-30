// SPDX-License-Identifier: GPL-3.0-only
// Buildings: a placement or a demolition made through the game's own
// functions from the inputs of the PC that clicked (lockstep commands
// LS_CMD_PLACE / LS_CMD_DEMOLISH, made on every PC at a tick boundary).
//
// Placement: the clicking PC's building tool hands over the INPUTS of its
// placement call FUN_1402bade0 - cursor (world+0xf6c..f74), yaw (+0xf94),
// mirror, param_5, its pick modes and build mode (world/local_builds).
// Every PC calls the same function with them, so each engine redoes
// snapping, auto-connections, access roads and helper objects itself.
//
// Demolition: the building is found by position (obj+0x350) and type
// category (type+0x360) and the engine's demolish function FUN_140168730 runs
// with the clicking PC's arguments (world/demolish_hook).
#pragma once

#include "tesmio_plugin.h"
#include "build_mode.h"

#define BUILDING_PLACE 0
#define BUILDING_DEMOLISH 1

struct BuildingMsg {
    int kind;                   // BUILDING_*
    BuildMode mode;             // PLACE: the clicking PC's rubles/dollars/realistic choice
    float x, y, z;              // PLACE: the cursor; DEMOLISH: the building (obj+0x350)
    char typeName[64];          // building type, e.g. "residental_brick_1"
    float cursorX, cursorY, cursorZ; // placement cursor of the creating call
    float cursorRot;            // placement yaw input (radians)
    unsigned long long param5;  // the creating call's param_5
    int mirrored;               // building mirrored (world+0x11db1)
    // DEMOLISH only
    int demolishCategory;       // *(obj+0x318)+0x360, -1 = unknown
    unsigned long long demolishArgs[5]; // FUN_140168730's p3..p7, raw
    // PLACE: the clicking PC's pick modes, world +f600 (bit 0) and +f698 (bit 1) -
    // two of the player's own switches the network pick 3c4850 reads when the access road is laid;
    // bit 2: the slow-rotate binding (Ctrl) held, which leaves out the automatic connections
    int pickModes;
};
#define BUILDING_PICK_NO_LINKS 4
#define BUILDING_NO_LINKS_BINDING "BUILDING_SLOW_ROTATE"

void Buildings_Bind(const TsmHost* host);

// True while a placement or demolition command is being made.
bool Buildings_IsApplyingCommand();

// Main thread, at a tick boundary. True when the engine made it.
bool Buildings_ApplyPlacementCommand(const BuildingMsg& msg);
// The building tool's searches for buildings to connect to (see buildings.cpp).
bool Buildings_InstallLinkSearch();
bool Buildings_ApplyDemolishCommand(const BuildingMsg& msg);
