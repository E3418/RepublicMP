// Determinism patches: the rest of the camera, keyboard and frame-rate paths
// inside the simulation, so two runs (two PCs) from the same save step the
// same way. Active only together with the determinism layer (determinism.h:
// the probe now, a lockstep session later) - never in a 0.4 replay session,
// where some of them would only cost visuals.
//
// Byte patches, each checked against the original bytes first (nothing is
// written unless every site matches) and put back when the layer goes off:
//   - vehicles "became visible": the two blocks gated on a vehicle entering
//     the camera (6a5475 in 6a5410, 6ab9d4 in 6a7520) re-target its agents
//     and draw random numbers - skipped, as for any vehicle off the camera;
//   - vehicle placement: road and rail vehicles' scene nodes (6af0c0,
//     6ad960) are placed the off-screen way; the traffic code tests the
//     vehicle ahead through those nodes. On-screen vehicles are drawn
//     without the terrain tilt;
//   - vehicle "checked" flag +13da starts from 0, not from the camera flag
//     (66e74c): the drive routine skipped its neighbour check for vehicles
//     the camera saw;
//   - road sign zones (548b36) update only while the piece lists vehicles,
//     not while the camera sees it;
//   - followed vehicle: the camera's own update calls of a followed vehicle
//     (10e4f, 10e87, 10ebd, 10f22) are NOPs and the early worker updates it
//     like any other (3fadbc);
//   - debug keys inside the simulation: Ctrl+N / Ctrl+M (building pass), T
//     (vehicle routine), H (Late body, twice), Ctrl in the school window,
//     Ctrl+I (car building), E+L+C (depot), and the calendar keys (fa41).
//
// Call relays (installed at start, behaving natively while the layer is off):
//   - building visibility (13ce50): the building pass's GetNode answers an
//     off-screen node on the simulation thread, so +115a/+115c and the agent
//     lists that follow them are the same everywhere;
//   - saplings (9e4d, 30d9b2): a new planting entry starts growing in Late on
//     the tick it appears, not on whichever rendered frame came next; the
//     forest add itself stays on the render thread, queued.
// And the Late body (30d1a0):
//   - population count world+0xc720 is recounted with the interface's rule
//     before Early and before Late (the simulation reads it; the interface
//     recounts it on its own frames);
//   - the autosave timer world+0 is held at 0 (a save ages every vehicle).
//
// (by SoylentGreed)
#pragma once

#include "tesmio_plugin.h"

void DetPatches_Bind(const TsmHost* host);

// Installs the call relays and the Late hook. Call once from TsmPluginStart.
bool DetPatches_Install();

// Writes the byte patches and turns the relays on. Main thread, with the
// simulation not running (the probe calls it right before a load).
void DetPatches_Apply();

// Per frame (Present hook): puts the byte patches back once the layer is off.
void DetPatches_Tick();

// Main thread, right before Early (determinism.cpp's step hook).
void DetPatches_BeforeEarly(__int64 world);

// For the probe log: the last population count written (-1 = never).
int DetPatches_LastPopulation();
