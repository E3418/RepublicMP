// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the join tool (join_hub_all: two edges of one type at a node made
// one, the node removed) as a command (by SoylentGreed).
//
// The Late dispatcher calls 3c9a00(world, 1) at 313656 every frame the tool
// is selected. It clears the hover words +f648/+f650/+f6a0 and walks every
// network of the world's table (+14034 entries of 0x50 from +14038) through
// its camera-visible edges (+aa0) and their end nodes: a node with exactly
// two edges of one type and nothing attached, within 10 of the cursor +f6c
// (edges of type 0xa/0x14 also by the camera ray, exe+9e9d10 to world +f60),
// is the hover +f650; the network's +318/+31c hold a refusal (3) when the
// edges cannot be joined. With a click (exe+a54e91) and no refusal it merges
// the two edges with 53e630 and the network's own steps.
//
// The pick depends on this PC's camera, so a click runs the handler with the
// click hidden (the highlight only) and the picked node - its network's place
// in the table, its place in the network's committed nodes +298 and its
// position - is a command. Every PC runs the handler with the click, the
// cursor and the ray on that node and every network's visible lists holding
// only the edges near it, so it picks that node and joins natively; its own
// words are put back after.
#pragma once

#include "host.h"

#pragma pack(push, 1)
struct JoinMsg {
    unsigned magic;
    unsigned table, networks; // the network's place in the world's table, the table's size
    unsigned node, nodes;     // the node's place in the network's +298, the list's size
    float point[3];           // the node's position (bits compared)
};
#pragma pack(pop)

void JoinTool_Bind(const ModHost* host);

// False when the handler differs (the tool is then put away in lockstep).
bool JoinTool_Ready();

// The tool stub of the Late dispatcher calls this instead of 3c9a00.
unsigned __int64 JoinTool_Run(unsigned __int64 world, unsigned __int64 mode);

// The command (lockstep/commands).
bool JoinTool_ApplyCommand(const void* data, size_t size);
