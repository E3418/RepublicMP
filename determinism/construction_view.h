// SPDX-License-Identifier: GPL-3.0-only
// Construction sites drawn with this PC's view in a session.
//
// The site drawer 2dc470 (render thread; called by the colour pass at 3dd999
// with pass 0 and at ad26 with pass 1) walks the frame's building list
// world+11dd8 - a copy of the visible list world+11dc0 made by the interface
// routine (2dc3c0 from 317db0) - and draws a building's construction stages
// (PositionClip cut of each model piece by the stage records +610, the
// scaffolds +640/+658/+670, the dirt and gravel piles +718) only when its
// camera visibility +115a is set and its camera distance +115c is under
// each part's limit (2dca7d, 2dd7dd, 2de63a; 2de67b, 2de76e, 2de8b1,
// 2e0774, 2e0cee, 2e0efd). In a session the simulation keeps every
// building off the camera (+115a 0, +115c 100000.0 - the building
// visibility patch, by SoylentGreed), so no site showed its stages.
//
// In a session the drawer always runs inside this PC's view (sim_patches'
// VisibleIn, as the interface routine does): +115a/+115c of the buildings on
// this PC's camera are set for the call and put back after. The render and
// the simulation never overlap while the layer is on (the frame waits for the
// render), and random numbers come from the private stream. What the drawer
// writes into the listed buildings - the shipyard timer +fc0 and the stage
// records' slot data (+20b0..+21b8 of each 0x21b8-byte record, the bytes
// the scaffold generator 167540 writes too) - is kept before the call and
// put back after. Worker target vectors +6b8/+6d0 are detached during the
// drawer and replaced with private scratch vectors: Late 137b20 reads them
// and re-targets citizens when their count changes. All listed buildings,
// including untyped sites, are guarded. Rendering never publishes those
// local targets into the shared simulation.
//
// The log (Debug window) writes, every two seconds, the drawer's list and up
// to six construction sites on it: progress, flags, the simulation's and this
// PC's +115a/+115c, the stage index, the stage, scaffold, pile and part
// lists, and whether the drawer changed the kept fields.
#pragma once

#include "tesmio_plugin.h"

void ConstructionView_Bind(const TsmHost* host);

// The diagnostic log (remembered; off by default).
bool ConstructionView_GetLog();
void ConstructionView_SetLog(bool on);

// For sim_patches' relay around 2dc470 (render thread). Sample before this
// PC's view is set (the simulation's values), View after it, Keep right
// before the drawer, After right after it (puts the kept fields back when
// keep was set, and logs).
void ConstructionView_Sample(BYTE* world, int pass);
void ConstructionView_View();
void ConstructionView_Keep(BYTE* world, bool keep);
void ConstructionView_After(bool keep);
