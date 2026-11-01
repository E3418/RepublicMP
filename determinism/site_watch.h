// SPDX-License-Identifier: GPL-3.0-only
// Construction site watch: a diagnostic (Debug window, off by default) for
// the sites that go their own way on one PC with no command (#55: a road
// site's progress became -8025 and its stage 0 on the player only, the tick
// a neighbouring site finished).
//
// At the boundaries of a frame's phases - between frames, the commands,
// Prepare, Early, a held frame's drawing, Late, the interface, the site
// drawer, the cursor pick - each construction site's progress (+604), cached
// stage (+634), stage list (+610/+618) and current stage's materials are
// compared with the last look. A change in a phase where the tick does not
// run (anything but a tick's Early and Late), a progress outside 0..1, or a
// stage going back is logged with the tick, the phase and the thread; the
// two PCs' logs then show where one of them changed a site.
//
// Inside the tick (a road site finished at once on the player only, the
// tick a neighbouring site finished): each site's construction update
// 159070 is compared before and after - a jump of the progress, or a site
// finished well before 1, is logged with the materials of its stage and the
// call stack - and a building object made (25bab0) or a site destroyed
// (164bb0) while a world runs is logged with the stack, so a site replaced by
// another object shows who did it.
#pragma once

#include "tesmio_plugin.h"

void SiteWatch_Bind(const TsmHost* host);
// Remembered; off by default.
bool SiteWatch_GetOn();
void SiteWatch_SetOn(bool on);

// A world load: the sites are looked at afresh.
void SiteWatch_Reset();
// Compares the sites with the last look. `tick`: the tick or a command may
// change them in this phase (only invalid values and stages going back are
// logged then).
void SiteWatch_Check(const char* phase, bool tick);

// The object hooks (idle while the watch is off).
bool SiteWatch_Install();
// Around one site's construction update (determinism/sim_patches).
void SiteWatch_UpdateBefore(BYTE* building);
void SiteWatch_UpdateAfter(BYTE* building);
