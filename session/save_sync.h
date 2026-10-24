// SPDX-License-Identifier: GPL-3.0-only
// Multiplayer session: taking players into the host's running game, and
// re-syncs.
//
// Host: Host starts hosting and returns - the game keeps running while
// nobody is connected. When players connect, every game is paused and locked
// behind the sync screen (ui/sync_block) while the host saves, packs and
// sends the world to all of them at once and each unpacks and loads it; the
// host loads the same save meanwhile. Then the players open the lockstep
// connection (lockstep/lockstep_net), the screens lift and the shared
// simulation runs. Up to core/profile's max players (the host included); the
// join port stays open while there is room (a full session stops
// listening). The host keeps hosting until Stop hosting: a player who leaves
// is dropped and the others go on.
//
// Client: Connect pauses and locks the game at once and walks the same
// steps from the other side; the host's steps (saving, packing) are relayed
// so the sync screen always says what is happening. The session ends when
// the connection drops or on Disconnect.
//
// Before anything else both sides exchange version, protocol number and a
// CRC32 of their RepublicMP.dll (version.h), then their DLC lists and the
// names their players chose; a different build (or DLCs, with the check on)
// refuses the join.
//
// Re-sync: on request or a set time after the games differ, the host
// announces it on the lockstep connection, closes it and listens again;
// every player connects straight back and the normal join runs. A player who
// connects during the game joins through a re-sync of everyone.
//
// One session runs at a time: Host/Connect are refused while one is active.
#pragma once

#include "tesmio_plugin.h"
#include "../core/host_config.h"

#define SAVE_SYNC_PORT 5556
#define SAVE_SYNC_TEST_HOST_IP REPUBLICMP_HOST_IP
#define SAVE_SYNC_RECEIVED_SUFFIX "_received"

enum SessionState {
    SESSION_IDLE = 0,
    SESSION_HOST_LISTENING,  // hosting, no player yet
    SESSION_HOST_JOINING,    // a player is joining (both games locked)
    SESSION_HOST_IN_GAME,    // players are in the game, lockstep running
    SESSION_CLIENT_JOINING,  // connecting / receiving / loading (locked)
    SESSION_CLIENT_IN_GAME,  // in the host's game, lockstep running
};

void SaveSync_Bind(const TsmHost* host);

// Writes to tesmioloader.log and the in-game UI log.
void SaveStatus(const char* fmt, ...);

// True while a world is loaded (not the main menu, not mid-load).
bool SaveSync_IsWorldLoaded();

// The kind of game a session plays: the host's choice, sent in its hello.
enum { SESSION_MODE_COOP = 0, SESSION_MODE_REPUBLICS = 1 };
#define SESSION_REPUBLICS_PLAYERS 2

// Refused (no-op) while a session is active. Hosting needs a loaded world;
// leaving the world ends the session (SaveSync_Tick).
void SaveSync_StartHost(int gameMode = SESSION_MODE_COOP);
// The current/last session's mode (a player: the host's).
int SaveSync_GetGameMode();
// The session's size: the profile's for co-op, 2 for Republics.
int SaveSync_MaxPlayers();
void SaveSync_StartClient(const char* hostIp = nullptr); // nullptr = keep last IP

// Ends the session: stops hosting / leaves the host, closes the lockstep
// connection. Waits (accept, connect, save, load) react within ~200ms.
void SaveSync_RequestDisconnect();

// Host, player in the game: re-sync now (no-op otherwise).
void SaveSync_RequestResync();

// Host: re-sync this many seconds after the games differ (-1: never by itself).
void SaveSync_SetDesyncResyncSeconds(int seconds);
int SaveSync_GetDesyncResyncSeconds();
// Host: ms until that re-sync (0: none pending).
ULONGLONG SaveSync_GetMsUntilDesyncResync();

// True while a re-sync is under way (from the announcement to the player
// being back in the game).
bool SaveSync_IsResyncing();

// Client: the host announced a re-sync.
void SaveSync_OnResyncAnnounced();

// Per-frame (game thread, Present hook): ends the session when the player
// leaves the world.
void SaveSync_Tick();

// Refuse a join when the two games' DLCs differ (on by default; a join goes ahead when either player turns it off).
void SaveSync_SetDlcCheck(bool on);
bool SaveSync_GetDlcCheck();

SessionState SaveSync_GetState();
bool SaveSync_IsBusy();     // any state but SESSION_IDLE
bool SaveSync_IsHostRole(); // role of the current/last session
// Milliseconds since Host / Connect started the current session (0 when idle).
// The UI keeps its stop buttons disabled for a moment after a start, so a
// double click on Connect does not land on Disconnect.
ULONGLONG SaveSync_GetSessionAgeMs();
#define SAVE_SYNC_STOP_BUTTON_DELAY_MS 2000

// Who the session is with, for the status lines: host - the players' names (an
// address before a name is known); client - "<host's name> (<address>)"; or "".
const char* SaveSync_GetPeerIp();

// Why the last session ended or failed ("" if it ended normally).
const char* SaveSync_GetLastError();

// This build's identity, as exchanged in the handshake.
const char* SaveSync_GetLocalVersion();
unsigned int SaveSync_GetLocalDllCrc();

const char* SaveSync_GetTargetIp();

// Kept for the debug UI; true while hosting with nobody connected.
bool SaveSync_IsHostAwaitingClient();
