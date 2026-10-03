// Multiplayer session: joining a player into the host's running game and
// keeping track of whether that player is still there.
//
// Host: Host starts hosting and returns - the game keeps running while
// nobody is connected. When a player connects, both games are paused and
// locked behind the sync screen (ui/sync_block) while the host saves, packs
// and sends the world and the player unpacks and loads it. Then the live sync
// channels (build_sync, road_sync, terrain_sync) connect, the screens lift
// and both games resume together. The host keeps hosting: if the player
// leaves or the connection drops, it goes back to waiting for a player, until
// Stop hosting.
//
// Client: Connect pauses and locks the game at once and walks the same
// steps from the other side; the host's steps (saving, packing) are relayed
// so the sync screen always says what is happening. The session ends when
// the connection drops or on Disconnect.
//
// Before anything else both sides exchange version, protocol number and a
// CRC32 of their RepublicMP.dll (version.h); any difference refuses the join.
//
// One session runs at a time: Host/Connect are refused while one is active.
#pragma once

#include "tesmio_plugin.h"
#include "host_config.h"

#define SAVE_SYNC_PORT 5556
#define SAVE_SYNC_TEST_HOST_IP REPUBLICMP_HOST_IP
#define SAVE_SYNC_RECEIVED_SUFFIX "_received"

enum SessionState {
    SESSION_IDLE = 0,
    SESSION_HOST_LISTENING,  // hosting, no player yet
    SESSION_HOST_JOINING,    // a player is joining (both games locked)
    SESSION_HOST_IN_GAME,    // the player is in the game, live sync running
    SESSION_CLIENT_JOINING,  // connecting / receiving / loading (locked)
    SESSION_CLIENT_IN_GAME,  // in the host's game, live sync running
};

void SaveSync_Bind(const TsmHost* host);

// Writes to tesmioloader.log and the in-game UI log.
void SaveStatus(const char* fmt, ...);

// Refused (no-op) while a session is active.
void SaveSync_StartHost();
void SaveSync_StartClient(const char* hostIp = nullptr); // nullptr = keep last IP

// Ends the session: stops hosting / leaves the host, closes the live sync
// channels. Waits (accept, connect, save, load) react within ~200ms.
void SaveSync_RequestDisconnect();

SessionState SaveSync_GetState();
bool SaveSync_IsBusy();     // any state but SESSION_IDLE
bool SaveSync_IsHostRole(); // role of the current/last session

// The other player's IP (host: the connected player, client: the host), or "".
const char* SaveSync_GetPeerIp();

// Why the last session ended or failed ("" if it ended normally).
const char* SaveSync_GetLastError();

// This build's identity, as exchanged in the handshake.
const char* SaveSync_GetLocalVersion();
unsigned int SaveSync_GetLocalDllCrc();

const char* SaveSync_GetTargetIp();

// Kept for the debug UI; true while hosting with nobody connected.
bool SaveSync_IsHostAwaitingClient();
