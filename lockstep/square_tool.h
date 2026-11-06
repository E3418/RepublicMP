// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the pavement squares (second_road_*_square, water_square) as a
// command (by SoylentGreed).
//
// The second-road handler 3cf6f0(world, type, 1) takes its square branch: on the
// press it empties the piece list +f608 and puts the cursor in the first corner
// +f620; every held frame the cursor goes to the second corner +f62c and
// 3ce6c0(world, type, 1) (3cff11) builds a preview: real edges of the network
// +f7a8, a grid between the corners turned by the camera's yaw (exe+9e9d20). On
// the release 3ce6c0(world, type, 0) (3cff38) builds it for good and the handler
// starts each piece's construction (5247f0) and clears the ground under its points
// (3ae120).
// The preview's edges would live in the network on this PC between frames, so a
// tick would see them: in a session no preview is built, and the square appears
// when it is made. The final call is a command (type, both corners, the yaw); every
// PC makes that call and the handler's loop at the boundary, its own corners, yaw
// and list put back.
#pragma once

#include "host.h"

#pragma pack(push, 1)
struct SquareMsg {
    unsigned magic;
    int type;
    float first[3], second[3];
    float yaw;
};
#pragma pack(pop)

void SquareTool_Bind(const ModHost* host);

// False when a routine differs (the tools are then put away in lockstep).
bool SquareTool_Ready();

// The relays instead of 3ce6c0 at the two sites.
unsigned __int64 SquareTool_Preview(unsigned __int64 world, unsigned __int64 type, unsigned __int64 preview);
unsigned __int64 SquareTool_Final(unsigned __int64 world, unsigned __int64 type, unsigned __int64 preview);

// The square's four corners (the area rule).
void SquareTool_Corners(const SquareMsg& m, float corners[4][3]);

// The command (lockstep/commands).
bool SquareTool_ApplyCommand(const void* data, size_t size);
