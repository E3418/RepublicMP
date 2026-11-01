// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the private car building window's "Reset owners" (by SoylentGreed).
//
// The window 7a1610 acts on the press at 7a1a1f with the button hovered: for
// each car of the building's list +c70 whose home +4f8 is the building and
// whose owner +4e8 is set, 831c50(world, owner) (the owner leaves its car and
// building, 830820 re-targets it) and 3fbdc0(world, car, 0, 0, 1, 1) (the car
// sent home): citizens and cars on the clicking PC only.
//
// Both calls (7a1a88, 7a1aab) go through relays: in a session's interface
// frame neither is made, the first car names the building and the press
// becomes one command; every PC makes the loop at the boundary.
#pragma once

#include "tesmio_plugin.h"

void CarOwners_Bind(const TsmHost* host);
bool CarOwners_Install();

// The command (lockstep/commands).
bool CarOwners_ApplyCommand(const void* data, size_t size);
