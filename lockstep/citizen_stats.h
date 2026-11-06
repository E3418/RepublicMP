// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP by E3418 and Sohn (juniorsohn); all authors in README.md.
// Republics: the citizens' averages a player sees are their own republic's.
//
// The game averages happiness, health, food, loyalty and the other needs over
// every citizen, in two loops that only the interface's numbers come from:
//
//   317db0  the HUD's pass, every 3 s on the render thread: world +c6c8..+c788
//           (the top bar, the citizens and education panels)
//   2fd2e0  a statistics record: the population window's, the citizens'
//           record +11328, a city's own, and the day tick's for the history
//
// In a Republics session each loop skips the citizens of the other half, so
// the game's own math comes out for this PC's republic. What the simulation
// reads stays whole: the HUD's pass also writes the population +c720 the
// simulation scales by, which the determinism layer recounts before every Early
// and Late (so this is on only with the layer), and 2fd2e0 keeps every citizen
// on the simulation thread, where the day tick fills the history, and for a
// city's own record.
//
// A citizen belongs to the half of its home, else of the building or vehicle
// it is at; one with neither counts on both. The graphs (the history) and the
// change since the last record are still the whole map's.
#pragma once

#include "host.h"

void CitizenStats_Bind(const ModHost* host);
// Patches the two loops; outside a Republics session they count as before.
bool CitizenStats_Install();

// The mode: which half a point is on, 0 or 1; -1 while the cut is not known. It
// runs on the render thread, so it only uses a cut already measured.
typedef int (*CitizenStatsHalfRule)(const float pos[3]);
void CitizenStats_SetHalfRule(CitizenStatsHalfRule rule);
