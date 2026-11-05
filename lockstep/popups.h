// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the confirm and value popups' callbacks as commands (by SoylentGreed).
//
// The popup (world +13bf0) keeps a callback at +78 and its argument at +70;
// 3bd50 builds the entered values (a vector of 0x18-byte entries, the float
// at +10) and calls cb(values, argument) at 3bd9b, and every callback ends by
// destroying the values (42ba0). Some callbacks change lists inline on the
// clicking PC only:
//   799550 / 799600 the production queue's next / cancel all   (a building)
//   7723a0 "Cancel all source buildings"                        (a building)
//   745160 the link window's order (the values: positions)      (a building)
//   770640 the construction office's order or copy (positions)  (a building)
//   778480 the citizens' destinations' "Remove all connections" (a list)
//   778490 the citizens' destinations' percent (the values)     (a list)
// (a list: one of a building's destinations lists +f18, +f30, +f48, +f60).
// "Discard cargo from all wagons" (7495e0) and "Cancel all assignments"
// (7705c0) pass: their per-wagon and per-building calls are window calls
// (game_calls.cpp).
//
// The 10 bytes at 3bd94 (mov rdx,[rbx+70]; mov rcx,rax; call [rbx+78]) call a
// thunk here. In a session's interface frame one of these callbacks is not
// made: the building (and list) and the values become a command, and every PC
// makes the callback with a values vector of the game's allocator.
#pragma once

#include "host.h"

void Popups_Bind(const ModHost* host);
bool Popups_Install();

// The command (lockstep/commands).
bool Popups_ApplyCommand(const void* data, size_t size);
