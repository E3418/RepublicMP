// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the buoy tools (bojka_red, bojka_cancel) as a command (by SoylentGreed).
//
// The Late dispatcher calls 3c85b0(world, mode) every frame the tool is
// selected: mode 0 at 3146e7 places, mode 1 at 314709 removes. From the cursor
// +f6c, with the menus closed:
//   mode 0: the terrain under the cursor below -1, no buoy within 50 (3c8ba0)
//   and 3c8c60 not refusing: a press makes a node of the buoy network +fa28:
//   operator new(0xe8), ff950, +4 the cursor, +e0 = 0xb, 419410(world, node, 0,
//   1, 0) (its signal object, a random draw), +d0 = +d8 = 0, the sound, pushed
//   on the network's nodes +298.
//   mode 1: a press removes the first buoy within 25 with 3c9060(world, index).
// A press is decided on the clicking PC with these tests and the handler runs
// with it hidden (its preview). Every PC makes the node, or removes the named
// one, at the boundary.
#pragma once

#include "host.h"

#pragma pack(push, 1)
struct BuoyMsg {
    unsigned magic;
    unsigned mode;  // 0 a placement, 1 a removal
    int index;      // a removal: the node's place in +298
    unsigned nodes; // the list's size on the clicking PC
    float point[3]; // a placement: the cursor; a removal: the node (bits compared)
};
#pragma pack(pop)

void BuoyTool_Bind(const ModHost* host);

// False when a routine differs (the tools are then put away in lockstep).
bool BuoyTool_Ready();

// The tool stubs of the Late dispatcher call this instead of 3c85b0.
unsigned __int64 BuoyTool_Run(unsigned __int64 world, unsigned __int64 mode);

// The command (lockstep/commands).
bool BuoyTool_ApplyCommand(const void* data, size_t size);
