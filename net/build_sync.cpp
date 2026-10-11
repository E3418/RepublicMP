// See build_sync.h for the overall design.
#include "build_sync.h"
#include "network.h"
#include "../hooks/detect_spawn.h" // g_origUniversal, DetectSpawn_Begin/EndBuildingPlacement
#include "terrain_sync.h"            // TerrainSync_Begin/EndForeignReplay, fence
#include "../hooks/demolish_hook.h" // DemolishHook_CallOriginal
#include "../hooks/game_speed.h"    // GameSpeed_OnRemote
#include "save_sync.h"               // SaveSync_OnResyncAnnounced
#include "../ui/debug_log.h"
#include "../ui/sync_block.h"
#include <vector>
#include <memory>
#include <unordered_map>
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
#define OBJ_TYPE_OFFSET 0x318            // building type record
#define OBJ_PAY_RUB_OFFSET 0x608         // byte: finish construction with rubles
#define OBJ_PAY_USD_OFFSET 0x609         // byte: finish construction with dollars
#define SET_PAY_RVA 0x160550             // FUN_140160550(this, building, rub, usd)
#define PAY_SCAN_INTERVAL_MS 250
#define TYPE_CATEGORY_OFFSET 0x360       // int category in the type record

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
// Set by Reset to abandon a pending accept/connect (polled every ~200ms).
static volatile bool g_cancelConnect = false;
static volatile bool g_isHost = false;
static SOCKET g_sock = INVALID_SOCKET;
static char g_targetIp[64] = BUILD_SYNC_TEST_HOST_IP;
// One message plus, for BUILD_SYNC_KIND_ACCESS_ROAD, the segment after it.
struct BuildWireItem {
    BuildSyncMsg msg;
    std::shared_ptr<RoadSyncSegment> road;
};
static std::vector<BuildWireItem> g_incoming; // receive thread -> game thread

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

        // Noted right here, before this thread can see the connection close
        // that follows it, so the session never mistakes it for a drop.
        if (msg.kind == BUILD_SYNC_KIND_RESYNC) {
            Logf("republicmp  BUILD_SYNC: the host announced a re-sync");
            SaveSync_OnResyncAnnounced();
            continue;
        }

        BuildWireItem item;
        item.msg = msg;
        if (msg.kind == BUILD_SYNC_KIND_ACCESS_ROAD) {
            item.road = std::make_shared<RoadSyncSegment>();
            if (!NetworkRecvExact(g_sock, item.road.get(), sizeof(RoadSyncSegment))) {
                BuildSyncStatus("[BUILD_SYNC] connection lost");
                CloseConnection();
                break;
            }
            if (item.road->pointCount < 0 || item.road->pointCount > ROAD_SYNC_MAX_POINTS) {
                Logf("republicmp  BUILD_SYNC: access road with bad pointCount=%d, dropped", item.road->pointCount);
                continue;
            }
        }

        EnterCriticalSection(&g_queueLock);
        g_incoming.push_back(item);
        LeaveCriticalSection(&g_queueLock);

        if (msg.kind == BUILD_SYNC_KIND_PAY)
            Logf("republicmp  BUILD_SYNC: received pay at (%.2f,%.2f,%.2f) rub=%d usd=%d", msg.x, msg.y, msg.z,
                 msg.speed, msg.speedSaved);
        if (msg.kind == BUILD_SYNC_KIND_PLACE || msg.kind == BUILD_SYNC_KIND_DEMOLISH)
            Logf("republicmp  BUILD_SYNC: received %s type=[%s] x=%.2f y=%.2f z=%.2f fence=%d",
                 msg.kind == BUILD_SYNC_KIND_DEMOLISH ? "demolish" : "placement",
                 msg.typeName, msg.x, msg.y, msg.z, msg.terrainFence);
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
    SOCKET sock = NetworkHostAcceptOne(BUILD_SYNC_PORT, 300000, 30000, &g_cancelConnect);
    if (sock == INVALID_SOCKET && g_cancelConnect) {
        BuildSyncStatus("[BUILD_SYNC] stopped waiting for the connection");
        g_connecting = false;
        return 0;
    }
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
    SOCKET sock = NetworkConnect(g_targetIp, BUILD_SYNC_PORT, 15000, 30000, &g_cancelConnect);
    if (sock == INVALID_SOCKET && g_cancelConnect) {
        BuildSyncStatus("[BUILD_SYNC] stopped waiting for the connection");
        g_connecting = false;
        return 0;
    }
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
    g_cancelConnect = false;
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
    g_cancelConnect = false;
    g_connecting = true;
    CreateThread(nullptr, 0, ClientConnectWorkerThread, nullptr, 0, nullptr);
}

void BuildSync_Reset()
{
    if (g_connecting) g_cancelConnect = true;
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

static std::vector<BuildWireItem> g_outbound; // placements waiting for a connection

static volatile LONG g_applyingRemote = 0;

bool BuildSync_IsApplyingRemote()
{
    return g_applyingRemote != 0;
}

static void SendNow(const BuildWireItem& item)
{
    const BuildSyncMsg& msg = item.msg;
    if (msg.kind == BUILD_SYNC_KIND_ACCESS_ROAD) {
        if (!item.road || !NetworkSendExact(g_sock, &msg, sizeof(msg)) ||
            !NetworkSendExact(g_sock, item.road.get(), sizeof(RoadSyncSegment))) {
            BuildSyncStatus("[BUILD_SYNC] send failed, dropping connection");
            CloseConnection();
        }
        return;
    }
    if (msg.kind == BUILD_SYNC_KIND_SPEED_SET || msg.kind == BUILD_SYNC_KIND_SPEED_REQUEST ||
        msg.kind == BUILD_SYNC_KIND_SPEED_BOOST || msg.kind == BUILD_SYNC_KIND_TIME ||
        msg.kind == BUILD_SYNC_KIND_RESYNC) {
        if (!NetworkSendExact(g_sock, &msg, sizeof(msg))) {
            BuildSyncStatus("[BUILD_SYNC] send failed, dropping connection");
            CloseConnection();
        }
        return;
    }
    if (msg.kind == BUILD_SYNC_KIND_DEMOLISH) {
        Logf("republicmp  BUILD_SYNC: sending demolish at (%.2f,%.2f,%.2f) category=%d fence=%d",
             msg.x, msg.y, msg.z, msg.demolishCategory, msg.terrainFence);
        if (!NetworkSendExact(g_sock, &msg, sizeof(msg))) {
            BuildSyncStatus("[BUILD_SYNC] send failed, dropping connection");
            CloseConnection();
        }
        return;
    }
    char mode[64];
    Logf("republicmp  BUILD_SYNC: sending type=[%s] obj=(%.2f,%.2f,%.2f) cursor=(%.2f,%.2f,%.2f) cursorRot=%.4f p5=0x%llx pay=%s",
         msg.typeName, msg.x, msg.y, msg.z, msg.cursorX, msg.cursorY, msg.cursorZ, msg.cursorRot, msg.param5,
         BuildMode_Describe(msg.mode, mode, sizeof(mode)));
    if (!NetworkSendExact(g_sock, &msg, sizeof(msg))) {
        BuildSyncStatus("[BUILD_SYNC] send failed, dropping connection");
        CloseConnection();
    }
}

static void SendOrQueue(const BuildSyncMsg& msg, std::shared_ptr<RoadSyncSegment> road = nullptr)
{
    BuildWireItem item;
    item.msg = msg;
    item.road = road;
    if (!g_connected || !g_outbound.empty()) {
        g_outbound.push_back(item);
        Logf("republicmp  BUILD_SYNC: queued %s [%s] (%zu waiting for connection)",
             msg.kind == BUILD_SYNC_KIND_DEMOLISH ? "demolish" : "placement", msg.typeName, g_outbound.size());
        return;
    }
    SendNow(item);
}

void BuildSync_SendPlacement(const BuildSyncMsg& in)
{
    BuildSyncMsg msg = in;
    msg.kind = BUILD_SYNC_KIND_PLACE;
    msg.terrainFence = TerrainSync_GetFenceSeq();
    msg.mode = BuildMode_Read(); // same frame as the placement
    SendOrQueue(msg);
}

void BuildSync_SendSpeed(int kind, int speed, int speedSaved, int turbo)
{
    if (!g_connected) return;
    BuildSyncMsg msg = {};
    msg.kind = kind;
    strcpy_s(msg.typeName, "<speed>");
    msg.speed = speed;
    msg.speedSaved = speedSaved;
    msg.speedTurbo = turbo;
    // Behind queued placements, so order on the connection is kept.
    SendOrQueue(msg);
}

void BuildSync_SendTime(int day, int year, float progress)
{
    if (!g_connected) return;
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_TIME;
    strcpy_s(msg.typeName, "<time>");
    msg.speed = day;
    msg.speedSaved = year;
    msg.timeProgress = progress;
    SendOrQueue(msg);
}

void BuildSync_SendResync()
{
    if (!g_connected) return;
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_RESYNC;
    strcpy_s(msg.typeName, "<resync>");
    SendOrQueue(msg);
}

void BuildSync_SendAccessRoad(const RoadSyncSegment& seg)
{
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_ACCESS_ROAD;
    msg.terrainFence = TerrainSync_GetFenceSeq();
    msg.mode = BuildMode_Read();
    strcpy_s(msg.typeName, "<access road>");
    msg.x = seg.startNode[0]; msg.y = seg.startNode[1]; msg.z = seg.startNode[2];
    SendOrQueue(msg, std::make_shared<RoadSyncSegment>(seg));
}

void BuildSync_SendDemolish(const float pos[3], int category, const unsigned long long args[5])
{
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_DEMOLISH;
    msg.terrainFence = TerrainSync_GetFenceSeq();
    msg.x = pos[0]; msg.y = pos[1]; msg.z = pos[2];
    strcpy_s(msg.typeName, "<demolish>");
    msg.demolishCategory = category;
    for (int k = 0; k < 5; k++) msg.demolishArgs[k] = args[k];
    SendOrQueue(msg);
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

            BuildMode mine = BuildMode_Read();
            char theirsText[64], mineText[64];
            Logf("republicmp  BUILD_SYNC: applying type=[%s] cursor=(%.2f,%.2f,%.2f) cursorRot=%.4f mirrored=%d attempt %d/%d pay=%s (mine: %s)",
                 typeName, msg.cursorX, msg.cursorY, msg.cursorZ, msg.cursorRot, msg.mirrored,
                 attempt, BUILD_SYNC_MAX_APPLY_ATTEMPTS,
                 BuildMode_Describe(msg.mode, theirsText, sizeof(theirsText)),
                 BuildMode_Describe(mine, mineText, sizeof(mineText)));

            // The original (not the hook), so the placement is not sent back.
            // param_4/param_8 = 1 confirm the placement without a mouse click.
            // Terrain levelling under the building arrives over terrain sync.
            DetectSpawn_BeginBuildingPlacement();
            TerrainSync_BeginForeignReplay();
            InterlockedIncrement(&g_applyingRemote);
            // Built the way the sender chose; the local choice goes back after.
            BuildMode_Write(msg.mode);
            __try {
                g_origUniversal(thisPtr, typeName, 0, 1, msg.param5, 0xffffffff, 0, 1);
            } __finally {
                BuildMode_Write(mine);
                InterlockedDecrement(&g_applyingRemote);
                TerrainSync_EndForeignReplay();
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

// The local building at `pos` (0.5 m in x/z, 1 m in y) with this type
// category (-1 = any), nearest first; 0 if none.
static __int64 FindBuildingAt(float x, float y, float z, int category)
{
    __int64 found = 0;
    float bestD = 1e30f;
    __try {
        __int64* it = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        __int64* end = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        for (; it && it < end; ++it) {
            __int64 b = *it;
            if (!b) continue;
            const float* p = (const float*)(b + OBJ_POS_OFFSET);
            float dx = p[0] - x, dz = p[2] - z;
            if (fabsf(dx) >= 0.5f || fabsf(dz) >= 0.5f || fabsf(p[1] - y) >= 1.0f) continue;
            __int64 type = *(__int64*)(b + OBJ_TYPE_OFFSET);
            int cat = type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
            if (category >= 0 && cat != category) continue;
            float d = dx * dx + dz * dz;
            if (d < bestD) { bestD = d; found = b; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = 0;
    }
    return found;
}

// ---------------------------------------------------------------------------
// Pay-to-finish flags (see build_sync.h)
// ---------------------------------------------------------------------------

static std::unordered_map<__int64, unsigned char> g_payFlags; // building -> rub | usd << 1, as last seen
static ULONGLONG g_lastPayScanMs = 0;

// rub | usd << 1 of a building, or -1 if unreadable.
static int ReadPayFlags(__int64 b)
{
    __try {
        return (*(unsigned char*)(b + OBJ_PAY_RUB_OFFSET) ? 1 : 0) | (*(unsigned char*)(b + OBJ_PAY_USD_OFFSET) ? 2 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static bool ReadBuildingInfo(__int64 b, float pos[3], int* category)
{
    __try {
        const float* p = (const float*)(b + OBJ_POS_OFFSET);
        pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2];
        __int64 type = *(__int64*)(b + OBJ_TYPE_OFFSET);
        *category = type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static int SnapshotBuildings(__int64* out, int max)
{
    int n = 0;
    __try {
        __int64* it = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        __int64* end = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        for (; it && it < end && n < max; ++it)
            if (*it) out[n++] = *it;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

static void SendPay(__int64 b, int flags);

// Sends every building whose pay flag switched on since the last scan. A
// building seen for the first time is only recorded (a placement with money
// is already covered by the placement message).
static void ScanPayFlags()
{
    static std::vector<__int64> s_list;
    s_list.resize(65536);
    int n = SnapshotBuildings(s_list.data(), (int)s_list.size());

    std::unordered_map<__int64, unsigned char> next;
    next.reserve(n);
    for (int i = 0; i < n; i++) {
        __int64 b = s_list[i];
        int flags = ReadPayFlags(b);
        if (flags < 0) continue;
        auto old = g_payFlags.find(b);
        if (old != g_payFlags.end() && (flags & ~old->second) != 0) SendPay(b, flags);
        next[b] = (unsigned char)flags;
    }
    g_payFlags.swap(next);
}

static bool CallSetPay(__int64 thisPtr, __int64 b, int flags)
{
    typedef void (*SetPayFn)(__int64 thisPtr, __int64 building, unsigned char rub, unsigned char usd);
    __try {
        ((SetPayFn)((uintptr_t)g_exeBase + SET_PAY_RVA))(thisPtr, b, (flags & 1) ? 1 : 0, (flags & 2) ? 1 : 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static const char* PayName(int flags)
{
    return (flags & 3) == 3 ? "rubles+dollars" : (flags & 1) ? "rubles" : (flags & 2) ? "dollars" : "none";
}

static void SendPay(__int64 b, int flags)
{
    float pos[3];
    int category = -1;
    if (!ReadBuildingInfo(b, pos, &category)) return;
    BuildSyncMsg msg = {};
    msg.kind = BUILD_SYNC_KIND_PAY;
    msg.terrainFence = TerrainSync_GetFenceSeq();
    strcpy_s(msg.typeName, "<pay>");
    msg.x = pos[0]; msg.y = pos[1]; msg.z = pos[2];
    msg.demolishCategory = category;
    msg.speed = flags & 1;
    msg.speedSaved = (flags >> 1) & 1;
    Logf("republicmp  BUILD_SYNC: sending pay at (%.2f,%.2f,%.2f) category=%d - finish with %s",
         pos[0], pos[1], pos[2], category, PayName(flags));
    SendOrQueue(msg);
}

static void ApplyOneRemotePay(__int64 thisPtr, const BuildSyncMsg& msg)
{
    int flags = (msg.speed ? 1 : 0) | (msg.speedSaved ? 2 : 0);
    __int64 b = FindBuildingAt(msg.x, msg.y, msg.z, msg.demolishCategory);
    if (!b) {
        Logf("republicmp  BUILD_SYNC: pay at (%.2f,%.2f,%.2f) category=%d - no such building here, skipped",
             msg.x, msg.y, msg.z, msg.demolishCategory);
        return;
    }
    int before = ReadPayFlags(b);
    bool ok = true;
    if (before != flags) {
        TerrainSync_BeginForeignReplay();
        InterlockedIncrement(&g_applyingRemote);
        ok = CallSetPay(thisPtr, b, flags);
        InterlockedDecrement(&g_applyingRemote);
        TerrainSync_EndForeignReplay();
    }
    int after = ReadPayFlags(b);
    if (after >= 0) g_payFlags[b] = (unsigned char)after; // not a local change - do not send it back
    Logf("republicmp  BUILD_SYNC: pay at (%.2f,%.2f,%.2f) category=%d - finish with %s: %s (flags %s -> %s)",
         msg.x, msg.y, msg.z, msg.demolishCategory, PayName(flags),
         !ok ? "THREW" : before == flags ? "already set" : "applied",
         before < 0 ? "?" : PayName(before), after < 0 ? "?" : PayName(after));
}

// Replays a remote demolition on the local building at the sender's
// position. Returns false only if the replay threw; a building that is not
// there (already gone) counts as done.
static bool ApplyOneRemoteDemolish(__int64 thisPtr, const BuildSyncMsg& msg)
{
    __int64 found = FindBuildingAt(msg.x, msg.y, msg.z, msg.demolishCategory);
    if (!found) {
        Logf("republicmp  BUILD_SYNC: demolish at (%.2f,%.2f,%.2f) category=%d - no such building here (already gone), skipped",
             msg.x, msg.y, msg.z, msg.demolishCategory);
        return true;
    }

    TerrainSync_BeginForeignReplay();
    InterlockedIncrement(&g_applyingRemote);
    bool ok = DemolishHook_CallOriginal(thisPtr, found, msg.demolishArgs);
    InterlockedDecrement(&g_applyingRemote);
    TerrainSync_EndForeignReplay();

    // Removed at once, or (realistic mode) turned into a demolition site
    // that stays in the list until workers finish.
    bool stillListed = false;
    __try {
        __int64* it = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        __int64* end = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        for (; it && it < end; ++it)
            if (*it == found) { stillListed = true; break; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Logf("republicmp  BUILD_SYNC: demolish at (%.2f,%.2f,%.2f) category=%d args=[%llx,%llx,%llx,%llx,%llx] %s",
         msg.x, msg.y, msg.z, msg.demolishCategory,
         msg.demolishArgs[0] & 0xff, msg.demolishArgs[1] & 0xff, msg.demolishArgs[2] & 0xff,
         msg.demolishArgs[3] & 0xff, msg.demolishArgs[4] & 0xff,
         !ok ? "THREW" : stillListed ? "applied - building STILL in the list (demolition site?)" : "applied - building removed");
    return ok;
}

// A received placement/demolition waiting to be applied.
struct PendingPlacement {
    BuildSyncMsg msg;
    std::shared_ptr<RoadSyncSegment> road; // BUILD_SYNC_KIND_ACCESS_ROAD
    int attempts;               // failed attempts so far
    unsigned long long notBeforeMs; // GetTickCount64() of the next try
    unsigned long long fenceWaitStartMs; // 0 = not waiting for the terrain fence yet
};
static std::vector<PendingPlacement> g_pending;
static bool g_warnedWorldNotLoaded = false;

void BuildSync_Tick()
{
    EnterCriticalSection(&g_queueLock);
    std::vector<BuildSyncMsg> speedMsgs;
    for (const BuildWireItem& in : g_incoming) {
        const BuildSyncMsg& msg = in.msg;
        // Speed is not world state - applied at once, never behind a
        // placement waiting for its terrain or a retry.
        if (msg.kind == BUILD_SYNC_KIND_SPEED_SET || msg.kind == BUILD_SYNC_KIND_SPEED_REQUEST ||
            msg.kind == BUILD_SYNC_KIND_SPEED_BOOST || msg.kind == BUILD_SYNC_KIND_TIME) {
            speedMsgs.push_back(msg);
            continue;
        }
        PendingPlacement p = { msg, in.road, 0, 0, 0 };
        g_pending.push_back(p);
    }
    g_incoming.clear();
    LeaveCriticalSection(&g_queueLock);

    for (const BuildSyncMsg& m : speedMsgs) {
        if (m.kind == BUILD_SYNC_KIND_TIME) GameSpeed_OnRemoteTime(m.speed, m.speedSaved, m.timeProgress);
        else GameSpeed_OnRemote(m.kind, m.speed, m.speedSaved, m.speedTurbo);
    }

    FlushOutbound();

    // Pay flags: only while connected and in the world (the sync screen and
    // a fresh load would otherwise look like changes).
    if (g_connected && !SyncBlock_IsActive() && IsWorldLoaded()) {
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - g_lastPayScanMs >= PAY_SCAN_INTERVAL_MS) {
            g_lastPayScanMs = nowMs;
            ScanPayFlags();
        }
    } else {
        g_payFlags.clear();
    }

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
        if (!TerrainSync_FenceReached(p.msg.terrainFence)) {
            if (!p.fenceWaitStartMs) p.fenceWaitStartMs = now;
            if (now - p.fenceWaitStartMs < BUILD_SYNC_FENCE_TIMEOUT_MS) break;
            Logf("republicmp  BUILD_SYNC: terrain fence %d not reached after %dms - applying anyway",
                 p.msg.terrainFence, BUILD_SYNC_FENCE_TIMEOUT_MS);
        }
        if (p.msg.kind == BUILD_SYNC_KIND_DEMOLISH) {
            ApplyOneRemoteDemolish(thisPtr, p.msg);
            g_pending.erase(g_pending.begin());
            continue;
        }
        if (p.msg.kind == BUILD_SYNC_KIND_PAY) {
            ApplyOneRemotePay(thisPtr, p.msg);
            g_pending.erase(g_pending.begin());
            continue;
        }
        if (p.msg.kind == BUILD_SYNC_KIND_ACCESS_ROAD) {
            // Not into a manager the local player is dragging with.
            if (DetectSpawn_IsToolDragActive()) break;
            BuildMode mine = BuildMode_Read();
            BuildMode_Write(p.msg.mode);
            bool ok = p.road && RoadSync_ApplySegmentForBuild(*p.road);
            BuildMode_Write(mine);
            char theirsText[64], mineText[64];
            Logf("republicmp  BUILD_SYNC: access road at (%.2f,%.2f,%.2f) %s, pay=%s (mine: %s)",
                 p.msg.x, p.msg.y, p.msg.z, ok ? "built" : "REJECTED by the engine",
                 BuildMode_Describe(p.msg.mode, theirsText, sizeof(theirsText)),
                 BuildMode_Describe(mine, mineText, sizeof(mineText)));
            g_pending.erase(g_pending.begin());
            continue;
        }
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
