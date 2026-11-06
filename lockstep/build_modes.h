// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: each player's own construction mode (rubles, dollars or realistic,
// and the two settings that go with it; world/build_mode.h).
//
// The small build window's buttons write world +af8..+b04. The commands that
// build carry the clicking PC's mode, but the simulation reads these words too
// (168730 when a building is demolished from a tick, 2d9d40 when an edge splits)
// and so do commands that carry no mode. So the words were one value for the whole
// session, and one player's click changed the other's window.
// Now the world holds this player's own choice while this PC's interface and tools
// run. A change is a command, so every PC knows every player's mode in the same
// order. A tick runs with the host's mode and a command with its player's mode, on
// every PC alike. A load gives everyone the loaded save's mode.
#pragma once

#include "host.h"

void BuildModes_Bind(const ModHost* host);

// Once a frame, before the commands due are made: this PC's change becomes a command.
void BuildModes_Collect();

// A tick's own code runs between the two, a command of `player` between the others.
void BuildModes_SimBegin();
void BuildModes_SimEnd();
void BuildModes_CommandBegin(int player);
void BuildModes_CommandEnd();

// The command (lockstep/commands).
bool BuildModes_ApplyCommand(int player, const void* data, size_t size);
