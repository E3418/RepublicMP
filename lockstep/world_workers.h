// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the construction menu's "Max. workers without CO" (by SoylentGreed).
//
// The number field is a widget the world points to (+c790, heap): ff6a0(widget)
// gives its value, ff520(widget, value) sets it, and the save keeps it. The
// building tool (2d4672) and the demolition 168730 (16aae5) store int(value)
// as the new site's +630, workers it may have without a construction office;
// the simulation demolishes too, so every PC needs the same value. It was each
// PC's own field: a player who typed another number placed sites that worked
// otherwise on that PC only.
//
// Both readers call this DLL instead: in a session they answer the session's
// value. A change the player types becomes a command; every PC takes the
// ordered value and the other PCs' fields show it.
#pragma once

#include "host.h"

void WorldWorkers_Bind(const ModHost* host);
bool WorldWorkers_Install();

// Main thread, every frame of a session (commands' collect): a typed change.
void WorldWorkers_Collect();

// The command (lockstep/commands).
bool WorldWorkers_ApplyCommand(const void* data, size_t size);
