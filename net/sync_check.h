// Sync check: how far the two independently simulated worlds have drifted.
//
// Both games take a state summary (hooks/state_hash) right after the first
// simulation step of every game hour - at the same game time on both sides,
// since the client follows the host's clock. The player sends each summary to
// the host on the build_sync channel; the host compares it with its own
// summary of the same hour and shows the drift per subsystem (citizens,
// vehicles, storage, production). With auto re-sync on, a drift above the
// chosen limit triggers a re-sync (net/save_sync).
#pragma once

#include "tesmio_plugin.h"

// Wire form of one hourly summary (part of BuildSyncMsg).
struct SyncCheckWire {
    int year, day, hour;
    int buildings, vehicles, citizens;
    double storage, cargo, production;
};

struct SyncDrift {
    bool valid;
    int year, day, hour;
    float total;       // largest of the parts below, percent
    float citizens, vehicles, storage, production; // percent
    int hostVehicles, clientVehicles;
    int hostCitizens, clientCitizens;
};

void SyncCheck_Bind(const TsmHost* host);

// Registers the step observer (hooks/determinism). Call once at start.
void SyncCheck_Install();

// Per-frame (game thread, Present hook): sends the player's pending summary.
void SyncCheck_Tick();

// A summary from the player (host side; game thread, from BuildSync_Tick).
void SyncCheck_OnRemote(const SyncCheckWire& w);

// Host: last measured drift.
SyncDrift SyncCheck_GetDrift();

// Host: re-sync automatically when the drift exceeds this many percent (0 = off).
void SyncCheck_SetAutoResyncPercent(int percent);
int SyncCheck_GetAutoResyncPercent();
