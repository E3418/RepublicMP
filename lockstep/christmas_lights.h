// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the Christmas light lines (christmas_line_0..11, christmas_cancel)
// as commands (by SoylentGreed).
//
// The Late calls the light manager world +faf8 twice a frame: ea950(manager, t)
// at 315723 (the tool) and ebf00(manager, t) at 315749 (draw, blink, power). The
// manager: a vector of 0xf8-byte lines (+0 begin, +8 end, +10 capacity), +18 the
// scene the builder gets, +20 a drag under way, +21 its line valid, +24 its
// start, +30 its end.
//   christmas_line_N: a press pushes a new line (e6ce0, its start's anchor +38,
//   +8/+c/+10 from N); each held frame rebuilds it from its two points with
//   e6fb0(line, &start, &end, scene) (random draws, terrain, building collision)
//   into +21, the end's anchor +44; the release keeps a valid line of two or more
//   points (+98, 12 bytes each) and gives it its buildings, ea4b0(manager, line),
//   otherwise e6e00 and free. Any other tool, or none, drops a drag.
//   christmas_cancel: while the button is held, every line with an end (+68,
//   +74) within 1.0 of the camera ray is freed.
// In a session:
//   - the drag's line stays out of the vector except inside those two calls: a
//     building made or removed anywhere calls ea4b0(manager, 0) on the whole
//     vector, and it would give the line buildings on this PC only;
//   - a release is a command (the line's words, both anchors and both ends) and
//     this PC's own line is dropped; every PC makes the line at the boundary. The
//     clicking PC's result decides: a line of two or more points is kept on every
//     PC, whatever the builder's building collision answers there;
//   - a cancel is a command naming the lines by their ends; nothing is freed
//     until the boundary;
//   - ebf00 lights every line and draws power for a line's buildings out of
//     season while a christmas_line_ tool is selected. The tool stays selected
//     through a tick here, so with time passing ebf00 does not see it.
#pragma once

#include "host.h"

#define LIGHTS_MAX_REMOVED 8

#pragma pack(push, 1)
struct LightsMsg {
    unsigned magic;
    unsigned mode;  // 0 a line made, 1 lines freed
    unsigned count; // mode 1: how many of ends
    unsigned header[5];  // mode 0: the line's words +8..+1b
    float anchorStart[3], anchorEnd[3], start[3], end[3];
    float ends[LIGHTS_MAX_REMOVED][6]; // mode 1: each line's start and end (bits compared)
};
#pragma pack(pop)

void ChristmasLights_Bind(const ModHost* host);

// False when a routine differs (the tools are then put away in lockstep).
bool ChristmasLights_Ready();

// The tool stub instead of ea950 (315723); the relay instead of ebf00 (315749).
void ChristmasLights_Tool(unsigned __int64 manager, float t);
void ChristmasLights_Draw(unsigned __int64 manager, float t);

// The command (lockstep/commands).
bool ChristmasLights_ApplyCommand(const void* data, size_t size);
