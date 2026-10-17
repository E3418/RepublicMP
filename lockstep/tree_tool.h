// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: planting single trees as commands (by SoylentGreed).
//
// The build menu's single-tree tools (tree_*, palma*, jungle_*, fjord_*,
// afgan_/afghan_*, tallkriak*, leafkriak*: records with a species list at
// +98) go to 2f94c0(world) from the Late dispatcher (3129fc). A press
// (exe+a54e91) plants at once; while the button (exe+a54e90) is held the
// world's accumulator +11758 grows by the frame time (PowerTime on the timer
// exe+9d4ee0) and plants when it passes a limit. A planting puts several trees
// at random points inside the brush (+f98) with a random species.
//
// In lockstep a frame with the button down or pressed runs the tool with both
// hidden (only the brush is drawn) and its inputs - the tool, the cursor, the
// brush, the frame's timer and the press - become a command. Every PC runs
// the tool at the boundary with those inputs and the button set, so the
// random points and species come from the shared stream and the accumulator
// grows alike.
#pragma once

#include "tesmio_plugin.h"

void TreeTool_Bind(const TsmHost* host);

// False when the game's tree tool differs (the tree tools are then put away in lockstep).
bool TreeTool_Ready();

// The tool stub of 3129fc (sim_patches) calls this instead of 2f94c0.
unsigned __int64 TreeTool_Run(unsigned __int64 world);

// The command (lockstep/commands).
bool TreeTool_ApplyCommand(const void* data, size_t size);
