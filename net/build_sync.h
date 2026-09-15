// Building-placement network sync: a persistent TCP connection (separate
// from save_sync's one-shot file transfer) that relays "a building was
// placed here" between host and client. Symmetric for now - whichever side
// places a building sends it, the other side applies it locally via the
// game's own placement function. No authority checks yet.

#pragma once

#include "tesmio_plugin.h"
#include "../mp_config.h" 

#define BUILD_SYNC_PORT 5557
#define BUILD_SYNC_TEST_HOST_IP REPUBLICMP_HOST_IP

// rotSin/rotCos: yaw sin/cos pair - see the rotation comments in detect_spawn.cpp/build_sync.cpp for where these come from and how they're used.
struct BuildSyncMsg {
    float x, y, z;
    float rotSin, rotCos;
    char typeName[64];
};

void BuildSync_Bind(const TsmHost* host);

// Called from detect_spawn.cpp when a LOCAL placement is detected. No-op if no build-sync connection is active.
void BuildSync_SendPlacement(float x, float y, float z, float rotSin, float rotCos, const char* typeName);

// Called every frame from the Present hook (game thread). Applies any
// placements received from the network since the last call.
void BuildSync_Tick();

// UI-driven connection control (see ui/main_menu.cpp)
void BuildSync_StartHost();

// hostIp: pass nullptr (or empty) to keep using whatever IP was last set
// (BUILD_SYNC_TEST_HOST_IP / mp_config.h by default); pass a real IP string
// to override it for this and future calls.
void BuildSync_StartClient(const char* hostIp = nullptr);

// Force-closes an active connection so Host/Connect can be pressed again.
// NOTE: cannot interrupt an in-flight accept()/connect() - if called while
// still "Connecting...", that blocking call just runs to its own timeout.
void BuildSync_Reset();

bool BuildSync_IsConnected();
bool BuildSync_IsConnecting();
bool BuildSync_IsHostRole(); // true after BuildSync_StartHost(), false after BuildSync_StartClient() - meaningless while fully Idle
const char* BuildSync_GetTargetIp(); // current/default client target IP, for prefilling a UI field
