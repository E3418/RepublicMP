// SPDX-License-Identifier: GPL-3.0-only
// Building demolition capture: an inline hook on FUN_140168730, the
// engine's "demolish this building" function.
//
// FUN_140168730(thisPtr, building, p3, p4, p5, p6, p7) - p3/p4 in r8b/r9b,
// p5..p7 on the stack (byte flags; the callee reads only the low byte, the
// hook forwards the raw qwords). Player paths into it:
//   - terrain_demolish tool with the BUILDING_SLOW_ROTATE modifier held
//     (FUN_1403abc90, 4 call sites) - demolishes without the prompt;
//   - the "building_demolish_promt" confirmation button (FUN_1407fc880,
//     calls it with [dialog+0x240] = the building).
// Both write building+0xeb1 = 0 just before the call. It is also called from
// ~8 other places (simulation, other UI) and recursively for attached
// sub-buildings (FUN_140168730 itself).
//
// What becomes a command (in a session): every OUTERMOST call made by local
// play - not the recursive ones, not calls inside a building placement (every
// PC's placement makes those itself), not calls made while a command is being
// made. The building is identified by position (obj+0x350) and type category
// (*(obj+0x318)+0x360); every PC demolishes its own building there with the
// same flags (world/buildings, BUILDING_DEMOLISH).
#pragma once

#include "tesmio_plugin.h"

#define HOOK_DEMOLISH_RVA 0x168730

void DemolishHook_Bind(const TsmHost* host);
bool InstallDemolishHook();

// True while FUN_140168730 is running (local or a command's). Road commits
// made inside it are the demolition's own side effects and are not taken.
bool DemolishHook_InDemolish();

// Calls the original (unhooked) function, for making a demolition command.
// Writes building+0xeb1 = 0 first, as both player paths do. Returns false if
// it threw.
bool DemolishHook_CallOriginal(__int64 thisPtr, __int64 building, const unsigned long long args[5]);
