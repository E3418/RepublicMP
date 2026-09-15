// End-to-end flow: pressing Host opens a listening socket and returns
// immediately - the game keeps running completely normally, nothing paused,
// nothing saved, while nobody has connected yet (SaveSync_IsHostAwaitingClient()
// reports this window to the UI). Only once a real client actually connects
// does the flow pause the game, snapshot/save (live save if hooked, else
// fall back to an existing save), zip and send the CURRENT game - locking
// the game before anyone has asked to join would be the wrong trade-off.
// Once packaged, it stays paused/blocked through transfer -> unzip -> load
// -> unpause. No simultaneous unpause guarantee beyond the ready-handshake
// in save_sync.cpp.
//
// UI-only (ui/main_menu.cpp's Host/Connect buttons), via
// SaveSync_StartHost/StartClient below.
#pragma once

#include "tesmio_plugin.h"
#include "../mp_config.h" // REPUBLICMP_HOST_IP - one shared IP for every network module

#define SAVE_SYNC_PORT 5556
// Fallback only. TriggerSaveGame() (hooks/save_load.h, verified in x64dbg)
// succeeds on every normal run - the host generates its own random name
// per sync (see net/save_sync.cpp's GenerateRandomSaveName), so this
// constant isn't used on the happy path. Kept only as a last-resort
// fallback for the case TriggerSaveGame() itself reports failure.
#define SAVE_SYNC_TEST_SAVE_NAME "17171 - newsenderv2"
#define SAVE_SYNC_TEST_HOST_IP REPUBLICMP_HOST_IP
#define SAVE_SYNC_RECEIVED_SUFFIX "_received"

void SaveSync_Bind(const TsmHost* host);

void SaveStatus(const char* fmt, ...); // writes to tesmioloader.log + the in-game UI log (ui/ui_log.cpp) - no separate console anymore

// UI-driven equivalents of the O/I hotkeys above (see ui/main_menu.cpp) -
// mirrors net/build_sync.h's Start*/Is* pattern. Unlike build_sync's
// persistent connection, a save-sync run is one whole blocking flow (pause ->
// zip/transfer -> unzip/load -> unpause) done in one shot - "busy" covers the
// entire thing from press to finish, there's no separate "connected and
// idling" state to hold open or explicitly disconnect from afterwards.
void SaveSync_StartHost();

// hostIp: pass nullptr (or empty) to keep using whatever IP was last set
// (SAVE_SYNC_TEST_HOST_IP / mp_config.h by default); pass a real IP string to
// override it for this and future calls.
void SaveSync_StartClient(const char* hostIp = nullptr);

// No-op if nothing is running. Otherwise: logs "[DISCONNECT] ..." right
// away, tears down any active/connecting build_sync AND road_sync
// connections (BuildSync_Reset()/RoadSync_Reset() - a save_sync session and
// its auto-started build_sync/road_sync connections, see
// RunHostSendSaveFlow/RunClientReceiveSaveFlow, are one logical "session"
// as far as the user is concerned), and sets a cancel flag checked at every
// stage boundary of the running Host/Client flow, plus inside every polled
// wait: WaitForSaveToFinish/WaitForLoadToFinish (hooks/save_load.h, ~100ms
// reaction) and NetworkHostAcceptOne()/NetworkConnect() themselves
// (net/network.h, ~200ms reaction, polled internally rather than one single
// blocking OS call) - so a host just sitting there with no client connected
// yet, or a client still mid-connect, both react to Disconnect almost
// immediately instead of having to wait out accept()/connect(). The one
// thing still NOT interruptible is a transfer already in flight
// (NetworkSendFile()/NetworkReceiveFile() mid-chunk) - that's bounded by
// its own ioTimeoutMs at worst, not by this flag. Does NOT force the game
// back to the main menu (no confirmed hook for that) - it only stops OUR
// flow and logs it; leaving the actual game session/menu is still up to
// the player.
void SaveSync_RequestDisconnect();

bool SaveSync_IsBusy();
bool SaveSync_IsHostRole(); // true after SaveSync_StartHost(), false after SaveSync_StartClient() - meaningless while not busy

// true only for a host that's listening but has nobody connected yet (the
// window where nothing is paused/locked - see the flow comment above). UI
// uses this to label the Disconnect button "End Hosting"/"Close" instead,
// since there's no one connected to "disconnect" from at that point - just
// an open invitation to close. Always false for a client, and false again
// for a host once a real player has connected (from then on it's a normal
// Disconnect).
bool SaveSync_IsHostAwaitingClient();

const char* SaveSync_GetTargetIp(); // current/default client target IP, for prefilling a UI field
