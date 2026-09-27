// See build_sync.h for the overall design.
#include "build_sync.h"
#include "network.h"
#include "../hooks/detect_spawn.h" // g_origUniversal, DetectSpawn_Begin/EndBuildingPlacement
#include "../ui/debug_log.h"
#include <vector>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#pragma comment(lib, "User32.lib")

// ---------------------------------------------------------------------------
// Engine addresses and offsets
// ---------------------------------------------------------------------------

#define UNIVERSAL_THIS_RVA 0x9D4F10      // placement god-object (thisPtr)
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18 // city buildings vector
#define VEC_BUILDINGS_END_RVA 0x9E6A20

// thisPtr placement inputs.
#define CURSOR_X_OFFSET 0xf6c
#define CURSOR_Y_OFFSET 0xf70
#define CURSOR_Z_OFFSET 0xf74
#define CURSOR_COPY_OFFSET 0xf78         // mirrors the cursor x/y/z
#define CURSOR_YAW_OFFSET 0xf94          // yaw in radians
#define MIRROR_TOGGLE_OFFSET 0x11db0     // byte: player's mirror toggle
#define PLACEMENT_RESULT_OFFSET 0x11a10  // 3 ints: validation result of the last call
#define TOOL_GUARD1_OFFSET 0xfbc         // any nonzero -> placement function returns at once
#define TOOL_GUARD2_OFFSET 0xfbe
#define TOOL_GUARD3_OFFSET 0xfc3

#define RETRY_DELAY_MS 500

// Building object: yaw as three consistent sin/cos pairs, and position.
#define OBJ_ROT_SIN_OFFSET 0x340
#define OBJ_ROT_COS_OFFSET 0x348
#define OBJ_ROT2_COS_OFFSET 0x360        // second pair: cos, sin
#define OBJ_ROT2_SIN_OFFSET 0x368
#define OBJ_ROT3_NSIN_OFFSET 0x380       // third pair: -sin, cos
#define OBJ_ROT3_COS_OFFSET 0x388
#define OBJ_POS_OFFSET 0x350

// ---------------------------------------------------------------------------
// Module state and helpers
// ---------------------------------------------------------------------------

static CRITICAL_SECTION g_queueLock;

void BuildSync_Bind(const TsmHost* host)
{
    TsmBind(host);
    InitializeCriticalSection(&g_queueLock);
}

// Logs to the file log and to the Debug window.
static void BuildSyncStatus(const char* fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    Logf("republicmp  BUILD_SYNC: %s", body);
    DebugLog_Add(body);
}

static __int64 GetPlacementManagerThisPtr()
{
    return (__int64)((uintptr_t)g_exe + UNIVERSAL_THIS_RVA);
}

static bool IsWorldLoaded()
{
    void** vecBeginPtr = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
    void** vecEndPtr   = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
    __try {
        return (*vecBeginPtr != nullptr) && (*vecEndPtr != nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static long long BuildingCount(void* begin, void* end)
{
    return (begin && end) ? (long long)(((uintptr_t)end - (uintptr_t)begin) / sizeof(void*)) : -1;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

static volatile bool g_connected = false;
static volatile bool g_connecting = false;
static volatile bool g_isHost = false;
static SOCKET g_sock = INVALID_SOCKET;
static char g_targetIp[64] = BUILD_SYNC_TEST_HOST_IP;
static std::vector<BuildSyncMsg> g_incoming; // receive thread -> game thread

static void CloseConnection()
{
    g_connected = false;
    NetworkCloseSocket(g_sock);
    g_sock = INVALID_SOCKET;
}

static DWORD WINAPI ReceiveLoopThread(LPVOID)
{
    while (g_connected) {
        BuildSyncMsg msg = {};
        if (!NetworkRecvExact(g_sock, &msg, sizeof(msg))) {
            BuildSyncStatus("[BUILD_SYNC] connection lost");
            CloseConnection();
            break;
        }
        msg.typeName[sizeof(msg.typeName) - 1] = 0;

        EnterCriticalSection(&g_queueLock);
        g_incoming.push_back(msg);
        LeaveCriticalSection(&g_queueLock);

        Logf("republicmp  BUILD_SYNC: received type=[%s] x=%.2f y=%.2f z=%.2f",
             msg.typeName, msg.x, msg.y, msg.z);
    }
    return 0;
}

static void StartConnection(SOCKET sock)
{
    g_sock = sock;

    // Sends time out so a stalled peer cannot freeze the game thread;
    // receives block on the dedicated thread.
    DWORD sendTimeoutMs = 5000;
    DWORD recvTimeoutMs = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeoutMs, sizeof(sendTimeoutMs));
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeoutMs, sizeof(recvTimeoutMs));

    g_connected = true;
    CreateThread(nullptr, 0, ReceiveLoopThread, nullptr, 0, nullptr);
    BuildSyncStatus("[BUILD_SYNC] connection established - build away");
}

static DWORD WINAPI HostAcceptWorkerThread(LPVOID)
{
    BuildSyncStatus("[BUILD_SYNC] HOST, waiting for a client on port %d...", BUILD_SYNC_PORT);
    if (!NetworkInit()) {
        BuildSyncStatus("[BUILD_SYNC] network init failed, aborting");
        g_connecting = false;
        return 0;
    }
    SOCKET sock = NetworkHostAcceptOne(BUILD_SYNC_PORT, 300000, 30000);
    if (sock == INVALID_SOCKET) {
        BuildSyncStatus("[BUILD_SYNC] no client connected within 5 min, aborting - press Host##bs again to retry");
        g_connecting = false;
        return 0;
    }
    BuildSyncStatus("[BUILD_SYNC] client connected");
    StartConnection(sock);
    g_connecting = false;
    return 0;
}

static DWORD WINAPI ClientConnectWorkerThread(LPVOID)
{
    BuildSyncStatus("[BUILD_SYNC] CLIENT, connecting to host %s:%d...", g_targetIp, BUILD_SYNC_PORT);
    if (!NetworkInit()) {
        BuildSyncStatus("[BUILD_SYNC] network init failed, aborting");
        g_connecting = false;
        return 0;
    }
    SOCKET sock = NetworkConnect(g_targetIp, BUILD_SYNC_PORT, 15000, 30000);
    if (sock == INVALID_SOCKET) {
        BuildSyncStatus("[BUILD_SYNC] couldn't connect - is the host listening (Host##bs)? Is the IP right? - press Connect##bs again to retry");
        g_connecting = false;
        return 0;
    }
    BuildSyncStatus("[BUILD_SYNC] connected to host");
    StartConnection(sock);
    g_connecting = false;
    return 0;
}

void BuildSync_StartHost()
{
    if (g_connected || g_connecting) return;
    g_isHost = true;
    g_connecting = true;
    CreateThread(nullptr, 0, HostAcceptWorkerThread, nullptr, 0, nullptr);
}

void BuildSync_StartClient(const char* hostIp)
{
    if (g_connected || g_connecting) return;
    if (hostIp && hostIp[0]) {
        strncpy_s(g_targetIp, hostIp, _TRUNCATE);
    }
    g_isHost = false;
    g_connecting = true;
    CreateThread(nullptr, 0, ClientConnectWorkerThread, nullptr, 0, nullptr);
}

void BuildSync_Reset()
{
    if (g_connected) {
        BuildSyncStatus("[BUILD_SYNC] connection reset by user");
        CloseConnection();
    }
}

bool BuildSync_IsConnected()  { return g_connected; }
bool BuildSync_IsConnecting() { return g_connecting; }
bool BuildSync_IsHostRole()   { return g_isHost; }
const char* BuildSync_GetTargetIp() { return g_targetIp; }

// ---------------------------------------------------------------------------
// Sending (game thread only - the only thread that writes to the socket)
// ---------------------------------------------------------------------------

static std::vector<BuildSyncMsg> g_outbound; // placements waiting for a connection

static void SendNow(const BuildSyncMsg& msg)
{
    Logf("republicmp  BUILD_SYNC: sending type=[%s] obj=(%.2f,%.2f,%.2f) cursor=(%.2f,%.2f,%.2f) cursorRot=%.4f p5=0x%llx",
         msg.typeName, msg.x, msg.y, msg.z, msg.cursorX, msg.cursorY, msg.cursorZ, msg.cursorRot, msg.param5);
    if (!NetworkSendExact(g_sock, &msg, sizeof(msg))) {
        BuildSyncStatus("[BUILD_SYNC] send failed, dropping connection");
        CloseConnection();
    }
}

void BuildSync_SendPlacement(const BuildSyncMsg& msg)
{
    if (!g_connected || !g_outbound.empty()) {
        g_outbound.push_back(msg);
        Logf("republicmp  BUILD_SYNC: queued type=[%s] (%zu waiting for connection)", msg.typeName, g_outbound.size());
        return;
    }
    SendNow(msg);
}

static void FlushOutbound()
{
    while (g_connected && !g_outbound.empty()) {
        SendNow(g_outbound.front());
        if (!g_connected) break; // send failed - keep it for the next connection
        g_outbound.erase(g_outbound.begin());
    }
}

// ---------------------------------------------------------------------------
// Applying received placements (game thread only)
// ---------------------------------------------------------------------------

// The new object sitting on the placement cursor among vecBegin[from, to).
// Caller wraps in __try.
static void* PickMainNewObject(void** vecBegin, long long from, long long to, float cx, float cz)
{
    void* best = nullptr;
    float bestD2 = 0.0f;
    for (long long i = from; i < to; i++) {
        void* obj = vecBegin[i];
        if (!obj) continue;
        float dx = *(float*)((char*)obj + OBJ_POS_OFFSET) - cx;
        float dz = *(float*)((char*)obj + OBJ_POS_OFFSET + 8) - cz;
        float d2 = dx * dx + dz * dz;
        if (!best || d2 < bestD2) { best = obj; bestD2 = d2; }
    }
    return best;
}

// True while the placement function would return without doing anything.
static bool PlacementGuardBusy(__int64 thisPtr)
{
    __try {
        return *(unsigned char*)(thisPtr + TOOL_GUARD1_OFFSET) ||
               *(unsigned char*)(thisPtr + TOOL_GUARD2_OFFSET) ||
               *(unsigned char*)(thisPtr + TOOL_GUARD3_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Replays one placement through the engine's placement function with the
// sender's inputs, then applies the sender's exact rotation to the main new
// object. The local player's cursor/yaw/mirror are restored even if the call
// throws. Returns true if a building was created.
static bool ApplyOneRemotePlacement(__int64 thisPtr, const BuildSyncMsg& msg, int attempt)
{
    bool placed = false;
    __try {
        float* cursor = (float*)(thisPtr + CURSOR_X_OFFSET);
        float* cursorCopy = (float*)(thisPtr + CURSOR_COPY_OFFSET);
        float* yaw = (float*)(thisPtr + CURSOR_YAW_OFFSET);
        unsigned char* mirror = (unsigned char*)(thisPtr + MIRROR_TOGGLE_OFFSET);
        float savedCursor[3] = { cursor[0], cursor[1], cursor[2] };
        float savedCopy[3] = { cursorCopy[0], cursorCopy[1], cursorCopy[2] };
        float savedYaw = *yaw;
        unsigned char savedMirror = *mirror;

        __try {
            cursor[0] = cursorCopy[0] = msg.cursorX;
            cursor[1] = cursorCopy[1] = msg.cursorY;
            cursor[2] = cursorCopy[2] = msg.cursorZ;
            *yaw = msg.cursorRot;
            *mirror = msg.mirrored ? 1 : 0;

            char typeName[64];
            strncpy_s(typeName, msg.typeName, _TRUNCATE);

            void** vecBeginPtr = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
            void** vecEndPtr   = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
            long long countBefore = BuildingCount(*vecBeginPtr, *vecEndPtr);

            Logf("republicmp  BUILD_SYNC: applying type=[%s] cursor=(%.2f,%.2f,%.2f) cursorRot=%.4f mirrored=%d attempt %d/%d",
                 typeName, msg.cursorX, msg.cursorY, msg.cursorZ, msg.cursorRot, msg.mirrored,
                 attempt, BUILD_SYNC_MAX_APPLY_ATTEMPTS);

            // The original (not the hook), so the placement is not sent back.
            // param_4/param_8 = 1 confirm the placement without a mouse click.
            DetectSpawn_BeginBuildingPlacement();
            __try {
                g_origUniversal(thisPtr, typeName, 0, 1, msg.param5, 0xffffffff, 0, 1);
            } __finally {
                DetectSpawn_EndBuildingPlacement();
            }

            void* beginAfter = *vecBeginPtr;
            long long countAfter = BuildingCount(beginAfter, *vecEndPtr);
            void* obj = (countBefore >= 0 && countAfter > countBefore)
                ? PickMainNewObject((void**)beginAfter, countBefore, countAfter, msg.cursorX, msg.cursorZ)
                : nullptr;
            if (!obj) {
                const int* result = (const int*)(thisPtr + PLACEMENT_RESULT_OFFSET);
                Logf("republicmp  BUILD_SYNC: placement refused by the engine type=[%s] result=(%d,%d,%d)",
                     typeName, result[0], result[1], result[2]);
            } else {
                placed = true;
                float ox = *(float*)((char*)obj + OBJ_POS_OFFSET);
                float oz = *(float*)((char*)obj + OBJ_POS_OFFSET + 8);
                *(float*)((char*)obj + OBJ_ROT_SIN_OFFSET) = msg.rotSin;
                *(float*)((char*)obj + OBJ_ROT_COS_OFFSET) = msg.rotCos;
                *(float*)((char*)obj + OBJ_ROT2_COS_OFFSET) = msg.rotCos;
                *(float*)((char*)obj + OBJ_ROT2_SIN_OFFSET) = msg.rotSin;
                *(float*)((char*)obj + OBJ_ROT3_NSIN_OFFSET) = -msg.rotSin;
                *(float*)((char*)obj + OBJ_ROT3_COS_OFFSET) = msg.rotCos;
                Logf("republicmp  BUILD_SYNC: placed type=[%s] objects=%lld, main object delta to sender %.3fm",
                     typeName, countAfter - countBefore,
                     sqrtf((ox - msg.x) * (ox - msg.x) + (oz - msg.z) * (oz - msg.z)));
            }
        } __finally {
            cursor[0] = savedCursor[0]; cursor[1] = savedCursor[1]; cursor[2] = savedCursor[2];
            cursorCopy[0] = savedCopy[0]; cursorCopy[1] = savedCopy[1]; cursorCopy[2] = savedCopy[2];
            *yaw = savedYaw;
            *mirror = savedMirror;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  BUILD_SYNC: <exception applying remote placement>");
    }
    return placed;
}

// A received placement waiting to be applied.
struct PendingPlacement {
    BuildSyncMsg msg;
    int attempts;               // failed attempts so far
    unsigned long long notBeforeMs; // GetTickCount64() of the next try
};
static std::vector<PendingPlacement> g_pending;
static bool g_warnedWorldNotLoaded = false;

void BuildSync_Tick()
{
    EnterCriticalSection(&g_queueLock);
    for (const BuildSyncMsg& msg : g_incoming) {
        PendingPlacement p = { msg, 0, 0 };
        g_pending.push_back(p);
    }
    g_incoming.clear();
    LeaveCriticalSection(&g_queueLock);

    FlushOutbound();

    if (g_pending.empty()) return;

    if (!IsWorldLoaded()) {
        if (!g_warnedWorldNotLoaded) {
            Logf("republicmp  BUILD_SYNC: %zu remote placement(s) held until the world is loaded", g_pending.size());
            g_warnedWorldNotLoaded = true;
        }
        return;
    }
    g_warnedWorldNotLoaded = false;

    // Placements stay queued while the placement function is blocked.
    __int64 thisPtr = GetPlacementManagerThisPtr();
    if (PlacementGuardBusy(thisPtr)) return;

    // Applied in arrival order; a refused placement is retried after
    // RETRY_DELAY_MS, and later ones wait behind it so order is kept.
    unsigned long long now = GetTickCount64();
    while (!g_pending.empty()) {
        PendingPlacement& p = g_pending.front();
        if (now < p.notBeforeMs) break;
        if (ApplyOneRemotePlacement(thisPtr, p.msg, p.attempts + 1)) {
            g_pending.erase(g_pending.begin());
            continue;
        }
        if (++p.attempts >= BUILD_SYNC_MAX_APPLY_ATTEMPTS) {
            BuildSyncStatus("[BUILD_SYNC] gave up on [%s] at (%.1f, %.1f) after %d attempts",
                            p.msg.typeName, p.msg.cursorX, p.msg.cursorZ, p.attempts);
            g_pending.erase(g_pending.begin());
            continue;
        }
        p.notBeforeMs = now + RETRY_DELAY_MS;
        break;
    }
}
