// See build_sync.h.
#include "build_sync.h"
#include "network.h"
#include "../hooks/detect_spawn.h" // g_origUniversal, UniversalFn
#include "../ui/debug_log.h" // building sync is a dev/test feature - status goes to the Debug window's log, not the main one
#include <vector>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#pragma comment(lib, "User32.lib")

// Declared up here (rather than down by the other g_* statics further below)
// so BuildSync_Bind can initialize it - see the comment there.
static CRITICAL_SECTION g_queueLock;

void BuildSync_Bind(const TsmHost* host)
{
    TsmBind(host);
    // Initialized here rather than inside BuildSync_StartHost/StartClient so
    // the lock is ready regardless of which UI button (Host or Connect) is
    // pressed first.
    InitializeCriticalSection(&g_queueLock);
}

// Same idea as save_sync.cpp's BuildSyncStatus(), but feeding the Debug window's
// log (ui/debugui.cpp) instead of the main in-game log - building sync is
// still a rough dev/test feature (see ui/main_menu.cpp comments), the main
// log stays reserved for things the everyday user needs (connect, save
// loading, errors).
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

// Same fields detect_spawn.cpp's RunDebugSpawn() uses to drive a synthetic
// placement call - temporarily set the position, call the real function,
// restore it.
#define UNIVERSAL_OFFSET_X 0xf6c
#define UNIVERSAL_OFFSET_Y 0xf70
#define UNIVERSAL_OFFSET_Z 0xf74

// this+0xf94 is a float yaw angle (radians, same atan2(sin,cos) convention
// as the placed object's obj+0x340/obj+0x348), identified from a hexdump of
// thisPtr+0xf20..0xfc0 taken right before a real g_origUniversal call. It
// tracks the real placement's final rotation within 1-3 degrees (the small
// residual is preview-smoothing lag between snapshot and confirm, not
// measurement error). Setting it before the call lets the ENGINE'S OWN
// placement logic (collision, road/pipe/wire auto-connect, pathing) run
// against roughly the right orientation, instead of always defaulting to
// whatever it reads there normally - same idea as UNIVERSAL_OFFSET_X/Y/Z
// above, and same temp-set/call/restore pattern.
#define UNIVERSAL_OFFSET_ROTATION 0xf94

// The placement-manager "this" object is NOT heap-allocated and does NOT
// need to be captured from a local click. thisPtr always equals
// g_exeBase + 0x9D4F10, exactly - it lives at a fixed address inside the
// module's own static memory, same as every save/load DAT_... global read
// directly elsewhere in this codebase. This RVA also independently matches
// the stable "economy manager" pointer used for money payments (see
// wrsr-mp-hook-status-findings.md) - it's the same god-object, holding both
// economy and building-placement state. So: just compute it, no waiting on
// HookedUniversal to fire locally.
#define UNIVERSAL_THIS_RVA 0x9D4F10

// Same globals detect_spawn.cpp uses to tell whether the game world is
// actually loaded yet (begin/end of the city buildings vector). We don't
// want to call into the placement manager before the world exists - its
// fields would be garbage/uninitialized, even though its own address is
// always valid (it's static memory, always mapped).
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18
#define VEC_BUILDINGS_END_RVA 0x9E6A20

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

static volatile bool g_connected = false;
static volatile bool g_connecting = false; // an accept()/connect() attempt is in flight on its own thread
static volatile bool g_isHost = false;     // which role StartHost/StartClient last picked - only meaningful while busy
static SOCKET g_sock = INVALID_SOCKET;
static char g_targetIp[64] = BUILD_SYNC_TEST_HOST_IP; // client target - overridable from the UI, see BuildSync_StartClient
static std::vector<BuildSyncMsg> g_incoming;

static void CloseConnection()
{
    g_connected = false;
    NetworkCloseSocket(g_sock);
    g_sock = INVALID_SOCKET;
}

// Blocks waiting for BuildSyncMsg-s on the connection; pushes each one into
// the incoming queue for BuildSync_Tick() to apply on the game thread.
static DWORD WINAPI ReceiveLoopThread(LPVOID)
{
    while (g_connected) {
        BuildSyncMsg msg = {};
        if (!NetworkRecvExact(g_sock, &msg, sizeof(msg))) {
            BuildSyncStatus("[BUILD_SYNC] connection lost");
            CloseConnection();
            break;
        }
        msg.typeName[sizeof(msg.typeName) - 1] = 0; // guard against a corrupt/unterminated string

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

    // Sends must time out - a network stall must not freeze the game thread
    // forever. Receives block indefinitely: builds are rare, the connection
    // just idles between them.
    DWORD sendTimeoutMs = 5000;
    DWORD recvTimeoutMs = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeoutMs, sizeof(sendTimeoutMs));
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeoutMs, sizeof(recvTimeoutMs));

    g_connected = true;
    CreateThread(nullptr, 0, ReceiveLoopThread, nullptr, 0, nullptr);
    BuildSyncStatus("[BUILD_SYNC] connection established - build away");
}

// accept()/connect() block for up to several minutes - run them on their own
// thread so the game thread (and the UI polling BuildSync_IsConnecting() for
// the "Waiting for client.../Connecting..." status text) stays responsive
// instead of freezing until one succeeds or times out.
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
    BuildSyncStatus("[BUILD_SYNC] CLIENT, connecting to host %s:%d...",
               g_targetIp, BUILD_SYNC_PORT);
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

// UI-driven (ui/debugui.cpp's Host##bs/Connect##bs buttons).

void BuildSync_StartHost()
{
    if (g_connected || g_connecting) return; // one attempt/connection at a time, for now
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

void BuildSync_SendPlacement(float x, float y, float z, float rotSin, float rotCos, const char* typeName)
{
    if (!g_connected) return;

    BuildSyncMsg msg = {};
    msg.x = x; msg.y = y; msg.z = z;
    msg.rotSin = rotSin; msg.rotCos = rotCos;
    strncpy_s(msg.typeName, typeName, _TRUNCATE);

    Logf("republicmp  BUILD_SYNC: sending type=[%s] x=%.2f y=%.2f z=%.2f rotSin=%.4f rotCos=%.4f",
         typeName, x, y, z, rotSin, rotCos);

    if (!NetworkSendExact(g_sock, &msg, sizeof(msg))) {
        BuildSyncStatus("[BUILD_SYNC] send failed, dropping connection");
        CloseConnection();
    }
}

// Isolated in its own function (no destructible locals) because __try/__except
// can't be used in a function that also has C++ objects needing unwinding -
// same pattern as detect_spawn.cpp's RunDebugSpawn().
//
// Uses the same buildings-vector-growth check as hooks/detect_spawn.cpp's
// RunDebugSpawn()/HookedUniversal, since g_origUniversal doesn't return a
// status code: this is the only way to tell whether a remote placement was
// actually applied or silently rejected by the engine (collision, terrain,
// wrong confirm flags, etc.), and logs a real SUCCESS/FAILED verdict per
// remote placement rather than just "attempted".
static void ApplyOneRemotePlacement(__int64 thisPtr, const BuildSyncMsg& msg)
{
    __try {
        float* xField = (float*)(thisPtr + UNIVERSAL_OFFSET_X);
        float* yField = (float*)(thisPtr + UNIVERSAL_OFFSET_Y);
        float* zField = (float*)(thisPtr + UNIVERSAL_OFFSET_Z);
        float* rotField = (float*)(thisPtr + UNIVERSAL_OFFSET_ROTATION);

        float savedX = *xField;
        float savedY = *yField;
        float savedZ = *zField;
        float savedRot = *rotField;

        *xField = msg.x;
        *yField = msg.y;
        *zField = msg.z;
        // See the comment above UNIVERSAL_OFFSET_ROTATION - gives the
        // engine's own placement logic (collision, road-snap, pathing)
        // roughly the right orientation to work with, not just a cosmetic
        // fix applied after the fact.
        *rotField = atan2f(msg.rotSin, msg.rotCos);

        char typeNameCopy[64];
        strncpy_s(typeNameCopy, msg.typeName, _TRUNCATE);

        void** vecBeginPtr = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        void** vecEndPtr   = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        void* beginBefore = *vecBeginPtr;
        void* endBefore = *vecEndPtr;
        long long countBefore = (beginBefore && endBefore)
            ? (long long)(((uintptr_t)endBefore - (uintptr_t)beginBefore) / sizeof(void*))
            : -1;

        Logf("republicmp  BUILD_SYNC: applying remote placement type=[%s] x=%.2f y=%.2f z=%.2f",
             typeNameCopy, msg.x, msg.y, msg.z);

        // g_origUniversal, not HookedUniversal - this must not re-trigger
        // detection/broadcast, or a build would bounce back and forth forever.
        g_origUniversal(thisPtr, typeNameCopy, 0, 1, 0, 0xffffffff, 0, 1);

        void* beginAfter = *vecBeginPtr;
        void* endAfter = *vecEndPtr;
        long long countAfter = (beginAfter && endAfter)
            ? (long long)(((uintptr_t)endAfter - (uintptr_t)beginAfter) / sizeof(void*))
            : -1;
        long long countDelta = (countBefore >= 0 && countAfter >= 0) ? (countAfter - countBefore) : 0;

        if (countDelta > 0) {
            Logf("republicmp  BUILD_SYNC: remote placement SUCCESS type=[%s] (buildings %lld -> %lld)",
                 typeNameCopy, countBefore, countAfter);

            // UNIVERSAL_OFFSET_ROTATION above sets the real input field
            // BEFORE the call, so the engine's own placement logic sees the
            // right orientation - but that field only tracks the true angle
            // within 1-3 degrees (see the ROTIN_DIAG comment near
            // UNIVERSAL_OFFSET_ROTATION), so this exact patch stays too as
            // a safety net for a visually pixel-perfect match:
            // obj+0x340/obj+0x348 is the primary sin/cos pair
            // (sin^2+cos^2==1 holds across sampled placements);
            // obj+0x360/obj+0x368 and obj+0x380/obj+0x388 are two more
            // copies of the same pair (one swapped, one sin-negated) that
            // always mirror it, so all three need to be kept in sync or the
            // object's matrix would be internally inconsistent.
            if (endAfter) {
                void* newObjPtr = *((void**)endAfter - 1);
                if (newObjPtr) {
                    *(float*)((char*)newObjPtr + 0x340) = msg.rotSin;
                    *(float*)((char*)newObjPtr + 0x348) = msg.rotCos;
                    *(float*)((char*)newObjPtr + 0x360) = msg.rotCos;
                    *(float*)((char*)newObjPtr + 0x368) = msg.rotSin;
                    *(float*)((char*)newObjPtr + 0x380) = -msg.rotSin;
                    *(float*)((char*)newObjPtr + 0x388) = msg.rotCos;
                    Logf("republicmp  BUILD_SYNC: applied rotation sin=%.4f cos=%.4f to obj=%p",
                         msg.rotSin, msg.rotCos, newObjPtr);
                }
            }
        } else {
            // The engine silently refused it (collision, terrain, missing
            // prerequisite, wrong confirm flags for this building's category,
            // etc.) - g_origUniversal doesn't return a status code we've
            // found, so vector growth is the only way to tell. If this shows
            // up for a specific building type, that type's placement call
            // needs different param_3..param_8 flags than the ones hardcoded
            // below (0,1,0,0xffffffff,0,1 - only ever confirmed against
            // "residental_brick_1" via F9's debug spawn).
            Logf("republicmp  BUILD_SYNC: remote placement FAILED (no new building) type=[%s] x=%.2f y=%.2f z=%.2f",
                 typeNameCopy, msg.x, msg.y, msg.z);
        }

        *xField = savedX;
        *yField = savedY;
        *zField = savedZ;
        *rotField = savedRot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  BUILD_SYNC: <exception applying remote placement>");
    }
}

// Placements that arrived before the game world finished loading - held
// here instead of being dropped, and flushed as soon as it has. A rare edge
// case now that the placement-manager address is computed directly (see
// UNIVERSAL_THIS_RVA above) rather than waiting on a local click.
static std::vector<BuildSyncMsg> g_pending;
static bool g_warnedWorldNotLoaded = false;

void BuildSync_Tick()
{
    EnterCriticalSection(&g_queueLock);
    if (!g_incoming.empty()) {
        // Append rather than swap, so anything still waiting from a previous
        // tick (because the world wasn't loaded yet) isn't lost.
        g_pending.insert(g_pending.end(), g_incoming.begin(), g_incoming.end());
        g_incoming.clear();
    }
    LeaveCriticalSection(&g_queueLock);

    if (g_pending.empty()) return;

    if (!IsWorldLoaded()) {
        // Don't discard - just no world to place into yet (e.g. still on the
        // main menu). Log once so it doesn't spam every frame, keep queued.
        if (!g_warnedWorldNotLoaded) {
            Logf("republicmp  BUILD_SYNC: got %zu remote placement(s) but the world isn't loaded yet, holding",
                 g_pending.size());
            g_warnedWorldNotLoaded = true;
        }
        return;
    }

    if (g_warnedWorldNotLoaded) {
        Logf("republicmp  BUILD_SYNC: world loaded, applying %zu queued placement(s)", g_pending.size());
        g_warnedWorldNotLoaded = false;
    }

    __int64 thisPtr = GetPlacementManagerThisPtr();

    std::vector<BuildSyncMsg> toApply;
    toApply.swap(g_pending);
    for (const BuildSyncMsg& msg : toApply) {
        ApplyOneRemotePlacement(thisPtr, msg);
    }
}
