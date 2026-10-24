// SPDX-License-Identifier: GPL-3.0-only
// Networks: every drag-built infrastructure tool (roads, pipes, rails,
// footpaths, fences, power lines, conveyors, ...) made from data - the
// lockstep commands of the network tools.
//
// How the game builds these
// -------------------------
// Every drag tool is a per-frame wrapper (FUN_1403d00f0 for roads and one
// sibling per tool, dispatched by FUN_14030d1a0 on the active tool name) that
// drives three shared engine primitives on the tool's own "manager" object,
// found at a fixed offset off the placement god-object (thisPtr):
//   FUN_1404fc040  drag start  - picks/creates the start node (manager+0x428)
//   FUN_1404fcaf0  drag tick   - routes to the cursor, fills the route buffer
//   FUN_140518a80  commit      - turns the route into a real segment
//
// A finished segment is: start node -> interior points -> end node. The
// route buffer (manager+0x410/+0x418, 24-byte records: xyz + 3 engine floats)
// holds ONLY the interior points; the ends are the node objects
// manager+0x428 / manager+0x430 (or an existing node the end snapped to,
// manager+0x740 when manager+0x749 == 1).
//
// Capture
// -------
// world/local_builds.cpp hooks FUN_140518a80's entry and, for a tool
// manager, copies the interior records, both node positions and any building
// connection port at either end (NetSegment) into a command instead of
// letting the commit run. Commits that happen inside a building placement (a
// building laying its own access road) are not taken - every PC's placement
// lays them itself.
//
// Making it
// ---------
// The segment is made through the same primitives: the tool wrapper
// starts a drag on the start node, fc040 is re-run in fuzzy (+-1m) mode so the
// start attaches to the existing node there, fcaf0 is called ONCE with the
// cursor on the end node and the sender's interior records as its param_8
// (bulk route - the engine skips its own routing), and the wrapper releases,
// which commits. The result is bit-identical on every PC.
//
// Upgrades (re-typing an existing segment, e.g. gravel -> panels) take a
// different commit branch (manager+0x390 != 0) and travel as
// NetUpgradeMsg; see ApplyUpgrade in networks.cpp.
#pragma once

#include "tesmio_plugin.h"
#include "build_mode.h"
#include <stddef.h> // offsetof

// Upper bound on interior points per segment (the longest real drags seen
// were ~125 points; 742 was seen on a long road).
#define NET_MAX_POINTS 2048

// Tool categories. The value travels on the wire, so the order must match on
// both machines. Manager offsets / wrappers: see g_categories in networks.cpp.
enum NetCategory {
    NET_CATEGORY_ROAD = 0,         // road_*, bridges, tunnels, one-way
    NET_CATEGORY_PIPE = 1,         // pipe_underground, pipe_steam
    NET_CATEGORY_WATERPIPE = 2,    // waterpipe_*, sewage
    NET_CATEGORY_HEATPIPE = 3,     // heating_pipe
    NET_CATEGORY_RAIL = 4,         // rail_*
    NET_CATEGORY_PEDESTRIAN = 5,   // pedestrianway_*, pedestrian_tunel
    NET_CATEGORY_FENCE = 6,        // fence_*
    NET_CATEGORY_POWER_HIGH = 7,   // eletric_high*, eletric_uhigh*
    NET_CATEGORY_POWER_LOW = 8,    // eletric_low*, eletric_ulow*
    NET_CATEGORY_CONVEYOR = 9,     // conveyor
    NET_CATEGORY_CABLEWAY = 10,    // create_cableway
    NET_CATEGORY_AIRROAD = 11,     // create_airroad*
    NET_CATEGORY_FACTORYCONN = 12, // factoryconnection (+ direction, see NetDirectionMsg)
    NET_CATEGORY_SECOND_ROAD = 13, // second_road_* (decoration paths and pavements, by SoylentGreed)
    NET_CATEGORY_COUNT
};

struct NetCategoryInfo {
    const char* name;
    int managerOffset; // *(thisPtr + managerOffset) = the tool's manager object
    bool enabled;      // disabled categories are neither captured nor applied
};

// nullptr for an out-of-range category.
const NetCategoryInfo* Networks_GetCategoryInfo(int category);

// Category whose manager object is `manager`, or -1 (also for disabled ones).
int Networks_CategoryForManager(__int64 thisPtr, __int64 manager);

// Number of segments in a category's network, or -1 if unreadable
// (diagnostics: which networks a building placement adds to).
int Networks_SegmentCount(__int64 thisPtr, int category);

// One route record exactly as the engine stores it in manager+0x410
// (24 bytes). `extra` is engine-internal per-point data (orientation and
// running length); it is transmitted verbatim, never interpreted.
struct NetPoint {
    float pos[3];
    float extra[3];
};

// A building connection port at one end of a segment. Pipes, conveyors and
// power lines can end on a building instead of a node; the engine reads that
// from the "hovered port" globals DAT_1409e45b0 (building) / DAT_1409e45b8
// (port index). Buildings are identified by position (obj+0x350); the port
// index is per building type and therefore identical on both machines.
struct NetBuildingPort {
    int valid;       // 0 = this end is not on a building port
    int portIndex;
    float bldPos[3];
};

// Where a footpath end joins a road (the footpath tool's hover state, see
// Networks_ReadFootpathLinks): a road edge it splits at one of its points,
// and/or a road node it meets. The commit links the two networks' nodes
// there - the pedestrian crossing. Edges and nodes are identified by
// position (an edge by its end nodes and point count).
struct NetRoadLink {
    int hasEdge;
    int pointIndex;     // the edge's point the road is split at
    float edgeA[3];     // the edge's node 0
    float edgeB[3];     // the edge's node 1
    int edgePoints;     // the edge's point count
    int hasNode;
    float node[3];
};

// Everything needed to rebuild one committed segment.
struct NetSegment {
    int category;       // NetCategory
    int typeId;         // manager+0x528
    int variant;        // manager+0x52c
    float curveValue;   // manager+0x48, the tool's curve setting
    float startNode[3]; // start node position
    float endNode[3];   // end node position
    NetBuildingPort startPort;
    NetBuildingPort endPort;
    NetRoadLink startLink; // footpaths only
    NetRoadLink endLink;
    int pointCount;     // interior points, 0..NET_MAX_POINTS (power-line spans can have 0-1)
    NetPoint points[NET_MAX_POINTS];
};

// A segment command. Only the first pointCount points are meaningful.
struct NetSegmentMsg {
    BuildMode mode;   // the drawing PC's rubles/dollars/realistic choice (build_mode.h), made with it
    NetSegment seg;
};

// Upgrade of existing segments (FUN_140518a80's manager+0x390 branch). A
// single-segment drag re-types segment +0x380 over the point range
// (+0x388, +0x38c); a pathfound multi-segment drag re-types whole segments.
// Each piece identifies its segment by node positions.
#define NET_MAX_UPGRADE_PIECES 32
struct NetPiece {
    float segStart[3];  // segment's start node position
    float segEnd[3];    // segment's end node position
    int segPointCount;  // interior point count (identity check)
    int rangeStart;     // -1 = from the start node
    int rangeEnd;       // == segPointCount = to the end node
    int oldType;        // manager+0x394
    int oldVariant;     // manager+0x398
    int flag39c;        // manager+0x39c, one-way direction (0 = none)
};
struct NetUpgradeMsg {
    BuildMode mode;   // as NetSegmentMsg::mode
    int category;
    int newType;    // manager+0x528
    int newVariant; // manager+0x52c
    int flag5c4;    // manager+0x5c4
    int pieceCount;
    NetPiece pieces[NET_MAX_UPGRADE_PIECES];
};

// Demolition with the terrain_demolish tool. The tool, every frame, calls
// fcaf0(manager, &cursor, 0x65, 0, 0...) - the engine finds the piece of
// path under the cursor and arms the commit (manager+0x390) - and, while the
// button is held, commits with FUN_140518a80(manager, 0, 0, 0). Every PC
// repeats that with the clicking PC's cursor, so its own engine finds the
// segment and arms the commit. The piece the clicking PC's engine cut
// (manager+0x388 .. +0x38c on segment +0x380) also travels, as the positions
// of its two ends; if a PC's engine picked a different range, it is replaced
// with that one before the commit (route buffer and end nodes included, as
// the upgrade does). Short pieces, which the cursor pick skips, are armed
// through the node of the piece, as the tool does.
// The piece's segment also travels by identity - its place in the manager's
// segment list +0x2b0 and a check of its nodes and point count, the lists
// being equal on every PC in lockstep (by SoylentGreed: the bulldozer's
// commands replay "only when the same edge and range are selected"). A PC
// whose cursor pick finds another piece or none, as with a planned (white)
// piece under the cursor, arms that segment directly.
struct NetDemolishMsg {
    int category;
    float cursor[3];  // thisPtr+0xf6c at the sender's commit
    int hasRange;     // 0 = only the cursor is known
    float rangeA[3];  // start of the cut piece (point rs, or node 0 when rs = -1)
    float rangeB[3];  // end of the cut piece (point re, or node 1 when re = count)
    // Multi-segment demolish (manager+0x3d0 set): whole segments, each
    // demolished on its own at the receiver.
    int pieceCount;
    NetPiece pieces[8];
    int edgeIndex;      // the piece's segment in manager +0x2b0, -1 = not known
    unsigned edgeCheck; // Networks_EdgeCheck of that segment
};

// A segment's place in the manager's list +0x2b0 and its check (its nodes'
// positions and its point count); false when it is not listed.
bool Networks_EdgeIdentity(__int64 manager, __int64 seg, int* index, unsigned* check);

// Direction of a factory connection. The factoryconnection_oneway tool
// (FUN_1403d6e70 with mode 0x32) commits nothing: a click on an existing
// connection cycles segment+0x1bc (0 = both ways, 1 / 2 = one way, relative
// to the segment's node 0 / node 1) and refreshes the buildings at both ends
// (FUN_140191950). A changed direction becomes a command (see
// Networks_LockstepDirectionsDetect); every PC finds its segment like an
// upgrade piece, writes the direction (mirrored if its segment runs the other
// way) and runs the same refresh.
struct NetDirectionMsg {
    int category;
    NetPiece piece; // segStart / segEnd / segPointCount only
    int direction;              // segment+0x1bc
};

// "Start planned construction" (#20): drawn without starting, a road or rail
// is a plan (edge +0x140 = 1) on both PCs; the construction menu, the
// construction window and the network tool panel turn the plans into sites
// with FUN_1405258e0(network, dollars, rubles, count, group) - every planned
// edge of the network (count > 0: only among its last `count` edges; group
// >= 0: only edges of that group, +0x124) - or FUN_140525c30, the same over
// every group but `group`. That call travels as it is and every PC makes it
// on its own network: the same plans become sites there.
struct NetPlanStartMsg {
    int category;
    int exceptGroup; // 1 = FUN_140525c30
    int dollars;
    int rubles;
    int count;
    int group;
    BuildMode mode;  // the clicking PC's build mode: each new site takes it (FUN_1402d9d40)
};

// The group construction tools (group_build_rub = 0, group_build_usd = 1,
// group_build_co = 2, group_suspend_const = 3): FUN_14021c110(world, mode),
// called every frame one of them is selected. The first frame with the button
// up (exe+0xa54e90) and a drag marked (world+0x14033) commits: the box
// (world+0x13f5c..+0x13f70) turned by the camera yaw (exe+0x9e9d20) picks the
// planned pieces of every network (full edge lists) and the buildings, and
// starts (or suspends) their construction - no FUN_1405258e0 involved. That
// frame's box, cursor, yaw and build mode travel; every PC writes them,
// makes the same call and puts its own back.
// (by SoylentGreed: the commit frame and the state it reads)
struct NetGroupToolMsg {
    int category;    // NET_GROUP_TOOL_CATEGORY
    int mode;
    float box[6];    // world+0x13f5c: the moving corner, then the press point
    float cursor[3]; // world+0xf6c
    float yaw;       // exe+0x9e9d20
    BuildMode buildMode;
};

#define NET_GROUP_TOOL_CATEGORY (-2)

void Networks_Bind(const TsmHost* host);

// Footpath commit (inside the commit hook, before the original runs): reads
// where its ends join roads - the start from the footpath manager
// (+0x360 edge, +0x368 point, +0x370 node), the end from the hover globals
// (0x9E4590 / 0x9E4598 / 0x9E45A0). Clears both links for other networks.
void Networks_ReadFootpathLinks(__int64 manager, int category, NetSegment& seg);

// Clears the hover globals the local mouse leaves (the node, edge and road
// under the cursor, the hovered building port): the road planner reads them
// when it lays a path, a building's access road included. Not restored - the
// game recomputes them on the next frame.
void Networks_ClearHover();

// Hooks FUN_1405258e0 / FUN_140525c30 (see NetPlanStartMsg). Call once
// from TsmPluginStart.
bool Networks_InstallPlanStartHooks();

// Hooks FUN_14021c110 (see NetGroupToolMsg). Call once from
// TsmPluginStart.
bool Networks_InstallGroupToolHook();

// True while this module makes a command, so the commit hook does not take
// it as a new one.
bool Networks_IsApplyingCommand();

// Lockstep commands (lockstep/commands), made on every PC at a tick
// boundary, the PC that drew it included. One try each - a retry on a later
// frame would land at another point of the simulation. Main thread. True when
// the engine made it.
bool Networks_ApplyCommandSegment(const NetSegmentMsg& msg);
// Ends a drag the local player has under way (every tool manager's preview
// cleared, the drag flag down) before a command uses the tools; the drag's
// start is kept for Networks_ResumeLocalDrag.
void Networks_CancelLocalDrag();
// After the commands, in this PC's private scope (its draws, searches and
// view): a drag Networks_CancelLocalDrag ended begins again from its start
// (FUN_1404fc040 with the start's own arguments and hover) when the button is
// still held and the same tool is in hand - another player's command no
// longer drops the road being drawn. The next held frame draws it on to the
// cursor. Otherwise the drag stays ended.
void Networks_ResumeLocalDrag();
// Hooks FUN_1404fc040 (a drag's start) to keep this PC's own starts. Call
// once from TsmPluginStart.
bool Networks_InstallDragStartHook();
// Start planned construction (the interface's buttons) and the group tools'
// commit frame become commands while commands are captured; made through the
// replays above.
bool Networks_ApplyCommandPlanStart(const NetPlanStartMsg& msg);
bool Networks_ApplyCommandGroupTool(const NetGroupToolMsg& msg);
// Factory connection directions (factoryconnection_oneway cycles one inline
// in the Late body): Baseline records them, Detect puts a changed one back
// and hands it over as a command; ApplyCommandDirection makes it.
void Networks_LockstepDirectionsBaseline();
void Networks_LockstepDirectionsDetect();
bool Networks_ApplyCommandDirection(const NetDirectionMsg& msg);
bool Networks_ApplyCommandUpgrade(const NetUpgradeMsg& msg);
bool Networks_ApplyCommandDemolish(const NetDemolishMsg& msg);

// Lockstep: a bulldozer commit whose piece the hook cannot read (the cursor
// on a cut's end: the game removes through the hovered node or edge). The
// hovered node and edge travel as positions in the commit's network; every PC
// sets them, runs the tool's update at the cursor and commits what it picks.
struct NetDemolishHover {
    int nodeKind;    // 0 none, 1 = exe+9e4560, 2 = exe+9e45e0
    float node[3];
    int edgeKind;    // 0 none, 1 = exe+9e4558, 2 = exe+9e45d8
    float edgeA[3];  // the edge's node 0 and node 1
    float edgeB[3];
    int edgePoints;
    int nodeIndex;      // the node in the manager's committed nodes +0x298, -1 = not listed there
    unsigned nodeCheck; // its position's bits
};
struct NetDemolishHoverMsg {
    NetDemolishMsg m; // the cursor; no range, no pieces
    NetDemolishHover hover;
};
// The hovered node / edge as they are now, when they belong to this network.
bool Networks_ReadDemolishHover(__int64 manager, NetDemolishHover* out);
bool Networks_ApplyCommandDemolishHover(const NetDemolishHoverMsg& msg);
