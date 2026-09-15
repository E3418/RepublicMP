// See road_sync.h.
#include "road_sync.h"
#include "network.h"
#include "../ui/debug_log.h" // road sync is a dev/test feature - status goes to the Debug window's log, not the main one
#include "../hooks/detect_spawn.h" // DetectSpawn_IsToolDragActive() - see RoadSync_Tick's local-drag-collision guard
#include <vector>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "User32.lib")

static CRITICAL_SECTION g_queueLock;

void RoadSync_Bind(const TsmHost* host)
{
    TsmBind(host);
    InitializeCriticalSection(&g_queueLock);
}

static void RoadSyncStatus(const char* fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    Logf("republicmp  ROAD_SYNC: %s", body);
    DebugLog_Add(body);
}

// Same fixed placement-manager address every other module uses (detect_spawn.cpp,
// build_sync.cpp) - not heap-allocated, lives at a fixed static address, always
// valid to compute even before the world is loaded (its FIELDS aren't valid
// that early, but the address itself is - see IsWorldLoaded() below).
#define UNIVERSAL_THIS_RVA 0x9D4F10

// Same offsets detect_spawn.cpp uses (thisPtr+0xf7a0/+0xf858 - pointers to
// the road/pipe drag "manager" objects the three primitives below operate
// on). Duplicated here rather than shared, per this codebase's existing
// convention (each .cpp keeps its own copies of these small constants).
#define ROAD_MANAGER_OFFSET 0xf7a0
#define PIPE_MANAGER_OFFSET 0xf858

#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18
#define VEC_BUILDINGS_END_RVA 0x9E6A20

static __int64 GetWorldThisPtr()
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

// The three engine primitives a real mouse drag uses, called directly here
// instead of being hooked. Signatures and the "safe" constant args below
// come straight from Ghidra decompiles of all three functions plus the
// road/pipe tool-select wrappers (FUN_1403d00f0/FUN_1403b0e60) that call
// them during a real drag. Summary of what's confirmed vs assumed:
//
// - FUN_140518a80 (commit): confirmed simple. Every real commit call in both
//   wrapper decompiles passes manager,0,0,0,0,0,0 - copied verbatim here.
//
// - FUN_1404fcaf0 (per-waypoint tick): confirmed simple in the steady
//   "still dragging" case. Wrapper passes manager,&point,typeId,variant,
//   0,0,0,0,0,0 - copied verbatim (params 5-10 are genuinely zero in that
//   branch, not just masked-off garbage - traced through the wrapper's own
//   local var inits).
//
// - FUN_1404fc040 (drag start): the deep one - ~300 lines, does existing-
//   infrastructure-node snapping/lookup and allocates two 0xe8-byte objects
//   internally. The pipe wrapper's OWN first-drag-frame call is our model,
//   copied as closely as possible: manager, startPoint, 0, 0(exact-match
//   snap mode - NOT fuzzy, so it won't casually auto-snap to something a
//   floating-point coincidence made "close enough"), 0xffffffec (sentinel
//   "no explicit type" seen in the wrapper's own call), 0(no explicit
//   existing-node handle to attach to - safe default, this codebase has no
//   way to know the CLIENT's own local node handles even if the HOST snapped
//   to one), and a zeroed secondary point (no direction hint - matches what
//   the wrapper itself passes on a fresh drag start). FUN_1404fbd70 (which
//   the wrappers always call right after fc040) is a pure sound-effect
//   dispatcher (identifies the manager by comparing it against a table of
//   fixed tool-manager addresses and plays "tool_road_start"/etc.) -
//   deliberately NOT called here, it has no effect on game state.
//
// NOT YET LIVE-VERIFIED end to end - see detect_spawn.cpp's F11 self-test
// hotkey, which calls RoadSync_ApplySegment() directly (no network, no
// second machine) specifically to test this before trusting it with real
// remote data.
typedef void (*RoadFc040Fn)(__int64 manager, float* startPoint, unsigned __int64 param3,
                            char param4, int param5, unsigned __int64 param6, float* param7);
typedef void (*RoadFcaf0Fn)(__int64 manager, float* point, int typeId, unsigned int variant,
                            char p5, char p6, char p7, __int64 p8, char p9, char p10);
typedef void (*RoadCommitFn)(__int64 manager, unsigned __int64 p2, unsigned __int64 p3,
                             unsigned __int64 p4, unsigned int p5, unsigned __int64 p6,
                             unsigned __int64 p7);

#define ROAD_PRIM_FC040_RVA 0x4FC040
#define ROAD_PRIM_FCAF0_RVA 0x4FCAF0
#define ROAD_PRIM_COMMIT_RVA 0x518A80

static RoadFc040Fn RoadPrim_Fc040()  { return (RoadFc040Fn)((uintptr_t)g_exe + ROAD_PRIM_FC040_RVA); }
static RoadFcaf0Fn RoadPrim_Fcaf0()  { return (RoadFcaf0Fn)((uintptr_t)g_exe + ROAD_PRIM_FCAF0_RVA); }
static RoadCommitFn RoadPrim_Commit(){ return (RoadCommitFn)((uintptr_t)g_exe + ROAD_PRIM_COMMIT_RVA); }

// Isolated in its own function (no destructible locals) - same __try/__except
// constraint as build_sync.cpp's ApplyOneRemotePlacement.
void RoadSync_ApplySegment(int category, int typeId, int variant,
                           const float points[][3], int pointCount, float curveValue)
{
    if (pointCount < 2 || pointCount > ROAD_SYNC_MAX_POINTS) {
        Logf("republicmp  ROAD_SYNC: refusing to apply segment, pointCount=%d out of [2,%d]",
             pointCount, ROAD_SYNC_MAX_POINTS);
        return;
    }

    __try {
        __int64 thisPtr = GetWorldThisPtr();
        __int64 managerOffset = (category == ROAD_SYNC_CATEGORY_PIPE) ? PIPE_MANAGER_OFFSET : ROAD_MANAGER_OFFSET;
        __int64 manager = *(__int64*)(thisPtr + managerOffset);
        if (!manager) {
            Logf("republicmp  ROAD_SYNC: apply failed, local manager handle is null (category=%d)", category);
            return;
        }

        Logf("republicmp  ROAD_SYNC: applying segment category=%d type=%d variant=%d points=%d curve=%.3f start=(%.2f,%.2f,%.2f) end=(%.2f,%.2f,%.2f)",
             category, typeId, variant, pointCount, curveValue,
             points[0][0], points[0][1], points[0][2],
             points[pointCount - 1][0], points[pointCount - 1][1], points[pointCount - 1][2]);

        float startPoint[3] = { points[0][0], points[0][1], points[0][2] };
        float zeroPoint[3] = { 0.0f, 0.0f, 0.0f };
        RoadPrim_Fc040()(manager, startPoint, 0, 0, (int)0xffffffec, 0, zeroPoint);

        // Restore the curve accumulator AFTER fc040 (drag start), BEFORE
        // calling fcaf0. FUN_1404fc040 does its own manager setup for a
        // fresh drag and resets/defaults this field, so without this write
        // fcaf0 always routes with curve≈0/straight regardless of what the
        // original drag used.
        __try {
            *(float*)(manager + 0x48) = curveValue;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Logf("republicmp  ROAD_SYNC: <exception writing curve accumulator, continuing with default>");
        }

        // fcaf0 is called ONCE, with the FINAL point only, not once per
        // captured waypoint. FUN_1404fcaf0 is called every FRAME of a real
        // drag with the single CURRENT cursor point, and each call
        // RECOMPUTES THE WHOLE ROUTE from the drag start to that one point
        // (bounded by the curve accumulator) - it does not append a
        // segment. The captured points[] array is the OUTPUT of that last
        // recompute (read off manager+0x410..0x418 at commit), not a list
        // of successive cursor positions. Looping fcaf0 over every captured
        // point would replay the router's own output back into it as if
        // each point were a fresh, independent cursor target: on a curved
        // path those intermediate points are NOT a straight sequence moving
        // away from the start, so each call would re-route a full curved
        // path to a point that could be "behind" or "beside" the previous
        // call's route, producing a self-intersecting zigzag/spiral. On a
        // straight segment (curve≈0) this wouldn't be visible, since a
        // route to ANY point on a straight chord is a subset of the route
        // to the far end - order-independent. Single-call replay reproduces
        // the same shape a real drag's own LAST frame produces (start ->
        // current cursor point, curve already set above).
        float endPoint[3] = { points[pointCount - 1][0], points[pointCount - 1][1], points[pointCount - 1][2] };
        RoadPrim_Fcaf0()(manager, endPoint, typeId, (unsigned int)variant, 0, 0, 0, 0, 0, 0);

        RoadPrim_Commit()(manager, 0, 0, 0, 0, 0, 0);

        Logf("republicmp  ROAD_SYNC: apply DONE category=%d type=%d variant=%d points=%d curve=%.3f - check in-game whether it actually built",
             category, typeId, variant, pointCount, curveValue);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  ROAD_SYNC: <exception applying segment>");
    }
}

static volatile bool g_connected = false;
static volatile bool g_connecting = false;
static volatile bool g_isHost = false;
static SOCKET g_sock = INVALID_SOCKET;
static char g_targetIp[64] = ROAD_SYNC_TEST_HOST_IP;
static std::vector<RoadSyncMsg> g_incoming;

static void CloseConnection()
{
    g_connected = false;
    NetworkCloseSocket(g_sock);
    g_sock = INVALID_SOCKET;
}

static DWORD WINAPI ReceiveLoopThread(LPVOID)
{
    while (g_connected) {
        RoadSyncMsg msg = {};
        if (!NetworkRecvExact(g_sock, &msg, sizeof(msg))) {
            RoadSyncStatus("[ROAD_SYNC] connection lost");
            CloseConnection();
            break;
        }

        if (msg.pointCount < 2 || msg.pointCount > ROAD_SYNC_MAX_POINTS) {
            Logf("republicmp  ROAD_SYNC: received message with bad pointCount=%d, dropping", msg.pointCount);
            continue;
        }

        EnterCriticalSection(&g_queueLock);
        g_incoming.push_back(msg);
        LeaveCriticalSection(&g_queueLock);

        Logf("republicmp  ROAD_SYNC: received category=%d type=%d variant=%d points=%d curve=%.3f",
             msg.category, msg.typeId, msg.variant, msg.pointCount, msg.curveValue);
    }
    return 0;
}

static void StartConnection(SOCKET sock)
{
    g_sock = sock;

    DWORD sendTimeoutMs = 5000;
    DWORD recvTimeoutMs = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeoutMs, sizeof(sendTimeoutMs));
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeoutMs, sizeof(recvTimeoutMs));

    g_connected = true;
    CreateThread(nullptr, 0, ReceiveLoopThread, nullptr, 0, nullptr);
    RoadSyncStatus("[ROAD_SYNC] connection established");
}

static DWORD WINAPI HostAcceptWorkerThread(LPVOID)
{
    RoadSyncStatus("[ROAD_SYNC] HOST, waiting for a client on port %d...", ROAD_SYNC_PORT);
    if (!NetworkInit()) {
        RoadSyncStatus("[ROAD_SYNC] network init failed, aborting");
        g_connecting = false;
        return 0;
    }
    SOCKET sock = NetworkHostAcceptOne(ROAD_SYNC_PORT, 300000, 30000);
    if (sock == INVALID_SOCKET) {
        RoadSyncStatus("[ROAD_SYNC] no client connected within 5 min, aborting - press Host##rs again to retry");
        g_connecting = false;
        return 0;
    }
    RoadSyncStatus("[ROAD_SYNC] client connected");
    StartConnection(sock);
    g_connecting = false;
    return 0;
}

static DWORD WINAPI ClientConnectWorkerThread(LPVOID)
{
    RoadSyncStatus("[ROAD_SYNC] CLIENT, connecting to host %s:%d...", g_targetIp, ROAD_SYNC_PORT);
    if (!NetworkInit()) {
        RoadSyncStatus("[ROAD_SYNC] network init failed, aborting");
        g_connecting = false;
        return 0;
    }
    SOCKET sock = NetworkConnect(g_targetIp, ROAD_SYNC_PORT, 15000, 30000);
    if (sock == INVALID_SOCKET) {
        RoadSyncStatus("[ROAD_SYNC] couldn't connect - is the host listening (Host##rs)? Is the IP right? - press Connect##rs again to retry");
        g_connecting = false;
        return 0;
    }
    RoadSyncStatus("[ROAD_SYNC] connected to host");
    StartConnection(sock);
    g_connecting = false;
    return 0;
}

void RoadSync_StartHost()
{
    if (g_connected || g_connecting) return;
    g_isHost = true;
    g_connecting = true;
    CreateThread(nullptr, 0, HostAcceptWorkerThread, nullptr, 0, nullptr);
}

void RoadSync_StartClient(const char* hostIp)
{
    if (g_connected || g_connecting) return;
    if (hostIp && hostIp[0]) {
        strncpy_s(g_targetIp, hostIp, _TRUNCATE);
    }
    g_isHost = false;
    g_connecting = true;
    CreateThread(nullptr, 0, ClientConnectWorkerThread, nullptr, 0, nullptr);
}

void RoadSync_Reset()
{
    if (g_connected) {
        RoadSyncStatus("[ROAD_SYNC] connection reset by user");
        CloseConnection();
    }
}

bool RoadSync_IsConnected()  { return g_connected; }
bool RoadSync_IsConnecting() { return g_connecting; }
bool RoadSync_IsHostRole()   { return g_isHost; }
const char* RoadSync_GetTargetIp() { return g_targetIp; }

void RoadSync_SendSegment(int category, int typeId, int variant,
                          const float points[][3], int pointCount, float curveValue)
{
    if (!g_connected) return;
    if (pointCount < 2 || pointCount > ROAD_SYNC_MAX_POINTS) {
        Logf("republicmp  ROAD_SYNC: refusing to send segment, pointCount=%d out of [2,%d]",
             pointCount, ROAD_SYNC_MAX_POINTS);
        return;
    }

    RoadSyncMsg msg = {};
    msg.category = category;
    msg.typeId = typeId;
    msg.variant = variant;
    msg.pointCount = pointCount;
    msg.curveValue = curveValue;
    for (int i = 0; i < pointCount; i++) {
        msg.points[i][0] = points[i][0];
        msg.points[i][1] = points[i][1];
        msg.points[i][2] = points[i][2];
    }

    Logf("republicmp  ROAD_SYNC: sending category=%d type=%d variant=%d points=%d curve=%.3f",
         category, typeId, variant, pointCount, curveValue);

    if (!NetworkSendExact(g_sock, &msg, sizeof(msg))) {
        RoadSyncStatus("[ROAD_SYNC] send failed, dropping connection");
        CloseConnection();
    }
}

// Segments that arrived before the game world finished loading - held here
// instead of dropped, same pattern as build_sync.cpp's g_pending.
static std::vector<RoadSyncMsg> g_pending;
static bool g_warnedWorldNotLoaded = false;

void RoadSync_Tick()
{
    EnterCriticalSection(&g_queueLock);
    if (!g_incoming.empty()) {
        g_pending.insert(g_pending.end(), g_incoming.begin(), g_incoming.end());
        g_incoming.clear();
    }
    LeaveCriticalSection(&g_queueLock);

    if (g_pending.empty()) return;

    if (!IsWorldLoaded()) {
        if (!g_warnedWorldNotLoaded) {
            Logf("republicmp  ROAD_SYNC: got %zu remote segment(s) but the world isn't loaded yet, holding",
                 g_pending.size());
            g_warnedWorldNotLoaded = true;
        }
        return;
    }

    if (g_warnedWorldNotLoaded) {
        Logf("republicmp  ROAD_SYNC: world loaded, applying %zu queued segment(s)", g_pending.size());
        g_warnedWorldNotLoaded = false;
    }

    // Don't apply into a manager the LOCAL player is actively dragging on.
    // RoadSync_ApplySegment drives FUN_1404fc040/fcaf0 directly on the same
    // road/pipe manager object a real local drag uses - applying a remote
    // segment while the local player is mid-drag makes their own drag's
    // snap point jump to the REMOTE segment's point, and leaves the
    // manager's curve accumulator stuck at its max bound for several drags
    // afterward. Just wait - don't pop anything off g_pending this tick,
    // try again next frame; segments already queued stay queued (no data
    // loss, just delayed application until the local player releases their
    // own drag).
    if (DetectSpawn_IsToolDragActive()) {
        return;
    }

    std::vector<RoadSyncMsg> toApply;
    toApply.swap(g_pending);
    for (size_t i = 0; i < toApply.size(); i++) {
        // Re-check every iteration, not just once before the loop - the
        // local player could start a NEW drag in between two segments in a
        // multi-segment batch (unlikely within one tick, but free to guard).
        if (DetectSpawn_IsToolDragActive()) {
            g_pending.insert(g_pending.end(), toApply.begin() + i, toApply.end());
            break;
        }
        const RoadSyncMsg& msg = toApply[i];
        RoadSync_ApplySegment(msg.category, msg.typeId, msg.variant, msg.points, msg.pointCount, msg.curveValue);
    }
}
