// See road_sync.h for the overall design.
#include "road_sync.h"
#include "network.h"
#include "../ui/debug_log.h"
#include "../hooks/detect_spawn.h" // DetectSpawn_IsToolDragActive, g_origRoadCommit
#include <vector>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "User32.lib")

// Set to 1 to log every transmitted/received point at full precision.
#define ROAD_SYNC_VERBOSE_LOG 0

// ---------------------------------------------------------------------------
// Engine addresses (RVAs into SOVIET64.exe) and object offsets
// ---------------------------------------------------------------------------

#define UNIVERSAL_THIS_RVA 0x9D4F10       // placement god-object (thisPtr)
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18  // city buildings vector
#define VEC_BUILDINGS_END_RVA 0x9E6A20

#define FC040_RVA 0x4FC040                // drag start
#define FCAF0_RVA 0x4FCAF0                // drag tick
#define COMMIT_RVA 0x518A80               // commit
#define VEC_ASSIGN_RVA 0x21D2B0           // std::vector<24-byte record>::operator=

#define LMB_HELD_RVA 0xA54E90             // byte: left mouse button held
#define LMB_JUST_PRESSED_RVA 0xA54E91     // byte: left mouse button pressed this frame
#define PRESET_CURVE_MODE_RVA 0x9E8088    // int: preset curve index, -1 = free curve

// Hover state the tools read from the local mouse.
static const unsigned int kHoverPtrRvas[] = { 0x9E45E8, 0x9E4560, 0x9E4558, 0x9E45E0, 0x9E45D8 };
#define HOVER_FLAG_RVA 0x9E45F8
#define HOVER_PORT_BUILDING_RVA 0x9E45B0  // building whose connection port is hovered
#define HOVER_PORT_INDEX_RVA 0x9E45B8     // that port's index

// thisPtr fields used by the tool wrappers.
#define TOOL_GUARD1_OFFSET 0xfbc          // any nonzero -> wrappers do nothing (blocking UI)
#define TOOL_GUARD2_OFFSET 0xfbe
#define TOOL_GUARD3_OFFSET 0xfc3
#define CURSOR_X_OFFSET 0xf6c
#define CURSOR_Y_OFFSET 0xf70
#define CURSOR_Z_OFFSET 0xf74
#define CONTINUING_Y_OFFSET_OFFSET 0xf90  // added to cursor Y on drag ticks
#define SNAP_TARGET_PTR_OFFSET 0xf6d8     // drag-start snap target
#define SNAP_MODE_A_OFFSET 0xf658
#define SNAP_MODE_B_OFFSET 0xf668
#define TOOL_DRAG_ACTIVE_OFFSET 0xd2ac

// Manager fields.
#define MGR_CURVE_OFFSET 0x48
#define MGR_RELAX_TIMER_OFFSET 0x4c
#define MGR_VALID_OFFSET 0x315            // byte: 1 = current route is buildable
#define MGR_ERROR_CATEGORY_OFFSET 0x318   // int: -1 = no error
#define MGR_ERROR_SUBCODE_OFFSET 0x31c
#define MGR_HOVER_SEGMENT_COPY_OFFSET 0x3b8
#define MGR_ROUTE_BEGIN_OFFSET 0x410      // interior route records (24 bytes each)
#define MGR_ROUTE_END_OFFSET 0x418
#define MGR_START_NODE_OFFSET 0x428
#define MGR_END_NODE_OFFSET 0x430
#define MGR_START_ATTACHED_OFFSET 0x748   // byte: start is an existing node
#define MGR_END_ATTACHED_OFFSET 0x749     // byte: end snapped to an existing node
#define MGR_SEGMENTS_BEGIN_OFFSET 0xaa0   // this tool's segment objects
#define MGR_SEGMENTS_END_OFFSET 0xaa8
// Upgrade branch of the commit (FUN_140518a80 line 303).
#define MGR_UPG_SEGMENT_OFFSET 0x380
#define MGR_UPG_RANGE_START_OFFSET 0x388
#define MGR_UPG_RANGE_END_OFFSET 0x38c
#define MGR_UPG_GATE_OFFSET 0x390
#define MGR_UPG_OLD_TYPE_OFFSET 0x394
#define MGR_UPG_OLD_VARIANT_OFFSET 0x398
#define MGR_UPG_DIR_OFFSET 0x39c
#define MGR_UPG_MULTI_OFFSET 0x3d0
#define MGR_TYPE_OFFSET 0x528
#define MGR_VARIANT_OFFSET 0x52c
#define MGR_FLAG_5C4_OFFSET 0x5c4

// Segment object.
#define SEG_POINTS_BEGIN_OFFSET 0x08
#define SEG_POINTS_END_OFFSET 0x10
#define SEG_NODE0_OFFSET 0x20
#define SEG_NODE1_OFFSET 0x28
#define SEG_TYPE_OFFSET 0x120
#define SEG_VARIANT_OFFSET 0x124

#define NODE_POS_OFFSET 4                 // node object: float x,y,z
#define BUILDING_POS_OFFSET 0x350         // building object: float x,y,z

typedef void (*ToolWrapperFn)(__int64 thisPtr, int typeId, unsigned int variant);
typedef void (*Fc040Fn)(__int64 manager, float* startPoint, unsigned __int64 p3, char fuzzyMatch,
                        int p5, unsigned __int64 existingNode, float* directionHint);
typedef void (*Fcaf0Fn)(__int64 manager, const float* cursor, int typeId, unsigned int variant,
                        unsigned char p5, unsigned char p6, unsigned char p7,
                        __int64 routeVector, unsigned char p9, unsigned char p10);
typedef __int64* (*VecAssignFn)(__int64* dst, const void* srcVectorHeader);

// ---------------------------------------------------------------------------
// Categories
// ---------------------------------------------------------------------------

// Manager offsets and wrapper RVAs come from each wrapper's decompile; tool
// names from the strcmp chain in FUN_14030d1a0. FACTORYCONN is disabled: its
// wrapper (FUN_1403d6e70) runs extra building logic (FUN_140546db0) that a
// replay through the primitives does not reproduce.
static RoadSyncCategoryInfo g_categories[ROAD_SYNC_CATEGORY_COUNT] = {
    { "ROAD",        0xf7a0, true  },
    { "PIPE",        0xf858, true  },
    { "WATERPIPE",   0xfaf0, true  },
    { "HEATPIPE",    0xfa98, true  },
    { "RAIL",        0xf908, true  },
    { "PEDESTRIAN",  0xf968, true  },
    { "FENCE",       0xf8b0, true  },
    { "POWER_HIGH",  0xfa18, true  },
    { "POWER_LOW",   0xfa20, true  },
    { "CONVEYOR",    0xf800, true  },
    { "CABLEWAY",    0xfa40, true  },
    { "AIRROAD",     0xfa30, true  },
    { "FACTORYCONN", 0xf9c0, false },
};

static const unsigned int g_categoryWrapperRva[ROAD_SYNC_CATEGORY_COUNT] = {
    0x3D00F0, 0x3B0E60, 0x3B12B0, 0x3B4230, 0x3D4730, 0x3D68B0, 0x3B3820,
    0x3B8780, 0x3BAED0, 0x3AEC30, 0x3CBFA0, 0x3C9130, 0x3D6E70,
};

const RoadSyncCategoryInfo* RoadSync_GetCategoryInfo(int category)
{
    if (category < 0 || category >= ROAD_SYNC_CATEGORY_COUNT) return nullptr;
    return &g_categories[category];
}

int RoadSync_CategoryForManager(__int64 thisPtr, __int64 manager)
{
    if (!thisPtr || !manager) return -1;
    __try {
        for (int c = 0; c < ROAD_SYNC_CATEGORY_COUNT; c++) {
            if (g_categories[c].enabled && *(__int64*)(thisPtr + g_categories[c].managerOffset) == manager)
                return c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

static const char* CategoryName(int category)
{
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(category);
    return info ? info->name : "?";
}

// fc040's 5th argument as each wrapper passes it: 0xffffffec, except the two
// power-line wrappers, which pass the type id (fc040 compares it with 0x14).
static int Fc040Param5ForCategory(int category, int typeId)
{
    if (category == ROAD_SYNC_CATEGORY_POWER_HIGH || category == ROAD_SYNC_CATEGORY_POWER_LOW)
        return typeId;
    return (int)0xffffffec;
}

// ---------------------------------------------------------------------------
// Module state and small helpers
// ---------------------------------------------------------------------------

static CRITICAL_SECTION g_queueLock;
static volatile LONG g_applyingSegment = 0;

void RoadSync_Bind(const TsmHost* host)
{
    TsmBind(host);
    InitializeCriticalSection(&g_queueLock);
}

// Logs to the file log and to the Debug window.
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

bool RoadSync_IsApplyingSegment()
{
    return InterlockedCompareExchange(&g_applyingSegment, 0, 0) != 0;
}

// True while the tool wrappers' guard bytes are set (some blocking UI is
// open); the wrappers silently do nothing then, so applying must wait.
static bool ToolWrapperGuardBusy(__int64 thisPtr)
{
    __try {
        return *(unsigned char*)(thisPtr + TOOL_GUARD1_OFFSET) ||
               *(unsigned char*)(thisPtr + TOOL_GUARD2_OFFSET) ||
               *(unsigned char*)(thisPtr + TOOL_GUARD3_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Clears the local mouse-hover globals so a replay never snaps to whatever
// the local player is pointing at. Not restored afterwards: the commit may
// delete the hovered object, and the game recomputes hover next frame.
static void HoverGlobals_Clear()
{
    __try {
        for (size_t i = 0; i < sizeof(kHoverPtrRvas) / sizeof(kHoverPtrRvas[0]); i++)
            *(long long*)((uintptr_t)g_exe + kHoverPtrRvas[i]) = 0;
        *(unsigned char*)((uintptr_t)g_exe + HOVER_FLAG_RVA) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Local building at `pos` (0.5m in x/z, 1m in y), or 0.
static __int64 FindLocalBuildingAt(const float pos[3])
{
    __try {
        __int64* it = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        __int64* end = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        for (; it && it < end; ++it) {
            __int64 b = *it;
            if (!b) continue;
            const float* p = (const float*)(b + BUILDING_POS_OFFSET);
            if (fabsf(p[0] - pos[0]) < 0.5f && fabsf(p[2] - pos[2]) < 0.5f && fabsf(p[1] - pos[1]) < 1.0f)
                return b;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

// Sets the hovered-port globals to the local copy of `port`'s building, or
// clears them when the end is not on a building.
static void SetHoveredPort(const RoadSyncBuildingPort& port, const char* which)
{
    __int64 bld = 0;
    if (port.valid) {
        bld = FindLocalBuildingAt(port.bldPos);
        Logf("republicmp  ROAD_SYNC:   %s port: building at (%.2f,%.2f,%.2f) port=%d -> %s",
             which, port.bldPos[0], port.bldPos[1], port.bldPos[2], port.portIndex,
             bld ? "found" : "NOT FOUND (end left unconnected)");
    }
    __try {
        *(long long*)((uintptr_t)g_exe + HOVER_PORT_BUILDING_RVA) = bld;
        if (bld) *(int*)((uintptr_t)g_exe + HOVER_PORT_INDEX_RVA) = port.portIndex;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void ClearHoveredPort()
{
    __try {
        *(long long*)((uintptr_t)g_exe + HOVER_PORT_BUILDING_RVA) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Writes a position into a node object.
static void SetNodePos(__int64 node, const float pos[3])
{
    if (!node) return;
    float* p = (float*)(node + NODE_POS_OFFSET);
    p[0] = pos[0]; p[1] = pos[1]; p[2] = pos[2];
}

static bool NodeNear(__int64 node, const float pos[3])
{
    const float* p = (const float*)(node + NODE_POS_OFFSET);
    return fabsf(p[0] - pos[0]) < 0.5f && fabsf(p[2] - pos[2]) < 0.5f && fabsf(p[1] - pos[1]) < 1.0f;
}

// Layout of an MSVC std::vector header; lets fcaf0 / FUN_14021d2b0 read a
// route we own without constructing a std::vector inside an SEH function.
struct RawVectorHeader {
    const void* first;
    const void* last;
    const void* end;
};

// ---------------------------------------------------------------------------
// Segment replay
// ---------------------------------------------------------------------------

// True if the category already has a segment joining nodes at `a` and `b`
// (either orientation). A second segment between the same two nodes is never
// intended and corrupts connected buildings.
static bool SegmentBetweenNodesExists(int category, const float a[3], const float b[3])
{
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(category);
    if (!info) return false;
    __try {
        __int64 manager = *(__int64*)(GetWorldThisPtr() + info->managerOffset);
        if (!manager) return false;
        __int64* it = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
        for (; it && it < end; ++it) {
            __int64 seg = *it;
            if (!seg) continue;
            __int64 n0 = *(__int64*)(seg + SEG_NODE0_OFFSET);
            __int64 n1 = *(__int64*)(seg + SEG_NODE1_OFFSET);
            if (!n0 || !n1) continue;
            if ((NodeNear(n0, a) && NodeNear(n1, b)) || (NodeNear(n0, b) && NodeNear(n1, a)))
                return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// Rebuilds `seg` on this machine through the tool's own primitives (see
// road_sync.h). Returns false only if the replay threw; *outEngineAccepted is
// the engine's verdict right before the commit (+0x315 valid, no error).
// All spoofed input state is restored before returning.
static bool ApplySegmentImpl(const RoadSyncSegment& seg, bool* outEngineAccepted)
{
    *outEngineAccepted = false;

    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(seg.category);
    if (!info || !info->enabled || seg.pointCount < 0 || seg.pointCount > ROAD_SYNC_MAX_POINTS) {
        Logf("republicmp  ROAD_SYNC: refusing segment category=%d points=%d", seg.category, seg.pointCount);
        return false;
    }

    __int64 thisPtr = GetWorldThisPtr();
    unsigned char* lmbHeld = (unsigned char*)((uintptr_t)g_exe + LMB_HELD_RVA);
    unsigned char* lmbJustPressed = (unsigned char*)((uintptr_t)g_exe + LMB_JUST_PRESSED_RVA);
    int* presetCurveMode = (int*)((uintptr_t)g_exe + PRESET_CURVE_MODE_RVA);
    ToolWrapperFn wrapper = (ToolWrapperFn)((uintptr_t)g_exe + g_categoryWrapperRva[seg.category]);

    __int64 manager = 0;
    __try {
        manager = *(__int64*)(thisPtr + info->managerOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!manager) {
        Logf("republicmp  ROAD_SYNC: %s manager is null", info->name);
        return false;
    }
    if (ToolWrapperGuardBusy(thisPtr)) {
        Logf("republicmp  ROAD_SYNC: tool wrapper guard set, not applying");
        return false;
    }

    bool saved = false;
    unsigned char savedLmbHeld = 0, savedLmbJustPressed = 0, savedSnapModeA = 0, savedSnapModeB = 0;
    long long savedSnapTarget = 0;
    float savedContinuingY = 0.0f, savedCurve = 0.0f, savedRelax = 0.0f;
    int savedPresetCurveMode = -1;
    bool threw = false;

    __try {
        __try {
            Logf("republicmp  ROAD_SYNC: applying %s type=%d variant=%d points=%d start=(%.2f,%.2f,%.2f) end=(%.2f,%.2f,%.2f)",
                 info->name, seg.typeId, seg.variant, seg.pointCount,
                 seg.startNode[0], seg.startNode[1], seg.startNode[2],
                 seg.endNode[0], seg.endNode[1], seg.endNode[2]);

            HoverGlobals_Clear();

            // Save and neutralise the local player's input state.
            savedLmbHeld = *lmbHeld;
            savedLmbJustPressed = *lmbJustPressed;
            savedSnapTarget = *(long long*)(thisPtr + SNAP_TARGET_PTR_OFFSET);
            savedSnapModeA = *(unsigned char*)(thisPtr + SNAP_MODE_A_OFFSET);
            savedSnapModeB = *(unsigned char*)(thisPtr + SNAP_MODE_B_OFFSET);
            savedContinuingY = *(float*)(thisPtr + CONTINUING_Y_OFFSET_OFFSET);
            savedCurve = *(float*)(manager + MGR_CURVE_OFFSET);
            savedRelax = *(float*)(manager + MGR_RELAX_TIMER_OFFSET);
            savedPresetCurveMode = *presetCurveMode;
            saved = true;

            *(long long*)(thisPtr + SNAP_TARGET_PTR_OFFSET) = 0;
            *(unsigned char*)(thisPtr + SNAP_MODE_A_OFFSET) = 0;
            *(unsigned char*)(thisPtr + SNAP_MODE_B_OFFSET) = 0;
            *(float*)(thisPtr + CONTINUING_Y_OFFSET_OFFSET) = 0.0f;
            *presetCurveMode = -1;
            *(float*)(manager + MGR_CURVE_OFFSET) = seg.curveValue;

            // Drag start: the wrapper sets up its drag state and calls fc040;
            // fc040 is then re-run with fuzzy node matching so the start
            // attaches to the node already at this position.
            SetHoveredPort(seg.startPort, "start");
            *(float*)(thisPtr + CURSOR_X_OFFSET) = seg.startNode[0];
            *(float*)(thisPtr + CURSOR_Y_OFFSET) = seg.startNode[1];
            *(float*)(thisPtr + CURSOR_Z_OFFSET) = seg.startNode[2];
            *lmbHeld = 0;
            *lmbJustPressed = 1;
            wrapper(thisPtr, seg.typeId, (unsigned int)seg.variant);
            *lmbJustPressed = 0;

            float start[3] = { seg.startNode[0], seg.startNode[1], seg.startNode[2] };
            float noDirection[3] = { 0.0f, 0.0f, 0.0f };
            ((Fc040Fn)((uintptr_t)g_exe + FC040_RVA))(manager, start, 0, 1,
                Fc040Param5ForCategory(seg.category, seg.typeId), 0, noDirection);
            *(long long*)(manager + MGR_HOVER_SEGMENT_COPY_OFFSET) = 0;
            // A fresh start node gets manager+0x24 added to its height; the
            // sender's position already includes it.
            if (*(unsigned char*)(manager + MGR_START_ATTACHED_OFFSET) == 0)
                SetNodePos(*(__int64*)(manager + MGR_START_NODE_OFFSET), seg.startNode);
            *(float*)(manager + MGR_RELAX_TIMER_OFFSET) = 2.0f;

            // One drag tick with the sender's route as param_8: the engine
            // copies it into its route buffer instead of routing itself, and
            // matches the end node against existing nodes within +-1m.
            SetHoveredPort(seg.endPort, "end");
            RawVectorHeader route = { seg.points, seg.points + seg.pointCount, seg.points + seg.pointCount };
            float end[3] = { seg.endNode[0], seg.endNode[1], seg.endNode[2] };
            *lmbHeld = 1;
            ((Fcaf0Fn)((uintptr_t)g_exe + FCAF0_RVA))(manager, end, seg.typeId, (unsigned int)seg.variant,
                                                      0, 0, 0, (__int64)&route, 0, 0);
            if (*(unsigned char*)(manager + MGR_END_ATTACHED_OFFSET) == 0)
                SetNodePos(*(__int64*)(manager + MGR_END_NODE_OFFSET), seg.endNode);

            unsigned char valid = *(unsigned char*)(manager + MGR_VALID_OFFSET);
            int errCategory = *(int*)(manager + MGR_ERROR_CATEGORY_OFFSET);
            unsigned int errSubcode = *(unsigned int*)(manager + MGR_ERROR_SUBCODE_OFFSET);
            Logf("republicmp  ROAD_SYNC:   attach start=%d end=%d valid=%d errCategory=%d errSubcode=0x%x",
                 (int)*(unsigned char*)(manager + MGR_START_ATTACHED_OFFSET),
                 (int)*(unsigned char*)(manager + MGR_END_ATTACHED_OFFSET),
                 (int)valid, errCategory, errSubcode);
            *outEngineAccepted = (valid == 1) && (errCategory == -1);

            // Release: the wrapper commits and ends its drag.
            *lmbHeld = 0;
            wrapper(thisPtr, seg.typeId, (unsigned int)seg.variant);
            if (*(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) != 0)
                Logf("republicmp  ROAD_SYNC:   WARNING drag still active after release");
        } __finally {
            if (saved) {
                *lmbHeld = savedLmbHeld;
                *lmbJustPressed = savedLmbJustPressed;
                *(long long*)(thisPtr + SNAP_TARGET_PTR_OFFSET) = savedSnapTarget;
                *(unsigned char*)(thisPtr + SNAP_MODE_A_OFFSET) = savedSnapModeA;
                *(unsigned char*)(thisPtr + SNAP_MODE_B_OFFSET) = savedSnapModeB;
                *(float*)(thisPtr + CONTINUING_Y_OFFSET_OFFSET) = savedContinuingY;
                *presetCurveMode = savedPresetCurveMode;
                *(float*)(manager + MGR_CURVE_OFFSET) = savedCurve;
                *(float*)(manager + MGR_RELAX_TIMER_OFFSET) = savedRelax;
            }
            ClearHoveredPort();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        threw = true;
    }

    if (threw) {
        Logf("republicmp  ROAD_SYNC: <exception applying segment>");
        *outEngineAccepted = false;
        return false;
    }
    Logf("republicmp  ROAD_SYNC:   apply done - %s", *outEngineAccepted ? "engine ACCEPTED" : "engine REJECTED");
    return true;
}

static bool ApplySegment(const RoadSyncSegment& seg, bool* outEngineAccepted)
{
    InterlockedIncrement(&g_applyingSegment);
    bool result = ApplySegmentImpl(seg, outEngineAccepted);
    InterlockedDecrement(&g_applyingSegment);
    return result;
}

// ---------------------------------------------------------------------------
// Upgrade replay
// ---------------------------------------------------------------------------

// Local segment matching the piece by node positions and point count, in
// either orientation. Caller wraps in __try.
static __int64 FindLocalSegment(__int64 manager, const RoadSyncUpgradePiece& piece, bool* outReversed)
{
    __int64* it = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
    __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
    for (; it && it < end; ++it) {
        __int64 seg = *it;
        if (!seg) continue;
        long long count = (*(__int64*)(seg + SEG_POINTS_END_OFFSET) - *(__int64*)(seg + SEG_POINTS_BEGIN_OFFSET)) / 0x18;
        if (count != piece.segPointCount) continue;
        __int64 n0 = *(__int64*)(seg + SEG_NODE0_OFFSET);
        __int64 n1 = *(__int64*)(seg + SEG_NODE1_OFFSET);
        if (!n0 || !n1) continue;
        if (NodeNear(n0, piece.segStart) && NodeNear(n1, piece.segEnd)) { *outReversed = false; return seg; }
        if (NodeNear(n0, piece.segEnd) && NodeNear(n1, piece.segStart)) { *outReversed = true; return seg; }
    }
    return 0;
}

// Re-types one segment range: resets the manager with fc040, fills in the
// fields the engine's own re-type drag leaves behind (fcaf0 lines 1864-1907)
// and runs the commit, which takes its upgrade branch.
static bool ApplyUpgradePiece(int category, const RoadSyncUpgradeMsg& msg, const RoadSyncUpgradePiece& piece)
{
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(category);
    __try {
        __int64 manager = *(__int64*)(GetWorldThisPtr() + info->managerOffset);
        if (!manager) return false;

        bool reversed = false;
        __int64 seg = FindLocalSegment(manager, piece, &reversed);
        if (!seg) {
            Logf("republicmp  ROAD_SYNC: upgrade: no local %s segment at start=(%.2f,%.2f,%.2f) end=(%.2f,%.2f,%.2f) points=%d",
                 info->name, piece.segStart[0], piece.segStart[1], piece.segStart[2],
                 piece.segEnd[0], piece.segEnd[1], piece.segEnd[2], piece.segPointCount);
            return false;
        }

        int count = piece.segPointCount;
        int rs = piece.rangeStart, re = piece.rangeEnd, dir = piece.flag39c;
        if (reversed) {
            int nrs = count - 1 - re, nre = count - 1 - rs;
            rs = nrs; re = nre;
            if (dir == 1) dir = 2; else if (dir == 2) dir = 1;
        }
        if (rs < -1 || re > count || rs >= re) {
            Logf("republicmp  ROAD_SYNC: upgrade: bad range (%d,%d) for %d points", rs, re, count);
            return false;
        }

        // Range ends: a node or an interior point of the segment.
        const RoadSyncPoint* pts = *(const RoadSyncPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
        const float* a = (rs < 0) ? (const float*)(*(__int64*)(seg + SEG_NODE0_OFFSET) + NODE_POS_OFFSET) : pts[rs].pos;
        const float* b = (re >= count) ? (const float*)(*(__int64*)(seg + SEG_NODE1_OFFSET) + NODE_POS_OFFSET) : pts[re].pos;
        float splitStart[3] = { a[0], a[1], a[2] };
        float splitEnd[3] = { b[0], b[1], b[2] };

        float noDirection[3] = { 0.0f, 0.0f, 0.0f };
        ((Fc040Fn)((uintptr_t)g_exe + FC040_RVA))(manager, splitStart, 0, 1,
            Fc040Param5ForCategory(category, msg.newType), 0, noDirection);
        *(long long*)(manager + MGR_HOVER_SEGMENT_COPY_OFFSET) = 0;
        if (*(unsigned char*)(manager + MGR_START_ATTACHED_OFFSET) == 0)
            SetNodePos(*(__int64*)(manager + MGR_START_NODE_OFFSET), splitStart);
        SetNodePos(*(__int64*)(manager + MGR_END_NODE_OFFSET), splitEnd);

        // Route buffer = the segment's own records strictly inside the range.
        int first = rs + 1, n = re - first;
        RawVectorHeader route = { pts + first, pts + first + (n > 0 ? n : 0), pts + first + (n > 0 ? n : 0) };
        ((VecAssignFn)((uintptr_t)g_exe + VEC_ASSIGN_RVA))((__int64*)(manager + MGR_ROUTE_BEGIN_OFFSET), &route);

        *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET) = seg;
        *(int*)(manager + MGR_UPG_RANGE_START_OFFSET) = rs;
        *(int*)(manager + MGR_UPG_RANGE_END_OFFSET) = re;
        *(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) = 1;
        *(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) = 0;
        *(int*)(manager + MGR_UPG_OLD_TYPE_OFFSET) = piece.oldType;
        *(int*)(manager + MGR_UPG_OLD_VARIANT_OFFSET) = piece.oldVariant;
        *(int*)(manager + MGR_UPG_DIR_OFFSET) = dir;
        *(int*)(manager + MGR_TYPE_OFFSET) = msg.newType;
        *(int*)(manager + MGR_VARIANT_OFFSET) = msg.newVariant;
        *(int*)(manager + MGR_FLAG_5C4_OFFSET) = msg.flag5c4;

        Logf("republicmp  ROAD_SYNC: upgrade %s%s range=(%d,%d)/%d type %d/%d -> %d/%d dir=%d",
             info->name, reversed ? " (reversed)" : "", rs, re, count,
             *(int*)(seg + SEG_TYPE_OFFSET), *(int*)(seg + SEG_VARIANT_OFFSET), msg.newType, msg.newVariant, dir);

        RoadCommitFn commit = g_origRoadCommit ? g_origRoadCommit : (RoadCommitFn)((uintptr_t)g_exe + COMMIT_RVA);
        commit(manager, 0, 0, 0, 0, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  ROAD_SYNC: <exception applying upgrade piece>");
        return false;
    }
}

// Returns the number of pieces applied.
static int ApplyUpgrade(const RoadSyncUpgradeMsg& msg)
{
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(msg.category);
    if (!info || !info->enabled || msg.pieceCount < 1 || msg.pieceCount > ROAD_SYNC_MAX_UPGRADE_PIECES) {
        Logf("republicmp  ROAD_SYNC: refusing upgrade seq=%d category=%d pieces=%d", msg.seq, msg.category, msg.pieceCount);
        return 0;
    }
    if (ToolWrapperGuardBusy(GetWorldThisPtr())) return 0;

    HoverGlobals_Clear();
    InterlockedIncrement(&g_applyingSegment);
    int applied = 0;
    for (int i = 0; i < msg.pieceCount; i++) {
        if (ApplyUpgradePiece(msg.category, msg, msg.pieces[i])) applied++;
    }
    InterlockedDecrement(&g_applyingSegment);
    return applied;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

// One queued/received unit of work; segments and upgrades share one ordered
// stream so an upgrade never overtakes the segment it re-types.
struct RoadSyncItem {
    int tag; // ROAD_SYNC_WIRE_SEGMENT or ROAD_SYNC_WIRE_UPGRADE
    RoadSyncMsg seg;
    RoadSyncUpgradeMsg up;
};

static volatile bool g_connected = false;
static volatile bool g_connecting = false;
static volatile bool g_isHost = false;
static SOCKET g_sock = INVALID_SOCKET;
static char g_targetIp[64] = ROAD_SYNC_TEST_HOST_IP;
static std::vector<RoadSyncItem> g_incoming;          // receive thread -> game thread
static std::vector<RoadSyncAckMsg> g_incomingAcks;    // receive thread -> game thread

static void CloseConnection()
{
    g_connected = false;
    NetworkCloseSocket(g_sock);
    g_sock = INVALID_SOCKET;
}

static bool SendEnvelope(int tag, const void* payload, size_t payloadSize)
{
    if (!NetworkSendExact(g_sock, &tag, sizeof(tag))) return false;
    return NetworkSendExact(g_sock, payload, payloadSize);
}

static void LogSegmentPoints(const char* label, int seq, const RoadSyncSegment& seg)
{
#if ROAD_SYNC_VERBOSE_LOG
    for (int i = 0; i < seg.pointCount; i++) {
        Logf("republicmp  ROAD_SYNC:   %s[seq=%d][%d] x=%.9g y=%.9g z=%.9g", label, seq, i,
             seg.points[i].pos[0], seg.points[i].pos[1], seg.points[i].pos[2]);
    }
#else
    (void)label; (void)seq; (void)seg;
#endif
}

// Receives tagged payloads and hands them to the game thread.
static DWORD WINAPI ReceiveLoopThread(LPVOID)
{
    while (g_connected) {
        int tag = 0;
        if (!NetworkRecvExact(g_sock, &tag, sizeof(tag))) {
            RoadSyncStatus("[ROAD_SYNC] connection lost");
            CloseConnection();
            break;
        }

        if (tag == ROAD_SYNC_WIRE_SEGMENT || tag == ROAD_SYNC_WIRE_UPGRADE) {
            RoadSyncItem* item = new RoadSyncItem(); // ~15KB, keep it off the thread stack
            item->tag = tag;
            bool ok = (tag == ROAD_SYNC_WIRE_SEGMENT)
                ? NetworkRecvExact(g_sock, &item->seg, sizeof(item->seg))
                : NetworkRecvExact(g_sock, &item->up, sizeof(item->up));
            if (!ok) {
                delete item;
                RoadSyncStatus("[ROAD_SYNC] connection lost");
                CloseConnection();
                break;
            }

            if (tag == ROAD_SYNC_WIRE_SEGMENT) {
                const RoadSyncSegment& seg = item->seg.seg;
                if (seg.pointCount < 0 || seg.pointCount > ROAD_SYNC_MAX_POINTS) {
                    Logf("republicmp  ROAD_SYNC: received segment with bad pointCount=%d, dropping", seg.pointCount);
                    delete item;
                    continue;
                }
                Logf("republicmp  ROAD_SYNC: received seq=%d %s type=%d variant=%d points=%d",
                     item->seg.seq, CategoryName(seg.category), seg.typeId, seg.variant, seg.pointCount);
                LogSegmentPoints("recv", item->seg.seq, seg);
            } else {
                Logf("republicmp  ROAD_SYNC: received upgrade seq=%d %s type=%d variant=%d pieces=%d",
                     item->up.seq, CategoryName(item->up.category), item->up.newType, item->up.newVariant, item->up.pieceCount);
            }

            EnterCriticalSection(&g_queueLock);
            g_incoming.push_back(*item);
            LeaveCriticalSection(&g_queueLock);
            delete item;
        } else if (tag == ROAD_SYNC_WIRE_ACK) {
            RoadSyncAckMsg ack = {};
            if (!NetworkRecvExact(g_sock, &ack, sizeof(ack))) {
                RoadSyncStatus("[ROAD_SYNC] connection lost");
                CloseConnection();
                break;
            }
            EnterCriticalSection(&g_queueLock);
            g_incomingAcks.push_back(ack);
            LeaveCriticalSection(&g_queueLock);
        } else {
            // Framing is out of sync (mismatched builds) - cannot recover.
            RoadSyncStatus("[ROAD_SYNC] unknown wire tag %d, dropping connection (mismatched build?)", tag);
            CloseConnection();
            break;
        }
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

// ---------------------------------------------------------------------------
// Sending (game thread only)
// ---------------------------------------------------------------------------

// Sent items waiting for their ack - used only to flag unmatched acks.
static std::vector<int> g_pendingSends;
static int g_nextSeq = 0;
// Items captured while disconnected, sent in order once connected.
static std::vector<RoadSyncItem> g_outboundQueue;

static void SendItemNow(RoadSyncItem& item)
{
    int seq = ++g_nextSeq;
    bool sent;
    if (item.tag == ROAD_SYNC_WIRE_UPGRADE) {
        RoadSyncUpgradeMsg& up = item.up;
        up.seq = seq;
        Logf("republicmp  ROAD_SYNC: sending upgrade seq=%d %s type=%d variant=%d pieces=%d",
             seq, CategoryName(up.category), up.newType, up.newVariant, up.pieceCount);
        sent = SendEnvelope(ROAD_SYNC_WIRE_UPGRADE, &up, sizeof(up));
    } else {
        RoadSyncMsg& msg = item.seg;
        msg.seq = seq;
        Logf("republicmp  ROAD_SYNC: sending seq=%d %s type=%d variant=%d points=%d",
             seq, CategoryName(msg.seg.category), msg.seg.typeId, msg.seg.variant, msg.seg.pointCount);
        LogSegmentPoints("send", seq, msg.seg);
        sent = SendEnvelope(ROAD_SYNC_WIRE_SEGMENT, &msg, sizeof(msg));
    }

    if (!sent) {
        RoadSyncStatus("[ROAD_SYNC] send failed, dropping connection");
        CloseConnection();
        return;
    }
    g_pendingSends.push_back(seq);
}

static void FlushOutboundQueue()
{
    while (g_connected && !g_outboundQueue.empty()) {
        SendItemNow(g_outboundQueue.front());
        if (!g_connected) break; // send failed - keep it for the next connection
        g_outboundQueue.erase(g_outboundQueue.begin());
    }
}

static int SendOrQueue(RoadSyncItem& item)
{
    if (!g_connected || !g_outboundQueue.empty()) {
        g_outboundQueue.push_back(item);
        Logf("republicmp  ROAD_SYNC: queued %s (%zu waiting for connection)",
             item.tag == ROAD_SYNC_WIRE_UPGRADE ? "upgrade" : "segment", g_outboundQueue.size());
        return 0;
    }
    SendItemNow(item);
    return g_connected ? g_nextSeq : -1;
}

int RoadSync_SendSegment(const RoadSyncSegment& seg)
{
    if (seg.pointCount < 0 || seg.pointCount > ROAD_SYNC_MAX_POINTS) {
        Logf("republicmp  ROAD_SYNC: refusing to send segment, pointCount=%d", seg.pointCount);
        return -1;
    }
    static RoadSyncItem s_item; // large - game thread only
    memset(&s_item, 0, sizeof(s_item));
    s_item.tag = ROAD_SYNC_WIRE_SEGMENT;
    s_item.seg.seg = seg;
    return SendOrQueue(s_item);
}

int RoadSync_SendUpgrade(const RoadSyncUpgradeMsg& up)
{
    if (up.pieceCount < 1 || up.pieceCount > ROAD_SYNC_MAX_UPGRADE_PIECES) {
        Logf("republicmp  ROAD_SYNC: refusing to send upgrade, pieceCount=%d", up.pieceCount);
        return -1;
    }
    static RoadSyncItem s_item;
    memset(&s_item, 0, sizeof(s_item));
    s_item.tag = ROAD_SYNC_WIRE_UPGRADE;
    s_item.up = up;
    return SendOrQueue(s_item);
}

// ---------------------------------------------------------------------------
// Applying received work (game thread only)
// ---------------------------------------------------------------------------

static std::vector<RoadSyncItem> g_pending;
static bool g_warnedWorldNotLoaded = false;

static void SendAck(int seq, int category, bool success, int attempts)
{
    RoadSyncAckMsg ack = {};
    ack.seq = seq;
    ack.category = category;
    ack.success = success ? 1 : 0;
    ack.attempts = attempts;
    if (g_connected && !SendEnvelope(ROAD_SYNC_WIRE_ACK, &ack, sizeof(ack))) {
        RoadSyncStatus("[ROAD_SYNC] ack send failed, dropping connection");
        CloseConnection();
    }
}

// Applies a received segment, retrying only if the replay threw, and acks
// the engine's verdict. A segment already present between the same nodes is
// acked as built without replaying it.
static void ApplySegmentAndAck(const RoadSyncMsg& msg)
{
    const RoadSyncSegment& seg = msg.seg;
    if (SegmentBetweenNodesExists(seg.category, seg.startNode, seg.endNode)) {
        Logf("republicmp  ROAD_SYNC: seq=%d %s already exists between these nodes - skipped",
             msg.seq, CategoryName(seg.category));
        SendAck(msg.seq, seg.category, true, 0);
        return;
    }

    bool ok = false;
    bool engineAccepted = false;
    int attempt = 1;
    for (; attempt <= ROAD_SYNC_MAX_APPLY_ATTEMPTS; attempt++) {
        ok = ApplySegment(seg, &engineAccepted);
        if (ok) break;
    }
    if (!ok) attempt = ROAD_SYNC_MAX_APPLY_ATTEMPTS;

    Logf("republicmp  ROAD_SYNC: seq=%d %s after %d attempt(s)", msg.seq,
         !ok ? "GAVE UP" : engineAccepted ? "applied" : "REJECTED by the engine", attempt);
    SendAck(msg.seq, seg.category, ok && engineAccepted, attempt);
}

static void ApplyUpgradeAndAck(const RoadSyncUpgradeMsg& up)
{
    int applied = ApplyUpgrade(up);
    Logf("republicmp  ROAD_SYNC: upgrade seq=%d %s applied %d/%d piece(s)",
         up.seq, CategoryName(up.category), applied, up.pieceCount);
    SendAck(up.seq, up.category, applied == up.pieceCount, 1);
}

void RoadSync_Tick()
{
    FlushOutboundQueue();

    std::vector<RoadSyncAckMsg> acks;
    EnterCriticalSection(&g_queueLock);
    acks.swap(g_incomingAcks);
    if (!g_incoming.empty()) {
        g_pending.insert(g_pending.end(), g_incoming.begin(), g_incoming.end());
        g_incoming.clear();
    }
    LeaveCriticalSection(&g_queueLock);

    for (const RoadSyncAckMsg& ack : acks) {
        bool found = false;
        for (size_t i = 0; i < g_pendingSends.size(); i++) {
            if (g_pendingSends[i] == ack.seq) {
                g_pendingSends.erase(g_pendingSends.begin() + i);
                found = true;
                break;
            }
        }
        Logf("republicmp  ROAD_SYNC: ack seq=%d %s %s%s", ack.seq, CategoryName(ack.category),
             ack.success ? "CONFIRMED built" : "FAILED on the other side",
             found ? "" : " (no matching pending send)");
    }

    if (g_pending.empty()) return;

    if (!IsWorldLoaded()) {
        if (!g_warnedWorldNotLoaded) {
            Logf("republicmp  ROAD_SYNC: %zu remote item(s) held until the world is loaded", g_pending.size());
            g_warnedWorldNotLoaded = true;
        }
        return;
    }
    g_warnedWorldNotLoaded = false;

    // Never replay into a manager while the local player is dragging, or
    // while the tool wrappers are blocked by UI - items stay queued.
    __int64 thisPtr = GetWorldThisPtr();
    std::vector<RoadSyncItem> toApply;
    toApply.swap(g_pending);
    for (size_t i = 0; i < toApply.size(); i++) {
        if (DetectSpawn_IsToolDragActive() || ToolWrapperGuardBusy(thisPtr)) {
            g_pending.insert(g_pending.begin(), toApply.begin() + i, toApply.end());
            break;
        }
        if (toApply[i].tag == ROAD_SYNC_WIRE_UPGRADE)
            ApplyUpgradeAndAck(toApply[i].up);
        else
            ApplySegmentAndAck(toApply[i].seg);
    }
}
