// SPDX-License-Identifier: GPL-3.0-only
// See networks.h for the overall design.
#include "networks.h"
#include "../lockstep/commands.h"
#include "local_builds.h" // g_origRoadCommit
#include "view_list_scope.h"
#include "menus_closed.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#include <vector>
#include <unordered_map>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

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
#define MANAGER_RESET_RVA 0x542A30        // FUN_140542a30(manager, manager+0x408): clears the tool's preview state
#define MANAGER_RESET_STATE_OFFSET 0x408
#define ROAD_DEMOLISH_TYPE 0x65           // fcaf0 type argument of the demolish tool

#define LMB_HELD_RVA 0xA54E90             // byte: left mouse button held
#define LMB_JUST_PRESSED_RVA 0xA54E91     // byte: left mouse button pressed this frame
#define PRESET_CURVE_MODE_RVA 0x9E8088    // int: preset curve index, -1 = free curve

// Hover state the tools read from the local mouse.
//
// 0x9E4590 / 0x9E45A0: the road segment under the cursor. The footpath tool
// (manager+0x258 == 4) copies them at drag start (fc040, 0x4FC650) into
// manager+0x360 / +0x370, and its commit (0x51C9F6) then joins the path to
// that road segment, looking it up in the road manager's list (FUN_14053d510)
// and indexing with the result. Left set, a replayed footpath was joined to
// whatever road the LOCAL player happened to point at - and when that
// pointer was stale (the segment rebuilt meanwhile), the lookup gave -1 and
// the commit crashed half-way (0x56DFE4 via FUN_1405446d0), leaving the tool
// broken and the game crashing on the next frame.
static const unsigned int kHoverPtrRvas[] = { 0x9E45E8, 0x9E4560, 0x9E4558, 0x9E45E0, 0x9E45D8,
                                              0x9E4590, 0x9E45A0 };
#define HOVER_FLAG_RVA 0x9E45F8
// The footpath tool's road link (see Networks_ReadFootpathLinks).
#define HOVER_ROAD_EDGE_RVA 0x9E4590   // road edge under the cursor
#define HOVER_ROAD_POINT_RVA 0x9E4598  // int: its point the path would split it at
#define HOVER_ROAD_NODE_RVA 0x9E45A0   // road node under the cursor
#define MGR_ROAD_LINK_POINT_OFFSET 0x368
#define ROAD_NETWORK_OFFSET 0xf7a0      // world+0xf7a0 = DAT_1409e46b0, the road network
#define NET_NODES_OFFSET 0x298          // committed nodes: begin, end
#define HOVER_NODE_RVA 0x9E4560  // thisPtr+0xf650: hovered path node (fcaf0 DAT_1409e4560)
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
#define MGR_ROAD_LINK_OFFSET 0x360        // footpath tool: road segment the path joins (fc040 copies 0x9E4590)
#define MGR_ROAD_LINK_EXTRA_OFFSET 0x370  // ... and 0x9E45A0
#define MGR_ROUTE_BEGIN_OFFSET 0x410      // interior route records (24 bytes each)
#define MGR_ROUTE_END_OFFSET 0x418
#define MGR_START_NODE_OFFSET 0x428
#define MGR_END_NODE_OFFSET 0x430
#define MGR_START_ATTACHED_OFFSET 0x748   // byte: start is an existing node
#define MGR_END_ATTACHED_OFFSET 0x749     // byte: end snapped to an existing node
// The network's committed edges. (+0xaa0/+0xaa8 is only what the local
// camera sees - lookups there missed every piece the player looked away
// from; see view_list_scope.h.)
#define MGR_SEGMENTS_BEGIN_OFFSET 0x2b0
#define MGR_SEGMENTS_END_OFFSET 0x2b8
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
typedef void (*ManagerResetFn)(__int64 manager, __int64 state);

// ---------------------------------------------------------------------------
// Categories
// ---------------------------------------------------------------------------

// Manager offsets and wrapper RVAs come from each wrapper's decompile; tool
// names from the strcmp chain in FUN_14030d1a0. FACTORYCONN: drawing
// ("factoryconnection", wrapper mode = type id 0) and its demolish mode (100)
// drive the primitives exactly like the footpath wrapper. Only the
// factoryconnection_oneway mode (0x32) differs - it hit-tests existing
// connections (FUN_140546db0) and changes their direction without a commit;
// that travels as NetDirectionMsg.
static NetCategoryInfo g_categories[NET_CATEGORY_COUNT] = {
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
    { "FACTORYCONN", 0xf9c0, true  },
    // The second-road network (pavements and decoration paths: second_road_airplane type 0,
    // _basicroad 1, _l2 2, _l3 -2, _l4 -3). Its handler 3cf6f0 begins on the press and finishes
    // on the release; after a finish that appended an edge it starts the edge's construction
    // (5247f0) and clears the ground under it (3ae120) itself, so the replay's release through
    // the handler makes those steps too. (by SoylentGreed)
    { "SECOND_ROAD", 0xf7a8, true  },
};

static const unsigned int g_categoryWrapperRva[NET_CATEGORY_COUNT] = {
    0x3D00F0, 0x3B0E60, 0x3B12B0, 0x3B4230, 0x3D4730, 0x3D68B0, 0x3B3820,
    0x3B8780, 0x3BAED0, 0x3AEC30, 0x3CBFA0, 0x3C9130, 0x3D6E70, 0x3CF6F0,
};

const NetCategoryInfo* Networks_GetCategoryInfo(int category)
{
    if (category < 0 || category >= NET_CATEGORY_COUNT) return nullptr;
    return &g_categories[category];
}

int Networks_CategoryForManager(__int64 thisPtr, __int64 manager)
{
    if (!thisPtr || !manager) return -1;
    __try {
        for (int c = 0; c < NET_CATEGORY_COUNT; c++) {
            if (g_categories[c].enabled && *(__int64*)(thisPtr + g_categories[c].managerOffset) == manager)
                return c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

int Networks_SegmentCount(__int64 thisPtr, int category)
{
    if (category < 0 || category >= NET_CATEGORY_COUNT || !thisPtr) return -1;
    __try {
        __int64 manager = *(__int64*)(thisPtr + g_categories[category].managerOffset);
        if (!manager) return -1;
        __int64 begin = *(__int64*)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64 end = *(__int64*)(manager + MGR_SEGMENTS_END_OFFSET);
        return (begin && end >= begin) ? (int)((end - begin) / 8) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static const char* CategoryName(int category)
{
    if (category == NET_GROUP_TOOL_CATEGORY) return "GROUP TOOL";
    const NetCategoryInfo* info = Networks_GetCategoryInfo(category);
    return info ? info->name : "?";
}

// fc040's 5th argument as each wrapper passes it: 0xffffffec, except the two
// power-line wrappers, which pass the type id (fc040 compares it with 0x14).
static int Fc040Param5ForCategory(int category, int typeId)
{
    if (category == NET_CATEGORY_POWER_HIGH || category == NET_CATEGORY_POWER_LOW)
        return typeId;
    return (int)0xffffffec;
}

// ---------------------------------------------------------------------------
// Module state and small helpers
// ---------------------------------------------------------------------------

static volatile LONG g_applyingSegment = 0;

void Networks_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static __int64 GetWorldThisPtr()
{
    return (__int64)((uintptr_t)g_exe + UNIVERSAL_THIS_RVA);
}

bool Networks_IsApplyingCommand()
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

void Networks_ClearHover()
{
    HoverGlobals_Clear();
    __try {
        *(long long*)((uintptr_t)g_exe + HOVER_PORT_BUILDING_RVA) = 0;
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
static void SetHoveredPort(const NetBuildingPort& port, const char* which)
{
    __int64 bld = 0;
    if (port.valid) {
        bld = FindLocalBuildingAt(port.bldPos);
        Logf("republicmp  NETWORKS:   %s port: building at (%.2f,%.2f,%.2f) port=%d -> %s",
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

static bool IsPowerLine(int category)
{
    return category == NET_CATEGORY_POWER_HIGH || category == NET_CATEGORY_POWER_LOW;
}

// The committed node of `network` the drag tick would take for an end at
// `pos` (its own test, 0x4FEFF0: within 1m on each axis), nearest first; 0
// if none.
static __int64 FindNetworkNodeAt(__int64 network, const float pos[3])
{
    __int64 best = 0;
    float bestD = 0.0f;
    __try {
        __int64* it = *(__int64**)(network + NET_NODES_OFFSET);
        __int64* end = *(__int64**)(network + NET_NODES_OFFSET + 8);
        for (; it && it < end; ++it) {
            __int64 node = *it;
            if (!node) continue;
            const float* p = (const float*)(node + NODE_POS_OFFSET);
            float dx = fabsf(p[0] - pos[0]), dy = fabsf(p[1] - pos[1]), dz = fabsf(p[2] - pos[2]);
            if (dx >= 1.0f || dy >= 1.0f || dz >= 1.0f) continue;
            float d = dx * dx + dy * dy + dz * dz;
            if (!best || d < bestD) {
                best = node;
                bestD = d;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return best;
}

// A power-line drag end that took an existing node must sit where the sender's
// did; logs and puts it back if the engine moved it anyway.
static void CheckAttachedPowerNode(__int64 node, const float want[3], const char* which)
{
    if (!node) return;
    const float* p = (const float*)(node + NODE_POS_OFFSET);
    if (fabsf(p[1] - want[1]) <= 0.5f) return;
    Logf("republicmp  NETWORKS:   WARNING %s node at (%.2f,%.2f,%.2f), the sender's at (%.2f,%.2f,%.2f) - put back",
         which, p[0], p[1], p[2], want[0], want[1], want[2]);
    SetNodePos(node, want);
}

// ---------------------------------------------------------------------------
// Footpath road links
// ---------------------------------------------------------------------------
//
// The footpath commit (FUN_14051c9f6 onwards) joins each end to a road: the
// start to the edge in manager+0x360 split at point +0x368 and/or the node in
// +0x370 (copied from the hover globals when the drag began, FUN_1404fc040 at
// 0x4FC650), the end to the hover globals themselves. It splits the road
// edge there and links the road node and the path node (+0xd0 both ways) -
// the pedestrian crossing - and the tool then refreshes the roads. A replay
// with the globals cleared joined nothing: no crossing, the path only touching
// the road. The sender's links travel by position and are resolved here.

static bool InPointerList(__int64 list, __int64 p)
{
    __int64* it = *(__int64**)list;
    __int64* end = *(__int64**)(list + 8);
    for (; it && it < end; ++it)
        if (*it == p) return true;
    return false;
}

static __int64 RoadNetwork()
{
    return *(__int64*)(GetWorldThisPtr() + ROAD_NETWORK_OFFSET);
}

// Caller wraps in __try.
static void ReadLink(__int64 edge, int pointIndex, __int64 node, NetRoadLink& out)
{
    memset(&out, 0, sizeof(out));
    __int64 roads = RoadNetwork();
    if (!roads) return;
    if (edge && InPointerList(roads + MGR_SEGMENTS_BEGIN_OFFSET, edge)) {
        __int64 n0 = *(__int64*)(edge + SEG_NODE0_OFFSET);
        __int64 n1 = *(__int64*)(edge + SEG_NODE1_OFFSET);
        if (n0 && n1) {
            out.hasEdge = 1;
            out.pointIndex = pointIndex;
            memcpy(out.edgeA, (const float*)(n0 + NODE_POS_OFFSET), 12);
            memcpy(out.edgeB, (const float*)(n1 + NODE_POS_OFFSET), 12);
            out.edgePoints = (int)((*(__int64*)(edge + SEG_POINTS_END_OFFSET) -
                                    *(__int64*)(edge + SEG_POINTS_BEGIN_OFFSET)) / 0x18);
        }
    }
    if (node && InPointerList(roads + NET_NODES_OFFSET, node)) {
        out.hasNode = 1;
        memcpy(out.node, (const float*)(node + NODE_POS_OFFSET), 12);
    }
}

void Networks_ReadFootpathLinks(__int64 manager, int category, NetSegment& seg)
{
    memset(&seg.startLink, 0, sizeof(seg.startLink));
    memset(&seg.endLink, 0, sizeof(seg.endLink));
    if (category != NET_CATEGORY_PEDESTRIAN) return;
    __try {
        ReadLink(*(__int64*)(manager + MGR_ROAD_LINK_OFFSET), *(int*)(manager + MGR_ROAD_LINK_POINT_OFFSET),
                 *(__int64*)(manager + MGR_ROAD_LINK_EXTRA_OFFSET), seg.startLink);
        ReadLink(*(__int64*)((uintptr_t)g_exe + HOVER_ROAD_EDGE_RVA), *(int*)((uintptr_t)g_exe + HOVER_ROAD_POINT_RVA),
                 *(__int64*)((uintptr_t)g_exe + HOVER_ROAD_NODE_RVA), seg.endLink);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(&seg.startLink, 0, sizeof(seg.startLink));
        memset(&seg.endLink, 0, sizeof(seg.endLink));
    }
}

// The local road edge / node of a link, 0 if not here. Caller wraps in __try.
static __int64 FindLinkEdge(const NetRoadLink& l)
{
    __int64 roads = RoadNetwork();
    if (!roads || !l.hasEdge) return 0;
    __int64* it = *(__int64**)(roads + MGR_SEGMENTS_BEGIN_OFFSET);
    __int64* end = *(__int64**)(roads + MGR_SEGMENTS_END_OFFSET);
    for (; it && it < end; ++it) {
        __int64 e = *it;
        if (!e) continue;
        __int64 n0 = *(__int64*)(e + SEG_NODE0_OFFSET);
        __int64 n1 = *(__int64*)(e + SEG_NODE1_OFFSET);
        if (!n0 || !n1 || !NodeNear(n0, l.edgeA) || !NodeNear(n1, l.edgeB)) continue;
        int count = (int)((*(__int64*)(e + SEG_POINTS_END_OFFSET) - *(__int64*)(e + SEG_POINTS_BEGIN_OFFSET)) / 0x18);
        if (count == l.edgePoints) return e;
    }
    return 0;
}

static __int64 FindLinkNode(const NetRoadLink& l)
{
    __int64 roads = RoadNetwork();
    if (!roads || !l.hasNode) return 0;
    __int64* it = *(__int64**)(roads + NET_NODES_OFFSET);
    __int64* end = *(__int64**)(roads + NET_NODES_OFFSET + 8);
    for (; it && it < end; ++it)
        if (*it && NodeNear(*it, l.node)) return *it;
    return 0;
}

// Puts a link into the hover globals (what the footpath tool reads); false
// if a part of it is not here (that part stays 0 - the end only touches).
static bool SetRoadHover(const NetRoadLink& l, const char* which)
{
    if (!l.hasEdge && !l.hasNode) return true;
    __int64 edge = 0, node = 0;
    __try {
        edge = FindLinkEdge(l);
        node = FindLinkNode(l);
        if (edge && (l.pointIndex < 0 || l.pointIndex >= l.edgePoints)) edge = 0;
        *(long long*)((uintptr_t)g_exe + HOVER_ROAD_EDGE_RVA) = edge;
        *(int*)((uintptr_t)g_exe + HOVER_ROAD_POINT_RVA) = edge ? l.pointIndex : 0;
        *(long long*)((uintptr_t)g_exe + HOVER_ROAD_NODE_RVA) = node;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    bool ok = (!l.hasEdge || edge) && (!l.hasNode || node);
    Logf("republicmp  NETWORKS:   %s joins a road: edge %s (point %d), node %s", which,
         !l.hasEdge ? "-" : edge ? "found" : "NOT FOUND", l.pointIndex,
         !l.hasNode ? "-" : node ? "found" : "NOT FOUND");
    return ok;
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

// True if the same path is already built: a segment between the same two
// nodes with the same number of interior points and the same middle point
// (either direction). Two different routes between the same nodes are both
// kept.
static bool SegmentBetweenNodesExists(const NetSegment& want)
{
    const float* a = want.startNode;
    const float* b = want.endNode;
    const NetCategoryInfo* info = Networks_GetCategoryInfo(want.category);
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
            bool forward = NodeNear(n0, a) && NodeNear(n1, b);
            bool backward = NodeNear(n0, b) && NodeNear(n1, a);
            if (!forward && !backward) continue;
            const NetPoint* pts = *(const NetPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
            const NetPoint* pend = *(const NetPoint**)(seg + SEG_POINTS_END_OFFSET);
            int count = (pts && pend > pts) ? (int)(pend - pts) : 0;
            if (count != want.pointCount) continue;
            if (count == 0) return true;
            int mid = count / 2;
            const float* q = pts[forward ? mid : count - 1 - mid].pos;
            const float* w = want.points[mid].pos;
            if (fabsf(q[0] - w[0]) < 0.5f && fabsf(q[2] - w[2]) < 0.5f) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// Rebuilds `seg` on this machine through the tool's own primitives (see
// networks.h). Returns false only if the replay threw; *outEngineAccepted is
// the engine's verdict right before the commit (+0x315 valid, no error).
// All spoofed input state is restored before returning.

// Margin around a replayed piece for the view-list scope (80 m around a
// road, 150 m around a building).
#define SCOPE_ROAD_MARGIN 80.0f

struct ScopeBox {
    float minX, minZ, maxX, maxZ;
    bool any;
};

static void BoxAdd(ScopeBox& b, const float p[3])
{
    if (!b.any) { b.minX = b.maxX = p[0]; b.minZ = b.maxZ = p[2]; b.any = true; return; }
    if (p[0] < b.minX) b.minX = p[0];
    if (p[0] > b.maxX) b.maxX = p[0];
    if (p[2] < b.minZ) b.minZ = p[2];
    if (p[2] > b.maxZ) b.maxZ = p[2];
}

static bool BeginScope(const ScopeBox& b, float margin)
{
    return b.any && ViewListScope_Begin(b.minX - margin, b.minZ - margin, b.maxX + margin, b.maxZ + margin);
}

static bool ApplySegmentImplInner(const NetSegment& seg, bool* outEngineAccepted);

// The replay sees the same "camera-visible" edges as the sender's does.
static bool ApplySegmentImpl(const NetSegment& seg, bool* outEngineAccepted)
{
    ScopeBox box = {};
    BoxAdd(box, seg.startNode);
    BoxAdd(box, seg.endNode);
    for (int i = 0; i < seg.pointCount && i < NET_MAX_POINTS; i++) BoxAdd(box, seg.points[i].pos);
    bool scoped = BeginScope(box, SCOPE_ROAD_MARGIN);
    bool ok = ApplySegmentImplInner(seg, outEngineAccepted);
    if (scoped) ViewListScope_End();
    return ok;
}

static bool ApplySegmentImplInner(const NetSegment& seg, bool* outEngineAccepted)
{
    *outEngineAccepted = false;

    const NetCategoryInfo* info = Networks_GetCategoryInfo(seg.category);
    if (!info || !info->enabled || seg.pointCount < 0 || seg.pointCount > NET_MAX_POINTS) {
        Logf("republicmp  NETWORKS: refusing segment category=%d points=%d", seg.category, seg.pointCount);
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
        Logf("republicmp  NETWORKS: %s manager is null", info->name);
        return false;
    }
    if (ToolWrapperGuardBusy(thisPtr)) {
        Logf("republicmp  NETWORKS: tool wrapper guard set, not applying");
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
            Logf("republicmp  NETWORKS: applying %s type=%d variant=%d points=%d start=(%.2f,%.2f,%.2f) end=(%.2f,%.2f,%.2f)",
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
            // attaches to the node already at this position. A footpath takes
            // its start's road link from the hover globals there.
            if (seg.category == NET_CATEGORY_PEDESTRIAN) SetRoadHover(seg.startLink, "start");
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
            else if (IsPowerLine(seg.category))
                CheckAttachedPowerNode(*(__int64*)(manager + MGR_START_NODE_OFFSET), seg.startNode, "start");
            *(float*)(manager + MGR_RELAX_TIMER_OFFSET) = 2.0f;

            // One drag tick with the sender's route as param_8: the engine
            // copies it into its route buffer instead of routing itself, and
            // matches the end node against existing nodes within +-1m.
            SetHoveredPort(seg.endPort, "end");
            if (seg.category == NET_CATEGORY_PEDESTRIAN) {
                HoverGlobals_Clear();
                SetRoadHover(seg.endLink, "end");
            }
            // Power lines: the tick then lifts the end by the pole height
            // (manager+0x24, 0x4FF3D4) unless a node or an edge is hovered -
            // even when the end matched a node. The sender pointed at the
            // node it ends on, so the replay hovers that node too; without it
            // the end hung 20m above the node, a separate node, and the line
            // carried no power on that PC (#22) and was missed by the
            // sender's demolition (#23).
            __int64 endHoverNode = 0;
            if (IsPowerLine(seg.category) && !seg.endPort.valid) {
                endHoverNode = FindNetworkNodeAt(manager, seg.endNode);
                if (endHoverNode) *(long long*)((uintptr_t)g_exe + HOVER_NODE_RVA) = endHoverNode;
            }
            RawVectorHeader route = { seg.points, seg.points + seg.pointCount, seg.points + seg.pointCount };
            float end[3] = { seg.endNode[0], seg.endNode[1], seg.endNode[2] };
            *lmbHeld = 1;
            ((Fcaf0Fn)((uintptr_t)g_exe + FCAF0_RVA))(manager, end, seg.typeId, (unsigned int)seg.variant,
                                                      0, 0, 0, (__int64)&route, 0, 0);
            if (*(unsigned char*)(manager + MGR_END_ATTACHED_OFFSET) == 0)
                SetNodePos(*(__int64*)(manager + MGR_END_NODE_OFFSET), seg.endNode);
            else if (IsPowerLine(seg.category))
                CheckAttachedPowerNode(*(__int64*)(manager + MGR_END_NODE_OFFSET), seg.endNode, "end");
            if (IsPowerLine(seg.category))
                Logf("republicmp  NETWORKS:   power line end: %s", endHoverNode ? "on an existing node (hovered)" : "free");

            unsigned char valid = *(unsigned char*)(manager + MGR_VALID_OFFSET);
            int errCategory = *(int*)(manager + MGR_ERROR_CATEGORY_OFFSET);
            unsigned int errSubcode = *(unsigned int*)(manager + MGR_ERROR_SUBCODE_OFFSET);
            Logf("republicmp  NETWORKS:   attach start=%d end=%d valid=%d errCategory=%d errSubcode=0x%x",
                 (int)*(unsigned char*)(manager + MGR_START_ATTACHED_OFFSET),
                 (int)*(unsigned char*)(manager + MGR_END_ATTACHED_OFFSET),
                 (int)valid, errCategory, errSubcode);
            *outEngineAccepted = (valid == 1) && (errCategory == -1);

            // Release: the wrapper commits and ends its drag.
            *lmbHeld = 0;
            wrapper(thisPtr, seg.typeId, (unsigned int)seg.variant);
            if (*(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) != 0)
                Logf("republicmp  NETWORKS:   WARNING drag still active after release");
            if (endHoverNode) *(long long*)((uintptr_t)g_exe + HOVER_NODE_RVA) = 0;
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
            if (seg.category == NET_CATEGORY_PEDESTRIAN) HoverGlobals_Clear();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        threw = true;
    }

    if (threw) {
        Logf("republicmp  NETWORKS: <exception applying segment>");
        *outEngineAccepted = false;
        // The release never reached the wrapper's own end of the drag
        // (0x3D6A3F: thisPtr+0xd2ac = 0). Left set, the per-frame tool code
        // (FUN_14030d1a0) keeps working on the half-built drag and crashes
        // the game a frame later (0x310E58). End it the way the wrapper and
        // the demolish replay do.
        __try {
            *(unsigned char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) = 0;
            *(long long*)(manager + MGR_ROAD_LINK_OFFSET) = 0;
            *(long long*)(manager + MGR_ROAD_LINK_EXTRA_OFFSET) = 0;
            ((ManagerResetFn)((uintptr_t)g_exe + MANAGER_RESET_RVA))(manager, manager + MANAGER_RESET_STATE_OFFSET);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Logf("republicmp  NETWORKS: resetting the tool after the exception threw too");
        }
        HoverGlobals_Clear();
        return false;
    }
    Logf("republicmp  NETWORKS:   apply done - %s", *outEngineAccepted ? "engine ACCEPTED" : "engine REJECTED");
    return true;
}

static bool ApplySegment(const NetSegment& seg, bool* outEngineAccepted)
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
static __int64 FindLocalSegment(__int64 manager, const NetPiece& piece, bool* outReversed)
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
static bool ApplyUpgradePieceInner(int category, const NetUpgradeMsg& msg, const NetPiece& piece);

static bool ApplyUpgradePiece(int category, const NetUpgradeMsg& msg, const NetPiece& piece)
{
    ScopeBox box = {};
    BoxAdd(box, piece.segStart);
    BoxAdd(box, piece.segEnd);
    bool scoped = BeginScope(box, SCOPE_ROAD_MARGIN * 2);
    bool ok = ApplyUpgradePieceInner(category, msg, piece);
    if (scoped) ViewListScope_End();
    return ok;
}

static bool ApplyUpgradePieceInner(int category, const NetUpgradeMsg& msg, const NetPiece& piece)
{
    const NetCategoryInfo* info = Networks_GetCategoryInfo(category);
    __try {
        __int64 manager = *(__int64*)(GetWorldThisPtr() + info->managerOffset);
        if (!manager) return false;

        bool reversed = false;
        __int64 seg = FindLocalSegment(manager, piece, &reversed);
        if (!seg) {
            Logf("republicmp  NETWORKS: upgrade: no local %s segment at start=(%.2f,%.2f,%.2f) end=(%.2f,%.2f,%.2f) points=%d",
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
            Logf("republicmp  NETWORKS: upgrade: bad range (%d,%d) for %d points", rs, re, count);
            return false;
        }

        // Range ends: a node or an interior point of the segment.
        const NetPoint* pts = *(const NetPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
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

        Logf("republicmp  NETWORKS: %s %s%s range=(%d,%d)/%d type %d/%d -> %d/%d type394=%d dir=%d",
             piece.oldType == 0x65 ? "DEMOLISH" : "upgrade",
             info->name, reversed ? " (reversed)" : "", rs, re, count,
             *(int*)(seg + SEG_TYPE_OFFSET), *(int*)(seg + SEG_VARIANT_OFFSET), msg.newType, msg.newVariant,
             piece.oldType, dir);

        RoadCommitFn commit = g_origRoadCommit ? g_origRoadCommit : (RoadCommitFn)((uintptr_t)g_exe + COMMIT_RVA);
        commit(manager, 0, 0, 0, 0, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  NETWORKS: <exception applying upgrade piece>");
        return false;
    }
}

// Returns the number of pieces applied.
static int ApplyUpgrade(const NetUpgradeMsg& msg)
{
    const NetCategoryInfo* info = Networks_GetCategoryInfo(msg.category);
    if (!info || !info->enabled || msg.pieceCount < 1 || msg.pieceCount > NET_MAX_UPGRADE_PIECES) {
        Logf("republicmp  NETWORKS: refusing upgrade category=%d pieces=%d", msg.category, msg.pieceCount);
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
// Demolition (terrain_demolish tool)
// ---------------------------------------------------------------------------

// Where `pos` lies on segment `seg`: -1 = its start node, count = its end
// node, an interior point index, or -2 if it is none of them.
static int IndexOnSegment(__int64 seg, const float pos[3])
{
    const NetPoint* pts = *(const NetPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
    const NetPoint* end = *(const NetPoint**)(seg + SEG_POINTS_END_OFFSET);
    int count = (pts && end > pts) ? (int)(end - pts) : 0;
    __int64 n0 = *(__int64*)(seg + SEG_NODE0_OFFSET);
    __int64 n1 = *(__int64*)(seg + SEG_NODE1_OFFSET);
    if (n0 && NodeNear(n0, pos)) return -1;
    if (n1 && NodeNear(n1, pos)) return count;
    for (int i = 0; i < count; i++) {
        const float* q = pts[i].pos;
        if (fabsf(q[0] - pos[0]) < 0.05f && fabsf(q[2] - pos[2]) < 0.05f && fabsf(q[1] - pos[1]) < 0.5f)
            return i;
    }
    return -2;
}

static unsigned Fnv(const void* data, size_t n, unsigned h = 2166136261u)
{
    for (size_t i = 0; i < n; i++) {
        h ^= ((const unsigned char*)data)[i];
        h *= 16777619u;
    }
    return h | 1u;
}

static unsigned EdgeCheck(__int64 seg)
{
    float w[7] = {};
    memcpy(w, (const float*)(*(__int64*)(seg + SEG_NODE0_OFFSET) + NODE_POS_OFFSET), 12);
    memcpy(w + 3, (const float*)(*(__int64*)(seg + SEG_NODE1_OFFSET) + NODE_POS_OFFSET), 12);
    int points = (int)((*(__int64*)(seg + SEG_POINTS_END_OFFSET) - *(__int64*)(seg + SEG_POINTS_BEGIN_OFFSET)) / 0x18);
    memcpy(w + 6, &points, 4);
    return Fnv(w, sizeof(w));
}

// A node's check: its x and z. Its height can differ in the last bits between
// the PCs (a test found the same node by position but not by its full bits).
static unsigned NodeCheck(__int64 node)
{
    const float* p = (const float*)(node + NODE_POS_OFFSET);
    float xz[2] = { p[0], p[2] };
    return Fnv(xz, sizeof(xz));
}

bool Networks_EdgeIdentity(__int64 manager, __int64 seg, int* index, unsigned* check)
{
    *index = -1;
    *check = 0;
    __try {
        __int64* begin = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
        if (!seg || !begin || end < begin) return false;
        for (__int64* it = begin; it < end; ++it)
            if (*it == seg) {
                if (!*(__int64*)(seg + SEG_NODE0_OFFSET) || !*(__int64*)(seg + SEG_NODE1_OFFSET)) return false;
                *index = (int)(it - begin);
                *check = EdgeCheck(seg);
                return true;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *index = -1;
    }
    return false;
}

// The segment at index in the manager's list when its check matches, or 0.
static __int64 EdgeAt(__int64 manager, int index, unsigned check)
{
    __int64* begin = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
    __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
    if (index < 0 || !begin || end < begin || index >= end - begin) return 0;
    __int64 seg = begin[index];
    if (!seg || !*(__int64*)(seg + SEG_NODE0_OFFSET) || !*(__int64*)(seg + SEG_NODE1_OFFSET)) return 0;
    return EdgeCheck(seg) == check ? seg : 0;
}

// A segment of the manager that has both `a` and `b` on it (nodes or
// interior points), or 0.
static __int64 FindSegmentWithRange(__int64 manager, const float a[3], const float b[3])
{
    __int64* it = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
    __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
    for (; it && it < end; ++it) {
        __int64 seg = *it;
        if (!seg || !*(__int64*)(seg + SEG_NODE0_OFFSET) || !*(__int64*)(seg + SEG_NODE1_OFFSET)) continue;
        int ia = IndexOnSegment(seg, a);
        if (ia == -2) continue;
        int ib = IndexOnSegment(seg, b);
        if (ib != -2 && ib != ia) return seg;
    }
    return 0;
}

#define NODE_ALLOC_RVA 0x84DECC // operator new
#define NODE_CTOR_RVA 0x0FF950  // node constructor (returns the node)
#define NODE_OBJECT_SIZE 0xE8
typedef __int64 (*AllocFn)(unsigned long long size);
typedef __int64 (*NodeCtorFn)(__int64 node);

// A pick that reached a segment end may hold that segment's own node as the
// drag's start/end node (+0x748/+0x749 set, the node in the network's list).
// Moving it to a cut point dragged the rest of the line along: a whole-segment
// pick cut down to the sender's middle piece moved the segment's first node
// (at a switch's port) onto the cut, and the next demolitions removed the
// wrong pieces - a couple of spans stayed on the receiver (#23). Before the
// node moves, put a fresh copy in its place, as the drag tick itself does
// (0x4FE448..0x4FE48A).
// A drag node of the manager (+0x428 / +0x430), made when missing.
#define OPERATOR_NEW_RVA 0x84DECC
#define NODE_CTOR_RVA 0x0FF950
static const unsigned char kOperatorNewEntry[16] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
                                                     0xd9, 0xeb, 0x21, 0x48, 0x8b, 0xcb, 0xe8, 0xd5 };
static void EnsureDragNode(__int64 manager, int nodeOffset)
{
    if (*(__int64*)(manager + nodeOffset)) return;
    if (memcmp((const void*)((uintptr_t)g_exe + OPERATOR_NEW_RVA), kOperatorNewEntry, sizeof(kOperatorNewEntry)) != 0) return;
    typedef void* (*NewFn)(size_t);
    typedef __int64 (*NodeCtorFn)(void*);
    void* raw = ((NewFn)((uintptr_t)g_exe + OPERATOR_NEW_RVA))(0xE8);
    if (!raw) return;
    __int64 node = ((NodeCtorFn)((uintptr_t)g_exe + NODE_CTOR_RVA))(raw);
    *(__int64*)(manager + nodeOffset) = node;
    Logf("republicmp  NETWORKS:   made the manager's %s drag node for the demolition", nodeOffset == MGR_START_NODE_OFFSET ? "start" : "end");
}

static void DetachDragNode(__int64 manager, int nodeOffset, int attachedOffset, const float target[3])
{
    __int64 node = *(__int64*)(manager + nodeOffset);
    if (!node) return;
    const float* p = (const float*)(node + NODE_POS_OFFSET);
    if (p[0] == target[0] && p[1] == target[1] && p[2] == target[2]) return; // stays where it is
    if (!*(unsigned char*)(manager + attachedOffset) && !InPointerList(manager + NET_NODES_OFFSET, node)) return;
    __int64 fresh = ((AllocFn)((uintptr_t)g_exe + NODE_ALLOC_RVA))(NODE_OBJECT_SIZE);
    if (!fresh) return;
    ((NodeCtorFn)((uintptr_t)g_exe + NODE_CTOR_RVA))(fresh);
    memcpy((void*)(fresh + NODE_POS_OFFSET), p, 12);
    *(__int64*)(manager + nodeOffset) = fresh;
    *(unsigned char*)(manager + attachedOffset) = 0;
    Logf("republicmp  NETWORKS:   the cut's %s held the segment's own node at (%.2f,%.2f,%.2f) - a fresh node takes its place",
         nodeOffset == MGR_START_NODE_OFFSET ? "start" : "end", p[0], p[1], p[2]);
}

// After fcaf0 armed a demolish on segment +0x380: if its range is not the
// sender's, cut the sender's range instead - indices, the route buffer of
// the points strictly inside, and the two end nodes on the range ends (the
// same state the upgrade replay builds). If the range is on another segment
// of the same type, that segment is cut instead. Returns false if the
// sender's range was not found (nothing is changed then).
static bool AdoptSenderRange(__int64 manager, const NetDemolishMsg& m, int* engineRs, int* engineRe,
                             int* rsOut, int* reOut)
{
    __int64 seg = *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET);
    *engineRs = *(int*)(manager + MGR_UPG_RANGE_START_OFFSET);
    *engineRe = *(int*)(manager + MGR_UPG_RANGE_END_OFFSET);
    *rsOut = *engineRs;
    *reOut = *engineRe;
    if (!seg || !m.hasRange || *(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) != 0) return false;

    int a = IndexOnSegment(seg, m.rangeA);
    int b = IndexOnSegment(seg, m.rangeB);
    bool switched = false;
    if (a == -2 || b == -2 || a == b) {
        // The engine picked another segment (e.g. the first one of a hovered
        // junction node): move to the sender's if it is of the same kind.
        __int64 other = FindSegmentWithRange(manager, m.rangeA, m.rangeB);
        if (!other || *(int*)(other + SEG_TYPE_OFFSET) != *(int*)(seg + SEG_TYPE_OFFSET) ||
            *(int*)(other + SEG_VARIANT_OFFSET) != *(int*)(seg + SEG_VARIANT_OFFSET))
            return false;
        seg = other;
        a = IndexOnSegment(seg, m.rangeA);
        b = IndexOnSegment(seg, m.rangeB);
        switched = true; // written to +0x380 only once the range is accepted
    }
    int rs = a < b ? a : b, re = a < b ? b : a;
    const NetPoint* pts = *(const NetPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
    int count = (int)(*(const NetPoint**)(seg + SEG_POINTS_END_OFFSET) - pts);
    if (!switched && rs == *engineRs && re == *engineRe) return true; // same piece - nothing to change
    // A cut at the first/last interior point would leave a stub between it
    // and the node with no points at all; the game's frame code reads the
    // first point of every segment on a node (SOVIET64+0x526605) and crashes
    // on it. The engine snaps such ends to the node - do the same (the sender
    // may have had more points on its copy of the segment). Power-line spans
    // need no points (the game lays them with none), so a power line is cut
    // exactly where the sender cut it - snapped, it lost the next span too and
    // the sender's demolition of that span found it already gone.
    if (!IsPowerLine(m.category)) {
        if (rs == 0) rs = -1;
        if (re == count - 1) re = count;
    }
    if (rs >= re) return false;
    *rsOut = rs;
    *reOut = re;
    if (!switched && rs == *engineRs && re == *engineRe) return true;

    const float* pa = (rs < 0) ? (const float*)(*(__int64*)(seg + SEG_NODE0_OFFSET) + NODE_POS_OFFSET) : pts[rs].pos;
    const float* pb = (re >= count) ? (const float*)(*(__int64*)(seg + SEG_NODE1_OFFSET) + NODE_POS_OFFSET) : pts[re].pos;
    float splitStart[3] = { pa[0], pa[1], pa[2] };
    float splitEnd[3] = { pb[0], pb[1], pb[2] };

    int first = rs + 1, n = re - first;
    RawVectorHeader route = { pts + first, pts + first + (n > 0 ? n : 0), pts + first + (n > 0 ? n : 0) };
    ((VecAssignFn)((uintptr_t)g_exe + VEC_ASSIGN_RVA))((__int64*)(manager + MGR_ROUTE_BEGIN_OFFSET), &route);
    *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET) = seg;
    *(int*)(manager + MGR_UPG_RANGE_START_OFFSET) = rs;
    *(int*)(manager + MGR_UPG_RANGE_END_OFFSET) = re;
    DetachDragNode(manager, MGR_START_NODE_OFFSET, MGR_START_ATTACHED_OFFSET, splitStart);
    DetachDragNode(manager, MGR_END_NODE_OFFSET, MGR_END_ATTACHED_OFFSET, splitEnd);
    SetNodePos(*(__int64*)(manager + MGR_START_NODE_OFFSET), splitStart);
    SetNodePos(*(__int64*)(manager + MGR_END_NODE_OFFSET), splitEnd);
    return true;
}

// Repeats one frame of the demolish tool at the sender's cursor: reset the
// manager's preview state (as the tool does every frame), let fcaf0 pick the
// piece under the cursor, and commit if it found one. outRemoved = the engine
// armed and ran the commit.
static bool ApplyDemolishInner(const NetDemolishMsg& m, bool* outRemoved);

// The pick finds the piece wherever this player's camera is.
// Lockstep: the hovered node / edge a hover demolition carries (main thread, during its apply).
static const NetDemolishHover* g_applyHover = nullptr;
static const unsigned kHoverNodeRvas[2] = { 0x9E4560, 0x9E45E0 };
static const unsigned kHoverEdgeRvas[2] = { 0x9E4558, 0x9E45D8 };

static bool SamePos(const float* a, const float* b)
{
    return fabsf(a[0] - b[0]) < 0.01f && fabsf(a[1] - b[1]) < 0.01f && fabsf(a[2] - b[2]) < 0.01f;
}

static int EdgePointCount(__int64 edge)
{
    return (int)((*(__int64*)(edge + SEG_POINTS_END_OFFSET) - *(__int64*)(edge + SEG_POINTS_BEGIN_OFFSET)) / 0x18);
}

bool Networks_ReadDemolishHover(__int64 manager, NetDemolishHover* h)
{
    memset(h, 0, sizeof(*h));
    __try {
        __int64* begin = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
        for (int k = 0; k < 2 && !h->nodeKind; k++) {
            __int64 node = *(__int64*)((uintptr_t)g_exe + kHoverNodeRvas[k]);
            for (__int64* it = begin; node && it && it < end; ++it)
                if (*it && (*(__int64*)(*it + SEG_NODE0_OFFSET) == node || *(__int64*)(*it + SEG_NODE1_OFFSET) == node)) {
                    h->nodeKind = k + 1;
                    memcpy(h->node, (const float*)(node + NODE_POS_OFFSET), 12);
                    break;
                }
        }
        h->nodeIndex = -1;
        if (h->nodeKind) {
            __int64 node = *(__int64*)((uintptr_t)g_exe + kHoverNodeRvas[h->nodeKind - 1]);
            __int64* nb = *(__int64**)(manager + NET_NODES_OFFSET);
            __int64* ne = *(__int64**)(manager + NET_NODES_OFFSET + 8);
            for (__int64* it = nb; nb && it < ne; ++it)
                if (*it == node) {
                    h->nodeIndex = (int)(it - nb);
                    h->nodeCheck = NodeCheck(node);
                    break;
                }
        }
        for (int k = 0; k < 2 && !h->edgeKind; k++) {
            __int64 edge = *(__int64*)((uintptr_t)g_exe + kHoverEdgeRvas[k]);
            for (__int64* it = begin; edge && it && it < end; ++it)
                if (*it == edge) {
                    __int64 n0 = *(__int64*)(edge + SEG_NODE0_OFFSET), n1 = *(__int64*)(edge + SEG_NODE1_OFFSET);
                    if (!n0 || !n1) break;
                    h->edgeKind = k + 1;
                    memcpy(h->edgeA, (const float*)(n0 + NODE_POS_OFFSET), 12);
                    memcpy(h->edgeB, (const float*)(n1 + NODE_POS_OFFSET), 12);
                    h->edgePoints = EdgePointCount(edge);
                    break;
                }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(h, 0, sizeof(*h));
        return false;
    }
    return h->nodeKind || h->edgeKind;
}

// Sets this PC's hovered node / edge to the carried ones (after the replay cleared them).
// True when the carried node and edge were both found here.
static bool ApplyHoverRefs(__int64 manager, const NetDemolishHover& h)
{
    __int64* begin = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
    __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
    __int64 node = 0, edge = 0;
    const char* nodeBy = "position";
    if (h.nodeKind && h.nodeIndex >= 0) {
        // The node by its place in the committed nodes, which every PC has alike.
        __int64* nb = *(__int64**)(manager + NET_NODES_OFFSET);
        __int64* ne = *(__int64**)(manager + NET_NODES_OFFSET + 8);
        if (nb && ne > nb && h.nodeIndex < ne - nb && nb[h.nodeIndex] && NodeCheck(nb[h.nodeIndex]) == h.nodeCheck) {
            node = nb[h.nodeIndex];
            nodeBy = "list place";
        }
    }
    for (__int64* it = begin; it && it < end; ++it) {
        __int64 e = *it;
        if (!e) continue;
        __int64 n0 = *(__int64*)(e + SEG_NODE0_OFFSET), n1 = *(__int64*)(e + SEG_NODE1_OFFSET);
        if (!n0 || !n1) continue;
        const float* p0 = (const float*)(n0 + NODE_POS_OFFSET);
        const float* p1 = (const float*)(n1 + NODE_POS_OFFSET);
        if (h.nodeKind && !node) {
            if (SamePos(p0, h.node)) node = n0;
            else if (SamePos(p1, h.node)) node = n1;
        }
        if (h.edgeKind && !edge && SamePos(p0, h.edgeA) && SamePos(p1, h.edgeB) && EdgePointCount(e) == h.edgePoints) edge = e;
    }
    if (h.nodeKind && !node) {
        // Any committed node at the place (x and z close, the height near).
        __int64* nb = *(__int64**)(manager + NET_NODES_OFFSET);
        __int64* ne = *(__int64**)(manager + NET_NODES_OFFSET + 8);
        for (__int64* it = nb; nb && it < ne && !node; ++it) {
            if (!*it) continue;
            const float* p = (const float*)(*it + NODE_POS_OFFSET);
            if (fabsf(p[0] - h.node[0]) < 0.05f && fabsf(p[2] - h.node[2]) < 0.05f && fabsf(p[1] - h.node[1]) < 0.5f) {
                node = *it;
                nodeBy = "place among the nodes";
            }
        }
    }
    if (node) *(__int64*)((uintptr_t)g_exe + kHoverNodeRvas[h.nodeKind - 1]) = node;
    if (edge) *(__int64*)((uintptr_t)g_exe + kHoverEdgeRvas[h.edgeKind - 1]) = edge;
    Logf("republicmp  NETWORKS:   hover demolition: node %s%s%s (sender's #%d at (%.2f,%.2f,%.2f)), edge %s",
         !h.nodeKind ? "none" : node ? "found by " : "MISSING", node ? nodeBy : "", "", h.nodeIndex, h.node[0], h.node[1],
         h.node[2], !h.edgeKind ? "none" : edge ? "found" : "MISSING");
    return (!h.nodeKind || node) && (!h.edgeKind || edge);
}

bool Networks_ApplyCommandDemolishHover(const NetDemolishHoverMsg& msg)
{
    g_applyHover = &msg.hover;
    bool ok = Networks_ApplyCommandDemolish(msg.m);
    g_applyHover = nullptr;
    return ok;
}

static bool ApplyDemolish(const NetDemolishMsg& m, bool* outRemoved)
{
    ScopeBox box = {};
    BoxAdd(box, m.cursor);
    if (m.hasRange) { BoxAdd(box, m.rangeA); BoxAdd(box, m.rangeB); }
    bool scoped = BeginScope(box, SCOPE_ROAD_MARGIN);
    bool ok = ApplyDemolishInner(m, outRemoved);
    if (scoped) ViewListScope_End();
    return ok;
}

static bool ApplyDemolishInner(const NetDemolishMsg& m, bool* outRemoved)
{
    *outRemoved = false;
    int engineRs = 0, engineRe = 0, rs = 0, re = 0;
    bool adopted = false, viaNode = false, direct = false;
    const NetCategoryInfo* info = Networks_GetCategoryInfo(m.category);
    if (!info || !info->enabled) {
        Logf("republicmp  NETWORKS: refusing demolish category=%d", m.category);
        return false;
    }
    __int64 thisPtr = GetWorldThisPtr();
    if (ToolWrapperGuardBusy(thisPtr)) {
        Logf("republicmp  NETWORKS:   demolition not made - the tool wrapper guard is set on this PC");
        return false;
    }
    const char* why = nullptr; // why nothing was removed, for the log
    bool byIdentity = false;

    unsigned char* lmbHeld = (unsigned char*)((uintptr_t)g_exe + LMB_HELD_RVA);
    unsigned char* lmbJustPressed = (unsigned char*)((uintptr_t)g_exe + LMB_JUST_PRESSED_RVA);
    ManagerResetFn reset = (ManagerResetFn)((uintptr_t)g_exe + MANAGER_RESET_RVA);
    Fcaf0Fn fcaf0 = (Fcaf0Fn)((uintptr_t)g_exe + FCAF0_RVA);
    RoadCommitFn commit = g_origRoadCommit ? g_origRoadCommit : (RoadCommitFn)((uintptr_t)g_exe + COMMIT_RVA);

    bool threw = false;
    InterlockedIncrement(&g_applyingSegment);
    __try {
        __int64 manager = *(__int64*)(thisPtr + info->managerOffset);
        if (manager) {
            unsigned char savedHeld = *lmbHeld, savedPressed = *lmbJustPressed;
            float* cursor = (float*)(thisPtr + CURSOR_X_OFFSET);
            float savedCursor[3] = { cursor[0], cursor[1], cursor[2] };
            __try {
                HoverGlobals_Clear();
                // Without the sender's node or edge the cursor would pick another piece here: nothing is cut.
                bool hoverOk = !g_applyHover || ApplyHoverRefs(manager, *g_applyHover);
                if (!hoverOk) why = "the sender's hovered node or edge is not here";
                *lmbHeld = 0;
                *lmbJustPressed = 0;
                cursor[0] = m.cursor[0]; cursor[1] = m.cursor[1]; cursor[2] = m.cursor[2];
                reset(manager, manager + MANAGER_RESET_STATE_OFFSET);
                float c[3] = { m.cursor[0], m.cursor[1], m.cursor[2] };
                fcaf0(manager, c, ROAD_DEMOLISH_TYPE, 0, 0, 0, 0, 0, 0, 0);
                if (m.hasRange && m.edgeIndex >= 0) {
                    // The sender's segment by identity: when the cursor picked
                    // another piece or none, it is armed directly.
                    __int64 seg = EdgeAt(manager, m.edgeIndex, m.edgeCheck);
                    if (!seg)
                        why = "the sender's segment is not here (its place in the list or its check differs)";
                    else if (*(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) == 0 ||
                             *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET) != seg) {
                        EnsureDragNode(manager, MGR_START_NODE_OFFSET);
                        EnsureDragNode(manager, MGR_END_NODE_OFFSET);
                        if (*(__int64*)(manager + MGR_START_NODE_OFFSET) && *(__int64*)(manager + MGR_END_NODE_OFFSET)) {
                            *(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) = 0;
                            *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET) = seg;
                            *(int*)(manager + MGR_UPG_RANGE_START_OFFSET) = -2; // never the sender's, so it is written
                            *(int*)(manager + MGR_UPG_RANGE_END_OFFSET) = -2;
                            *(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) = 1;
                            direct = true;
                            byIdentity = true;
                        } else {
                            why = "no drag nodes to arm the sender's segment with";
                        }
                    }
                }
                if (*(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) == 0 && m.hasRange) {
                    // Picking by cursor skips short segments; the tool removes
                    // those through the hovered node (FUN_1403abc90 ~290-320,
                    // fcaf0 ~1833 / ~2032). Do the same: hover the local node
                    // at an end of the sender's piece and run the frame again.
                    __int64 seg = FindSegmentWithRange(manager, m.rangeA, m.rangeB);
                    if (seg) {
                        int ia = IndexOnSegment(seg, m.rangeA), ib = IndexOnSegment(seg, m.rangeB);
                        int lo = ia < ib ? ia : ib, hi = ia < ib ? ib : ia;
                        const NetPoint* pb = *(const NetPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
                        const NetPoint* pe = *(const NetPoint**)(seg + SEG_POINTS_END_OFFSET);
                        int count = (pb && pe > pb) ? (int)(pe - pb) : 0;
                        // The node the piece touches (node 0 for an interior piece)
                        // first, then the other end: a building's whole access
                        // road starts at the building's port node, whose first
                        // edge is not the road.
                        __int64 first = *(__int64*)(seg + (lo < 0 || hi < count ? SEG_NODE0_OFFSET : SEG_NODE1_OFFSET));
                        __int64 other = *(__int64*)(seg + (lo < 0 || hi < count ? SEG_NODE1_OFFSET : SEG_NODE0_OFFSET));
                        __int64 tryNodes[2] = { first, other };
                        for (int k = 0; k < 2 && !viaNode; k++) {
                            if (!tryNodes[k]) continue;
                            *(__int64*)((uintptr_t)g_exe + HOVER_NODE_RVA) = tryNodes[k];
                            reset(manager, manager + MANAGER_RESET_STATE_OFFSET);
                            fcaf0(manager, c, ROAD_DEMOLISH_TYPE, 0, 0, 0, 0, 0, 0, 0);
                            viaNode = *(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) != 0;
                        }
                        // The sender's tool hit this piece with its camera's ray
                        // (the hover edge, 0x9E4558); the replay has only the
                        // cursor, and next to a building neither the cursor nor
                        // a node picks it. Arm it the way fcaf0's node path does
                        // (0x4FEA52: +0x380 piece, +0x388/+0x38c range, +0x390
                        // hit) - AdoptSenderRange then writes the sender's range,
                        // route and cut points - if the drag nodes are there.
                        // The drag nodes exist only where this network's tool was
                        // used: a PC without them made the fallback, the other did not
                        // (a forklift junction stayed on one PC). They are made here
                        // as fcaf0 makes them (operator new 0xe8, node constructor ff950).
                        if (!viaNode) {
                            EnsureDragNode(manager, MGR_START_NODE_OFFSET);
                            EnsureDragNode(manager, MGR_END_NODE_OFFSET);
                        }
                        if (!viaNode && *(__int64*)(manager + MGR_START_NODE_OFFSET) &&
                            *(__int64*)(manager + MGR_END_NODE_OFFSET)) {
                            *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET) = seg;
                            *(int*)(manager + MGR_UPG_RANGE_START_OFFSET) = -2; // never the sender's, so it is written
                            *(int*)(manager + MGR_UPG_RANGE_END_OFFSET) = -2;
                            *(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) = 1;
                            direct = true;
                        }
                    }
                }
                if (*(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) != 0) {
                    adopted = AdoptSenderRange(manager, m, &engineRs, &engineRe, &rs, &re);
                    if ((viaNode || direct) && m.hasRange) {
                        // For the log: what the fallback is about to cut.
                        __int64 seg = *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET);
                        if (seg) {
                            const float* n0 = (const float*)(*(__int64*)(seg + SEG_NODE0_OFFSET) + NODE_POS_OFFSET);
                            const float* n1 = (const float*)(*(__int64*)(seg + SEG_NODE1_OFFSET) + NODE_POS_OFFSET);
                            const NetPoint* pb = *(const NetPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
                            const NetPoint* pe = *(const NetPoint**)(seg + SEG_POINTS_END_OFFSET);
                            Logf("republicmp  NETWORKS:   fallback segment (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) %d point(s), cut (%d,%d); sender's piece (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)",
                                 n0[0], n0[1], n0[2], n1[0], n1[1], n1[2], (pb && pe > pb) ? (int)(pe - pb) : 0,
                                 *(int*)(manager + MGR_UPG_RANGE_START_OFFSET), *(int*)(manager + MGR_UPG_RANGE_END_OFFSET),
                                 m.rangeA[0], m.rangeA[1], m.rangeA[2], m.rangeB[0], m.rangeB[1], m.rangeB[2]);
                        }
                    }
                    // Through the node, or armed directly, the engine's own pick
                    // is only a stand-in for the sender's piece - never cut it on
                    // its own.
                    if (hoverOk && (adopted || (!viaNode && !direct))) {
                        commit(manager, 0, 0, 0, 0, 0, 0);
                        *outRemoved = true;
                    } else if (!why) {
                        why = "the sender's range is not on the armed segment";
                    }
                } else if (!why) {
                    why = m.hasRange ? "the sender's piece is not under the cursor, at its nodes or on any segment here"
                                     : "nothing is under the cursor here";
                }
                reset(manager, manager + MANAGER_RESET_STATE_OFFSET);
            } __finally {
                HoverGlobals_Clear(); // the hovered node may be gone now
                *lmbHeld = savedHeld;
                *lmbJustPressed = savedPressed;
                cursor[0] = savedCursor[0]; cursor[1] = savedCursor[1]; cursor[2] = savedCursor[2];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        threw = true;
    }
    InterlockedDecrement(&g_applyingSegment);
    if (threw) Logf("republicmp  NETWORKS: <exception applying demolish>");
    else if (!*outRemoved)
        Logf("republicmp  NETWORKS:   demolition not made - %s (cursor (%.2f,%.2f,%.2f), segment #%d)", why ? why : "?",
             m.cursor[0], m.cursor[1], m.cursor[2], m.edgeIndex);
    else if (*outRemoved) {
        if (byIdentity)
            Logf("republicmp  NETWORKS:   the cursor picked another piece or none - armed the sender's segment #%d directly",
                 m.edgeIndex);
        else if (direct)
            Logf("republicmp  NETWORKS:   not picked at the cursor or its nodes - armed directly on the piece");
        else if (viaNode)
            Logf("republicmp  NETWORKS:   nothing picked at the cursor - armed through the piece's node");
        if (!m.hasRange)
            Logf("republicmp  NETWORKS:   range (%d,%d) chosen here (sender sent no range)", engineRs, engineRe);
        else if (!adopted)
            Logf("republicmp  NETWORKS:   sender's range is not on the segment here - kept this engine's (%d,%d)",
                 engineRs, engineRe);
        else if (rs != engineRs || re != engineRe)
            Logf("republicmp  NETWORKS:   range (%d,%d) -> sender's (%d,%d)", engineRs, engineRe, rs, re);
        else
            Logf("republicmp  NETWORKS:   range (%d,%d) = sender's", rs, re);
    }
    return !threw;
}

// Multi-segment demolish: each whole segment found here by its nodes is
// demolished on its own - cursor on its middle, range node to node.
static bool ApplyDemolishPieces(const NetDemolishMsg& m, bool* outRemoved)
{
    *outRemoved = false;
    const NetCategoryInfo* info = Networks_GetCategoryInfo(m.category);
    if (!info || !info->enabled) return false;
    bool ok = true;
    for (int i = 0; i < m.pieceCount && i < 8; i++) {
        NetDemolishMsg one = {};
        one.category = m.category;
        one.edgeIndex = -1;
        bool found = false;
        __try {
            __int64 manager = *(__int64*)(GetWorldThisPtr() + info->managerOffset);
            bool reversed = false;
            __int64 seg = manager ? FindLocalSegment(manager, m.pieces[i], &reversed) : 0;
            if (seg) {
                const NetPoint* pts = *(const NetPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
                const NetPoint* pend = *(const NetPoint**)(seg + SEG_POINTS_END_OFFSET);
                int count = (pts && pend > pts) ? (int)(pend - pts) : 0;
                const float* n0 = (const float*)(*(__int64*)(seg + SEG_NODE0_OFFSET) + NODE_POS_OFFSET);
                const float* n1 = (const float*)(*(__int64*)(seg + SEG_NODE1_OFFSET) + NODE_POS_OFFSET);
                if (count > 0) memcpy(one.cursor, pts[count / 2].pos, 12);
                else for (int k = 0; k < 3; k++) one.cursor[k] = (n0[k] + n1[k]) * 0.5f;
                memcpy(one.rangeA, n0, 12);
                memcpy(one.rangeB, n1, 12);
                one.hasRange = 1;
                Networks_EdgeIdentity(manager, seg, &one.edgeIndex, &one.edgeCheck); // found here: its own identity
                found = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!found) {
            Logf("republicmp  NETWORKS:   demolish piece %d: no local segment (start=(%.2f,%.2f,%.2f) end=(%.2f,%.2f,%.2f) points=%d)",
                 i, m.pieces[i].segStart[0], m.pieces[i].segStart[1], m.pieces[i].segStart[2],
                 m.pieces[i].segEnd[0], m.pieces[i].segEnd[1], m.pieces[i].segEnd[2], m.pieces[i].segPointCount);
            continue;
        }
        bool removed = false;
        if (!ApplyDemolish(one, &removed)) ok = false;
        if (removed) *outRemoved = true;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Lockstep commands (see networks.h)
// ---------------------------------------------------------------------------

// The PC that drew the segment did not make its commit, so its tool still
// shows the finished drag; the game's own preview cleanup (542a30) clears it
// before the replay starts a drag of its own on the same manager. Skipped
// while the local player is dragging again.
static void ClearCommandPreview(int category)
{
    const NetCategoryInfo* info = Networks_GetCategoryInfo(category);
    if (!info) return;
    __try {
        __int64 thisPtr = GetWorldThisPtr();
        __int64 manager = *(__int64*)(thisPtr + info->managerOffset);
        if (manager && *(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) == 0)
            ((ManagerResetFn)((uintptr_t)g_exe + MANAGER_RESET_RVA))(manager, manager + MANAGER_RESET_STATE_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A drag's start as this PC's tool made it (FUN_1404fc040's arguments and the
// hover it read), kept by the hook below; the replays' own starts are not.
#define SELECTED_TOOL_OFFSET 0xd428
#define HOVER_BLOCK_RVA 0x9E4558 // the hover globals (kHoverPtrRvas, the flag, the port)
#define HOVER_BLOCK_BYTES 0xA8
struct DragStart {
    bool valid;
    __int64 manager;
    float start[3];
    unsigned __int64 p3;
    char fuzzy;
    int p5;
    unsigned __int64 node;
    bool hasDirection;
    float direction[3];
    unsigned char hover[HOVER_BLOCK_BYTES];
    __int64 tool;
};
static DragStart g_dragStart = {};  // the latest local start
static DragStart g_dragYielded = {}; // the drag a command ended, to begin again
static Fc040Fn g_origDragStart = nullptr;
static bool g_resumingDrag = false;

static void HookedDragStart(__int64 manager, float* startPoint, unsigned __int64 p3, char fuzzy, int p5,
                            unsigned __int64 node, float* direction)
{
    if (!g_resumingDrag && !Commands_Applying() && InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0) {
        __try {
            DragStart s = {};
            s.manager = manager;
            if (startPoint) memcpy(s.start, startPoint, sizeof(s.start));
            s.p3 = p3;
            s.fuzzy = fuzzy;
            s.p5 = p5;
            s.node = node;
            s.hasDirection = direction != nullptr;
            if (direction) memcpy(s.direction, direction, sizeof(s.direction));
            memcpy(s.hover, (const void*)((uintptr_t)g_exe + HOVER_BLOCK_RVA), HOVER_BLOCK_BYTES);
            __int64 world = GetWorldThisPtr();
            s.tool = *(__int64*)(world + SELECTED_TOOL_OFFSET);
            s.valid = startPoint != nullptr;
            g_dragStart = s;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_dragStart.valid = false;
        }
    }
    g_origDragStart(manager, startPoint, p3, fuzzy, p5, node, direction);
}

// The manager is one of the networks' (it may have been made again since).
static bool IsNetworkManager(__int64 world, __int64 manager)
{
    for (int c = 0; c < NET_CATEGORY_COUNT; c++)
        if (manager && *(__int64*)(world + g_categories[c].managerOffset) == manager) return true;
    return false;
}

// The node is still one of the network's committed nodes.
static bool NodeListed(__int64 manager, __int64 node)
{
    __int64* it = *(__int64**)(manager + NET_NODES_OFFSET);
    __int64* end = *(__int64**)(manager + NET_NODES_OFFSET + 8);
    if (!it || end < it || end - it > 4000000) return false;
    for (; it < end; ++it)
        if (*it == node) return true;
    return false;
}

void Networks_CancelLocalDrag()
{
    __try {
        __int64 thisPtr = GetWorldThisPtr();
        if (*(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) == 0) return;
        g_dragYielded = g_dragStart.valid && IsNetworkManager(thisPtr, g_dragStart.manager) ? g_dragStart : DragStart{};
        for (int c = 0; c < NET_CATEGORY_COUNT; c++) {
            __int64 manager = *(__int64*)(thisPtr + g_categories[c].managerOffset);
            if (manager) ((ManagerResetFn)((uintptr_t)g_exe + MANAGER_RESET_RVA))(manager, manager + MANAGER_RESET_STATE_OFFSET);
        }
        *(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) = 0;
        Logf("republicmp  NETWORKS: the local drag was paused for a lockstep command");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Networks_ResumeLocalDrag()
{
    if (!g_dragYielded.valid) return;
    DragStart s = g_dragYielded;
    g_dragYielded.valid = false;
    if (!g_origDragStart) return;
    const char* why = nullptr;
    unsigned char now[HOVER_BLOCK_BYTES];
    __try {
        __int64 world = GetWorldThisPtr();
        if (*(unsigned char*)((uintptr_t)g_exe + LMB_HELD_RVA) == 0) why = "the button was let go";
        else if (*(__int64*)(world + SELECTED_TOOL_OFFSET) != s.tool) why = "another tool is in hand";
        else if (*(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) != 0) why = "another drag began";
        else if (!IsNetworkManager(world, s.manager)) why = "the network is gone";
        if (!why) {
            // A start node the command removed: the start attaches by position instead.
            if (s.node && !NodeListed(s.manager, (__int64)s.node)) {
                s.node = 0;
                s.fuzzy = 1;
            }
            unsigned char* hover = (unsigned char*)((uintptr_t)g_exe + HOVER_BLOCK_RVA);
            memcpy(now, hover, HOVER_BLOCK_BYTES);
            memcpy(hover, s.hover, HOVER_BLOCK_BYTES);
            g_resumingDrag = true;
            __try {
                g_origDragStart(s.manager, s.start, s.p3, s.fuzzy, s.p5, s.node, s.hasDirection ? s.direction : nullptr);
            } __finally {
                g_resumingDrag = false;
                memcpy(hover, now, HOVER_BLOCK_BYTES);
            }
            *(char*)(world + TOOL_DRAG_ACTIVE_OFFSET) = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while beginning it";
        __try {
            *(char*)(GetWorldThisPtr() + TOOL_DRAG_ACTIVE_OFFSET) = 0;
            ((ManagerResetFn)((uintptr_t)g_exe + MANAGER_RESET_RVA))(s.manager, s.manager + MANAGER_RESET_STATE_OFFSET);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (why) Logf("republicmp  NETWORKS: the paused local drag ended - %s", why);
    else Logf("republicmp  NETWORKS: the paused local drag goes on from its start");
}

// mov [rsp+8],rbx / mov [rsp+10h],rsi / mov [rsp+18h],rdi - 15 bytes
static const unsigned char kDragStartExpect[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18
};

bool Networks_InstallDragStartHook()
{
    return InstallInlineHook(g_exeBase + FC040_RVA, (void*)HookedDragStart, (void**)&g_origDragStart, kDragStartExpect,
                             sizeof(kDragStartExpect), "republicmp drag start");
}

// The world's building list +0x11b08 (construction sites of networks are in it).
#define WORLD_BUILDINGS_OFFSET 0x11B08

static int BuildingCount()
{
    __try {
        __int64 world = GetWorldThisPtr();
        __int64 b = *(__int64*)(world + WORLD_BUILDINGS_OFFSET), e = *(__int64*)(world + WORLD_BUILDINGS_OFFSET + 8);
        return b && e >= b ? (int)((e - b) / 8) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

#define BUILDING_POS_OFFSET 0x350      // building: float x, y, z
#define BUILDING_TYPE_OFFSET 0x318     // building: its type record
#define TYPE_CATEGORY_OFFSET 0x360     // type record: int category
#define REJECTED_NEAR_M 20.0f

// A segment the engine refused here: the buildings near its points, so the
// two PCs' logs show what this PC has in the way that the other has not.
static void LogBuildingsNear(const NetSegment& seg)
{
    int logged = 0;
    __try {
        __int64 world = GetWorldThisPtr();
        __int64* b = *(__int64**)(world + WORLD_BUILDINGS_OFFSET);
        __int64* e = *(__int64**)(world + WORLD_BUILDINGS_OFFSET + 8);
        for (__int64* it = b; b && it < e && logged < 6; ++it) {
            __int64 bld = *it;
            if (!bld) continue;
            const float* p = (const float*)(bld + BUILDING_POS_OFFSET);
            float best = 1e30f;
            for (int i = -2; i < seg.pointCount && i < NET_MAX_POINTS; i++) {
                const float* q = i == -2 ? seg.startNode : i == -1 ? seg.endNode : seg.points[i].pos;
                float dx = p[0] - q[0], dz = p[2] - q[2], d = dx * dx + dz * dz;
                if (d < best) best = d;
            }
            if (best > REJECTED_NEAR_M * REJECTED_NEAR_M) continue;
            __int64 type = *(__int64*)(bld + BUILDING_TYPE_OFFSET);
            Logf("republicmp  NETWORKS:   near the refused piece: building #%d, category 0x%X at (%.2f,%.2f,%.2f), %.1f m",
                 (int)(it - b), type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1, p[0], p[1], p[2], sqrtf(best));
            ++logged;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!logged) Logf("republicmp  NETWORKS:   near the refused piece: no building within %.0f m", REJECTED_NEAR_M);
}

#define SEG_PLANNED_OFFSET 0x140 // segment: byte 1 = planned (white); a piece joined to it starts its construction (51c89b)

// The pieces at the new segment's ends and their planned bytes (+140, +141),
// before it is laid: compared between the PCs' logs when a segment makes a
// construction site on one PC only.
static void LogPiecesAtEnds(const NetSegment& seg)
{
    const NetCategoryInfo* info = Networks_GetCategoryInfo(seg.category);
    if (!info) return;
    char text[400] = "";
    size_t used = 0;
    int found = 0;
    __try {
        __int64 manager = *(__int64*)(GetWorldThisPtr() + info->managerOffset);
        __int64* b = manager ? *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET) : nullptr;
        __int64* e = manager ? *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET) : nullptr;
        for (__int64* it = b; b && it < e && found < 6; ++it) {
            __int64 s = *it;
            if (!s) continue;
            __int64 n0 = *(__int64*)(s + SEG_NODE0_OFFSET), n1 = *(__int64*)(s + SEG_NODE1_OFFSET);
            bool touches = (n0 && (NodeNear(n0, seg.startNode) || NodeNear(n0, seg.endNode))) ||
                           (n1 && (NodeNear(n1, seg.startNode) || NodeNear(n1, seg.endNode)));
            if (!touches) continue;
            int n = _snprintf_s(text + used, sizeof(text) - used, _TRUNCATE, " #%d planned %u/%u type %d;", (int)(it - b),
                                *(unsigned char*)(s + SEG_PLANNED_OFFSET), *(unsigned char*)(s + SEG_PLANNED_OFFSET + 1),
                                *(int*)(s + SEG_TYPE_OFFSET));
            if (n > 0) used += n;
            ++found;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (found) Logf("republicmp  NETWORKS:   pieces at its ends:%s", text);
}

bool Networks_ApplyCommandSegment(const NetSegmentMsg& msg)
{
    const NetSegment& seg = msg.seg;
    ClearCommandPreview(seg.category);
    if (SegmentBetweenNodesExists(seg)) {
        Logf("republicmp  NETWORKS: command %s already exists between these nodes - skipped", CategoryName(seg.category));
        return false;
    }
    BuildMode mine = BuildMode_Read();
    BuildMode_Write(msg.mode);
    bool engineAccepted = false;
    int before = BuildingCount();
    LogPiecesAtEnds(seg);
    bool ok = ApplySegment(seg, &engineAccepted);
    // Compared between the PCs' logs: a segment that makes construction
    // sites on one PC and none on the other (a planned road, type -1).
    Logf("republicmp  NETWORKS:   buildings %d -> %d (type %d, %s)", before, BuildingCount(), seg.typeId,
         engineAccepted ? "accepted" : "rejected");
    if (ok && !engineAccepted) LogBuildingsNear(seg);
    BuildMode_Write(mine);
    return ok && engineAccepted;
}

bool Networks_ApplyCommandUpgrade(const NetUpgradeMsg& msg)
{
    ClearCommandPreview(msg.category);
    BuildMode mine = BuildMode_Read();
    BuildMode_Write(msg.mode);
    int applied = ApplyUpgrade(msg);
    BuildMode_Write(mine);
    Logf("republicmp  NETWORKS: command upgrade %s applied %d/%d piece(s)", CategoryName(msg.category), applied, msg.pieceCount);
    return applied == msg.pieceCount;
}

bool Networks_ApplyCommandDemolish(const NetDemolishMsg& msg)
{
    bool removed = false;
    bool ok = msg.pieceCount > 0 ? ApplyDemolishPieces(msg, &removed) : ApplyDemolish(msg, &removed);
    return ok && removed;
}

// ---------------------------------------------------------------------------
// Factory connection direction (game thread only, see NetDirectionMsg)
// ---------------------------------------------------------------------------

#define SEG_DIRECTION_OFFSET 0x1bc       // int: 0 both ways, 1 / 2 one way (node 0 / node 1 relative)
#define NODE_BUILDINGS_BEGIN_OFFSET 0x70 // node: buildings attached to it
#define NODE_BUILDINGS_END_OFFSET 0x78
#define REFRESH_BUILDING_RVA 0x191950    // FUN_140191950(thisPtr, building)
#define DIR_SCAN_MAX 4096

typedef void (*RefreshBuildingFn)(__int64 thisPtr, __int64 building);

struct DirSnapshot {
    __int64 seg;
    int direction;
    NetPiece piece;
};
static DirSnapshot g_dirSnapshot[DIR_SCAN_MAX];

// Copies every factory connection's direction and identity; -1 on a fault.
static int ReadFactoryDirections()
{
    int n = 0;
    __try {
        __int64 manager = *(__int64*)(GetWorldThisPtr() + g_categories[NET_CATEGORY_FACTORYCONN].managerOffset);
        if (!manager) return 0;
        __int64* it = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
        for (; it && it < end && n < DIR_SCAN_MAX; ++it) {
            __int64 seg = *it;
            if (!seg) continue;
            __int64 n0 = *(__int64*)(seg + SEG_NODE0_OFFSET);
            __int64 n1 = *(__int64*)(seg + SEG_NODE1_OFFSET);
            if (!n0 || !n1) continue;
            DirSnapshot& s = g_dirSnapshot[n++];
            memset(&s, 0, sizeof(s));
            s.seg = seg;
            s.direction = *(int*)(seg + SEG_DIRECTION_OFFSET);
            memcpy(s.piece.segStart, (const float*)(n0 + NODE_POS_OFFSET), sizeof(s.piece.segStart));
            memcpy(s.piece.segEnd, (const float*)(n1 + NODE_POS_OFFSET), sizeof(s.piece.segEnd));
            s.piece.segPointCount =
                (int)((*(__int64*)(seg + SEG_POINTS_END_OFFSET) - *(__int64*)(seg + SEG_POINTS_BEGIN_OFFSET)) / 0x18);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

// Writes the direction into the local segment and refreshes its buildings.
// Returns the segment (0 = not found or faulted).
static __int64 ApplyDirection(const NetDirectionMsg& m, int* outLocalDirection)
{
    const NetCategoryInfo* info = Networks_GetCategoryInfo(m.category);
    if (!info || m.direction < 0 || m.direction > 2) return 0;
    __int64 thisPtr = GetWorldThisPtr();
    RefreshBuildingFn refresh = (RefreshBuildingFn)((uintptr_t)g_exe + REFRESH_BUILDING_RVA);
    __try {
        __int64 manager = *(__int64*)(thisPtr + info->managerOffset);
        bool reversed = false;
        __int64 seg = manager ? FindLocalSegment(manager, m.piece, &reversed) : 0;
        if (!seg) return 0;
        int dir = (reversed && m.direction) ? 3 - m.direction : m.direction;
        *(int*)(seg + SEG_DIRECTION_OFFSET) = dir;
        __int64 nodes[2] = { *(__int64*)(seg + SEG_NODE0_OFFSET), *(__int64*)(seg + SEG_NODE1_OFFSET) };
        for (__int64 node : nodes) {
            if (!node) continue;
            __int64* first = *(__int64**)(node + NODE_BUILDINGS_BEGIN_OFFSET);
            __int64* last = *(__int64**)(node + NODE_BUILDINGS_END_OFFSET);
            if (first && first < last && *first) refresh(thisPtr, *first);
        }
        *outLocalDirection = dir;
        return seg;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Starting planned construction (see NetPlanStartMsg)
// ---------------------------------------------------------------------------

#define PLAN_START_RVA 0x5258E0        // FUN_1405258e0(network, dollars, rubles, count, group)
#define PLAN_START_EXCEPT_RVA 0x525C30 // FUN_140525c30(network, dollars, rubles, -, group)
#define EDGE_PLANNED_OFFSET 0x140      // byte: 1 = a plan not started yet
#define EDGE_GROUP_OFFSET 0x124

// mov rax,rsp / mov [rax+10h],dl / push rbp,rsi,rdi,r12,r13,r14 - 15 bytes
static const unsigned char kPlanStartExpect[15] = {
    0x48, 0x8B, 0xC4, 0x88, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56
};
// mov rax,rsp / mov [rax+18h],r8b / mov [rax+10h],dl / push rbp,rsi,rdi,r12 - 15 bytes
static const unsigned char kPlanStartExceptExpect[15] = {
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x40, 0x18, 0x88, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54
};

// The interface's calls (return addresses = CALL + 5): the construction
// menu (FUN_140086f40), the construction window and the network tool panel
// (FUN_140395c90). The others - a building's placement, the area paste -
// start their own plans as part of an action that is synced itself.
static const unsigned int kPlanStartUiReturns[] = {
    0x7ECE0, 0x7EF10, 0x7F100, 0x881B1, 0x88A11, 0x88CC1, 0x39F0C0, 0x39F6F8, 0x39F8D8
};
static const unsigned int kPlanStartExceptUiReturns[] = { 0x7ECBA, 0x7EEEA, 0x7F0DA, 0x8818D, 0x889ED, 0x88C9D };

typedef __int64 (*PlanStartFn)(__int64 network, unsigned char dollars, unsigned char rubles, __int64 count, __int64 group);
static PlanStartFn g_origPlanStart = nullptr;
static PlanStartFn g_origPlanStartExcept = nullptr;

static bool IsUiCall(void* ret, const unsigned int* rvas, size_t n)
{
    unsigned long long rva = (unsigned long long)((BYTE*)ret - (BYTE*)g_exe);
    for (size_t i = 0; i < n; i++)
        if (rva == rvas[i]) return true;
    return false;
}

// Plans the call would start: planned edges among the last `count` (all if
// count <= 0), in `group` (or every group but it, or all when group < 0).
static int CountStartablePlans(__int64 network, bool exceptGroup, int count, int group)
{
    int n = 0;
    __try {
        __int64* begin = *(__int64**)(network + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64* end = *(__int64**)(network + MGR_SEGMENTS_END_OFFSET);
        if (!begin || end < begin) return 0;
        long long size = end - begin;
        long long from = (!exceptGroup && count > 0 && count < size) ? size - count : 0;
        for (long long i = from; i < size; i++) {
            __int64 e = begin[i];
            if (!e || *(unsigned char*)(e + EDGE_PLANNED_OFFSET) != 1) continue;
            int g = *(int*)(e + EDGE_GROUP_OFFSET);
            if (group >= 0 && (exceptGroup ? g == group : g != group)) continue;
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

static __int64 PlanStartCommon(bool exceptGroup, void* ret, __int64 network, unsigned char dollars,
                               unsigned char rubles, __int64 count, __int64 group)
{
    PlanStartFn orig = exceptGroup ? g_origPlanStartExcept : g_origPlanStart;
    // Lockstep: the button press becomes a command, made on every PC.
    if (Commands_TakeLocal() && InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0 &&
        (exceptGroup ? IsUiCall(ret, kPlanStartExceptUiReturns, sizeof(kPlanStartExceptUiReturns) / sizeof(kPlanStartExceptUiReturns[0]))
                     : IsUiCall(ret, kPlanStartUiReturns, sizeof(kPlanStartUiReturns) / sizeof(kPlanStartUiReturns[0])))) {
        int lsCategory = Networks_CategoryForManager(GetWorldThisPtr(), network);
        if (lsCategory >= 0) {
            NetPlanStartMsg m = {};
            m.category = lsCategory;
            m.exceptGroup = exceptGroup ? 1 : 0;
            m.dollars = dollars ? 1 : 0;
            m.rubles = rubles ? 1 : 0;
            m.count = (int)count;
            m.group = (int)group;
            m.mode = BuildMode_Read();
            Commands_Submit(LS_CMD_PLAN_START, &m, sizeof(m));
            return 0;
        }
    }
    return orig(network, dollars, rubles, count, group);
}

static __int64 HookedPlanStart(__int64 network, unsigned char dollars, unsigned char rubles, __int64 count, __int64 group)
{
    return PlanStartCommon(false, _ReturnAddress(), network, dollars, rubles, count, group);
}

static __int64 HookedPlanStartExcept(__int64 network, unsigned char dollars, unsigned char rubles, __int64 count, __int64 group)
{
    return PlanStartCommon(true, _ReturnAddress(), network, dollars, rubles, count, group);
}

bool Networks_InstallPlanStartHooks()
{
    bool a = InstallInlineHook(g_exeBase + PLAN_START_RVA, (void*)HookedPlanStart, (void**)&g_origPlanStart,
                               kPlanStartExpect, sizeof(kPlanStartExpect), "republicmp plan start");
    bool b = InstallInlineHook(g_exeBase + PLAN_START_EXCEPT_RVA, (void*)HookedPlanStartExcept,
                               (void**)&g_origPlanStartExcept, kPlanStartExceptExpect, sizeof(kPlanStartExceptExpect),
                               "republicmp plan start except");
    return a && b;
}

static bool CallPlanStart(PlanStartFn fn, __int64 network, const NetPlanStartMsg& m)
{
    __try {
        fn(network, m.dollars ? 1 : 0, m.rubles ? 1 : 0, m.count, m.group);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ApplyPlanStart(const NetPlanStartMsg& m)
{
    const NetCategoryInfo* info = Networks_GetCategoryInfo(m.category);
    PlanStartFn fn = m.exceptGroup ? g_origPlanStartExcept : g_origPlanStart;
    __int64 network = 0;
    __try {
        network = info ? *(__int64*)(GetWorldThisPtr() + info->managerOffset) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        network = 0;
    }
    if (!network || !fn) {
        Logf("republicmp  NETWORKS: plan start %s - no network here, skipped", CategoryName(m.category));
        return false;
    }
    bool except = m.exceptGroup != 0;
    int before = CountStartablePlans(network, except, m.count, m.group);
    // The new sites take the clicking PC's build mode, not this PC's: with
    // "rubles" here a realistic plan from the other PC finished at once.
    BuildMode mine = BuildMode_Read();
    BuildMode_Write(m.mode);
    InterlockedIncrement(&g_applyingSegment);
    bool ok = CallPlanStart(fn, network, m);
    InterlockedDecrement(&g_applyingSegment);
    BuildMode_Write(mine);
    int started = before - CountStartablePlans(network, except, m.count, m.group);
    char theirs[96];
    Logf("republicmp  NETWORKS: plan start %s dollars=%d rubles=%d pay=%s - %s: %d plan(s) started",
         CategoryName(m.category), m.dollars, m.rubles, BuildMode_Describe(m.mode, theirs, sizeof(theirs)),
         ok ? "applied" : "THREW", started);
    return ok && started > 0;
}

// ---------------------------------------------------------------------------
// Group construction tools (see NetGroupToolMsg)
// ---------------------------------------------------------------------------

#define GROUP_TOOL_RVA 0x21C110
#define GROUP_TOOL_YAW_RVA 0x9E9D20   // float: camera yaw that turns the box
#define GROUP_TOOL_DOWN_RVA 0xA54E90  // byte: mouse button held
#define GROUP_TOOL_PRESS_RVA 0xA54E91 // byte: mouse button pressed this frame
#define WORLD_GROUP_BOX_OFFSET 0x13f5c
#define WORLD_GROUP_DRAG_OFFSET 0x14033
#define WORLD_CURSOR_OFFSET 0xf6c
#define WORLD_NETWORK_COUNT_OFFSET 0x14034 // int: entries below
#define WORLD_NETWORK_ENTRIES_OFFSET 0x14038 // entries of 0x50, the network at +0
#define WORLD_NETWORK_ENTRY_SIZE 0x50

// mov rax,rsp / push rbp,rsi,rdi,r12,r13,r14,r15 / lea rbp,[rsp-70h] - 19 bytes
static const unsigned char kGroupToolExpect[19] = {
    0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0x90
};

typedef __int64 (*GroupToolFn)(__int64 world, int mode);
static GroupToolFn g_origGroupTool = nullptr;

// What the commit reads besides the menus (menus_closed.h closes those).
struct GroupToolState {
    float box[6];
    float cursor[3];
    float yaw;
    unsigned char drag, down, press;
};

static void ReadGroupToolState(__int64 world, GroupToolState& s)
{
    BYTE* exe = g_exeBase;
    memcpy(s.box, (void*)(world + WORLD_GROUP_BOX_OFFSET), sizeof(s.box));
    memcpy(s.cursor, (void*)(world + WORLD_CURSOR_OFFSET), sizeof(s.cursor));
    s.yaw = *(float*)(exe + GROUP_TOOL_YAW_RVA);
    s.drag = *(unsigned char*)(world + WORLD_GROUP_DRAG_OFFSET);
    s.down = exe[GROUP_TOOL_DOWN_RVA];
    s.press = exe[GROUP_TOOL_PRESS_RVA];
}

static void WriteGroupToolState(__int64 world, const GroupToolState& s)
{
    BYTE* exe = g_exeBase;
    memcpy((void*)(world + WORLD_GROUP_BOX_OFFSET), s.box, sizeof(s.box));
    memcpy((void*)(world + WORLD_CURSOR_OFFSET), s.cursor, sizeof(s.cursor));
    *(float*)(exe + GROUP_TOOL_YAW_RVA) = s.yaw;
    *(unsigned char*)(world + WORLD_GROUP_DRAG_OFFSET) = s.drag;
    exe[GROUP_TOOL_DOWN_RVA] = s.down;
    exe[GROUP_TOOL_PRESS_RVA] = s.press;
}

// Planned pieces (edge +0x140 = 1) over every network the tool walks.
static int CountPlannedPieces(__int64 world)
{
    int n = 0;
    __try {
        int networks = *(int*)(world + WORLD_NETWORK_COUNT_OFFSET);
        for (int k = 0; k < networks && k < 256; k++) {
            __int64 network = *(__int64*)(world + WORLD_NETWORK_ENTRIES_OFFSET + (__int64)k * WORLD_NETWORK_ENTRY_SIZE);
            if (!network) continue;
            __int64* begin = *(__int64**)(network + MGR_SEGMENTS_BEGIN_OFFSET);
            __int64* end = *(__int64**)(network + MGR_SEGMENTS_END_OFFSET);
            if (!begin || end < begin) continue;
            for (__int64* p = begin; p < end; p++)
                if (*p && *(unsigned char*)(*p + EDGE_PLANNED_OFFSET) == 1) n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

static __int64 HookedGroupTool(__int64 world, int mode)
{
    // Lockstep: the commit frame becomes a command; this PC drops its drag
    // mark instead of committing, so the next frame does not commit again.
    if (Commands_TakeLocal() && InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0) {
        GroupToolState s;
        bool commit = false;
        __try {
            BYTE* w = (BYTE*)world;
            ReadGroupToolState(world, s);
            commit = !w[0xfbc] && !w[0xfbe] && !w[0xfc3] && !s.down && s.drag;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            commit = false;
        }
        if (!commit) return g_origGroupTool(world, mode);
        NetGroupToolMsg m = {};
        m.category = NET_GROUP_TOOL_CATEGORY;
        m.mode = mode;
        memcpy(m.box, s.box, sizeof(m.box));
        memcpy(m.cursor, s.cursor, sizeof(m.cursor));
        m.yaw = s.yaw;
        m.buildMode = BuildMode_Read();
        Commands_Submit(LS_CMD_GROUP_TOOL, &m, sizeof(m));
        *(unsigned char*)(world + WORLD_GROUP_DRAG_OFFSET) = 0;
        return 0;
    }
    return g_origGroupTool(world, mode);
}

bool Networks_InstallGroupToolHook()
{
    return InstallInlineHook(g_exeBase + GROUP_TOOL_RVA, (void*)HookedGroupTool, (void**)&g_origGroupTool,
                             kGroupToolExpect, sizeof(kGroupToolExpect), "republicmp group tool");
}

static bool CallGroupTool(__int64 world, int mode)
{
    __try {
        g_origGroupTool(world, mode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The menus are already closed by the caller's MenusClosedScope.
static bool ApplyGroupTool(const NetGroupToolMsg& m)
{
    __int64 world = GetWorldThisPtr();
    if (!g_origGroupTool || m.mode < 0 || m.mode > 3) {
        Logf("republicmp  NETWORKS: group tool mode %d - not applied", m.mode);
        return false;
    }
    // All of it lives in the exe image (the world object is a global).
    GroupToolState own, replay;
    int before = CountPlannedPieces(world);
    BuildMode mine = BuildMode_Read();
    ReadGroupToolState(world, own);
    memcpy(replay.box, m.box, sizeof(replay.box));
    memcpy(replay.cursor, m.cursor, sizeof(replay.cursor));
    replay.yaw = m.yaw;
    replay.drag = 1;
    replay.down = 0;
    replay.press = 0;
    WriteGroupToolState(world, replay);
    BuildMode_Write(m.buildMode);
    InterlockedIncrement(&g_applyingSegment);
    bool ok = CallGroupTool(world, m.mode);
    InterlockedDecrement(&g_applyingSegment);
    WriteGroupToolState(world, own); // this PC's own drag, cursor, camera and input
    BuildMode_Write(mine);
    int started = before - CountPlannedPieces(world);
    char theirs[96];
    Logf("republicmp  NETWORKS: group tool mode %d pay=%s - %s: %d planned piece(s) started",
         m.mode, BuildMode_Describe(m.buildMode, theirs, sizeof(theirs)), ok ? "applied" : "THREW", started);
    return ok;
}

bool Networks_ApplyCommandPlanStart(const NetPlanStartMsg& msg)
{
    return ApplyPlanStart(msg);
}

bool Networks_ApplyCommandGroupTool(const NetGroupToolMsg& msg)
{
    MenusClosedScope menus;
    return ApplyGroupTool(msg);
}

static std::unordered_map<__int64, int> g_lsDirections;

void Networks_LockstepDirectionsBaseline()
{
    g_lsDirections.clear();
    int n = ReadFactoryDirections();
    for (int i = 0; i < n; i++) {
        g_lsDirections[g_dirSnapshot[i].seg] = g_dirSnapshot[i].direction;
    }
}

void Networks_LockstepDirectionsDetect()
{
    int n = ReadFactoryDirections();
    for (int i = 0; i < n; i++) {
        const DirSnapshot& s = g_dirSnapshot[i];
        auto old = g_lsDirections.find(s.seg);
        if (old == g_lsDirections.end() || old->second == s.direction) continue;
        NetDirectionMsg m = {};
        m.category = NET_CATEGORY_FACTORYCONN;
        m.piece = s.piece;
        m.direction = s.direction;
        Commands_Submit(LS_CMD_DIRECTION, &m, sizeof(m));
        NetDirectionMsg back = m; // this PC's own segment, as it was
        back.direction = old->second;
        int local = 0;
        ApplyDirection(back, &local);
    }
}

bool Networks_ApplyCommandDirection(const NetDirectionMsg& msg)
{
    int local = 0;
    bool ok = ApplyDirection(msg, &local) != 0;
    Logf("republicmp  NETWORKS: command direction %d %s", msg.direction, ok ? "applied" : "NOT applied - connection not found");
    return ok;
}
