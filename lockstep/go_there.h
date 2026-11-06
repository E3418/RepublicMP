// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: "Go there" (terrain_go_there) as a command (by SoylentGreed).
//
// The Late dispatcher calls 3ca560(world) at 314930 every frame the tool is
// selected. With a vehicle selected through its window (+d298, its window +d2a0
// the first of +cac0), the menus closed and a click, it sends the vehicle to the
// cursor inline: an empty stop record 1512a0 on its stack with +190 the fixed
// node a556b0, vehicle +728 the point, +680 emptied and one null pushed, the
// records +638 cleared (1ff7a0) and the record pushed (1ff8f0), +698 = 0, +d3a =
// 1, then 1513f0 on the record's two halves.
// In a session the handler runs with the click hidden and the vehicle and the
// point are a command. Every PC makes these steps with the record zero-filled
// first: on the game's stack its other words are whatever the stack held.
#pragma once

#include "host.h"
#include "world_refs.h"

#pragma pack(push, 1)
struct GoThereMsg {
    unsigned magic;
    RouteRef vehicle;
    float point[3];
};
#pragma pack(pop)

void GoThere_Bind(const ModHost* host);

// False when a routine differs (the tool is then put away in lockstep).
bool GoThere_Ready();

// The tool stub of the Late dispatcher calls this instead of 3ca560.
unsigned __int64 GoThere_Run(unsigned __int64 world);

// The command (lockstep/commands).
bool GoThere_ApplyCommand(const void* data, size_t size);
