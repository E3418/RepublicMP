// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the construction offices assign tool's selection as a command.
//
// The tool (assign_co, 5eedf0) and its window keep their state in an object
// at [world +d448]: the mode (+8: assign offices, assign construction, cancel
// construction), the selected offices (+d0..+d8, building pointers) and ten
// quick selection slots (+180..+188, 0x20 bytes each, the slot's offices at
// +8..+10; +178 the slot in use), saved with the city (5f2a20). The window,
// the clicks on offices and the number keys change it on one PC only, so each
// player kept a different selection.
// Once a frame (main thread) the state is compared with the session's; a
// change is sent whole, and every PC writes it (the offices by identity) at
// the boundary. Two players changing it at once end with the later one on
// every PC. The assignments the tool makes are game calls (game_calls.cpp).
#pragma once

#include "tesmio_plugin.h"

void OfficeSelection_Bind(const TsmHost* host);

// Main thread, once a frame while commands are captured.
void OfficeSelection_Poll();

// The command (lockstep/commands).
bool OfficeSelection_ApplyCommand(const void* data, size_t size);
