// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: starting and suspending research as a command (by SoylentGreed).
//
// The research panel of a research building's window (2f7ed0) works on the
// world's research list (world +11778..+11780, 0xe8-byte entries): a press on
// a project (2f8281) writes the building into the entry's +d8 (2f82b4) and
// "Suspend research" (2f888f) clears it (2f88a3). The list lives on the heap,
// so the interface switches never saw it: research started or stopped on the
// clicking PC only. (The building's researcher count +11a8 is a building word
// the interface switches carry.)
// Around the interface routine on frames with a click, every entry's +d8 is
// read before and after; a changed entry is put back at once, and every PC
// writes it (the building by identity) at the boundary.
#pragma once

#include "tesmio_plugin.h"

void Research_Bind(const TsmHost* host);

// Render thread, around the interface routine. simBusy: the simulation's phase
// runs now (nothing is compared).
void Research_Before(bool simBusy);
void Research_After(bool simBusy);

// The command (lockstep/commands).
bool Research_ApplyCommand(const void* data, size_t size);
