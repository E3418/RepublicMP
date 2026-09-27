// Save sync: brings a joining player into the host's current game.
//
// Host flow: Host opens a listening socket and returns immediately - the game
// keeps running while nobody is connected. When a client connects, the host
// pauses, saves the current game under a random name, zips it, sends it and
// stays blocked until the client reports it has loaded; then both unpause.
//
// Client flow: Connect blocks the game, receives and unzips the save, loads
// it through the game's own load path and signals the host when ready.
//
// At the end of both flows build sync and road sync connect automatically.
// A save-sync run is one blocking flow; "busy" covers it from start to finish.
#pragma once

#include "tesmio_plugin.h"
#include "host_config.h"

#define SAVE_SYNC_PORT 5556
#define SAVE_SYNC_TEST_HOST_IP REPUBLICMP_HOST_IP
// Existing save used only if triggering a live save fails.
#define SAVE_SYNC_TEST_SAVE_NAME "17171 - newsenderv2"
#define SAVE_SYNC_RECEIVED_SUFFIX "_received"

void SaveSync_Bind(const TsmHost* host);

// Writes to tesmioloader.log and the in-game UI log.
void SaveStatus(const char* fmt, ...);

void SaveSync_StartHost();
void SaveSync_StartClient(const char* hostIp = nullptr); // nullptr = keep last IP

// Stops the running flow and closes the build/road sync connections of this
// session. Waits (accept, connect, save, load) react within ~200ms; a file
// transfer already in progress is bounded by its own I/O timeout. Does not
// leave the game session itself.
void SaveSync_RequestDisconnect();

bool SaveSync_IsBusy();
bool SaveSync_IsHostRole();

// True only for a host that is listening with no client yet (nothing paused).
bool SaveSync_IsHostAwaitingClient();

const char* SaveSync_GetTargetIp();
