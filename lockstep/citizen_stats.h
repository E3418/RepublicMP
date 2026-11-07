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
// it is at; one with neither counts on both.
//
// The history stays the whole map's too (the day's price update reads it).
// When the day tick pushes a record (4488d0), 2fd2e0 counts its citizens again
// once per half into a record of the mod's, on the simulation thread: the same
// numbers on every PC. They are kept by the record's place in the history (one
// date can have several records) and in the save's republicmp.ini; the
// population window's graphs (34b470) draw a copy of each record with this
// PC's half. A record from before (an older save, a session that was not
// Republics) is drawn whole. Still the whole map's: the births, deaths,
// escapes and immigrants (counted as they happen) and the change since the last
// record in the panels (8d490, 8c3c0).
#pragma once

#include "host.h"

void CitizenStats_Bind(const ModHost* host);
// Patches the two loops; outside a Republics session they count as before.
bool CitizenStats_Install();

// The mode: which half a point is on, 0 or 1; -1 while the cut is not known. It
// runs on the render thread, so it only uses a cut already measured.
typedef int (*CitizenStatsHalfRule)(const float pos[3]);
void CitizenStats_SetHalfRule(CitizenStatsHalfRule rule);
