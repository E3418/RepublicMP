// SPDX-License-Identifier: GPL-3.0-only
// Citizen trace for desync reports (#55): the moments a citizen boards a
// vehicle, gets a workplace, or is removed, kept in a ring while the layer is
// active and written to the log at a desync, so the two PCs' logs show the
// first of these that happened at a different tick or not at all.
//
//   0x831120(world, person, vehicle) -> al: the person boarded (customs house
//            riders included; their count +0x1234 drives the next arrivals)
//   0x824860(world, person, ...)     -> the job search; person+0x690 the workplace
//   0x835D70(world, person, ...)     -> the person leaves the game
// A person is named by the bits of its age (+0xD4, drawn at random when it is
// made); a building by its index (+0x00), a vehicle by its index (+0x04).
#pragma once

#include "tesmio_plugin.h"

void CitizenTrace_Bind(const TsmHost* host);
bool CitizenTrace_Install();

// Writes the events of ticks fromTick+1 .. toTick to the log (any thread).
void CitizenTrace_Dump(unsigned long long fromTick, unsigned long long toTick);
