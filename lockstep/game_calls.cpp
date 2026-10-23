// SPDX-License-Identifier: GPL-3.0-only
// See game_calls.h. The sites and their argument kinds follow SoylentGreed's
// game-call commands (RepublicLAN, read against the disassembly of 1.1.1.9).
#include "game_calls.h"
#include "commands.h"
#include "world_refs.h"
#include "line_accept.h"
#include "junction_accept.h"
#include "rename.h"
#include "new_city.h"
#include <array>
#include <limits.h>
#include <string.h>
#include <utility>

#define WORLD_RVA 0x9D4F10
#define SEND_TO_DEPOT 0x3FBDC0
#define LINES_OFFSET 0x12800
#define DELETE_CLEANUP 0x4962C0
#define DRAG_RVA 0x996B98        // the depot window's dragged vehicle (interface state)
#define MAX_ARGS 7

// Argument kinds: x ignored (0), W the world, M the pointer world+worldPointer
// holds, V a vehicle, B a building, L a line, O a vehicle, building or line,
// o the same or null, b a byte, I a 32-bit integer, F a float (the third or
// the fourth argument only: it travels in xmm2 or xmm3), A the world address
// world+worldPointer or null, R a building's address plus worldPointer (a list
// inside it), D one of a building's citizens' destinations lists (the
// building plus +f18, +f30, +f48 or +f60).
struct GameCallSite {
    unsigned site, target;
    const char* name;
    const char* schema;
    unsigned worldPointer;
    unsigned result; // what a captured call answers the window
    bool orDepot;    // when the routine answers false, the window sends the vehicle (argument 2) to its depot
    int special;
};

enum {
    CALL_PLAIN = 0,
    // Not made during a session at all: the window changes the simulation every
    // frame on this PC only (it shows what the simulation last computed instead).
    CALL_PRESENTATION,
    // The line window's Accept: the copy goes to line_accept.cpp.
    CALL_LINE_ACCEPT,
    // The Accept block's later call (presentation): the vehicles' +66c are put back first.
    CALL_ACCEPT_MEASURES,
    // The lines list's Delete: +1 = 0, 499600, then 4962c0(manager, 1).
    CALL_DELETE_LINE,
    // The junction window's Accept: the copy goes to junction_accept.cpp.
    CALL_JUNCTION_ACCEPT,
    // The settings page applies its copy of the game rules: the copy goes to commands.cpp.
    CALL_GAME_RULES,
    // Made on this PC only, never carried: a cache the simulation rebuilds before it reads it.
    CALL_LOCAL,
    // The routine reads the depot window's dragged vehicle (exe+996b98): it travels as argument 0,
    // and every PC puts it in its own global for the call.
    CALL_DRAG,
    // The rename window's write (5290 = swprintf): a city's or a crossroad's name goes to rename.cpp.
    CALL_RENAME,
    // A window that makes the call on every frame it is open: the same call again within
    // REPEAT_MS is the same request, taken here but sent again only REFRESH_MS after the last.
    CALL_REPEATING,
    // The city tool's new city (32f450): the position and the name go to new_city.cpp.
    CALL_NEW_CITY,
    // The city tool's 32ff40 right after it: made by that command.
    CALL_NEW_CITY_ASSIGN,
};
#define REPEAT_MS 1500
#define REFRESH_MS 20000

// Appended only: a command names its site by index.
static const GameCallSite kSites[] = {
    { 0x7AA221, 0x497B10, "line window: copy schedule to vehicle", "MOObo", 0x12800, 0, false },
    { 0x7AA235, 0x3BB330, "line window: refresh vehicle after schedule copy", "WO", 0, 0, false },
    { 0x7AA369, 0x1AAAA0, "line window: replace vehicle", "WoOb", 0, 0, false },
    { 0x7AA989, 0x3BB330, "line window: refresh vehicle", "WO", 0, 0, false },
    { 0x7D9959, 0x6BF1D0, "vehicle window: rebuild route after assignment", "Vbbb", 0, 0, false },
    { 0x7D9C51, 0x6BF1D0, "vehicle window: rebuild route after assignment (second)", "Vbbb", 0, 0, false },
    { 0x7DA2F1, 0x6C52C0, "vehicle window: release vehicle", "V", 0, 0, false },
    { 0x7DA2FC, 0x3BB330, "vehicle window: refresh vehicle", "WO", 0, 0, false },
    { 0x7DA182, 0x6B7670, "vehicle window: toggle vehicle state", "V", 0, 0, false },
    { 0x7DA341, 0x6B7670, "vehicle window: toggle vehicle state (second)", "V", 0, 0, false },
    { 0x7D726E, 0x3FBDC0, "vehicle window: send to depot (keyboard)", "WVbbbb", 0, 0, false },
    { 0x7DA119, 0x3FBDC0, "vehicle window: send to depot", "WVbbbb", 0, 0, false },
    { 0x7E747B, 0x497B10, "vehicle window: clear the vehicle's line", "MoVbo", 0x12800, 0, false },
    { 0x7E7483, 0x67CC40, "vehicle window: refresh the vehicle after clearing its line", "V", 0, 0, false },
    { 0x7DA126, 0x3FB8E0, "vehicle window: send back to workplace", "xV", 0, 1, true },
    { 0x7DA70B, 0x6710B0, "vehicle window: next color/skin", "VI", 0, 0, false },
    { 0x7D5128, 0x3BB330, "line window: launch vehicle", "WO", 0, 0, false },
    { 0x7DA1CB, 0x6147E0, "vehicle window: locomotive back to auto-replacement", "MV", 0xD440, 0, false },
    { 0x7D39A7, 0x4915B0, "line window: clear line data", "L", 0, 0, false },
    { 0x359077, 0x496DE0, "lines list: create new line", "MI", 0x12800, 0, false },
    { 0x701D7E, 0x3FBDC0, "building window: call all vehicles home (each vehicle)", "WVbbbb", 0, 0, false },
    { 0x743BA3, 0x1EA1A0, "building window: try sending all vehicles with cargo", "WO", 0, 0, false },
    { 0x747C29, 0x1EA2A0, "building window (second): try sending all vehicles with cargo", "WO", 0, 0, false },
    // 6bf1d0 rebuilds the cargo summary (vehicle +458/+470, the window's "Carried" list) from the
    // train's cargo; it writes nothing else, and the simulation's readers rebuild it first.
    { 0x7D709A, 0x6BF1D0, "vehicle window: cargo summary for display", "Vbbb", 0, 0, false, CALL_LOCAL },
    { 0x7E9D46, 0x6BF1D0, "vehicle window: cargo summary for the schedule list", "Vbbb", 0, 0, false, CALL_LOCAL },
    { 0x7D1DC0, 0x495830, "line window: accept changes", "xLxx", 0, 0, false, CALL_LINE_ACCEPT },
    { 0x7D1DCB, 0x4945A0, "line window: accept (merge equal lines)", "ML", 0x12800, 0, false, CALL_PRESENTATION },
    { 0x7D1E4E, 0x22A6D0, "line window: accept (mark the line changed)", "Lx", 0, 0, false, CALL_ACCEPT_MEASURES },
    { 0x7D1E5D, 0x497650, "line window: accept (vehicles take the stops)", "ML", 0x12800, 0, false, CALL_PRESENTATION },
    { 0x7D1E6B, 0x499600, "line window: accept (name a private line)", "xL", 0, 0, false, CALL_PRESENTATION },
    { 0x3592F1, 0x499600, "lines list: delete line", "xL", 0, 0, false, CALL_DELETE_LINE },
    { 0x3592FF, 0x4962C0, "lines list: delete line (cleanup)", "Mb", 0x12800, 0, false, CALL_PRESENTATION },
    // A late-phase tool (main thread): building_addfield adds the picked field to the farm +d298.
    { 0x1D87D4, 0x162100, "field tool: add a field to a farm", "WBOF", 0, 0, false },
    // The junction window's Accept block (7595bb..7595f9): the copy into the junction is carried,
    // the rest of the block is made by the command on every PC.
    { 0x7595C2, 0x224300, "junction window: accept (reset the junction)", "x", 0, 0, false, CALL_PRESENTATION },
    { 0x7595CD, 0x226530, "junction window: accept (copy into the junction)", "xx", 0, 0, false, CALL_JUNCTION_ACCEPT },
    { 0x7595E5, 0x22A6D0, "junction window: accept (mark the junction changed)", "xx", 0, 0, false, CALL_PRESENTATION },
    { 0x7595EC, 0x0F98D0, "junction window: accept (take the junction off the list)", "xx", 0, 0, false, CALL_PRESENTATION },
    // The game menu's settings page: "Save settings", closing the window and the "Change settings?"
    // popup apply the page's copy of the game rules with 337170(world).
    { 0x33A5CF, 0x337170, "settings page: save settings", "W", 0, 0, false, CALL_GAME_RULES },
    { 0x33710F, 0x337170, "settings page: close", "W", 0, 0, false, CALL_GAME_RULES },
    { 0x7FEA3F, 0x337170, "settings popup: change settings", "W", 0, 0, false, CALL_GAME_RULES },
    // Selling a vehicle: the confirm window's Yes (7f2d50) and the building windows' "sell all
    // vehicles" prompts call 3dee50(world, vehicle, 1, 0, 0, 0, 0.0f), which queues the sale.
    { 0x7F3280, 0x3DEE50, "sell a vehicle", "WObbbbF", 0, 0, false },
    { 0x7F3D50, 0x3DEE50, "sell vehicles of a building", "WObbbbF", 0, 0, false },
    { 0x7F3E4E, 0x3DEE50, "sell a listed vehicle", "WObbbbF", 0, 0, false },
    // Links and office assignments the building windows remove or make: lists of
    // the buildings (+f98/+fb0, the assigned building's +888), heap only.
    { 0x7447FF, 0x1C7E80, "building window: remove the building from its partner's list", "xOO", 0, 0, false },
    { 0x74480A, 0x162B40, "building window: remove link record by index", "xOI", 0, 0, false },
    { 0x748BC9, 0x1C7E80, "building window (second): remove the building from its partner's list", "xOO", 0, 0, false },
    { 0x748BD7, 0x162B40, "building window (second): remove link record by index", "xOI", 0, 0, false },
    { 0x74BBC9, 0x162B40, "station window: remove link record by index", "xOI", 0, 0, false },
    { 0x76759A, 0x1C7E80, "link list window: remove the building from its partner's list", "xOO", 0, 0, false },
    { 0x7675AA, 0x162BD0, "link list window: remove the link record to a partner", "xOO", 0, 0, false },
    { 0x7676BE, 0x162360, "link list window: unassign building from office", "WOO", 0, 0, false },
    { 0x76F79A, 0x162360, "office window: unassign building from office", "WOO", 0, 0, false },
    { 0x770615, 0x162360, "office list: unassign building from office", "WOO", 0, 0, false },
    { 0x745149, 0x162A30, "clear all links (confirmation)", "xO", 0, 0, false },
    { 0x71454A, 0x161C90, "assign construction office", "WOOFbb", 0, 0, false },
    { 0x7145CF, 0x162360, "unassign construction office", "WOO", 0, 0, false },
    { 0x717D4A, 0x161C90, "assign construction office (second window)", "WOOFbb", 0, 0, false },
    { 0x717DD0, 0x162360, "unassign construction office (second window)", "WOO", 0, 0, false },
    // The depot window composes trains by drag and drop: 3fdef0(world, vehicle) takes the dragged
    // vehicle off its head, 3fd5d0(world, head, vehicle, index, 1, 0, &result) couples it at the
    // index (-1 the end; the result only picks the window's message), 3fe520(-, head) moves the
    // dragged vehicle and its wagons onto the head. With the vehicle window's replace or line copy
    // tool selected, its rows act like the line window's.
    { 0x7C7A08, 0x3FDEF0, "depot window: uncouple the dragged vehicle (train drop)", "WV", 0, 0, false },
    { 0x7C7A15, 0x3FE520, "depot window: attach the dragged train to the vehicle", "VV", 0, 0, false, CALL_DRAG },
    { 0x7C7E87, 0x3FDEF0, "depot window: uncouple the dragged vehicle (row drop)", "WV", 0, 0, false },
    { 0x7C7ED8, 0x3FD5D0, "depot window: couple the dragged vehicle at the row", "WVVIbbx", 0, 0, false },
    { 0x7C8CB0, 0x3FDEF0, "depot window: uncouple the dragged vehicle (end drop)", "WV", 0, 0, false },
    { 0x7C8CE7, 0x3FD5D0, "depot window: couple the dragged vehicle at the end", "WVVIbbx", 0, 0, false },
    { 0x7C81F4, 0x1AB290, "depot window: replace tool (prepare the replaced vehicle)", "WV", 0, 0, false },
    { 0x7C8205, 0x1AAAA0, "depot window: replace tool (replace by the row's vehicle)", "WoOb", 0, 0, false },
    { 0x7C8458, 0x497B10, "depot window: line copy tool (put the row's vehicle on the line)", "MLVbo", 0x12800, 0, false },
    { 0x7C846C, 0x3BB330, "depot window: line copy tool (refresh the vehicle)", "WO", 0, 0, false },
    { 0x8018D0, 0x5290, "rename window: write the name", "xxx", 0, 0, false, CALL_RENAME },
    // Construction offices (by SoylentGreed). The office pick tools of the Late dispatcher
    // (building_copyconstruction 1d6cb0 / 1d8330, building_addconstruction 1d9980,
    // building_adddemolition 1da580): every routine a click makes, in click order -
    // assignment 161c90, a reconstruction 1a8d70 first where needed, the construction list's
    // refresh 1a04f0. Their demolitions (168730) go through the demolition hook.
    { 0x1D70EE, 0x161C90, "office pick tool: assign", "WOOFbb", 0, 0, false },
    { 0x1D83C2, 0x161C90, "office pick tool: assign (each listed)", "WOOFbb", 0, 0, false },
    { 0x1D9FFD, 0x1A8D70, "office pick tool: start reconstruction", "WObb", 0, 0, false },
    { 0x1DA02A, 0x161C90, "office pick tool: assign after reconstruction", "WOOFbb", 0, 0, false },
    { 0x1DA48D, 0x161C90, "office pick tool: assign (second path)", "WOOFbb", 0, 0, false },
    { 0x1DAAD2, 0x161C90, "office pick tool: assign demolition", "WOOFbb", 0, 0, false },
    { 0x1DAAE9, 0x1A04F0, "office pick tool: refresh construction list", "WBb", 0, 0, false },
    { 0x1DB07D, 0x161C90, "office pick tool: assign demolition (second path)", "WOOFbb", 0, 0, false },
    { 0x1DB0A1, 0x1A04F0, "office pick tool: refresh construction list (second path)", "WBb", 0, 0, false },
    // The area office tool (assign_co, 5eedf0): its selection assigned one by one (a
    // reconstruction first where needed) or unassigned; the selection is cleared locally.
    { 0x5E9C86, 0x1A8D70, "area office tool: start reconstruction", "WObb", 0, 0, false },
    { 0x5E9CB1, 0x161C90, "area office tool: assign", "WOOFbb", 0, 0, false },
    { 0x5E9D76, 0x1A8D70, "area office tool: start reconstruction (second)", "WObb", 0, 0, false },
    { 0x5E9D95, 0x161C90, "area office tool: assign (second)", "WOOFbb", 0, 0, false },
    { 0x5EA0F6, 0x1A8D70, "area office tool: start reconstruction (third)", "WObb", 0, 0, false },
    { 0x5EA121, 0x161C90, "area office tool: assign (third)", "WOOFbb", 0, 0, false },
    { 0x5EA1E6, 0x1A8D70, "area office tool: start reconstruction (fourth)", "WObb", 0, 0, false },
    { 0x5EA205, 0x161C90, "area office tool: assign (fourth)", "WOOFbb", 0, 0, false },
    { 0x5EA305, 0x162360, "area office tool: unassign", "WOO", 0, 0, false },
    { 0x5EA3D6, 0x162360, "area office tool: unassign (second)", "WOO", 0, 0, false },
    // The office window's "add construction": 1561c0(world, 0, office, resource mode, byte)
    // assigns the waiting sites of that resource to the office, on every frame the window is open.
    { 0x779AE0, 0x1561C0, "office window: add construction (each resource)", "WxBIb", 0, 0, false, CALL_REPEATING },
    { 0x779CB5, 0x1561C0, "office window: add construction (mode 7)", "WxBIb", 0, 0, false, CALL_REPEATING },
    { 0x779D77, 0x1561C0, "office window: add construction (mode 8)", "WxBIb", 0, 0, false, CALL_REPEATING },
    { 0x779E07, 0x1561C0, "office window: add construction (mode 0)", "WxBIb", 0, 0, false, CALL_REPEATING },
    { 0x779E98, 0x1561C0, "office window: add construction (mode 1)", "WxBIb", 0, 0, false, CALL_REPEATING },
    { 0x779F3A, 0x1561C0, "office window: add construction (mode 2)", "WxBIb", 0, 0, false, CALL_REPEATING },
    // Construction buttons of the building windows.
    { 0x776144, 0x1A8D70, "building window: start reconstruction", "WObb", 0, 0, false },
    { 0x7763B7, 0x1A8D70, "building window: start reconstruction (second button)", "WObb", 0, 0, false },
    { 0x6F9F42, 0x160570, "building window header: continue construction", "WOb", 0, 0, false },
    { 0x7702C4, 0x1A04F0, "construction panel: rebuild the construction list", "WBb", 0, 0, false },
    { 0x77DBB7, 0x1A04F0, "construction window: rebuild the construction list", "WBb", 0, 0, false },
    // Building pick tools: one routine call per click (copy waste and transfer settings, copy
    // distribution links, copy specific sources) and the distribution office's lists (also the
    // destinations panel's pick; 1d4330 never reads its first argument, rcx is left over).
    { 0x1DBCBE, 0x1DC5D0, "copy waste settings tool: copy", "WBB", 0, 0, false },
    { 0x1DC00F, 0x1DC7D0, "copy transfer settings tool: copy", "WBB", 0, 0, false },
    { 0x2B8A8E, 0x2B8B10, "copy distribution tool: copy links", "xBB", 0, 0, false },
    { 0x1D6C18, 0x1D7E10, "copy sources tool: copy", "xBBxA", 0xF5C8, 0, false },
    { 0x1DB790, 0x1D4330, "distribution office tool: add to list +f18", "xBR", 0xF18, 0, false },
    { 0x1DB7A5, 0x1D4330, "distribution office tool: add to list +f48", "xBR", 0xF48, 0, false },
    { 0x1DB7BA, 0x1D4330, "distribution office tool: add to list +f30", "xBR", 0xF30, 0, false },
    { 0x1DB7D3, 0x1D4330, "distribution office tool: add to list +f60", "xBR", 0xF60, 0, false },
    // The citizens' destinations panel 776d40, drawn by the windows of homes, stops and the
    // other buildings with the list the window's +260 selects (by SoylentGreed): "any
    // building" 1d4330(-, 0, list) (a picked building goes through the tool above),
    // "Remove" 1d4440(-, selected, list), the share buttons 1d4550(-, entry, delta, list).
    { 0x77746B, 0x1D4330, "destinations panel: add any building", "xxD", 0, 0, false },
    { 0x77757F, 0x1D4440, "destinations panel: remove the selected entry", "xID", 0, 0, false },
    { 0x7781D9, 0x1D4550, "destinations panel: share up", "xIFD", 0, 0, false },
    { 0x77836D, 0x1D4550, "destinations panel: share down", "xIFD", 0, 0, false },
    { 0x32F3E3, 0x32F450, "city tool: make the city", "xxx", 0, 0, false, CALL_NEW_CITY },
    { 0x32F403, 0x32FF40, "city tool: give the city its buildings", "xx", 0, 0, false, CALL_NEW_CITY_ASSIGN },
};
#define SITE_COUNT (sizeof(kSites) / sizeof(kSites[0]))

enum { OBJ_NONE = 0, OBJ_VEHICLE = 1, OBJ_BUILDING = 2, OBJ_LINE = 3 };

// The citizens' destinations lists of a building: workers, students, passengers, tourists.
static const unsigned kDestinationLists[] = { 0xF18, 0xF30, 0xF48, 0xF60 };

static bool DestinationList(unsigned offset)
{
    for (unsigned o : kDestinationLists)
        if (o == offset) return true;
    return false;
}

#pragma pack(push, 1)
struct GameCallMsg {
    unsigned index;
    unsigned target;              // the site's routine, a check
    unsigned char kinds[MAX_ARGS]; // OBJ_* of an object argument
    RouteRef refs[MAX_ARGS];
    unsigned values[MAX_ARGS];    // b / I arguments
};
#pragma pack(pop)

static bool g_siteOn[SITE_COUNT];

void GameCalls_Bind(const TsmHost* host)
{
    TsmBind(host);
}
static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

typedef U64 (*CallFn)(U64, U64, U64, U64, U64, U64, U64);
typedef U64 (*CallF3Fn)(U64, U64, float, U64, U64, U64, U64);
typedef U64 (*CallF4Fn)(U64, U64, U64, float, U64, U64, U64);

// A site whose third argument is a float (kept as its bits in a[2]).
static bool Float3(const GameCallSite& s)
{
    return s.schema[0] && s.schema[1] && s.schema[2] == 'F';
}

// A site whose fourth argument is a float (kept as its bits in a[3]).
static bool Float4(const GameCallSite& s)
{
    return s.schema[0] && s.schema[1] && s.schema[2] && s.schema[3] == 'F';
}

static float AsFloat(U64 bits)
{
    unsigned b = (unsigned)bits;
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static U64 CallSite(const GameCallSite& s, const U64* a)
{
    if (Float3(s))
        return ((CallF3Fn)(g_exeBase + s.target))(a[0], a[1], AsFloat(a[2]), a[3], a[4], a[5], a[6]);
    if (Float4(s))
        return ((CallF4Fn)(g_exeBase + s.target))(a[0], a[1], a[2], AsFloat(a[3]), a[4], a[5], a[6]);
    return ((CallFn)(g_exeBase + s.target))(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
}

static bool ObjectRef(U64 object, unsigned char& kind, RouteRef& r)
{
    if (Ref_Vehicle(object, r)) kind = OBJ_VEHICLE;
    else if (Ref_Building(object, r)) kind = OBJ_BUILDING;
    else if (Ref_Line(object, r)) kind = OBJ_LINE;
    else return false;
    return true;
}

static U64 ObjectAt(unsigned char kind, const RouteRef& r)
{
    switch (kind) {
    case OBJ_VEHICLE: return Ref_VehicleAt(r);
    case OBJ_BUILDING: return Ref_BuildingAt(r);
    case OBJ_LINE: return Ref_LineAt(r);
    }
    return 0;
}

// The arguments as identities; nullptr, or why they cannot travel.
static const char* Capture(unsigned index, const U64* a, GameCallMsg& m)
{
    const GameCallSite& s = kSites[index];
    U64 world = (U64)World();
    memset(&m, 0, sizeof(m));
    m.index = index;
    m.target = s.target;
    for (int j = 0; j < MAX_ARGS; j++) m.refs[j] = kNoRef;
    for (int j = 0; s.schema[j]; j++) {
        switch (s.schema[j]) {
        case 'x': break;
        case 'W': if (a[j] != world) return "an argument is not the world"; break;
        case 'M': if (a[j] != Q(world + s.worldPointer)) return "an argument is not the world's object"; break;
        case 'V':
            if (!Ref_Vehicle(a[j], m.refs[j])) return "a vehicle is not in the world's list";
            m.kinds[j] = OBJ_VEHICLE;
            break;
        case 'B':
            if (!Ref_Building(a[j], m.refs[j])) return "a building is not in the world's list";
            m.kinds[j] = OBJ_BUILDING;
            break;
        case 'L':
            if (!Ref_Line(a[j], m.refs[j])) return "a line is not in the lines manager";
            m.kinds[j] = OBJ_LINE;
            break;
        case 'o':
            if (!a[j]) break;
            // fall through
        case 'O':
            if (!ObjectRef(a[j], m.kinds[j], m.refs[j])) return "an object is not in the world's lists";
            break;
        case 'b': m.values[j] = (unsigned)(a[j] & 0xff); break;
        case 'I': case 'F': m.values[j] = (unsigned)a[j]; break;
        case 'A':
            if (a[j] && a[j] != world + s.worldPointer) return "an argument is not the world's field";
            m.values[j] = a[j] ? 1 : 0;
            break;
        case 'R':
            if (!a[j] || !Ref_Building(a[j] - s.worldPointer, m.refs[j])) return "a list's building is not in the world's list";
            m.kinds[j] = OBJ_BUILDING;
            break;
        case 'D':
            for (unsigned o : kDestinationLists)
                if (a[j] > o && Ref_Building(a[j] - o, m.refs[j])) {
                    m.values[j] = o;
                    break;
                }
            if (!m.values[j]) return "a destinations list's building is not in the world's list";
            m.kinds[j] = OBJ_BUILDING;
            break;
        default: return "unknown argument kind";
        }
    }
    return nullptr;
}

static const char* Rebuild(const GameCallMsg& m, U64* a)
{
    const GameCallSite& s = kSites[m.index];
    U64 world = (U64)World();
    for (int j = 0; j < MAX_ARGS; j++) a[j] = 0;
    for (int j = 0; s.schema[j]; j++) {
        switch (s.schema[j]) {
        case 'W': a[j] = world; break;
        case 'M': a[j] = Q(world + s.worldPointer); break;
        case 'V': case 'B': case 'L': case 'O': case 'o':
            if (m.kinds[j] == OBJ_NONE) {
                if (s.schema[j] != 'o') return "an object is missing";
                break;
            }
            if (!(a[j] = ObjectAt(m.kinds[j], m.refs[j]))) return "an object is gone";
            break;
        case 'b': a[j] = m.values[j] & 0xff; break;
        case 'I': case 'F': a[j] = m.values[j]; break;
        case 'A': a[j] = m.values[j] ? world + s.worldPointer : 0; break;
        case 'R':
            if (m.kinds[j] != OBJ_BUILDING || !(a[j] = Ref_BuildingAt(m.refs[j]))) return "a list's building is gone";
            a[j] += s.worldPointer;
            break;
        case 'D':
            if (m.kinds[j] != OBJ_BUILDING || !DestinationList(m.values[j])) return "not a destinations list";
            if (!(a[j] = Ref_BuildingAt(m.refs[j]))) return "a destinations list's building is gone";
            a[j] += m.values[j];
            break;
        }
    }
    return nullptr;
}

// CALL_REPEATING: the calls seen lately (the interface thread only).
struct RepeatSeen {
    GameCallMsg msg;
    ULONGLONG seen, sent;
};
static RepeatSeen g_repeats[32];

// True when this repeating call is to be sent now: a new one, or one sent REFRESH_MS ago.
static bool RepeatDue(const GameCallMsg& m)
{
    ULONGLONG now = GetTickCount64();
    RepeatSeen* free = nullptr;
    for (RepeatSeen& r : g_repeats) {
        if (r.seen && now - r.seen > REPEAT_MS) r.seen = 0;
        if (!r.seen) {
            if (!free) free = &r;
            continue;
        }
        if (memcmp(&r.msg, &m, sizeof(m)) != 0) continue;
        r.seen = now;
        if (now - r.sent < REFRESH_MS) return false;
        r.sent = now;
        return true;
    }
    if (!free) free = &g_repeats[0];
    free->msg = m;
    free->seen = free->sent = now;
    return true;
}

static U64 Run(unsigned index, const U64* a)
{
    const GameCallSite& s = kSites[index];
    if (!Commands_TakeUi()) {
        // The junction window and the settings page are never opened by a command: in a
        // session their call not taken stays on this PC.
        static int s_logged = 0;
        if ((s.special == CALL_JUNCTION_ACCEPT || s.special == CALL_GAME_RULES) && Commands_Capturing() && s_logged < 16) {
            ++s_logged;
            Logf("republicmp  GAME CALL: %s made on this PC only (while a command was being made)", s.name);
        }
        return CallSite(s, a);
    }
    switch (s.special) {
    case CALL_PRESENTATION:
        return s.result;
    case CALL_LOCAL:
        return CallSite(s, a);
    case CALL_RENAME:
        // Other names (buildings, vehicles, line copies) are written here and carried as they were.
        return Rename_Capture(a[0], a[1], a[2]) ? 0 : CallSite(s, a);
    case CALL_NEW_CITY:
        return NewCity_Capture(a[1], a[2]) ? 0 : CallSite(s, a);
    case CALL_NEW_CITY_ASSIGN:
        return NewCity_TakeAssign() ? 0 : CallSite(s, a);
    case CALL_ACCEPT_MEASURES:
        LineAccept_RestoreMeasures();
        return s.result;
    case CALL_LINE_ACCEPT:
        LineAccept_Capture(a[1], a[2]);
        return s.result;
    case CALL_JUNCTION_ACCEPT:
        JunctionAccept_Capture(a[0], a[1]);
        return s.result;
    case CALL_GAME_RULES:
        Commands_CaptureGameRules();
        return s.result;
    case CALL_DELETE_LINE:
        // The list cleared the line's +1 before the call: put back until the delete is made.
        __try {
            if (a[1]) *(BYTE*)(a[1] + 1) = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        break;
    }
    U64 args[MAX_ARGS];
    memcpy(args, a, sizeof(args));
    GameCallMsg m;
    const char* why = nullptr;
    __try {
        if (s.special == CALL_DRAG && !(args[0] = *(U64*)(g_exeBase + DRAG_RVA))) why = "no vehicle is dragged";
        if (!why) why = Capture(index, args, m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading the arguments";
    }
    if (why)
        Logf("republicmp  GAME CALL: %s not shared - %s; nothing is done", s.name, why);
    else if (s.special != CALL_REPEATING || RepeatDue(m))
        Commands_Submit(LS_CMD_GAME_CALL, &m, sizeof(m));
    return s.result;
}

template <unsigned I>
static U64 Thunk(U64 a1, U64 a2, U64 a3, U64 a4, U64 a5, U64 a6, U64 a7)
{
    U64 a[MAX_ARGS] = { a1, a2, a3, a4, a5, a6, a7 };
    return Run(I, a);
}

// The same for a site whose third argument is a float (xmm2): its bits go in a[2].
template <unsigned I>
static U64 ThunkF3(U64 a1, U64 a2, float a3, U64 a4, U64 a5, U64 a6, U64 a7)
{
    unsigned bits;
    memcpy(&bits, &a3, 4);
    U64 a[MAX_ARGS] = { a1, a2, bits, a4, a5, a6, a7 };
    return Run(I, a);
}

// The same for a site whose fourth argument is a float (xmm3): its bits go in a[3].
template <unsigned I>
static U64 ThunkF4(U64 a1, U64 a2, U64 a3, float a4, U64 a5, U64 a6, U64 a7)
{
    unsigned bits;
    memcpy(&bits, &a4, 4);
    U64 a[MAX_ARGS] = { a1, a2, a3, bits, a5, a6, a7 };
    return Run(I, a);
}

template <size_t... I>
static constexpr std::array<void*, sizeof...(I)> MakeThunks(std::index_sequence<I...>)
{
    return { { (void*)&Thunk<(unsigned)I>... } };
}

template <size_t... I>
static constexpr std::array<void*, sizeof...(I)> MakeThunksF3(std::index_sequence<I...>)
{
    return { { (void*)&ThunkF3<(unsigned)I>... } };
}

template <size_t... I>
static constexpr std::array<void*, sizeof...(I)> MakeThunksF4(std::index_sequence<I...>)
{
    return { { (void*)&ThunkF4<(unsigned)I>... } };
}

bool GameCalls_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(GameCallMsg)) return false;
    GameCallMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.index >= SITE_COUNT || kSites[m.index].target != m.target) {
        Logf("republicmp  GAME CALL: unknown site %u - not made", m.index);
        return false;
    }
    const GameCallSite& s = kSites[m.index];
    U64 a[MAX_ARGS];
    const char* why = nullptr;
    U64 r = 0;
    U64* drag = nullptr;
    U64 dragged = 0;
    __try {
        why = Rebuild(m, a);
        if (!why && s.special != CALL_PLAIN && s.special != CALL_DELETE_LINE && s.special != CALL_DRAG &&
            s.special != CALL_REPEATING)
            why = "not a command site";
        if (!why) {
            if (s.special == CALL_DELETE_LINE) *(BYTE*)(a[1] + 1) = 0;
            if (s.special == CALL_DRAG) {
                drag = (U64*)(g_exeBase + DRAG_RVA);
                dragged = *drag;
                *drag = a[0];
            }
            r = CallSite(s, a);
            if (drag) *drag = dragged;
            if (s.special == CALL_DELETE_LINE) {
                typedef void (*CleanupFn)(U64, U64);
                ((CleanupFn)(g_exeBase + DELETE_CLEANUP))(Q((U64)World() + LINES_OFFSET), 1);
            }
            if (s.orDepot && !(r & 0xff) && a[1]) {
                typedef void (*DepotFn)(U64, U64, U64, U64, U64, U64);
                ((DepotFn)(g_exeBase + SEND_TO_DEPOT))((U64)World(), a[1], 0, 0, 1, 1);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "the call faulted";
        if (drag) *drag = dragged;
    }
    Logf("republicmp  GAME CALL: %s: %s", s.name, why ? why : "made");
    return why == nullptr;
}

bool GameCalls_Install()
{
    static const auto thunks = MakeThunks(std::make_index_sequence<SITE_COUNT>());
    static const auto thunksF3 = MakeThunksF3(std::make_index_sequence<SITE_COUNT>());
    static const auto thunksF4 = MakeThunksF4(std::make_index_sequence<SITE_COUNT>());
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) {
        Logf("republicmp  GAME CALL: no relay page near the executable - window buttons are not shared");
        return false;
    }
    DWORD old;
    VirtualProtect(relay, SITE_COUNT * 16, PAGE_EXECUTE_READWRITE, &old);
    for (size_t i = 0; i < SITE_COUNT; i++) {
        BYTE* r = relay + i * 16;
        r[0] = 0xff; r[1] = 0x25; r[2] = r[3] = r[4] = r[5] = 0;
        void* to = Float3(kSites[i]) ? thunksF3[i] : Float4(kSites[i]) ? thunksF4[i] : thunks[i];
        memcpy(r + 6, &to, 8);
    }
    VirtualProtect(relay, SITE_COUNT * 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, SITE_COUNT * 16);
    int on = 0;
    for (size_t i = 0; i < SITE_COUNT; i++) {
        BYTE* at = g_exeBase + kSites[i].site;
        int rel = 0;
        memcpy(&rel, at + 1, 4);
        if (at[0] != 0xE8 || kSites[i].site + 5 + rel != kSites[i].target) {
            Logf("republicmp  GAME CALL: site 0x%X (%s) differs - left alone", kSites[i].site, kSites[i].name);
            continue;
        }
        long long to = (long long)(relay + i * 16) - (long long)(at + 5);
        if (to < INT_MIN || to > INT_MAX) continue;
        unsigned char call[5] = { 0xE8 };
        int rel32 = (int)to;
        memcpy(call + 1, &rel32, 4);
        DWORD prot;
        VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &prot);
        memcpy(at, call, 5);
        VirtualProtect(at, 5, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), at, 5);
        g_siteOn[i] = true;
        on++;
    }
    Logf("republicmp  GAME CALL: %d of %d window call sites relayed", on, (int)SITE_COUNT);
    return on == (int)SITE_COUNT;
}
