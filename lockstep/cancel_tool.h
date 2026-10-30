// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the network cancel tools (tool_road_cancel and the other
// networks' *_cancel) as a command.
//
// Their handler 214220 runs every frame the tool is selected: it picks an
// edge of the tool's network (21bc50 from the selected entry +d428) among
// this PC's camera-visible edges (+aa0) along the cursor's ray, and with a
// click on an edge that holds nothing (+38..+40 empty) removes the whole
// edge at once with 5434c0(network, edge, 0, -1, 0) at 2144d2 - not through
// the network commit the capture takes, so it was made on the clicking PC
// only and the PCs differed (a site gone on one of them).
//
// That call goes through a relay here: on the clicking PC it becomes a
// command (the network's place in the world's table, the edge's place in its
// committed edges +2b0 and its two nodes' positions); every PC makes the same
// call on that edge at the boundary.
#pragma once

#include "tesmio_plugin.h"

void CancelTool_Bind(const TsmHost* host);
bool CancelTool_Install();

// The command (lockstep/commands).
bool CancelTool_ApplyCommand(const void* data, size_t size);
