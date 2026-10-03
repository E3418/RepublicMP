// See detect_spawn.h.
#include "detect_spawn.h"
#include "present_hook.h"
#include "game_speed.h"
#include "window_focus.h"
#include "../net/build_sync.h"
#include "../net/road_sync.h"
#include "demolish_hook.h" // DemolishHook_InDemolish
#include "../ui/sync_block.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "User32.lib")

void DetectSpawn_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ---------------------------------------------------------------------------
// Hook targets
// ---------------------------------------------------------------------------

const unsigned char kUniversalExpect[21] = {
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xD8,
    0x53, 0xFF, 0xFF
};

// Ends right after `lea rbp,[rax-0x4e8]`, so the trampoline never splits an
// instruction.
const unsigned char kRoadCommitExpect[19] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x8D, 0xA8, 0x18, 0xFB, 0xFF, 0xFF
};

UniversalFn g_origUniversal = nullptr;
RoadCommitFn g_origRoadCommit = nullptr;

// ---------------------------------------------------------------------------
// Engine addresses and offsets
// ---------------------------------------------------------------------------

#define UNIVERSAL_THIS_RVA 0x9D4F10      // placement god-object (thisPtr)
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18 // city buildings vector
#define VEC_BUILDINGS_END_RVA 0x9E6A20
#define HOVER_PORT_BUILDING_RVA 0x9E45B0 // building whose connection port is hovered
#define HOVER_PORT_INDEX_RVA 0x9E45B8

// thisPtr fields.
#define CURSOR_X_OFFSET 0xf6c            // placement cursor
#define CURSOR_Y_OFFSET 0xf70
#define CURSOR_Z_OFFSET 0xf74
#define CURSOR_YAW_OFFSET 0xf94          // placement yaw (radians)
#define PLACEMENT_RESULT_OFFSET 0x11a10  // validation code after a placement call
#define MIRROR_EFFECTIVE_OFFSET 0x11db1  // byte: mirror state the last placement call used
#define TOOL_DRAG_ACTIVE_OFFSET 0xd2ac   // byte: a path-tool drag is in progress

// Building object fields.
#define OBJ_ROT_SIN_OFFSET 0x340
#define OBJ_ROT_COS_OFFSET 0x348
#define OBJ_POS_OFFSET 0x350             // float x,y,z

// Tool manager fields (see net/road_sync.cpp for the full list).
#define MGR_CURVE_OFFSET 0x48
#define MGR_VALID_OFFSET 0x315
#define MGR_UPG_SEGMENT_OFFSET 0x380
#define MGR_UPG_RANGE_START_OFFSET 0x388
#define MGR_UPG_RANGE_END_OFFSET 0x38c
#define MGR_UPG_GATE_OFFSET 0x390
#define MGR_UPG_OLD_TYPE_OFFSET 0x394
#define MGR_UPG_OLD_VARIANT_OFFSET 0x398
#define MGR_UPG_DIR_OFFSET 0x39c
#define MGR_UPG_MULTI_OFFSET 0x3d0
#define MGR_UPG_PATH_INDEX_OFFSET 0x3d8
#define MGR_UPG_PATHS_BEGIN_OFFSET 0x3f0
#define MGR_UPG_PATHS_END_OFFSET 0x3f8
#define MGR_ROUTE_BEGIN_OFFSET 0x410
#define MGR_ROUTE_END_OFFSET 0x418
#define MGR_START_NODE_OFFSET 0x428
#define MGR_END_NODE_OFFSET 0x430
#define MGR_START_PORT_BUILDING_OFFSET 0x350
#define MGR_START_PORT_INDEX_OFFSET 0x358
#define MGR_TYPE_OFFSET 0x528
#define MGR_VARIANT_OFFSET 0x52c
#define MGR_FLAG_5C4_OFFSET 0x5c4
#define MGR_END_SNAPPED_NODE_OFFSET 0x740
#define MGR_END_ATTACHED_OFFSET 0x749
#define ROUTE_RECORD_SIZE 0x18

// Segment object fields.
#define SEG_POINTS_BEGIN_OFFSET 0x08
#define SEG_POINTS_END_OFFSET 0x10
#define SEG_NODE0_OFFSET 0x20
#define SEG_NODE1_OFFSET 0x28
#define SEG_TYPE_OFFSET 0x120
#define SEG_VARIANT_OFFSET 0x124
#define SEG_SKIP_FLAG_OFFSET 0x129       // commit skips segments with this set
#define SEG_KIND_OFFSET 300              // commit skips kinds 2 and 3

#define NODE_POS_OFFSET 4

// Demolish/cancel tools use type ids >= 100 (e.g. road_destroy).
#define TOOL_TYPE_DEMOLISH_MIN 100

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

// Number of elements in the buildings vector, -1 if unreadable.
static long long BuildingCount(void* begin, void* end)
{
    return (begin && end) ? (long long)(((uintptr_t)end - (uintptr_t)begin) / sizeof(void*)) : -1;
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

static volatile long long g_universalCallCount = 0;
static volatile long long g_universalMultiPushCount = 0;

DWORD WINAPI HeartbeatThread(LPVOID)
{
    long long lastUniversalCalls = 0;
    long long lastPresentCalls = 0;
    for (;;) {
        Sleep(2000);
        long long curUniversalCalls = g_universalCallCount;
        long long curPresentCalls = g_presentCallCount;
        Logf("republicmp  heartbeat: universalCalls total=%lld (+%lld) universalMultiPush total=%lld presentCalls total=%lld (+%lld) gameSpeed=%d gameSpeedSaved=%d",
             curUniversalCalls, curUniversalCalls - lastUniversalCalls,
             g_universalMultiPushCount,
             curPresentCalls, curPresentCalls - lastPresentCalls,
             ReadGameSpeed(), ReadGameSpeedSaved());
        lastUniversalCalls = curUniversalCalls;
        lastPresentCalls = curPresentCalls;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Building type name helpers
// ---------------------------------------------------------------------------

// Copies the ASCII building-type name passed to the placement function.
static void SafeReadCStringDirect(const char* ptr, char* outBuf, size_t outBufSize)
{
    outBuf[0] = 0;
    __try {
        if (!ptr) {
            strncpy(outBuf, "<null>", outBufSize - 1);
            return;
        }
        size_t len = 0;
        while (len < outBufSize - 1) {
            char c = ptr[len];
            if (c == 0) break;
            if (c < 0x20 || c >= 0x7F) {
                strncpy(outBuf, "<non-ascii>", outBufSize - 1);
                return;
            }
            len++;
        }
        strncpy(outBuf, ptr, len);
        outBuf[len] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strncpy(outBuf, "<unreadable>", outBufSize - 1);
    }
}

static bool LooksLikeValidType(const char* s)
{
    return s[0] != '<' && strlen(s) >= 2;
}

// ---------------------------------------------------------------------------
// F9 debug spawn
// ---------------------------------------------------------------------------

#define DEBUG_SPAWN_VK VK_F9
#define DEBUG_SPAWN_POLL_MS 50
#define DEBUG_SPAWN_TEST_TYPE_NAME "residental_brick_1"
#define DEBUG_SPAWN_OFFSET_METERS_X 5.0f

static volatile LONG g_debugSpawnPending = 0;

// Places a test building 5m from the current cursor through the real
// placement function. Game thread only.
static void RunDebugSpawn(__int64 thisPtr)
{
    __try {
        float* xField = (float*)(thisPtr + CURSOR_X_OFFSET);
        float savedX = *xField;
        *xField = savedX + DEBUG_SPAWN_OFFSET_METERS_X;

        char typeName[] = DEBUG_SPAWN_TEST_TYPE_NAME;
        void** vecBeginPtr = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        void** vecEndPtr   = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        long long countBefore = BuildingCount(*vecBeginPtr, *vecEndPtr);

        g_origUniversal(thisPtr, typeName, 0, 1, 0, 0xffffffff, 0, 1);

        long long countAfter = BuildingCount(*vecBeginPtr, *vecEndPtr);
        int resultCode = *(int*)(thisPtr + PLACEMENT_RESULT_OFFSET);
        Logf("republicmp  DEBUG_SPAWN: type=[%s] buildings %lld -> %lld result=%d",
             typeName, countBefore, countAfter, resultCode);

        *xField = savedX;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  DEBUG_SPAWN: <exception during synthetic placement>");
    }
}

DWORD WINAPI DebugSpawnThread(LPVOID)
{
    bool keyWasDown = false;
    for (;;) {
        Sleep(DEBUG_SPAWN_POLL_MS);
        bool keyIsDown = IsGameWindowFocused() && (GetAsyncKeyState(DEBUG_SPAWN_VK) & 0x8000) != 0;
        if (keyIsDown && !keyWasDown) {
            InterlockedExchange(&g_debugSpawnPending, 1);
            Logf("republicmp  DEBUG_SPAWN: request queued (F9)");
        }
        keyWasDown = keyIsDown;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Infrastructure commit capture
// ---------------------------------------------------------------------------

// >0 while a building placement call is running.
static volatile LONG g_buildingPlacementDepth = 0;
void DetectSpawn_BeginBuildingPlacement() { InterlockedIncrement(&g_buildingPlacementDepth); }
void DetectSpawn_EndBuildingPlacement()   { InterlockedDecrement(&g_buildingPlacementDepth); }
bool DetectSpawn_InBuildingPlacement()    { return InterlockedCompareExchange(&g_buildingPlacementDepth, 0, 0) != 0; }

// Captured inside the commit hook, sent from DetectSpawn_Tick. The hook runs
// inside the game's commit and must not allocate, log or do network I/O.
static RoadSyncSegment g_segmentCapture;
static volatile LONG g_segmentPending = 0;
static RoadSyncUpgradeMsg g_upgradeCapture;
static volatile LONG g_upgradePending = 0;

// Reason the last tool commit was not captured, logged from the game thread.
static volatile LONG g_commitSkipPending = 0;
static struct { int category; const char* reason; int count; int valid; int type; } g_commitSkip;

static void NoteCommitSkip(int category, const char* reason, int count, int valid, int type)
{
    if (InterlockedCompareExchange(&g_commitSkipPending, 0, 0) != 0) return;
    g_commitSkip.category = category;
    g_commitSkip.reason = reason;
    g_commitSkip.count = count;
    g_commitSkip.valid = valid;
    g_commitSkip.type = type;
    InterlockedExchange(&g_commitSkipPending, 1);
}

static void ReadNodePos(__int64 node, float out[3])
{
    for (int k = 0; k < 3; k++) out[k] = *(float*)(node + NODE_POS_OFFSET + k * 4);
}

// Segment end nodes: start = manager+0x428; end = the existing node the end
// snapped to (manager+0x740 when +0x749 == 1), else the temp end node +0x430.
// Caller wraps in __try.
static bool ReadSegmentEndNodes(__int64 manager, float outStart[3], float outEnd[3])
{
    __int64 startNode = *(__int64*)(manager + MGR_START_NODE_OFFSET);
    __int64 endNode = 0;
    if (*(unsigned char*)(manager + MGR_END_ATTACHED_OFFSET) == 1)
        endNode = *(__int64*)(manager + MGR_END_SNAPPED_NODE_OFFSET);
    if (!endNode) endNode = *(__int64*)(manager + MGR_END_NODE_OFFSET);
    if (!startNode || !endNode) return false;
    ReadNodePos(startNode, outStart);
    ReadNodePos(endNode, outEnd);
    return true;
}

// Building connection port at one segment end. Caller wraps in __try.
static void ReadBuildingPort(__int64 building, int portIndex, RoadSyncBuildingPort* out)
{
    out->valid = building ? 1 : 0;
    if (!building) return;
    out->portIndex = portIndex;
    for (int k = 0; k < 3; k++) out->bldPos[k] = *(float*)(building + OBJ_POS_OFFSET + k * 4);
}

// Identifies an existing segment by its nodes. Caller wraps in __try.
static bool FillUpgradePiece(__int64 seg, RoadSyncUpgradePiece* p)
{
    if (!seg) return false;
    __int64 n0 = *(__int64*)(seg + SEG_NODE0_OFFSET);
    __int64 n1 = *(__int64*)(seg + SEG_NODE1_OFFSET);
    if (!n0 || !n1) return false;
    ReadNodePos(n0, p->segStart);
    ReadNodePos(n1, p->segEnd);
    p->segPointCount = (int)((*(__int64*)(seg + SEG_POINTS_END_OFFSET) -
                              *(__int64*)(seg + SEG_POINTS_BEGIN_OFFSET)) / ROUTE_RECORD_SIZE);
    return true;
}

// Captures an upgrade commit (manager+0x390 != 0): either one segment over a
// point range (+0x3d0 == 0), or every segment of the pathfound list the
// commit walks (+0x3f0[+0x3d8], same filter as FUN_140518a80 lines 349-414).
// Caller wraps in __try.
static void CaptureUpgrade(__int64 manager, int category)
{
    if (InterlockedCompareExchange(&g_upgradePending, 0, 0) != 0) return;

    RoadSyncUpgradeMsg m = {};
    m.category = category;
    m.newType = *(int*)(manager + MGR_TYPE_OFFSET);
    m.newVariant = *(int*)(manager + MGR_VARIANT_OFFSET);
    m.flag5c4 = *(int*)(manager + MGR_FLAG_5C4_OFFSET);
    if (m.newType >= TOOL_TYPE_DEMOLISH_MIN) return;
    int dirFlag = *(int*)(manager + MGR_UPG_DIR_OFFSET);

    if (*(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) == 0) {
        RoadSyncUpgradePiece& p = m.pieces[0];
        if (!FillUpgradePiece(*(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET), &p)) return;
        p.rangeStart = *(int*)(manager + MGR_UPG_RANGE_START_OFFSET);
        p.rangeEnd = *(int*)(manager + MGR_UPG_RANGE_END_OFFSET);
        p.oldType = *(int*)(manager + MGR_UPG_OLD_TYPE_OFFSET);
        p.oldVariant = *(int*)(manager + MGR_UPG_OLD_VARIANT_OFFSET);
        p.flag39c = dirFlag;
        m.pieceCount = 1;
    } else {
        int idx = *(int*)(manager + MGR_UPG_PATH_INDEX_OFFSET);
        char* listBegin = *(char**)(manager + MGR_UPG_PATHS_BEGIN_OFFSET);
        char* listEnd = *(char**)(manager + MGR_UPG_PATHS_END_OFFSET);
        if (idx < 0 || !listBegin || idx >= (int)((listEnd - listBegin) / 0x18)) return;
        __int64* segIt = *(__int64**)(listBegin + idx * 0x18);
        __int64* segEnd = *(__int64**)(listBegin + idx * 0x18 + 8);
        __int64 segs[ROAD_SYNC_MAX_UPGRADE_PIECES];
        int n = 0;
        for (; segIt && segIt < segEnd && n < ROAD_SYNC_MAX_UPGRADE_PIECES; ++segIt) {
            __int64 seg = *segIt;
            if (!seg || *(char*)(seg + SEG_SKIP_FLAG_OFFSET) != 0 ||
                (unsigned)(*(int*)(seg + SEG_KIND_OFFSET) - 2) <= 1)
                continue;
            if (!FillUpgradePiece(seg, &m.pieces[n])) continue;
            m.pieces[n].rangeStart = -1;
            m.pieces[n].rangeEnd = m.pieces[n].segPointCount;
            m.pieces[n].oldType = *(int*)(seg + SEG_TYPE_OFFSET);
            m.pieces[n].oldVariant = *(int*)(seg + SEG_VARIANT_OFFSET);
            segs[n++] = seg;
        }
        // One-way direction per segment: the commit derives it from a path
        // bitset; approximated here from how each segment chains to its
        // neighbour (forward -> 1, reversed -> 2).
        for (int i = 0; i < n; i++) {
            if (dirFlag != 1) { m.pieces[i].flag39c = dirFlag; continue; }
            __int64 a0 = *(__int64*)(segs[i] + SEG_NODE0_OFFSET), a1 = *(__int64*)(segs[i] + SEG_NODE1_OFFSET);
            bool forward = true;
            if (i + 1 < n) {
                __int64 b0 = *(__int64*)(segs[i + 1] + SEG_NODE0_OFFSET), b1 = *(__int64*)(segs[i + 1] + SEG_NODE1_OFFSET);
                forward = (a1 == b0 || a1 == b1);
            } else if (i > 0) {
                __int64 b0 = *(__int64*)(segs[i - 1] + SEG_NODE0_OFFSET), b1 = *(__int64*)(segs[i - 1] + SEG_NODE1_OFFSET);
                forward = (a0 == b0 || a0 == b1);
            }
            m.pieces[i].flag39c = forward ? 1 : 2;
        }
        m.pieceCount = n;
    }
    if (m.pieceCount < 1) return;

    g_upgradeCapture = m;
    MemoryBarrier();
    InterlockedExchange(&g_upgradePending, 1);
}

// Captures a finished segment: interior route records, both node positions
// and building ports. Caller wraps in __try.
static void CaptureSegment(__int64 manager, int category)
{
    char* begin = *(char**)(manager + MGR_ROUTE_BEGIN_OFFSET);
    char* end = *(char**)(manager + MGR_ROUTE_END_OFFSET);
    // A never-allocated route is (null, null) = 0 records - valid for a short
    // power-line span.
    if (!begin != !end || end < begin) {
        NoteCommitSkip(category, "route buffer pointers inconsistent", -1, -1, -1);
        return;
    }
    size_t bytes = (size_t)(end - begin);
    int count = (int)(bytes / ROUTE_RECORD_SIZE);
    int type = *(int*)(manager + MGR_TYPE_OFFSET);
    unsigned char valid = *(unsigned char*)(manager + MGR_VALID_OFFSET);

    if (bytes % ROUTE_RECORD_SIZE != 0 || count > ROAD_SYNC_MAX_POINTS) {
        NoteCommitSkip(category, "route buffer size invalid or too long", count, valid, type);
        return;
    }
    if (valid != 1) {
        NoteCommitSkip(category, "engine marked the route invalid", count, valid, type);
        return;
    }
    if (type >= TOOL_TYPE_DEMOLISH_MIN) {
        NoteCommitSkip(category, "demolish tool (not synced)", count, valid, type);
        return;
    }
    if (InterlockedCompareExchange(&g_segmentPending, 0, 0) != 0) {
        NoteCommitSkip(category, "previous capture not sent yet", count, valid, type);
        return;
    }

    RoadSyncSegment& seg = g_segmentCapture;
    if (!ReadSegmentEndNodes(manager, seg.startNode, seg.endNode)) {
        NoteCommitSkip(category, "end nodes unreadable", count, valid, type);
        return;
    }
    seg.category = category;
    seg.typeId = type;
    seg.variant = *(int*)(manager + MGR_VARIANT_OFFSET);
    seg.curveValue = *(float*)(manager + MGR_CURVE_OFFSET);
    seg.pointCount = count;
    memcpy(seg.points, begin, bytes);
    ReadBuildingPort(*(__int64*)(manager + MGR_START_PORT_BUILDING_OFFSET),
                     *(int*)(manager + MGR_START_PORT_INDEX_OFFSET), &seg.startPort);
    ReadBuildingPort(*(__int64*)((uintptr_t)g_exe + HOVER_PORT_BUILDING_RVA),
                     *(int*)((uintptr_t)g_exe + HOVER_PORT_INDEX_RVA), &seg.endPort);

    MemoryBarrier();
    InterlockedExchange(&g_segmentPending, 1);
}

// Entry of FUN_140518a80. Captures tool commits, then always runs the
// original unchanged.
void HookedRoadCommit(__int64 manager,
                      unsigned __int64 p2, unsigned __int64 p3,
                      unsigned __int64 p4, unsigned int p5,
                      unsigned __int64 p6, unsigned __int64 p7)
{
    int category = RoadSync_CategoryForManager(GetPlacementManagerThisPtr(), manager);
    // Tool wrappers always commit with p2 == 0; replays of remote work are
    // never echoed back.
    // Commits inside a demolition are its own side effects - the demolition
    // itself is synced (hooks/demolish_hook).
    if (category >= 0 && p2 == 0 && !RoadSync_IsApplyingSegment() && !DemolishHook_InDemolish()) {
        if (InterlockedCompareExchange(&g_buildingPlacementDepth, 0, 0) != 0) {
            NoteCommitSkip(category, "laid by a building placement (recreated by the building replay)", -1, -1, -1);
        } else {
            __try {
                if (*(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) != 0)
                    CaptureUpgrade(manager, category);
                else
                    CaptureSegment(manager, category);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // Nothing may escape into the game's commit.
            }
        }
    }
    g_origRoadCommit(manager, p2, p3, p4, p5, p6, p7);
}

// Sends whatever the commit hook captured since the last frame.
static void FlushCommitCaptures()
{
    if (InterlockedCompareExchange(&g_commitSkipPending, 0, 0) != 0) {
        Logf("republicmp  ROAD_COMMIT: %s commit not sent - %s (points=%d valid=%d type=%d)",
             RoadSync_GetCategoryInfo(g_commitSkip.category)->name, g_commitSkip.reason,
             g_commitSkip.count, g_commitSkip.valid, g_commitSkip.type);
        InterlockedExchange(&g_commitSkipPending, 0);
    }

    if (InterlockedCompareExchange(&g_segmentPending, 0, 0) != 0) {
        const RoadSyncSegment& seg = g_segmentCapture;
        Logf("republicmp  ROAD_COMMIT: %s type=%d variant=%d points=%d start=(%.3f,%.3f,%.3f) end=(%.3f,%.3f,%.3f) startPort=%d endPort=%d",
             RoadSync_GetCategoryInfo(seg.category)->name, seg.typeId, seg.variant, seg.pointCount,
             seg.startNode[0], seg.startNode[1], seg.startNode[2],
             seg.endNode[0], seg.endNode[1], seg.endNode[2],
             seg.startPort.valid ? seg.startPort.portIndex : -1,
             seg.endPort.valid ? seg.endPort.portIndex : -1);
        RoadSync_SendSegment(seg);
        InterlockedExchange(&g_segmentPending, 0);
    }

    if (InterlockedCompareExchange(&g_upgradePending, 0, 0) != 0) {
        RoadSyncUpgradeMsg up = g_upgradeCapture;
        InterlockedExchange(&g_upgradePending, 0);
        Logf("republicmp  ROAD_COMMIT: upgrade %s newType=%d newVariant=%d pieces=%d",
             RoadSync_GetCategoryInfo(up.category)->name, up.newType, up.newVariant, up.pieceCount);
        RoadSync_SendUpgrade(up);
    }
}

// ---------------------------------------------------------------------------
// Per-frame tick
// ---------------------------------------------------------------------------

static bool g_toolDragActive = false;

bool DetectSpawn_IsToolDragActive()
{
    return g_toolDragActive;
}

void DetectSpawn_Tick()
{
    if (!IsWorldLoaded()) return;

    __int64 thisPtr = GetPlacementManagerThisPtr();
    __try {
        g_toolDragActive = *(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_toolDragActive = false;
    }

    FlushCommitCaptures();

    if (InterlockedExchange(&g_debugSpawnPending, 0) != 0)
        RunDebugSpawn(thisPtr);
}

// ---------------------------------------------------------------------------
// Building placement hook
// ---------------------------------------------------------------------------

// Entry of FUN_1402bade0. Runs the original, and if it created buildings,
// sends the placement (see detect_spawn.h).
void HookedUniversal(__int64 param_1, char* param_2, char param_3, char param_4,
                      unsigned __int64 param_5, unsigned int param_6,
                      char param_7, char param_8)
{
    // Placements are refused while the save-sync block screen is up.
    if (SyncBlock_IsActive()) return;

    g_universalCallCount++;

    char typeName[64];
    SafeReadCStringDirect(param_2, typeName, sizeof(typeName));

    void** vecBeginPtr = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
    void** vecEndPtr   = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
    void* beginBefore = nullptr;
    void* endBefore = nullptr;
    float cursorX = 0, cursorY = 0, cursorZ = 0, cursorYaw = 0;
    __try {
        beginBefore = *vecBeginPtr;
        endBefore = *vecEndPtr;
        // Placement inputs of this exact call - replayed as-is by the receiver.
        cursorX = *(float*)(param_1 + CURSOR_X_OFFSET);
        cursorY = *(float*)(param_1 + CURSOR_Y_OFFSET);
        cursorZ = *(float*)(param_1 + CURSOR_Z_OFFSET);
        cursorYaw = *(float*)(param_1 + CURSOR_YAW_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    DetectSpawn_BeginBuildingPlacement();
    __try {
        g_origUniversal(param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8);
    } __finally {
        DetectSpawn_EndBuildingPlacement();
    }

    __try {
        void* beginAfter = *vecBeginPtr;
        void* endAfter = *vecEndPtr;
        long long countBefore = BuildingCount(beginBefore, endBefore);
        long long countAfter = BuildingCount(beginAfter, endAfter);
        if (countBefore < 0 || countAfter <= countBefore) return; // preview frame or refused

        long long countDelta = countAfter - countBefore;
        if (countDelta > 1) g_universalMultiPushCount++;

        // The main object is the one created on the cursor; helper objects
        // of a multi-push sit elsewhere.
        void** vec = (void**)beginAfter;
        void* mainObj = nullptr;
        float bestD2 = 0.0f;
        for (long long i = countBefore; i < countAfter; i++) {
            void* obj = vec[i];
            if (!obj) continue;
            float dx = *(float*)((char*)obj + OBJ_POS_OFFSET) - cursorX;
            float dz = *(float*)((char*)obj + OBJ_POS_OFFSET + 8) - cursorZ;
            float d2 = dx * dx + dz * dz;
            if (!mainObj || d2 < bestD2) { mainObj = obj; bestD2 = d2; }
        }
        if (!mainObj) return;

        BuildSyncMsg msg = {};
        msg.x = *(float*)((char*)mainObj + OBJ_POS_OFFSET);
        msg.y = *(float*)((char*)mainObj + OBJ_POS_OFFSET + 4);
        msg.z = *(float*)((char*)mainObj + OBJ_POS_OFFSET + 8);
        msg.rotSin = *(float*)((char*)mainObj + OBJ_ROT_SIN_OFFSET);
        msg.rotCos = *(float*)((char*)mainObj + OBJ_ROT_COS_OFFSET);
        msg.cursorX = cursorX;
        msg.cursorY = cursorY;
        msg.cursorZ = cursorZ;
        msg.cursorRot = cursorYaw;
        msg.param5 = (unsigned long long)param_5;
        msg.mirrored = *(unsigned char*)(param_1 + MIRROR_EFFECTIVE_OFFSET) != 0;
        strncpy_s(msg.typeName, LooksLikeValidType(typeName) ? typeName : "<unknown>", _TRUNCATE);

        Logf("republicmp  BUILD: placed type=[%s] objects=%lld pos=(%.2f,%.2f,%.2f) rotDeg=%.2f mirrored=%d cursor=(%.2f,%.2f,%.2f)",
             msg.typeName, countDelta, msg.x, msg.y, msg.z,
             atan2(msg.rotSin, msg.rotCos) * 180.0 / 3.14159265358979323846,
             msg.mirrored, cursorX, cursorY, cursorZ);

        BuildSync_SendPlacement(msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  BUILD: <exception reading the new building>");
    }
}
