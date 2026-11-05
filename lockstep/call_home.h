// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: "Call all vehicles home" on a partner building (by SoylentGreed).
//
// The building window 6f8cc0 (r12 the building, r13 the world) reads the
// press at 701a2c and, hovered (the window [rbp+18] +20), for a building whose
// type has +364 == 0x20 and +360 <= 1 and that has a partner (+d08), takes the
// partner's vehicles inline (701a49..701d1b), on the clicking PC only:
//   1. each 0x60-byte record of the building's +a10 list with kind 9 and an
//      object at +30: 523c70([world+fa40], object, building);
//   2. the same for the partner's +a10 records (still with this building);
//   3. every vehicle of the building's +c70 list gets +4f8 = building;
//   4. while the partner's +c70 list is not empty: its first vehicle gets
//      +4f0 = +4f8 = building, goes on the building's list (b1700 makes room)
//      and leaves the partner's.
// Other buildings send each vehicle home with a window call (game_calls.cpp).
//
// Here the press on such a building is answered "no press" and becomes a
// command (both buildings); every PC makes the four steps at the boundary.
#pragma once

#include "host.h"

void CallHome_Bind(const ModHost* host);
bool CallHome_Install();

// The command (lockstep/commands).
bool CallHome_ApplyCommand(const void* data, size_t size);
