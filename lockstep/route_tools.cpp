// SPDX-License-Identifier: GPL-3.0-only
// See route_tools.h. The capture and the replay follow SoylentGreed's stop
// and copy line tool routes (RepublicLAN, read against the disassembly of
// 1.1.1.9).
#include "route_tools.h"
#include "commands.h"
#include "world_refs.h"
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define TARGET_OFFSET 0xD298      // the tool's object: a vehicle or a line
#define WINDOW_OFFSET 0xD2A0      // the tool's window
#define SELECTED_OFFSET 0xD428    // the selected tool
#define MENU_OFFSETS { 0xFBC, 0xFBE, 0xFC3 }
#define WINDOW_INDEX 0x264        // the window's stop index
#define WINDOW_KIND 0x248         // 1: a vehicle window
#define WINDOW_VEHICLE 0x240
#define LINE_WINDOWS_RVA 0x9E19D0 // the line windows (a vector of window pointers)
#define LINE_WINDOW_COPY 0x270    // a line window's working copy
#define LINES_OFFSET 0x12800      // the lines manager: +0 and +18 line vectors
#define PRESS_RVA 0xA54E91        // the left press, as the tools read it
#define CTRL_RVA 0xA54BAD         // the left Ctrl key
#define STAND_IN_BYTES 0x400

static const unsigned kStopTools[2] = { 0x2B2760, 0x2B4A50 };
#define LINE_COPY_TOOL 0x2B4C60
#define LINE_MARK 0x40CDE0
#define LINE_TEST 0x498990
#define LINE_HOME_TEST 0x1AA540
#define LINE_PUT 0x497B10
#define LINE_START 0x3BB330
#define DEPOT_TOOL 0x2B2DD0
#define DEPOT_MARK 0x40BD60
#define DEPOT_MARK_RETURN 0x2B3795 // after the mark's call at 2b3790
#define DEPOT_LIST_BEGIN (0x100 - 0x58) // the tool's vehicle list, from the mark caller's rsp
#define DEPOT_LIST_END (0x100 - 0x50)
#define DEPOT_MAX 64

struct RoutineEntry {
    unsigned rva;
    unsigned char entry[16];
    size_t size; // 0: all 16 bytes
};
static const RoutineEntry kRoutines[] = {
    { 0x2B5120, { 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x44, 0x89, 0x44, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89 } },
    { 0x2B2760, { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0x99, 0x98, 0xd2, 0x00, 0x00, 0x48 } },
    { 0x2B4A50, { 0x40, 0x53, 0x57, 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0xb9, 0x98, 0xd2, 0x00, 0x00, 0x48, 0x8b } },
    { 0x2B4C60, { 0x40, 0x55, 0x56, 0x41, 0x54, 0x48, 0x8d, 0x6c, 0x24, 0xd0, 0x48, 0x81, 0xec, 0x30, 0x01, 0x00 } },
    { 0x40CDE0, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xf8, 0xe5, 0xff, 0xff, 0xb8, 0x08, 0x1b } },
    { 0x498990, { 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83 } },
    { 0x1AA540, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x9a, 0xf8, 0x04, 0x00 } },
    { 0x3BB330, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x50, 0x83, 0xb9, 0xbc, 0x05, 0x00, 0x00 } },
    { 0x497B10, { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xf0 } },
    // The depot tool's frame (mov rax,rsp; five pushes; lea rbp,[rax-108h]) and its mark.
    { 0x2B2DD0, { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xf8 } },
    { 0x2B2DE0, { 0xfe, 0xff, 0xff, 0x48, 0x81, 0xec, 0xe0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 10 },
    { 0x40BD60, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xf8, 0xe5, 0xff, 0xff, 0xb8, 0x08, 0x1b } },
    // The sending steps.
    { 0x68B2B0, { 0x48, 0x8b, 0x81, 0x08, 0x17, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x29, 0xf3, 0x0f, 0x10, 0x05 } },
    { 0x3E2900, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x44, 0x88, 0x4c, 0x24, 0x20, 0x56 } },
    { 0x3E3F40, { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xec } },
    { 0x3BB640, { 0x4d, 0x8b, 0xd0, 0x48, 0x85, 0xd2, 0x75, 0x03, 0x32, 0xc0, 0xc3, 0x48, 0x8b, 0x82, 0xb0, 0x05 } },
    { 0x3A38F0, { 0x48, 0x8b, 0x82, 0x48, 0x0a, 0x00, 0x00, 0x45, 0x33, 0xc0, 0x48, 0x2b, 0x82, 0x40, 0x0a, 0x00 } },
    { 0x3A3830, { 0x45, 0x33, 0xd2, 0x0f, 0x28, 0xd9, 0x45, 0x85, 0xc9, 0x79, 0x56, 0x49, 0x8b, 0x80, 0x48, 0x0a } },
    { 0x6DE260, { 0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x83, 0xb9, 0x04, 0x1a, 0x00, 0x00, 0x03, 0x48, 0x8b, 0xf9 } },
    { 0x1AE130, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xd9, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48 } },
    { 0x6B7070, { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x81, 0xec, 0xe0, 0x01, 0x00, 0x00, 0x48 } },
    { 0x6B7420, { 0x40, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0x5c, 0x24, 0x40, 0x4d, 0x8b, 0xf8 } },
};

typedef U64 (*PickFn)(U64, U64, U64, U64, U64, U64, U64*, U64*);
typedef U64 (*ToolFn)(U64 world);
typedef U64 (*MarkFn)(U64 world, U64 vehicle, U64 a3, U64 refused);
typedef unsigned char (*LineTestFn)(U64 world, U64 vehicle, U64 line, int* result, U64 zero);
typedef unsigned char (*HomeTestFn)(U64 world, U64 vehicle);
typedef void (*LinePutFn)(U64 lines, U64 line, U64 vehicle, U64 one, U64 subject);
typedef void (*StartFn)(U64 world, U64 vehicle);

static int g_ready = -1;
static unsigned g_origin = 0; // this PC's commands, to use its own window

void RouteTools_Bind(const TsmHost* host)
{
    TsmBind(host);
    g_origin = (unsigned)(GetTickCount64() * 2654435761u) ^ GetCurrentProcessId() ^ (unsigned)(U64)&g_origin;
    if (!g_origin) g_origin = 1;
}

bool RouteTools_Ready()
{
    if (g_ready < 0) {
        g_ready = 1;
        for (const RoutineEntry& e : kRoutines) {
            if (memcmp(g_exeBase + e.rva, e.entry, e.size ? e.size : sizeof(e.entry)) != 0) {
                Logf("republicmp  ROUTE: game routine at 0x%X differs - the route tools are put away in lockstep", e.rva);
                g_ready = 0;
                break;
            }
        }
    }
    return g_ready == 1;
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
enum { ROUTE_VEHICLE_STOP = 0, ROUTE_LINE_STOP = 1, ROUTE_LINE_COPY = 2, ROUTE_DEPOT = 3 };

struct RouteMsg {
    unsigned kind;
    unsigned origin;
    int index;        // stops: the window's stop index
    RouteRef object;  // stops: the vehicle (kind 0) or the line (kind 1); line copy: the line
    RouteRef building;
    RouteRef node;
    RouteRef vehicle; // line copy: the picked vehicle
    RouteRef subject; // line copy: the window's vehicle, or none
    unsigned char ctrl;
};

// The depot tool: the depot and the vehicles the tool listed, in its order.
struct DepotMsg {
    unsigned kind; // ROUTE_DEPOT
    unsigned count;
    RouteRef depot;
    RouteRef vehicles[DEPOT_MAX];
};
#pragma pack(pop)


static const char* KindText(unsigned kind)
{
    switch (kind) {
    case ROUTE_VEHICLE_STOP: return "a vehicle's stop";
    case ROUTE_LINE_STOP: return "a line's stop";
    case ROUTE_LINE_COPY: return "a vehicle on a line";
    case ROUTE_DEPOT: return "vehicles sent to a depot";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// The clicking PC
// ---------------------------------------------------------------------------

// An open line window whose working copy is this object.
static bool LineWindowCopy(U64 object)
{
    U64 v = (U64)(g_exeBase + LINE_WINDOWS_RVA);
    U64 begin = Q(v), end = Q(v + 8);
    if (!begin || end < begin || (end - begin) / 8 > 4096) return false;
    for (U64 at = begin; at < end; at += 8) {
        U64 w = Q(at);
        if (w && Q(w + LINE_WINDOW_COPY) == object) return true;
    }
    return false;
}

static void Refused(const char* what, const char* why)
{
    Logf("republicmp  ROUTE: %s not shared - %s; nothing is done", what, why);
}

static const char* CaptureStop(int kind, U64 building, U64 node, RouteMsg& m)
{
    U64 world = (U64)World();
    U64 object = Q(world + TARGET_OFFSET), window = Q(world + WINDOW_OFFSET);
    memset(&m, 0, sizeof(m));
    m.kind = kind == 0 ? ROUTE_VEHICLE_STOP : ROUTE_LINE_STOP;
    m.origin = g_origin;
    m.building = m.node = m.vehicle = m.subject = kNoRef;
    if (!object || !window) return "no window";
    m.index = *(int*)(window + WINDOW_INDEX);
    if (kind == 0 ? !Ref_Vehicle(object, m.object) : !Ref_Line(object, m.object)) return kind == 0 ? "the vehicle" : "the line";
    if (building && !Ref_Building(building, m.building)) return "the building";
    if (node && !Ref_Node(node, m.node)) return "the node";
    if (!building && !node) return "nothing picked";
    return nullptr;
}

// The replay (main thread): while set, the pick answers 1 with the carried result.
static bool t_forced = false;
static U64 t_forcedBuilding = 0, t_forcedNode = 0;
static int t_forcedUsed = 0;

U64 RouteTools_StopPick(int kind, U64 world, U64 a2, U64 a3, U64 a4, U64 a5, U64 a6, U64* building, U64* node)
{
    if (t_forced) {
        t_forcedUsed++;
        if (building) *building = t_forcedBuilding;
        if (node) *node = t_forcedNode;
        return 1;
    }
    U64 r = ((PickFn)(g_exeBase + 0x2B5120))(world, a2, a3, a4, a5, a6, building, node);
    if (!(r & 0xff) || !Commands_TakeLocal()) return r;
    __try {
        // The line window's "New stop" edits its working copy: local until the window's Accept.
        if (kind == 1 && LineWindowCopy(Q((U64)World() + TARGET_OFFSET))) return r;
        RouteMsg m;
        const char* why = RouteTools_Ready() ? CaptureStop(kind, building ? *building : 0, node ? *node : 0, m)
                                              : "a game routine differs";
        if (why)
            Refused(KindText(kind), why);
        else
            Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(KindText(kind), "a fault while reading it");
    }
    if (building) *building = 0;
    if (node) *node = 0;
    return r & ~(U64)0xff; // the tool adds nothing now
}

// The copy line tool with the click hidden: its mark tells the pick.
static bool g_markArmed = false, g_markSeen = false, g_markAllowed = false;
static U64 g_markVehicle = 0;

U64 RouteTools_LineMark(U64 world, U64 vehicle, U64 a3, U64 refused)
{
    if (g_markArmed && !g_markSeen) {
        g_markSeen = true;
        g_markVehicle = vehicle;
        g_markAllowed = (refused & 0xff) == 0;
    }
    return ((MarkFn)(g_exeBase + LINE_MARK))(world, vehicle, a3, refused);
}

U64 RouteTools_LineCopyTool(U64 world)
{
    ToolFn tool = (ToolFn)(g_exeBase + LINE_COPY_TOOL);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world);
    U64 window = Q(world + WINDOW_OFFSET), subject = 0;
    if (window && *(int*)(window + WINDOW_KIND) == 1) subject = Q(window + WINDOW_VEHICLE);
    unsigned char ctrl = *(g_exeBase + CTRL_RVA);
    BYTE kept = *press;
    *press = 0;
    g_markArmed = true;
    g_markSeen = g_markAllowed = false;
    g_markVehicle = 0;
    U64 r = tool(world);
    g_markArmed = false;
    *press = kept;
    if (!g_markSeen || !g_markAllowed || !g_markVehicle) return r;
    const char* what = KindText(ROUTE_LINE_COPY);
    if (!RouteTools_Ready()) {
        Refused(what, "a game routine differs");
        return r;
    }
    RouteMsg m;
    memset(&m, 0, sizeof(m));
    m.kind = ROUTE_LINE_COPY;
    m.origin = g_origin;
    m.index = 0;
    m.building = m.node = m.subject = kNoRef;
    m.ctrl = ctrl ? 1 : 0;
    __try {
        if (!Ref_Line(Q(world + TARGET_OFFSET), m.object)) Refused(what, "the line");
        else if (!Ref_Vehicle(g_markVehicle, m.vehicle)) Refused(what, "the vehicle");
        else if (subject && !Ref_Vehicle(subject, m.subject)) Refused(what, "the window's vehicle");
        else Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(what, "a fault while reading it");
    }
    return r;
}

// The depot tool (vehicle_godepo, vehicle_godepo_batch) with the click hidden:
// its mark gives the depot and, from the tool's frame, the vehicles it listed.
static bool g_depotArmed = false, g_depotSeen = false, g_depotOverflow = false;
static U64 g_depot = 0;
static U64 g_depotList[DEPOT_MAX];
static unsigned g_depotCount = 0;

U64 RouteTools_DepotMark(U64 world, U64 depot, U64 a3, U64 a4, U64 a5, U64 callerRsp, U64 returnTo)
{
    if (g_depotArmed && !g_depotSeen) {
        g_depotSeen = true;
        g_depot = depot;
        __try {
            if (returnTo == (U64)(g_exeBase + DEPOT_MARK_RETURN)) {
                U64 begin = Q(callerRsp + DEPOT_LIST_BEGIN), end = Q(callerRsp + DEPOT_LIST_END);
                if (begin && end >= begin && (end - begin) % 8 == 0) {
                    U64 n = (end - begin) / 8;
                    if (n > DEPOT_MAX) g_depotOverflow = true;
                    else {
                        for (U64 i = 0; i < n; i++) g_depotList[i] = Q(begin + i * 8);
                        g_depotCount = (unsigned)n;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_depotCount = 0;
        }
    }
    typedef U64 (*DepotMarkFn)(U64, U64, U64, U64, U64);
    return ((DepotMarkFn)(g_exeBase + DEPOT_MARK))(world, depot, a3, a4, a5);
}

U64 RouteTools_DepotTool(U64 world, U64 batch)
{
    typedef U64 (*DepotToolFn)(U64, U64);
    DepotToolFn tool = (DepotToolFn)(g_exeBase + DEPOT_TOOL);
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal()) return tool(world, batch);
    BYTE kept = *press;
    *press = 0;
    g_depotArmed = true;
    g_depotSeen = g_depotOverflow = false;
    g_depot = 0;
    g_depotCount = 0;
    U64 r = tool(world, batch);
    g_depotArmed = false;
    *press = kept;
    if (!g_depotSeen || !g_depot || (!g_depotCount && !g_depotOverflow)) return r;
    const char* what = KindText(ROUTE_DEPOT);
    if (g_depotOverflow) {
        Refused(what, "more vehicles than a command carries");
        return r;
    }
    if (!RouteTools_Ready()) {
        Refused(what, "a game routine differs");
        return r;
    }
    DepotMsg m;
    memset(&m, 0, sizeof(m));
    m.kind = ROUTE_DEPOT;
    m.count = g_depotCount;
    for (unsigned i = 0; i < DEPOT_MAX; i++) m.vehicles[i] = kNoRef;
    __try {
        const char* why = nullptr;
        if (!Ref_Building(g_depot, m.depot)) why = "the depot";
        for (unsigned i = 0; !why && i < g_depotCount; i++)
            if (!Ref_Vehicle(g_depotList[i], m.vehicles[i])) why = "a vehicle";
        if (why) Refused(what, why);
        else Commands_Submit(LS_CMD_ROUTE, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Refused(what, "a fault while reading it");
    }
    return r;
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

static BYTE g_standInWindow[STAND_IN_BYTES], g_standInTool[STAND_IN_BYTES];

// The tool itself with this PC's own state put aside: the object as +d298, a
// window (this PC's own when it is the clicking PC and still shows the object,
// else a stand-in holding the index) as +d2a0, a tool selected, menus closed,
// the pick forced to the carried result.
static const char* ApplyStop(const RouteMsg& m, int* used)
{
    U64 world = (U64)World();
    U64 object = m.kind == ROUTE_VEHICLE_STOP ? Ref_VehicleAt(m.object) : Ref_LineAt(m.object);
    if (!object) return m.kind == ROUTE_VEHICLE_STOP ? "the vehicle is gone" : "the line is gone";
    U64 building = 0, node = 0;
    if (m.building.place >= 0 && !(building = Ref_BuildingAt(m.building))) return "the building is gone";
    if (m.node.place >= 0 && !(node = Ref_NodeAt(m.node))) return "the node is gone";
    if (!building && !node) return "nothing picked";
    static const unsigned kMenus[3] = MENU_OFFSETS;
    U64 own298 = Q(world + TARGET_OFFSET), own2a0 = Q(world + WINDOW_OFFSET), own428 = Q(world + SELECTED_OFFSET);
    BYTE menus[3];
    for (int i = 0; i < 3; i++) menus[i] = *(BYTE*)(world + kMenus[i]);
    bool ownWindow = m.origin == g_origin && own2a0 && own298 == object;
    memset(g_standInWindow, 0, sizeof(g_standInWindow));
    memset(g_standInTool, 0, sizeof(g_standInTool));
    U64 window = ownWindow ? own2a0 : (U64)g_standInWindow;
    *(int*)(window + WINDOW_INDEX) = m.index;
    *(U64*)(world + TARGET_OFFSET) = object;
    *(U64*)(world + WINDOW_OFFSET) = window;
    *(U64*)(world + SELECTED_OFFSET) = own428 ? own428 : (U64)g_standInTool;
    for (int i = 0; i < 3; i++) *(BYTE*)(world + kMenus[i]) = 0;
    t_forced = true;
    t_forcedBuilding = building;
    t_forcedNode = node;
    t_forcedUsed = 0;
    bool faulted = false;
    __try {
        ((ToolFn)(g_exeBase + kStopTools[m.kind]))(world);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
    }
    t_forced = false;
    *used = t_forcedUsed;
    *(U64*)(world + TARGET_OFFSET) = own298;
    *(U64*)(world + WINDOW_OFFSET) = own2a0;
    *(U64*)(world + SELECTED_OFFSET) = own428;
    for (int i = 0; i < 3; i++) *(BYTE*)(world + kMenus[i]) = menus[i];
    return faulted ? "the tool faulted" : nullptr;
}

// The copy line tool's click steps (2b4f69..2b4fd6).
static const char* ApplyLineCopy(const RouteMsg& m, int* result)
{
    U64 world = (U64)World();
    U64 line = Ref_LineAt(m.object), vehicle = Ref_VehicleAt(m.vehicle), subject = 0;
    if (!line) {
        Logf("republicmp  ROUTE: the line (list %u, place %d) is not in this PC's lines manager", m.object.list, m.object.place);
        return "the line is gone";
    }
    if (!vehicle) return "the vehicle is gone";
    if (m.subject.place >= 0 && !(subject = Ref_VehicleAt(m.subject))) return "the window's vehicle is gone";
    U64 lines = Q(world + LINES_OFFSET);
    if (!lines) return "no lines manager";
    *result = -1;
    __try {
        ((LineTestFn)(g_exeBase + LINE_TEST))(world, vehicle, line, result, 0);
        if (*result != -1) return nullptr;
        bool home = Q(vehicle + 0x4F8) != 0 && ((HomeTestFn)(g_exeBase + LINE_HOME_TEST))(world, vehicle) != 0;
        ((LinePutFn)(g_exeBase + LINE_PUT))(lines, line, vehicle, 1, subject);
        if (home && m.ctrl) ((StartFn)(g_exeBase + LINE_START))(world, vehicle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

// The depot tool's sending steps (2b380b..2b3a63), last listed vehicle first.
static const char* ApplyDepot(const DepotMsg& m, unsigned* sent)
{
    typedef unsigned char (*UnusableFn)(U64 vehicle);
    typedef unsigned char (*DepotTypeFn)(U64 world, U64 depot, U64 type, U64, U64);
    typedef unsigned char (*LinkedFn)(U64, U64 depot, U64 vehicle, U64);
    typedef int (*SlotFn)(U64, U64 depot);
    typedef unsigned char (*SlotFitsFn)(U64, float, U64 depot, int);
    typedef void (*UnloadFn)(U64 vehicle);
    typedef unsigned char (*AtHomeFn)(U64, U64 vehicle);
    typedef void (*RouteFn)(U64 world, U64, U64 vehicle, U64);
    typedef void (*SendFn)(U64 vehicle, U64 depot);
    typedef void (*SendResultFn)(U64 vehicle, U64 depot, int*, int*);
    U64 world = (U64)World();
    U64 depot = Ref_BuildingAt(m.depot);
    if (!depot) return "the depot is gone";
    if (!m.count || m.count > DEPOT_MAX) return "no vehicles";
    U64 list[DEPOT_MAX] = {};
    for (unsigned i = 0; i < m.count; i++)
        if (!(list[i] = Ref_VehicleAt(m.vehicles[i]))) return "a vehicle is gone";
    *sent = 0;
    __try {
        for (unsigned k = m.count; k-- > 0;) {
            U64 v = list[k];
            U64 type = Q(v + 0x1708);
            if (m.count > 1 && *(unsigned*)(type + 0x294) != 1) continue;
            if (((UnusableFn)(g_exeBase + 0x68B2B0))(v)) continue;
            if (!((DepotTypeFn)(g_exeBase + 0x3E2900))(world, depot, type, 0, 0) ||
                !((DepotTypeFn)(g_exeBase + 0x3E3F40))(world, depot, type, 0, 0))
                continue;
            if (*(BYTE*)(Q(v + 0x1710) + 0x25C)) {
                float load = *(float*)(type + 0x7A78);
                for (U64 at = Q(v + 0x3A0); at < Q(v + 0x3A8); at += 8) load += *(float*)(Q(Q(at) + 0x1708) + 0x7A78);
                if (((LinkedFn)(g_exeBase + 0x3BB640))(0, depot, v, 0) && load > *(float*)(Q(depot + 0x318) + 0x9E4))
                    continue;
            }
            if (Q(v + 0x4F8) == depot) continue;
            if (!Q(depot + 0x5D0) && *(unsigned*)(type + 0x294) == 6) {
                int slot = ((SlotFn)(g_exeBase + 0x3A38F0))(0, depot);
                if (slot < 0 || !((SlotFitsFn)(g_exeBase + 0x3A3830))(0, *(float*)(type + 0x7A78), depot, slot)) continue;
            }
            U64 dt = Q(depot + 0x318);
            if (dt && *(unsigned*)(dt + 0x360) == 0x2B && Q(v + 0x490) != Q(v + 0x488)) continue;
            if (*(unsigned*)(type + 0x294) == 8 && Q(v + 0xC80)) continue;
            if (*(unsigned*)(type + 0x294) == 0xA && *(unsigned*)(v + 0x1A04) == 3) ((UnloadFn)(g_exeBase + 0x6DE260))(v);
            if (Q(v + 0x4F8) && !((AtHomeFn)(g_exeBase + 0x1AA540))(0, v)) {
                *(U64*)(v + 0x600) = depot;
                U64 route = Q(v + 0x1AD0);
                if (!route) continue;
                ((RouteFn)(g_exeBase + 0x1AE130))(world, route, v, 0);
            } else {
                ((SendFn)(g_exeBase + 0x6B7070))(v, depot);
                int a = -1, c = -1;
                ((SendResultFn)(g_exeBase + 0x6B7420))(v, depot, &a, &c);
            }
            ++*sent;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the steps faulted";
    }
    return nullptr;
}

bool RouteTools_ApplyCommand(const void* data, size_t size)
{
    if (!RouteTools_Ready()) return false;
    if (size == sizeof(DepotMsg) && *(const unsigned*)data == ROUTE_DEPOT) {
        DepotMsg d;
        memcpy(&d, data, sizeof(d));
        unsigned sent = 0;
        const char* why = ApplyDepot(d, &sent);
        Logf("republicmp  ROUTE: %s (%u listed, %u sent): %s", KindText(ROUTE_DEPOT), d.count, sent, why ? why : "done");
        return why == nullptr;
    }
    if (size != sizeof(RouteMsg)) return false;
    RouteMsg m;
    memcpy(&m, data, sizeof(m));
    const char* why = nullptr;
    char detail[64] = "";
    if (m.kind == ROUTE_VEHICLE_STOP || m.kind == ROUTE_LINE_STOP) {
        int used = 0;
        why = ApplyStop(m, &used);
        if (!why && !used) why = "the tool did not pick";
        sprintf_s(detail, ", stop index %d", m.index);
    } else if (m.kind == ROUTE_LINE_COPY) {
        int result = -1;
        why = ApplyLineCopy(m, &result);
        if (!why && result != -1) sprintf_s(detail, " - refused by the game (%d)", result);
    } else {
        why = "unknown kind";
    }
    Logf("republicmp  ROUTE: %s%s%s: %s", KindText(m.kind), detail, m.origin == g_origin ? " (this PC)" : "",
         why ? why : "done");
    return why == nullptr;
}
