// SPDX-License-Identifier: GPL-3.0-only
// See local_builds.h.
#include "local_builds.h"
#include "../game/present_hook.h"
#include "buildings.h"
#include "networks.h"
#include "view_list_scope.h"
#include "demolish_hook.h" // DemolishHook_InDemolish
#include "../ui/sync_block.h"
#include "../lockstep/commands.h"
#include "../determinism/determinism.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "User32.lib")

void LocalBuilds_Bind(const TsmHost* host)
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
#define HOVER_NODE_RVA 0x9E4560          // network node under the cursor (power lines join on it)
#define HOVER_EDGE_RVA 0x9E4558          // network edge under the cursor

// thisPtr fields.
#define CURSOR_X_OFFSET 0xf6c            // placement cursor
#define CURSOR_Y_OFFSET 0xf70
#define CURSOR_Z_OFFSET 0xf74
#define CURSOR_YAW_OFFSET 0xf94          // placement yaw (radians)
#define PLACEMENT_RESULT_OFFSET 0x11a10  // validation code after a placement call
#define MIRROR_EFFECTIVE_OFFSET 0x11db1  // byte: mirror state the last placement call used
#define TOOL_DRAG_ACTIVE_OFFSET 0xd2ac   // byte: a path-tool drag is in progress
#define ACTIVE_TOOL_OFFSET 0xD428        // char*: name of the active tool

// Building object fields.
#define OBJ_ROT_SIN_OFFSET 0x340
#define OBJ_ROT_COS_OFFSET 0x348
#define OBJ_POS_OFFSET 0x350             // float x,y,z

// Tool manager fields (see world/networks.cpp for the full list).
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

#define PLACE_LMB_HELD_RVA 0xA54E90         // byte: left mouse button held
#define PLACE_LMB_JUST_PRESSED_RVA 0xA54E91 // byte: pressed this frame
#define PLACE_SCOPE_MARGIN 150.0f           // view-list scope around a placement
#define MGR_SEGMENTS_BEGIN_OFFSET 0x2b0  // the network's committed edges (+0xaa0 is only the camera's)
#define MGR_SEGMENTS_END_OFFSET 0x2b8

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
// Infrastructure commit capture
// ---------------------------------------------------------------------------

// >0 while a building placement call is running.
static volatile LONG g_buildingPlacementDepth = 0;
void LocalBuilds_BeginBuildingPlacement() { InterlockedIncrement(&g_buildingPlacementDepth); }
void LocalBuilds_EndBuildingPlacement()   { InterlockedDecrement(&g_buildingPlacementDepth); }
bool LocalBuilds_InBuildingPlacement()    { return InterlockedCompareExchange(&g_buildingPlacementDepth, 0, 0) != 0; }

// Reason the last tool commit was not captured, logged from the game thread.
static volatile LONG g_commitSkipPending = 0;
static struct { int category; const char* reason; int count; int valid; int type; int error; unsigned subcode; int attached[2]; } g_commitSkip;

static void NoteCommitSkip(int category, const char* reason, int count, int valid, int type)
{
    if (InterlockedCompareExchange(&g_commitSkipPending, 0, 0) != 0) return;
    g_commitSkip.category = category;
    g_commitSkip.reason = reason;
    g_commitSkip.count = count;
    g_commitSkip.valid = valid;
    g_commitSkip.type = type;
    g_commitSkip.error = -1;
    g_commitSkip.subcode = 0;
    g_commitSkip.attached[0] = g_commitSkip.attached[1] = -1;
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
static void ReadBuildingPort(__int64 building, int portIndex, NetBuildingPort* out)
{
    out->valid = building ? 1 : 0;
    if (!building) return;
    out->portIndex = portIndex;
    for (int k = 0; k < 3; k++) out->bldPos[k] = *(float*)(building + OBJ_POS_OFFSET + k * 4);
}

// Identifies an existing segment by its nodes. Caller wraps in __try.
static bool FillUpgradePiece(__int64 seg, NetPiece* p)
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
static bool FillUpgrade(__int64 manager, int category, NetUpgradeMsg& m)
{
    m.category = category;
    m.newType = *(int*)(manager + MGR_TYPE_OFFSET);
    m.newVariant = *(int*)(manager + MGR_VARIANT_OFFSET);
    m.flag5c4 = *(int*)(manager + MGR_FLAG_5C4_OFFSET);
    if (m.newType >= TOOL_TYPE_DEMOLISH_MIN) return false;
    int dirFlag = *(int*)(manager + MGR_UPG_DIR_OFFSET);

    if (*(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) == 0) {
        NetPiece& p = m.pieces[0];
        if (!FillUpgradePiece(*(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET), &p)) return false;
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
        if (idx < 0 || !listBegin || idx >= (int)((listEnd - listBegin) / 0x18)) return false;
        __int64* segIt = *(__int64**)(listBegin + idx * 0x18);
        __int64* segEnd = *(__int64**)(listBegin + idx * 0x18 + 8);
        __int64 segs[NET_MAX_UPGRADE_PIECES];
        int n = 0;
        for (; segIt && segIt < segEnd && n < NET_MAX_UPGRADE_PIECES; ++segIt) {
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
    return m.pieceCount >= 1;
}

// Reads the committed route out of the manager into seg: interior route
// records, both node positions and building ports. Runs inside the game's
// commit: no allocation, logging or I/O. Caller wraps in __try.
static bool FillSegment(__int64 manager, int category, NetSegment& seg)
{
    char* begin = *(char**)(manager + MGR_ROUTE_BEGIN_OFFSET);
    char* end = *(char**)(manager + MGR_ROUTE_END_OFFSET);
    // A never-allocated route is (null, null) = 0 records - valid for a short
    // power-line span.
    if (!begin != !end || end < begin) {
        NoteCommitSkip(category, "route buffer pointers inconsistent", -1, -1, -1);
        return false;
    }
    size_t bytes = (size_t)(end - begin);
    int count = (int)(bytes / ROUTE_RECORD_SIZE);
    int type = *(int*)(manager + MGR_TYPE_OFFSET);
    unsigned char valid = *(unsigned char*)(manager + MGR_VALID_OFFSET);

    if (bytes % ROUTE_RECORD_SIZE != 0 || count > NET_MAX_POINTS) {
        NoteCommitSkip(category, "route buffer size invalid or too long", count, valid, type);
        return false;
    }
    if (valid != 1) {
        NoteCommitSkip(category, "engine marked the route invalid", count, valid, type);
        // The game's own reason: its error category and subcode, and which ends are attached.
        g_commitSkip.error = *(int*)(manager + 0x318);
        g_commitSkip.subcode = *(unsigned*)(manager + 0x31c);
        g_commitSkip.attached[0] = *(unsigned char*)(manager + 0x748);
        g_commitSkip.attached[1] = *(unsigned char*)(manager + 0x749);
        return false;
    }
    if (type >= TOOL_TYPE_DEMOLISH_MIN) {
        NoteCommitSkip(category, "demolish tool (not synced)", count, valid, type);
        return false;
    }
    if (!ReadSegmentEndNodes(manager, seg.startNode, seg.endNode)) {
        NoteCommitSkip(category, "end nodes unreadable", count, valid, type);
        return false;
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
    Networks_ReadFootpathLinks(manager, category, seg); // where a footpath joins roads (crossings)

    return true;
}

// terrain_demolish works through the path managers' upgrade branch; it is
// recognised by the active tool and becomes the cursor plus the cut piece
// (see NetDemolishMsg).
static bool IsDemolishToolActive()
{
    char tool[64];
    if (!SafeReadStr(*(void**)(GetPlacementManagerThisPtr() + ACTIVE_TOOL_OFFSET), tool, sizeof(tool)))
        return false;
    return strcmp(tool, "terrain_demolish") == 0;
}

// Multi-segment demolish: the whole segments of path +0x3d8 in the list at
// +0x3f0 (the same list the upgrade capture reads).
static int ReadDemolishPieces(__int64 manager, NetPiece* out, int max)
{
    if (*(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) == 0) return 0;
    int idx = *(int*)(manager + MGR_UPG_PATH_INDEX_OFFSET);
    char* listBegin = *(char**)(manager + MGR_UPG_PATHS_BEGIN_OFFSET);
    char* listEnd = *(char**)(manager + MGR_UPG_PATHS_END_OFFSET);
    if (idx < 0 || !listBegin || idx >= (int)((listEnd - listBegin) / 0x18)) return 0;
    __int64* segIt = *(__int64**)(listBegin + idx * 0x18);
    __int64* segEnd = *(__int64**)(listBegin + idx * 0x18 + 8);
    int n = 0;
    for (; segIt && segIt < segEnd && n < max; ++segIt) {
        __int64 seg = *segIt;
        if (!seg || *(char*)(seg + SEG_SKIP_FLAG_OFFSET) != 0 || (unsigned)(*(int*)(seg + SEG_KIND_OFFSET) - 2) <= 1)
            continue;
        if (!FillUpgradePiece(seg, &out[n])) continue;
        out[n].rangeStart = -1;
        out[n].rangeEnd = out[n].segPointCount;
        n++;
    }
    return n;
}
// The piece the commit is about to cut: ends of range (+0x388, +0x38c) on
// segment +0x380 - an interior point, or the segment's node for -1 / count.
static bool ReadDemolishRange(__int64 manager, float a[3], float b[3])
{
    if (*(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) != 0) return false;
    __int64 seg = *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET);
    if (!seg) return false;
    const float* pts = *(const float**)(seg + SEG_POINTS_BEGIN_OFFSET);
    const float* end = *(const float**)(seg + SEG_POINTS_END_OFFSET);
    int count = (pts && end > pts) ? (int)((end - pts) / 6) : 0; // 24-byte records
    int rs = *(int*)(manager + MGR_UPG_RANGE_START_OFFSET);
    int re = *(int*)(manager + MGR_UPG_RANGE_END_OFFSET);
    if (rs < -1 || re > count || rs >= re) return false;
    __int64 n0 = *(__int64*)(seg + SEG_NODE0_OFFSET);
    __int64 n1 = *(__int64*)(seg + SEG_NODE1_OFFSET);
    const float* pa = (rs < 0) ? (n0 ? (const float*)(n0 + NODE_POS_OFFSET) : nullptr) : pts + rs * 6;
    const float* pb = (re >= count) ? (n1 ? (const float*)(n1 + NODE_POS_OFFSET) : nullptr) : pts + re * 6;
    if (!pa || !pb) return false;
    memcpy(a, pa, 12);
    memcpy(b, pb, 12);
    return true;
}

// Lockstep (lockstep/commands): the local tool's commit becomes a command and
// is not made here - every PC makes it at the next tick boundary, this one
// included. Three kinds: a demolition with terrain_demolish (a cut the hook
// cannot read - the cursor on a hovered node - goes with the hovered node or
// edge), an upgrade, or a segment.
// A commit the engine marked invalid builds nothing anywhere. True when the
// commit was taken (the original is then not called).
static NetSegmentMsg g_commandSegment; // ~48 KB: main thread only

// Lockstep: the demolition tool commits every frame while the button is held
// over a piece, and the piece stays until its command is made at a tick
// boundary - a piece already sent and not made yet is not sent again. Main
// thread (the commit hook and the commands' boundary).
#define DEMOLISH_PENDING_MAX 16
#define DEMOLISH_MAX_PIECES 64 // whole segments of one removal drag
#define DEMOLISH_PENDING_MS 5000
struct PendingDemolish {
    ULONGLONG at;
    NetDemolishMsg msg;
    __int64 seg; // a single piece: its segment and point range at the capture
    int rs, re;
    bool hovered; // a hover demolition: the hovered node / edge are its key
    NetDemolishHover hover;
};
static PendingDemolish g_pendingDemolish[DEMOLISH_PENDING_MAX];

static bool SameDemolish(const NetDemolishMsg& a, const NetDemolishMsg& b)
{
    if (!a.hasRange && !a.pieceCount) return false; // a hover demolition: compared by its hover
    if (a.category != b.category || a.hasRange != b.hasRange || a.pieceCount != b.pieceCount) return false;
    if (a.hasRange) return memcmp(a.rangeA, b.rangeA, sizeof(a.rangeA)) == 0 && memcmp(a.rangeB, b.rangeB, sizeof(a.rangeB)) == 0;
    return memcmp(a.pieces, b.pieces, sizeof(a.pieces[0]) * (a.pieceCount > 0 ? a.pieceCount : 0)) == 0;
}

// True when this piece is still waiting for its command - or, on the same
// segment, overlaps one that is (the brush moved while the cut waited for the
// tick boundary; after that cut the overlapping window would miss its ends) -
// else it is remembered. seg 0: whole segments, compared as they are.
static bool DemolishAlreadySent(const NetDemolishMsg& m, __int64 seg = 0, int rs = 0, int re = 0,
                                const NetDemolishHover* hover = nullptr)
{
    ULONGLONG now = GetTickCount64();
    int free = -1;
    for (int i = 0; i < DEMOLISH_PENDING_MAX; i++) {
        PendingDemolish& p = g_pendingDemolish[i];
        if (p.at && now - p.at > DEMOLISH_PENDING_MS) p.at = 0;
        if (!p.at) {
            if (free < 0) free = i;
            continue;
        }
        if (hover) {
            if (p.hovered && p.msg.category == m.category && memcmp(&p.hover, hover, sizeof(*hover)) == 0) return true;
            continue;
        }
        if (SameDemolish(p.msg, m)) return true;
        if (seg && p.seg == seg && rs < p.re && p.rs < re) return true;
    }
    if (free < 0) free = 0;
    PendingDemolish& n = g_pendingDemolish[free];
    n.at = now;
    n.msg = m;
    n.seg = seg;
    n.rs = rs;
    n.re = re;
    n.hovered = hover != nullptr;
    if (hover) n.hover = *hover;
    else memset(&n.hover, 0, sizeof(n.hover));
    return false;
}

void LocalBuilds_DemolishMade(const NetDemolishMsg& m, const NetDemolishHover* hover)
{
    for (int i = 0; i < DEMOLISH_PENDING_MAX; i++) {
        PendingDemolish& p = g_pendingDemolish[i];
        if (!p.at) continue;
        if (hover ? (p.hovered && p.msg.category == m.category && memcmp(&p.hover, hover, sizeof(*hover)) == 0)
                  : (!p.hovered && SameDemolish(p.msg, m)))
            p.at = 0;
    }
}

// Why a bulldozer commit's piece could not be read (lockstep diagnostics, capped).
static int g_hoverDemolishLogged = 0;

// The piece +0x380 is one of the network's segments (after a removal the tool
// keeps its selection on the segment that is gone).
static bool SegmentListed(__int64 manager, __int64 seg)
{
    __int64* it = *(__int64**)(manager + 0x2B0);
    __int64* end = *(__int64**)(manager + 0x2B8);
    for (; seg && it && it < end; ++it)
        if (*it == seg) return true;
    return false;
}

static bool TakeCommitAsCommand(__int64 manager, int category)
{
    __try {
        if (IsDemolishToolActive()) {
            bool multi = *(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) != 0;
            __int64 seg = *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET);
            // No piece under the brush: the commit removes only with +0x390 set (518a80).
            // Said in the log: a click on a piece the bulldozer does not pick
            // left no trace before (a player whose demolitions did nothing).
            if (*(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) == 0) {
                static int s_logged = 0;
                if (s_logged < 20) {
                    ++s_logged;
                    __int64 seg = *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET);
                    Logf("republicmp  LOCKSTEP: bulldozer on %s - no piece picked under the cursor (segment %s), nothing removed",
                         Networks_GetCategoryInfo(category)->name, !seg ? "none" : SegmentListed(manager, seg) ? "listed" : "not in the network");
                }
                return true;
            }
            bool listed = multi || SegmentListed(manager, seg);
            NetDemolishMsg m = {};
            __int64 thisPtr = GetPlacementManagerThisPtr();
            m.category = category;
            m.cursor[0] = *(float*)(thisPtr + CURSOR_X_OFFSET);
            m.cursor[1] = *(float*)(thisPtr + CURSOR_Y_OFFSET);
            m.cursor[2] = *(float*)(thisPtr + CURSOR_Z_OFFSET);
            m.hasRange = listed && ReadDemolishRange(manager, m.rangeA, m.rangeB) ? 1 : 0;
            m.edgeIndex = -1;
            if (m.hasRange && !multi) Networks_EdgeIdentity(manager, seg, &m.edgeIndex, &m.edgeCheck);
            // A removal drag over many segments: the message holds 8, so the
            // rest go in further commands (whole segments, independent).
            static NetPiece s_pieces[DEMOLISH_MAX_PIECES];
            int all = m.hasRange || !listed ? 0 : ReadDemolishPieces(manager, s_pieces, DEMOLISH_MAX_PIECES);
            if (!m.hasRange && all == 0) {
                // The piece is not readable (in normal play these are the commits
                // the old capture had to find by comparing segments): the cursor
                // on a cut's end, removed through the hovered node or edge.
                int rs = *(int*)(manager + MGR_UPG_RANGE_START_OFFSET), re = *(int*)(manager + MGR_UPG_RANGE_END_OFFSET);
                int count = !multi && seg && listed ? (int)((*(__int64*)(seg + SEG_POINTS_END_OFFSET) - *(__int64*)(seg + SEG_POINTS_BEGIN_OFFSET)) / 0x18) : -1;
                static NetDemolishHoverMsg s_hover;
                memset(&s_hover, 0, sizeof(s_hover));
                s_hover.m = m;
                bool haveHover = Networks_ReadDemolishHover(manager, &s_hover.hover);
                if (g_hoverDemolishLogged < 60) {
                    ++g_hoverDemolishLogged;
                    Logf("republicmp  LOCKSTEP: bulldozer piece not readable (multi %d, segment %s, points %d, range %d..%d) - hovered node %d, edge %d%s",
                         multi ? 1 : 0, !seg ? "none" : listed ? "listed" : "not in the network", count, rs, re,
                         s_hover.hover.nodeKind, s_hover.hover.edgeKind, haveHover ? " - sent with the hover" : " - nothing to send");
                }
                if (haveHover && !DemolishAlreadySent(m, 0, 0, 0, &s_hover.hover))
                    Commands_Submit(LS_CMD_NET_DEMOLISH, &s_hover, sizeof(s_hover));
                return true;
            }
            if (m.hasRange) {
                if (!DemolishAlreadySent(m, seg, *(int*)(manager + MGR_UPG_RANGE_START_OFFSET),
                                         *(int*)(manager + MGR_UPG_RANGE_END_OFFSET))) {
                    Logf("republicmp  LOCKSTEP: demolition piece (%.1f,%.1f)-(%.1f,%.1f) range (%d,%d)", m.rangeA[0], m.rangeA[2],
                         m.rangeB[0], m.rangeB[2], *(int*)(manager + MGR_UPG_RANGE_START_OFFSET),
                         *(int*)(manager + MGR_UPG_RANGE_END_OFFSET));
                    Commands_Submit(LS_CMD_NET_DEMOLISH, &m, sizeof(m));
                }
            } else if (all > 0) {
                const int chunk = (int)(sizeof(m.pieces) / sizeof(m.pieces[0]));
                for (int at = 0; at < all; at += chunk) {
                    m.pieceCount = all - at < chunk ? all - at : chunk;
                    memset(m.pieces, 0, sizeof(m.pieces));
                    memcpy(m.pieces, s_pieces + at, sizeof(m.pieces[0]) * m.pieceCount);
                    if (DemolishAlreadySent(m)) continue;
                    Logf("republicmp  LOCKSTEP: demolition of %d whole segment(s) (%d-%d of %d in the drag)", m.pieceCount, at + 1,
                         at + m.pieceCount, all);
                    Commands_Submit(LS_CMD_NET_DEMOLISH, &m, sizeof(m));
                }
            } else {
                int count = 0;
                if (!multi && seg) count = (int)((*(__int64*)(seg + SEG_POINTS_END_OFFSET) - *(__int64*)(seg + SEG_POINTS_BEGIN_OFFSET)) / 0x18);
                NoteCommitSkip(category, "lockstep: the demolition piece cannot be read (count, range start, range end)", count,
                               *(int*)(manager + MGR_UPG_RANGE_START_OFFSET), *(int*)(manager + MGR_UPG_RANGE_END_OFFSET));
            }
            return true;
        }
        if (*(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) != 0) {
            NetUpgradeMsg m = {};
            if (FillUpgrade(manager, category, m)) {
                m.mode = BuildMode_Read();
                Commands_Submit(LS_CMD_UPGRADE, &m, sizeof(m));
            }
            return true;
        }
        memset(&g_commandSegment, 0, sizeof(g_commandSegment));
        if (FillSegment(manager, category, g_commandSegment.seg)) {
            g_commandSegment.mode = BuildMode_Read();
            Commands_Submit(LS_CMD_SEGMENT, &g_commandSegment, sizeof(g_commandSegment));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        NoteCommitSkip(category, "lockstep: exception reading the commit - not made", -1, -1, -1);
        return true;
    }
}

// Outside the lockstep: a tool's commit the area rule refuses
// (Commands_RefusedHere) - the bulldozer by its cursor and its cut's ends, a
// new or re-typed piece by its points, read as the command would carry them.
// shortcut: a bulldozer drag over many segments and the other removal tools
// (FillSegment refuses them) are not checked here; the lockstep checks them.
static bool CommitRefusedHere(__int64 manager, int category)
{
    if (!Commands_ChecksHere()) return false;
    __try {
        if (IsDemolishToolActive()) {
            __int64 thisPtr = GetPlacementManagerThisPtr();
            float cursor[3] = { *(float*)(thisPtr + CURSOR_X_OFFSET), *(float*)(thisPtr + CURSOR_Y_OFFSET),
                                *(float*)(thisPtr + CURSOR_Z_OFFSET) };
            if (Commands_PointRefusedHere(cursor)) return true;
            float a[3], b[3];
            return *(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) != 0 && ReadDemolishRange(manager, a, b) &&
                   (Commands_PointRefusedHere(a) || Commands_PointRefusedHere(b));
        }
        if (*(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) != 0) {
            NetUpgradeMsg m = {};
            return FillUpgrade(manager, category, m) && Commands_RefusedHere(LS_CMD_UPGRADE, &m, sizeof(m));
        }
        memset(&g_commandSegment, 0, sizeof(g_commandSegment));
        return FillSegment(manager, category, g_commandSegment.seg) &&
               Commands_RefusedHere(LS_CMD_SEGMENT, &g_commandSegment, sizeof(g_commandSegment));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Entry of FUN_140518a80. In a session a tool's commit becomes a command
// (TakeCommitAsCommand); otherwise the original runs unchanged, unless the
// area rule refuses it.
void HookedRoadCommit(__int64 manager,
                      unsigned __int64 p2, unsigned __int64 p3,
                      unsigned __int64 p4, unsigned int p5,
                      unsigned __int64 p6, unsigned __int64 p7)
{
    int category = Networks_CategoryForManager(GetPlacementManagerThisPtr(), manager);
    bool own = category >= 0 && p2 == 0 && !Networks_IsApplyingCommand() && !DemolishHook_InDemolish() &&
               InterlockedCompareExchange(&g_buildingPlacementDepth, 0, 0) == 0;
    if (own && Commands_TakeLocal() && TakeCommitAsCommand(manager, category)) return;
    if (own && CommitRefusedHere(manager, category)) return;
    // A bulldozer commit on a network this mod does not carry is made on this
    // PC only: said in the log, as the other PCs keep the piece.
    if (category < 0 && Commands_TakeLocal() && IsDemolishToolActive()) {
        static int s_logged = 0;
        if (s_logged < 20) {
            ++s_logged;
            Logf("republicmp  LOCKSTEP: bulldozer on a network this mod does not carry (manager %p) - made on this PC only",
                 (void*)manager);
        }
    }
    g_origRoadCommit(manager, p2, p3, p4, p5, p6, p7);
}

bool LocalBuilds_IsDemolishToolActive()
{
    return IsDemolishToolActive();
}

// Logs why a tool commit was not taken (from the game thread).
static void LogCommitSkip()
{
    if (InterlockedCompareExchange(&g_commitSkipPending, 0, 0) == 0) return;
    Logf("republicmp  ROAD_COMMIT: %s commit not taken - %s (points=%d valid=%d type=%d error=%d subcode=0x%x attached=%d/%d)",
         Networks_GetCategoryInfo(g_commitSkip.category)->name, g_commitSkip.reason,
         g_commitSkip.count, g_commitSkip.valid, g_commitSkip.type, g_commitSkip.error, g_commitSkip.subcode,
         g_commitSkip.attached[0], g_commitSkip.attached[1]);
    InterlockedExchange(&g_commitSkipPending, 0);
}

// ---------------------------------------------------------------------------
// Per-frame tick
// ---------------------------------------------------------------------------

static bool g_toolDragActive = false;

bool LocalBuilds_IsToolDragActive()
{
    return g_toolDragActive;
}

void LocalBuilds_Tick()
{
    if (!IsWorldLoaded()) return;

    __int64 thisPtr = GetPlacementManagerThisPtr();
    __try {
        g_toolDragActive = *(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_toolDragActive = false;
    }

    LogCommitSkip();
}

// ---------------------------------------------------------------------------
// Building placement hook
// ---------------------------------------------------------------------------

// Lockstep (lockstep/commands): the building tool runs with the real mouse,
// so the game itself decides what a press is: levelling the ground (2cb6aa,
// held: the timer +d2bc), an error, the interface's (2bade0 returns at once
// while a menu is open). Only the press that would place - the left-press
// read at 2cc37c with the placement valid (world+11a10 == -1) - is taken here
// (LocalBuilds_PlacementPress, through sim_patches): it reads "not pressed"
// for the game and becomes a placement command that every PC makes at the
// next tick boundary (Buildings_ApplyPlacementCommand) from the placement
// call's inputs.
static __int64 g_placeWorld = 0;
static char* g_placeType = nullptr;
static unsigned __int64 g_placeParam5 = 0;
static bool g_placeOpen = false;

static void PlacementAsCommand(__int64 param_1, char* param_2, char param_3, char param_4,
                               unsigned __int64 param_5, unsigned int param_6, char param_7, char param_8)
{
    void** vecBeginPtr = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
    void** vecEndPtr = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
    long long before = BuildingCount(*vecBeginPtr, *vecEndPtr);
    g_placeWorld = param_1;
    g_placeType = param_2;
    g_placeParam5 = param_5;
    g_placeOpen = true;
    LocalBuilds_BeginBuildingPlacement();
    __try {
        g_origUniversal(param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8);
    } __finally {
        LocalBuilds_EndBuildingPlacement();
        g_placeOpen = false;
    }
    if (BuildingCount(*vecBeginPtr, *vecEndPtr) != before)
        Logf("republicmp  LOCKSTEP: the building tool placed a building here by itself - not covered, the PCs will differ");
}

// Outside the lockstep: the building tool's placing press at a spot the area
// rule refuses; sim_patches then reads it as not pressed.
bool LocalBuilds_PlacementRefusedHere()
{
    if (!Commands_ChecksHere()) return false;
    float pos[3];
    __try {
        __int64 world = GetPlacementManagerThisPtr();
        if (*(int*)(world + PLACEMENT_RESULT_OFFSET) != -1) return false; // not a valid place: the game shows why
        pos[0] = *(float*)(world + CURSOR_X_OFFSET);
        pos[1] = *(float*)(world + CURSOR_Y_OFFSET);
        pos[2] = *(float*)(world + CURSOR_Z_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return Commands_PointRefusedHere(pos);
}

bool LocalBuilds_PlacementPress()
{
    if (!g_placeOpen || !Commands_TakeLocal()) return false;
    __int64 world = g_placeWorld;
    BuildingMsg msg = {};
    __try {
        if (*(int*)(world + 0x11A10) != -1) return false; // not a valid place: the game shows why
        char typeName[64];
        SafeReadCStringDirect(g_placeType, typeName, sizeof(typeName));
        if (!LooksLikeValidType(typeName)) return false;
        msg.kind = BUILDING_PLACE;
        msg.mode = BuildMode_Read();
        msg.cursorX = msg.x = *(float*)(world + CURSOR_X_OFFSET);
        msg.cursorY = msg.y = *(float*)(world + CURSOR_Y_OFFSET);
        msg.cursorZ = msg.z = *(float*)(world + CURSOR_Z_OFFSET);
        msg.cursorRot = *(float*)(world + CURSOR_YAW_OFFSET);
        msg.param5 = (unsigned long long)g_placeParam5;
        msg.mirrored = *(unsigned char*)(world + MIRROR_EFFECTIVE_OFFSET) != 0;
        msg.pickModes = (*(unsigned char*)(world + 0xF600) ? 1 : 0) | (*(unsigned char*)(world + 0xF698) ? 2 : 0);
        strncpy_s(msg.typeName, typeName, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    Commands_Submit(LS_CMD_PLACE, &msg, sizeof(msg));
    return true;
}

// Entry of FUN_1402bade0: in a session the building tool's frames run inside
// PlacementAsCommand (its placing press becomes a command); otherwise the
// original runs unchanged.
void HookedUniversal(__int64 param_1, char* param_2, char param_3, char param_4,
                      unsigned __int64 param_5, unsigned int param_6,
                      char param_7, char param_8)
{
    // Placements are refused while the save-sync block screen is up.
    if (SyncBlock_IsActive()) return;
    if (Commands_TakeLocal() && !Buildings_IsApplyingCommand()) {
        PlacementAsCommand(param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8);
        return;
    }
    g_origUniversal(param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8);
}
