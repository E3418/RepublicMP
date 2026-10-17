// SPDX-License-Identifier: GPL-3.0-only
// See route_tools.h. The capture and the replay follow SoylentGreed's stop
// and copy line tool routes (RepublicLAN, read against the disassembly of
// 1.1.1.9).
#include "route_tools.h"
#include "commands.h"
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
#define VEHICLES_OFFSET 0x12810
#define BUILDINGS_OFFSET 0x11B08
#define NODE_LIST 0x298           // a network's nodes
#define PRESS_RVA 0xA54E91        // the left press, as the tools read it
#define CTRL_RVA 0xA54BAD         // the left Ctrl key
#define STAND_IN_BYTES 0x400

static const unsigned kNodeFields[2] = { 0xF908, 0xFA28 };
static const unsigned kStopTools[2] = { 0x2B2760, 0x2B4A50 };
#define LINE_COPY_TOOL 0x2B4C60
#define LINE_MARK 0x40CDE0
#define LINE_TEST 0x498990
#define LINE_HOME_TEST 0x1AA540
#define LINE_PUT 0x497B10
#define LINE_START 0x3BB330

struct RoutineEntry {
    unsigned rva;
    unsigned char entry[16];
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
};

typedef unsigned __int64 U64;
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
            if (memcmp(g_exeBase + e.rva, e.entry, sizeof(e.entry)) != 0) {
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
// Identities the same on every PC
// ---------------------------------------------------------------------------
//
// In lockstep the world's lists are the same on every PC: an object travels as
// its place in its list and a check.

#pragma pack(push, 1)
struct RouteRef {
    unsigned list;  // a line: 0 or 1 (the manager's +0 / +18); a node: the network's field
    int place;      // -1: none
    unsigned check; // a building's or vehicle's id, an FNV of a line's or node's words
};

enum { ROUTE_VEHICLE_STOP = 0, ROUTE_LINE_STOP = 1, ROUTE_LINE_COPY = 2 };

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
#pragma pack(pop)

static unsigned Fnv(const unsigned* words, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++)
        for (int b = 0; b < 4; b++) {
            h ^= (words[i] >> (8 * b)) & 0xffu;
            h *= 16777619u;
        }
    return h | 1u;
}

static bool Find(U64 vector, U64 object, int* place)
{
    U64 begin = Q(vector), end = Q(vector + 8);
    if (!begin || end < begin) return false;
    for (U64 at = begin; at < end; at += 8)
        if (Q(at) == object) {
            *place = (int)((at - begin) / 8);
            return true;
        }
    return false;
}

static U64 At(U64 vector, int place)
{
    U64 begin = Q(vector), end = Q(vector + 8);
    if (!begin || place < 0 || end < begin || (U64)place >= (end - begin) / 8) return 0;
    return Q(begin + (U64)place * 8);
}

static bool VehicleRef(U64 vehicle, RouteRef& r)
{
    r.list = 0;
    if (!vehicle || !Find((U64)World() + VEHICLES_OFFSET, vehicle, &r.place)) return false;
    r.check = *(unsigned*)(vehicle + 4);
    return true;
}

static U64 VehicleAt(const RouteRef& r)
{
    U64 v = At((U64)World() + VEHICLES_OFFSET, r.place);
    return v && *(unsigned*)(v + 4) == r.check ? v : 0;
}

static bool BuildingRef(U64 building, RouteRef& r)
{
    r.list = 0;
    if (!building || !Find((U64)World() + BUILDINGS_OFFSET, building, &r.place)) return false;
    r.check = *(unsigned*)building;
    return true;
}

static U64 BuildingAt(const RouteRef& r)
{
    U64 b = At((U64)World() + BUILDINGS_OFFSET, r.place);
    return b && *(unsigned*)b == r.check ? b : 0;
}

static unsigned LineCheck(U64 line)
{
    unsigned w[3] = { 0x4c494e45u, *(unsigned*)(line + 4), *(unsigned*)(line + 8) };
    return Fnv(w, 3);
}

static bool LineRef(U64 line, RouteRef& r)
{
    U64 lines = Q((U64)World() + LINES_OFFSET);
    if (!line || !lines) return false;
    for (unsigned k = 0; k < 2; k++)
        if (Find(lines + k * 0x18, line, &r.place)) {
            r.list = k;
            r.check = LineCheck(line);
            return true;
        }
    return false;
}

static U64 LineAt(const RouteRef& r)
{
    U64 lines = Q((U64)World() + LINES_OFFSET);
    if (!lines || r.list > 1) return 0;
    U64 l = At(lines + r.list * 0x18, r.place);
    return l && LineCheck(l) == r.check ? l : 0;
}

static unsigned NodeCheck(U64 node)
{
    unsigned w[3] = { 0x4e4f4445u, *(unsigned*)(node + 4), *(unsigned*)(node + 0xc) };
    return Fnv(w, 3);
}

static bool NodeRef(U64 node, RouteRef& r)
{
    for (unsigned field : kNodeFields) {
        U64 net = Q((U64)World() + field);
        if (net && Find(net + NODE_LIST, node, &r.place)) {
            r.list = field;
            r.check = NodeCheck(node);
            return true;
        }
    }
    return false;
}

static U64 NodeAt(const RouteRef& r)
{
    if (r.list != kNodeFields[0] && r.list != kNodeFields[1]) return 0;
    U64 net = Q((U64)World() + r.list);
    U64 n = net ? At(net + NODE_LIST, r.place) : 0;
    return n && NodeCheck(n) == r.check ? n : 0;
}

static const RouteRef kNone = { 0, -1, 0 };

static const char* KindText(unsigned kind)
{
    switch (kind) {
    case ROUTE_VEHICLE_STOP: return "a vehicle's stop";
    case ROUTE_LINE_STOP: return "a line's stop";
    case ROUTE_LINE_COPY: return "a vehicle on a line";
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
    m.building = m.node = m.vehicle = m.subject = kNone;
    if (!object || !window) return "no window";
    m.index = *(int*)(window + WINDOW_INDEX);
    if (kind == 0 ? !VehicleRef(object, m.object) : !LineRef(object, m.object)) return kind == 0 ? "the vehicle" : "the line";
    if (building && !BuildingRef(building, m.building)) return "the building";
    if (node && !NodeRef(node, m.node)) return "the node";
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
    m.building = m.node = m.subject = kNone;
    m.ctrl = ctrl ? 1 : 0;
    __try {
        if (!LineRef(Q(world + TARGET_OFFSET), m.object)) Refused(what, "the line");
        else if (!VehicleRef(g_markVehicle, m.vehicle)) Refused(what, "the vehicle");
        else if (subject && !VehicleRef(subject, m.subject)) Refused(what, "the window's vehicle");
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
    U64 object = m.kind == ROUTE_VEHICLE_STOP ? VehicleAt(m.object) : LineAt(m.object);
    if (!object) return m.kind == ROUTE_VEHICLE_STOP ? "the vehicle is gone" : "the line is gone";
    U64 building = 0, node = 0;
    if (m.building.place >= 0 && !(building = BuildingAt(m.building))) return "the building is gone";
    if (m.node.place >= 0 && !(node = NodeAt(m.node))) return "the node is gone";
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
    U64 line = LineAt(m.object), vehicle = VehicleAt(m.vehicle), subject = 0;
    if (!line) return "the line is gone";
    if (!vehicle) return "the vehicle is gone";
    if (m.subject.place >= 0 && !(subject = VehicleAt(m.subject))) return "the window's vehicle is gone";
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

bool RouteTools_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(RouteMsg) || !RouteTools_Ready()) return false;
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
