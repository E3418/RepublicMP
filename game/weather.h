// SPDX-License-Identifier: GPL-3.0-only
// The weather at the start of a shared simulation.
//
// Temperature, wind, snow, the rain and road wetness live on the world object
// (+0xE28..+0xE78) and are not in the save (by Sohn): a load keeps whatever
// weather the PC had before, so two PCs loading the same save start with
// different weather. In a session the weather only moves on admitted ticks
// and draws from the shared generator, so the start is all that differs -
// and the heating plants burn by the temperature, the vehicles slow down on
// wet or snowy roads: the games drift apart.
//
// At the session's load every PC starts the weather the way the game does
// for a new world (294388: 333bd0(world, 1), then 5cfea0(world, 1)): the
// season and the temperature from the date, the rest from the shared
// generator - the same weather everywhere, nobody's copied.
#pragma once

#include "tesmio_plugin.h"

void Weather_Bind(const TsmHost* host);

// Main thread, right after the session's load, with the determinism layer on.
void Weather_StartShared();
