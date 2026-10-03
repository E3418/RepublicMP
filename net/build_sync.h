// Build sync: network replication of building placements and demolitions.
//
// Sending side: hooks/detect_spawn.cpp's placement hook sends one
// BuildSyncMsg per building the local player creates. It carries the
// placement INPUTS of the creating call - cursor position (thisPtr+0xf6c..f74),
// yaw (thisPtr+0xf94) and param_5 - plus the resulting main object's position
// and rotation for verification.
//
// Receiving side: the same placement function (FUN_1402bade0) is called with
// the sender's inputs and mirror state, so the engine redoes snapping,
// auto-connections and helper objects itself. The main new object then gets
// the sender's exact rotation (the engine smooths yaw, so the input alone
// lands within ~1-3 degrees). The function silently does nothing while the
// tool guard bytes are set (blocking UI, window activation), so placements
// wait for them to clear and a refused placement is retried a few times.
//
// Demolitions (hooks/demolish_hook) travel on the same connection as
// BUILD_SYNC_KIND_DEMOLISH, so a demolition never overtakes the placement of
// the building it removes. The receiver finds its building by position and
// type category and runs the engine's demolish function with the sender's
// flags; a building that is already gone is skipped.
//
// Every message carries a terrain fence (see terrain_sync.h) and is applied
// only after the terrain edits made before it.
//
// Transport: one dedicated TCP connection (BUILD_SYNC_PORT), fixed-size
// messages, symmetric (either side sends). Placements made while
// disconnected are queued. Both machines must run the same build.
#pragma once

#include "tesmio_plugin.h"
#include "host_config.h"

#define BUILD_SYNC_PORT 5557
#define BUILD_SYNC_TEST_HOST_IP REPUBLICMP_HOST_IP

#define BUILD_SYNC_KIND_PLACE 0
#define BUILD_SYNC_KIND_DEMOLISH 1

struct BuildSyncMsg {
    int kind;                   // BUILD_SYNC_KIND_*
    int terrainFence;           // TerrainSync_GetFenceSeq() at send
    float x, y, z;              // PLACE: main object position (verification only); DEMOLISH: the building (obj+0x350)
    float rotSin, rotCos;       // main object yaw (obj+0x340 / obj+0x348)
    char typeName[64];          // building type, e.g. "residental_brick_1"
    float cursorX, cursorY, cursorZ; // placement cursor of the creating call
    float cursorRot;            // placement yaw input (radians)
    unsigned long long param5;  // the creating call's param_5
    int mirrored;               // building mirrored (thisPtr+0x11db1 after the call)
    // DEMOLISH only
    int demolishCategory;       // *(obj+0x318)+0x360, -1 = unknown
    unsigned long long demolishArgs[5]; // FUN_140168730's p3..p7, raw
};

// Longest a received message waits for its terrain fence before it is
// applied anyway.
#define BUILD_SYNC_FENCE_TIMEOUT_MS 3000

// A remote placement the engine refuses (no new building) is retried on the
// following frames up to this many times before it is dropped.
#define BUILD_SYNC_MAX_APPLY_ATTEMPTS 3

void BuildSync_Bind(const TsmHost* host);

// Sends a local placement, or queues it while disconnected. Game thread only.
void BuildSync_SendPlacement(const BuildSyncMsg& msg);

// Sends a local demolition (hooks/demolish_hook). Game thread only.
void BuildSync_SendDemolish(const float pos[3], int category, const unsigned long long args[5]);

// True while a remote placement or demolition is being replayed.
bool BuildSync_IsApplyingRemote();

// Per-frame (game thread, Present hook): sends queued placements and applies
// received ones once the world is loaded.
void BuildSync_Tick();

// Connection control (UI / save-sync flow).
void BuildSync_StartHost();
void BuildSync_StartClient(const char* hostIp = nullptr); // nullptr = keep last IP

// Closes an active connection. Cannot interrupt an in-flight accept/connect.
void BuildSync_Reset();

bool BuildSync_IsConnected();
bool BuildSync_IsConnecting();
bool BuildSync_IsHostRole();
const char* BuildSync_GetTargetIp();
