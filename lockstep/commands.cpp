// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP by E3418 and Sohn (juniorsohn); all authors in README.md.
// See commands.h.
#include "commands.h"
#include "wallets.h"
#include "../determinism/determinism.h"
#include "../world/networks.h"
#include "../world/buildings.h"
#include "../world/menus_closed.h"
#include "../world/local_builds.h"
#include "lockstep_net.h"
#include "remote_sounds.h"
#include "../game/input_block.h"
#include "../game/world.h"
#include "../session/save_sync.h"
#include "building_settings.h"
#include "../world/terrain_hook.h"
#include "purchase.h"
#include "route_tools.h"
#include "game_calls.h"
#include "line_accept.h"
#include "world_refs.h"
#include "vehicle_switches.h"
#include "building_switches.h"
#include "brush_tools.h"
#include "hover_tools.h"
#include "junction_accept.h"
#include "cargo_settings.h"
#include "source_slots.h"
#include "rename.h"
#include "immigrant_invite.h"
#include "shared_save.h"
#include "research.h"
#include "link_tools.h"
#include "office_selection.h"
#include "new_city.h"
#include "join_tool.h"
#include "cancel_tool.h"
#include "world_workers.h"
#include "cancel_plans.h"
#include "plan_remove.h"
#include "car_owners.h"
#include "line_spacing.h"
#include "crime_cancel.h"
#include "call_home.h"
#include "popups.h"
#include "presence.h"
#include "buoy_tool.h"
#include "go_there.h"
#include "christmas_lights.h"
#include "square_tool.h"
#include "build_modes.h"
#include "../determinism/waste_trace.h"
#include "../core/guard.h"
#include <vector>
#include <string.h>

void Commands_Bind(const ModHost* host)
{
    ModHost_Bind(host);
    Purchase_Bind(host);
    RouteTools_Bind(host);
    GameCalls_Bind(host);
    LineAccept_Bind(host);
    WorldRefs_Bind(host);
    VehicleSwitches_Bind(host);
    BuildingSwitches_Bind(host);
    BrushTools_Bind(host);
    HoverTools_Bind(host);
    JunctionAccept_Bind(host);
    CargoSettings_Bind(host);
    SourceSlots_Bind(host);
    Rename_Bind(host);
    ImmigrantInvite_Bind(host);
    SharedSave_Bind(host);
    Research_Bind(host);
    LinkTools_Bind(host);
    OfficeSelection_Bind(host);
    NewCity_Bind(host);
    JoinTool_Bind(host);
    CancelTool_Bind(host);
    WorldWorkers_Bind(host);
    CancelPlans_Bind(host);
    PlanRemove_Bind(host);
    CarOwners_Bind(host);
    LineSpacing_Bind(host);
    CrimeCancel_Bind(host);
    CallHome_Bind(host);
    Popups_Bind(host);
    Presence_Bind(host);
    WasteTrace_Bind(host);
    BuoyTool_Bind(host);
    GoThere_Bind(host);
    ChristmasLights_Bind(host);
    SquareTool_Bind(host);
    BuildModes_Bind(host);
}

struct Command {
    int kind;
    int player = 0; // 0 the host (and the probe's own), 1.. a player
    unsigned long long afterTick;
    unsigned seq;
    std::vector<BYTE> data;
};

static CRITICAL_SECTION g_commandsLock;
static bool g_commandsLockReady = false;
static std::vector<Command> g_outbox;    // captured, not yet given a boundary
static std::vector<Command> g_record;    // probe run 1: made, in order
static size_t g_replayAt = 0;            // probe run 2: next recorded command
static size_t g_replayed = 0;
static volatile bool g_capturing = false;
static bool ApplyGameRules(const void* data, size_t size);
static bool g_replaying = false;
static volatile LONG g_applying = 0;
static DWORD g_applyThread = 0;
static int g_applyPlayer = 0; // the player of the command being made
static volatile bool g_applyRemote = false; // the command being made is another player's
static volatile LONG64 g_applyEpoch = 0;
static unsigned g_seq = 0;
static int g_session = COMMANDS_OFF; // in a session the player keeps the host's commands in g_record
static CommandAreaFn g_areaRule = nullptr;

static void Lock()
{
    if (!g_commandsLockReady) {
        InitializeCriticalSection(&g_commandsLock);
        g_commandsLockReady = true;
    }
    EnterCriticalSection(&g_commandsLock);
}

static void Unlock()
{
    LeaveCriticalSection(&g_commandsLock);
}

// Lock() for a scope: a failed push (core/guard.h) cannot leave the queue locked.
struct CommandsLockScope {
    CommandsLockScope() { Lock(); }
    ~CommandsLockScope() { Unlock(); }
};

bool Commands_Capturing()
{
    return g_capturing;
}

bool Commands_TakeLocal()
{
    return g_capturing && g_applying == 0 && !Determinism_InStepFrame();
}

unsigned long long Commands_ApplyEpoch()
{
    return (unsigned long long)InterlockedCompareExchange64(&g_applyEpoch, 0, 0);
}

bool Commands_TakeUi()
{
    return g_capturing && !(g_applying != 0 && GetCurrentThreadId() == g_applyThread);
}

static const char* KindName(int kind)
{
    switch (kind) {
    case LS_CMD_SEGMENT: return "segment";
    case LS_CMD_UPGRADE: return "upgrade";
    case LS_CMD_NET_DEMOLISH: return "network demolition";
    case LS_CMD_PLACE: return "placement";
    case LS_CMD_DEMOLISH: return "building demolition";
    case LS_CMD_SETTING: return "building setting";
    case LS_CMD_PLAN_START: return "start of planned construction";
    case LS_CMD_GROUP_TOOL: return "group tool";
    case LS_CMD_DIRECTION: return "factory connection direction";
    case LS_CMD_TERRAIN: return "terrain edits";
    case LS_CMD_MONEY: return "money";
    case LS_CMD_PURCHASE: return "purchase";
    case LS_CMD_ROUTE: return "route tool";
    case LS_CMD_GAME_CALL: return "window button";
    case LS_CMD_LINE_ACCEPT: return "line accept";
    case LS_CMD_VEHICLE_SWITCH: return "vehicle setting";
    case LS_CMD_BUILDING_SWITCH: return "window setting";
    case LS_CMD_BRUSH: return "brush";
    case LS_CMD_HOVER: return "road hover tool";
    case LS_CMD_JUNCTION: return "junction accept";
    case LS_CMD_GAME_RULES: return "game rules";
    case LS_CMD_CARGO_SETTINGS: return "cargo settings";
    case LS_CMD_SOURCE_SLOTS: return "source slots";
    case LS_CMD_RENAME: return "rename";
    case LS_CMD_LEDGER: return "ledger row";
    case LS_CMD_INVITE: return "immigrant invitation";
    case LS_CMD_SAVE: return "save";
    case LS_CMD_RESEARCH: return "research";
    case LS_CMD_LINK: return "link";
    case LS_CMD_OFFICE_SELECTION: return "office selection";
    case LS_CMD_NEW_CITY: return "new city";
    case LS_CMD_JOIN: return "join";
    case LS_CMD_NET_CANCEL: return "network cancel";
    case LS_CMD_WORKERS: return "workers without an office";
    case LS_CMD_CANCEL_PLANS: return "cancel all plans";
    case LS_CMD_SPAWN_BUILDING: return "building spawn";
    case LS_CMD_PLAN_REMOVE: return "plan removal";
    case LS_CMD_CAR_OWNERS: return "car owners";
    case LS_CMD_LINE_SPACING: return "line spacing";
    case LS_CMD_CRIME_CANCEL: return "crime cancel";
    case LS_CMD_CALL_HOME: return "call home";
    case LS_CMD_POPUP: return "popup";
    case LS_CMD_BUOY: return "buoy";
    case LS_CMD_GO_THERE: return "go there";
    case LS_CMD_LIGHTS: return "christmas lights";
    case LS_CMD_SQUARE: return "pavement square";
    case LS_CMD_BUILD_MODE: return "construction mode";
    }
    return "?";
}

// Terrain brushes and their cost come every frame a tool is used: not logged one by one.
static bool Frequent(int kind)
{
    return kind == LS_CMD_TERRAIN || kind == LS_CMD_MONEY || kind == LS_CMD_BRUSH || kind == LS_CMD_LEDGER;
}

void Commands_Submit(int kind, const void* data, size_t size)
{
    // RepublicMP (E3418, Sohn). The command list without the host ordering
    // it at the tick boundary is a desync, not multiplayer.
    if (!g_capturing) return;
    // Called from the game's tools and windows: nothing may leave it.
    bool ok = Guard_Call("capturing a command", [&] {
        Command c;
        c.kind = kind;
        c.afterTick = 0;
        c.seq = 0;
        c.data.assign((const BYTE*)data, (const BYTE*)data + size);
        CommandsLockScope lock;
        g_outbox.push_back(std::move(c));
    });
    if (!ok) Logf("republicmp  LOCKSTEP: %s NOT captured - it stays on this PC only", KindName(kind));
    else if (!Frequent(kind)) Logf("republicmp  LOCKSTEP: %s captured - made at the next boundary", KindName(kind));
}

// The republic's balances: world +0x588 rubles, +0x580 dollars (doubles).
#define MONEY_RUB_RVA (0x9D4F10 + 0x588)
#define MONEY_USD_RVA (0x9D4F10 + 0x580)
static double* Rubles() { return (double*)(g_exeBase + MONEY_RUB_RVA); }
static double* Dollars() { return (double*)(g_exeBase + MONEY_USD_RVA); }

static bool ApplyMoney(const LsMoneyDelta& d)
{
    *Rubles() += d.rubles;
    *Dollars() += d.dollars;
    return true;
}

static bool g_madeThisFrame = false;

void Commands_SetAreaRule(CommandAreaFn fn) { g_areaRule = fn; }

// The area rule over the points a command's message carries. Commands that
// name buildings check their targets in their module (Commands_BuildingInArea);
// planned construction keeps to the player's plans in world/networks.
static bool InArea(int kind, int player, const void* p, size_t n)
{
    auto ok = [&](const float* pos) { return g_areaRule(player, pos, true); };
    switch (kind) {
    case LS_CMD_PLACE:
    case LS_CMD_DEMOLISH:
        if (n == sizeof(BuildingMsg)) return ok(&((const BuildingMsg*)p)->x);
        break;
    case LS_CMD_SEGMENT:
        if (n == sizeof(NetSegmentMsg)) {
            const NetSegment& s = ((const NetSegmentMsg*)p)->seg;
            if (!ok(s.startNode) || !ok(s.endNode)) return false;
            for (int i = 0; i < s.pointCount && i < NET_MAX_POINTS; i++)
                if (!ok(s.points[i].pos)) return false;
        }
        break;
    case LS_CMD_UPGRADE:
        if (n == sizeof(NetUpgradeMsg)) {
            const NetUpgradeMsg& m = *(const NetUpgradeMsg*)p;
            for (int i = 0; i < m.pieceCount && i < NET_MAX_UPGRADE_PIECES; i++)
                if (!ok(m.pieces[i].segStart) || !ok(m.pieces[i].segEnd)) return false;
        }
        break;
    case LS_CMD_NET_DEMOLISH:
        if (n == sizeof(NetDemolishMsg)) {
            const NetDemolishMsg& m = *(const NetDemolishMsg*)p;
            if (!ok(m.cursor)) return false;
            if (m.hasRange && (!ok(m.rangeA) || !ok(m.rangeB))) return false;
            for (int i = 0; i < m.pieceCount && i < 8; i++)
                if (!ok(m.pieces[i].segStart) || !ok(m.pieces[i].segEnd)) return false;
        } else if (n == sizeof(NetDemolishHoverMsg)) {
            const NetDemolishHoverMsg& h = *(const NetDemolishHoverMsg*)p;
            if (!ok(h.m.cursor)) return false;
            if (h.hover.nodeKind && !ok(h.hover.node)) return false;
            if (h.hover.edgeKind && (!ok(h.hover.edgeA) || !ok(h.hover.edgeB))) return false;
        }
        break;
    case LS_CMD_NET_CANCEL:
        if (n == sizeof(CancelMsg)) {
            const CancelMsg& m = *(const CancelMsg*)p;
            if (!ok(m.ends[0]) || !ok(m.ends[1])) return false;
        }
        break;
    case LS_CMD_DIRECTION:
        if (n == sizeof(NetDirectionMsg)) {
            const NetPiece& piece = ((const NetDirectionMsg*)p)->piece;
            if (!ok(piece.segStart) || !ok(piece.segEnd)) return false;
        }
        break;
    case LS_CMD_GROUP_TOOL: // the box (turned by the camera's yaw): its four corners, either turn
        if (n == sizeof(NetGroupToolMsg)) {
            const NetGroupToolMsg& m = *(const NetGroupToolMsg*)p;
            const float* a = &m.box[3]; // the press point
            const float* b = &m.box[0]; // the moving corner
            if (!ok(a) || !ok(b) || !ok(m.cursor)) return false;
            float dx = b[0] - a[0], dz = b[2] - a[2];
            for (float s : { 1.0f, -1.0f }) {
                float c = cosf(m.yaw), sn = s * sinf(m.yaw);
                float u = dx * c + dz * sn, v = -dx * sn + dz * c; // into the box's frame
                float p1[3] = { a[0] + u * c, a[1], a[2] + u * sn };
                float p2[3] = { a[0] - v * sn, a[1], a[2] + v * c };
                if (!ok(p1) || !ok(p2)) return false;
            }
        }
        break;
    case LS_CMD_TERRAIN: // every edit of the frame
        if (n % sizeof(LsTerrainEdit) == 0) {
            const LsTerrainEdit* e = (const LsTerrainEdit*)p;
            for (size_t i = 0; i < n / sizeof(LsTerrainEdit); i++)
                if ((e[i].args.hasPos && !ok(e[i].args.pos)) || (e[i].args.hasPos2 && !ok(e[i].args.pos2))) return false;
        }
        break;
    case LS_CMD_BRUSH: // the brush's centre and points on its rim
        if (n == sizeof(BrushMsg)) {
            static const float kRim[16][2] = { { 1, 0 }, { 0.9239f, 0.3827f }, { 0.7071f, 0.7071f }, { 0.3827f, 0.9239f },
                                               { 0, 1 }, { -0.3827f, 0.9239f }, { -0.7071f, 0.7071f }, { -0.9239f, 0.3827f },
                                               { -1, 0 }, { -0.9239f, -0.3827f }, { -0.7071f, -0.7071f }, { -0.3827f, -0.9239f },
                                               { 0, -1 }, { 0.3827f, -0.9239f }, { 0.7071f, -0.7071f }, { 0.9239f, -0.3827f } };
            const BrushSample& s = ((const BrushMsg*)p)->sample;
            if (!ok(s.cursor)) return false;
            for (int k = 0; k < 16; k++) {
                float q[3] = { s.cursor[0] + s.brush * kRim[k][0], s.cursor[1], s.cursor[2] + s.brush * kRim[k][1] };
                if (!ok(q)) return false;
            }
        }
        break;
    case LS_CMD_SETTING: // found by its position on every PC, not through world_refs
        if (n == sizeof(BuildingSettingMsg)) return ok(((const BuildingSettingMsg*)p)->pos);
        break;
    case LS_CMD_JOIN:
        if (n == sizeof(JoinMsg)) return ok(((const JoinMsg*)p)->point);
        break;
    case LS_CMD_BUOY:
        if (n == sizeof(BuoyMsg)) return ok(((const BuoyMsg*)p)->point);
        break;
    case LS_CMD_LIGHTS:
        if (n == sizeof(LightsMsg)) {
            const LightsMsg& m = *(const LightsMsg*)p;
            if (m.mode == 0) return ok(m.start) && ok(m.end);
            for (unsigned i = 0; i < m.count && i < LIGHTS_MAX_REMOVED; i++)
                if (!ok(m.ends[i]) || !ok(&m.ends[i][3])) return false;
        }
        break;
    case LS_CMD_SQUARE:
        if (n == sizeof(SquareMsg)) {
            float corners[4][3];
            SquareTool_Corners(*(const SquareMsg*)p, corners);
            for (const float* c : corners)
                if (!ok(c)) return false;
        }
        break;
    case LS_CMD_HOVER: // a sign, waypoint, rail signal or junction: where the sender clicked
        if (n == sizeof(HoverMsg)) return ok((const float*)((const HoverMsg*)p)->pick.cursor);
        break;
    }
    return true;
}

bool Commands_ChecksHere()
{
    return !g_capturing && g_areaRule && g_areaRule(COMMANDS_LOCAL_PLAYER, nullptr, false);
}

bool Commands_RefusedHere(int kind, const void* data, size_t size)
{
    return Commands_ChecksHere() && !InArea(kind, COMMANDS_LOCAL_PLAYER, data, size);
}

bool Commands_PointRefusedHere(const float pos[3])
{
    return Commands_ChecksHere() && !g_areaRule(COMMANDS_LOCAL_PLAYER, pos, true);
}

bool Commands_PointInArea(const float pos[3], bool notify)
{
    if (!g_areaRule || g_applying == 0 || GetCurrentThreadId() != g_applyThread) return true;
    return g_areaRule(g_applyPlayer, pos, notify);
}

bool Commands_BuildingInArea(unsigned long long building)
{
    if (!g_areaRule || !building || g_applying == 0 || GetCurrentThreadId() != g_applyThread) return true;
    float pos[3];
    __try {
        memcpy(pos, (const void*)(building + GAME_BUILDING_POS_OFFSET), sizeof(pos));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return Commands_PointInArea(pos);
}

#define VEHICLE_PARENT 0x398     // a wagon's or trailer's head vehicle
#define VEHICLE_REGISTERED 0x4F8 // the building it is registered at (0 while a depot vehicle is out)
#define VEHICLE_IN 0x4F0         // the building it is in now
#define VEHICLE_DEPOT 0x600      // the depot it is on its way to (a realistic purchase waiting at a customs house)
#define VEHICLE_POS 0xDE0        // float x, y, z

// The building is still in the world's list (world +11b08). Caller wraps in __try.
static bool BuildingListed(unsigned long long building)
{
    BYTE* world = g_exeBase + 0x9D4F10;
    unsigned long long* it = *(unsigned long long**)(world + 0x11B08);
    unsigned long long* end = *(unsigned long long**)(world + 0x11B10);
    if (!it || end < it || end - it > 1000000) return false;
    for (; it < end; ++it)
        if (*it == building) return true;
    return false;
}

// shortcut: the game keeps no owner of a vehicle, so it counts as on the side of
// the depot it is going to, the building it is registered at or in, else of
// where it is; a stored owner would have to be recorded at purchase and saved.
bool Commands_VehicleSide(unsigned long long vehicle, unsigned long long* building, float pos[3])
{
    *building = 0;
    if (!vehicle) return false;
    __try {
        for (int i = 0; i < 16 && *(unsigned long long*)(vehicle + VEHICLE_PARENT); i++)
            vehicle = *(unsigned long long*)(vehicle + VEHICLE_PARENT);
        unsigned long long b = *(unsigned long long*)(vehicle + VEHICLE_DEPOT);
        if (b && !BuildingListed(b)) b = 0; // a depot demolished since: not read
        if (!b) b = *(unsigned long long*)(vehicle + VEHICLE_REGISTERED);
        if (!b) b = *(unsigned long long*)(vehicle + VEHICLE_IN);
        memcpy(pos, (const void*)(vehicle + VEHICLE_POS), sizeof(float) * 3);
        *building = b;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool Commands_VehicleInArea(unsigned long long vehicle)
{
    if (!g_areaRule || !vehicle || g_applying == 0 || GetCurrentThreadId() != g_applyThread) return true;
    unsigned long long building;
    float pos[3];
    if (!Commands_VehicleSide(vehicle, &building, pos)) return true;
    return building ? Commands_BuildingInArea(building) : Commands_PointInArea(pos);
}

#define LINE_STATIONS 0x210 // a line's stations: begin, end (buildings, or a border connection)
#define WORLD_BORDER_A 0x11AF8 // world: the western and the Soviet border connection
#define WORLD_BORDER_B 0x11B00

bool Commands_StationInArea(unsigned long long station)
{
    if (!station) return true;
    __try {
        BYTE* world = g_exeBase + 0x9D4F10;
        if (station == *(unsigned long long*)(world + WORLD_BORDER_A) || station == *(unsigned long long*)(world + WORLD_BORDER_B))
            return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return Commands_BuildingInArea(station);
}

bool Commands_LineInArea(unsigned long long line)
{
    if (!g_areaRule || !line || g_applying == 0 || GetCurrentThreadId() != g_applyThread) return true;
    __try {
        unsigned long long begin = *(unsigned long long*)(line + LINE_STATIONS), end = *(unsigned long long*)(line + LINE_STATIONS + 8);
        if (!begin || end < begin || (end - begin) / 8 > 4096) return true;
        for (unsigned long long p = begin; p < end; p += 8)
            if (!Commands_StationInArea(*(unsigned long long*)p)) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return true;
}

bool Commands_PerPlayer()
{
    return g_areaRule && g_areaRule(0, nullptr, false);
}

int Commands_ApplyingPlayer()
{
    return g_applying != 0 && GetCurrentThreadId() == g_applyThread ? g_applyPlayer : -1;
}

static CommandGlobalFn g_globalRule = nullptr;

void Commands_SetGlobalRule(CommandGlobalFn fn) { g_globalRule = fn; }

static CommandBorderFn g_borderRule = nullptr;
void Commands_SetBorderRule(CommandBorderFn fn) { g_borderRule = fn; }
bool Commands_BorderAllows(unsigned long long type, const float corners[4][3])
{
    return g_borderRule && g_borderRule(type, corners);
}

static CommandSpawnedFn g_spawned = nullptr;
void Commands_SetSpawned(CommandSpawnedFn fn) { g_spawned = fn; }

// A building finished at once (BUILDING_PICK_FINISHED), with the game's "no charges"
// flag on while it is made: nobody pays.
#define WORLD_FREE_BUILDING 0x578
#define BORDER_CHECK_OFF_RVA 0xA54BC0
static bool ApplySpawnBuilding(const BuildingMsg& m)
{
    int* free = (int*)(g_exeBase + 0x9D4F10 + WORLD_FREE_BUILDING);
    int wasFree = *free;
    double rub = *Rubles(), usd = *Dollars();
    // A $BORDER_BUILDING goes only on the map's border (2caf91 -> 411270, error 5,
    // 0x4E1F) unless this byte is set, which also lifts 410d50 while it is.
    unsigned char* anywhere = g_exeBase + BORDER_CHECK_OFF_RVA;
    unsigned char wasAnywhere = *anywhere;
    *free = 1;
    *anywhere = 1;
    RemoteSounds_Silence(true);
    bool ok = Buildings_ApplyPlacementCommand(m);
    RemoteSounds_Silence(false);
    *anywhere = wasAnywhere;
    *free = wasFree;
    if (*Rubles() != rub || *Dollars() != usd)
        Logf("republicmp  LOCKSTEP: a spawned building would have cost %.2f rubles %.2f dollars - put back", rub - *Rubles(), usd - *Dollars());
    *Rubles() = rub;
    *Dollars() = usd;
    return ok;
}

bool Commands_SpawnBuildingNow(const BuildingMsg& m)
{
    bool ok = ApplySpawnBuilding(m);
    if (g_spawned) g_spawned(ok);
    return ok;
}

bool Commands_GlobalAllowed()
{
    int player = Commands_ApplyingPlayer();
    return !g_globalRule || player < 0 || g_globalRule(player);
}

// The module of a command's kind makes it; false: not made.
static bool ApplyKind(const Command& c)
{
    const void* p = c.data.data();
    size_t n = c.data.size();
    bool ok = false;
    switch (c.kind) {
    case LS_CMD_SEGMENT:
        if (n == sizeof(NetSegmentMsg)) ok = Networks_ApplyCommandSegment(*(const NetSegmentMsg*)p);
        break;
    case LS_CMD_UPGRADE:
        if (n == sizeof(NetUpgradeMsg)) ok = Networks_ApplyCommandUpgrade(*(const NetUpgradeMsg*)p);
        break;
    case LS_CMD_NET_DEMOLISH:
        if (n == sizeof(NetDemolishMsg)) {
            ok = Networks_ApplyCommandDemolish(*(const NetDemolishMsg*)p);
            LocalBuilds_DemolishMade(*(const NetDemolishMsg*)p);
        } else if (n == sizeof(NetDemolishHoverMsg)) {
            const NetDemolishHoverMsg& h = *(const NetDemolishHoverMsg*)p;
            ok = Networks_ApplyCommandDemolishHover(h);
            LocalBuilds_DemolishMade(h.m, &h.hover);
        }
        break;
    case LS_CMD_PLACE:
        if (n == sizeof(BuildingMsg)) ok = Buildings_ApplyPlacementCommand(*(const BuildingMsg*)p);
        break;
    case LS_CMD_DEMOLISH:
        if (n == sizeof(BuildingMsg)) ok = Buildings_ApplyDemolishCommand(*(const BuildingMsg*)p);
        break;
    case LS_CMD_SPAWN_BUILDING:
        if (n == sizeof(BuildingMsg) && c.player == 0) ok = ApplySpawnBuilding(*(const BuildingMsg*)p);
        if (g_spawned) g_spawned(ok);
        break;
    case LS_CMD_SETTING:
        if (n == sizeof(BuildingSettingMsg)) ok = BuildingSettings_ApplyCommand(*(const BuildingSettingMsg*)p);
        break;
    case LS_CMD_PLAN_START:
        if (n == sizeof(NetPlanStartMsg)) ok = Networks_ApplyCommandPlanStart(*(const NetPlanStartMsg*)p);
        break;
    case LS_CMD_GROUP_TOOL:
        if (n == sizeof(NetGroupToolMsg)) ok = Networks_ApplyCommandGroupTool(*(const NetGroupToolMsg*)p);
        break;
    case LS_CMD_DIRECTION:
        if (n == sizeof(NetDirectionMsg)) ok = Networks_ApplyCommandDirection(*(const NetDirectionMsg*)p);
        break;
    case LS_CMD_TERRAIN:
        ok = TerrainHook_ApplyCommand(p, n);
        break;
    case LS_CMD_MONEY:
        if (n == sizeof(LsMoneyDelta)) ok = ApplyMoney(*(const LsMoneyDelta*)p);
        break;
    case LS_CMD_PURCHASE:
        ok = Purchase_ApplyCommand(p, n);
        break;
    case LS_CMD_ROUTE:
        ok = RouteTools_ApplyCommand(p, n);
        break;
    case LS_CMD_GAME_CALL:
        ok = GameCalls_ApplyCommand(p, n);
        break;
    case LS_CMD_LINE_ACCEPT:
        ok = LineAccept_ApplyCommand(p, n);
        break;
    case LS_CMD_VEHICLE_SWITCH:
        ok = VehicleSwitches_ApplyCommand(p, n);
        break;
    case LS_CMD_BUILDING_SWITCH:
        ok = BuildingSwitches_ApplyCommand(p, n);
        break;
    case LS_CMD_BRUSH:
        ok = BrushTools_ApplyCommand(p, n);
        break;
    case LS_CMD_HOVER:
        ok = HoverTools_ApplyCommand(p, n);
        break;
    case LS_CMD_JUNCTION:
        ok = JunctionAccept_ApplyCommand(p, n);
        break;
    case LS_CMD_GAME_RULES:
        if (!Commands_GlobalAllowed()) Logf("republicmp  LOCKSTEP:   the game rules are the host's in this mode - not changed");
        else ok = ApplyGameRules(p, n);
        break;
    case LS_CMD_CARGO_SETTINGS:
        ok = CargoSettings_ApplyCommand(p, n);
        break;
    case LS_CMD_SOURCE_SLOTS:
        ok = SourceSlots_ApplyCommand(p, n);
        break;
    case LS_CMD_RENAME:
        ok = Rename_ApplyCommand(p, n);
        break;
    case LS_CMD_LEDGER:
        ok = BrushTools_ApplyLedger(p, n);
        break;
    case LS_CMD_INVITE:
        ok = ImmigrantInvite_ApplyCommand(p, n);
        break;
    case LS_CMD_SAVE:
        ok = SharedSave_ApplyCommand(p, n);
        break;
    case LS_CMD_RESEARCH:
        ok = Research_ApplyCommand(p, n);
        break;
    case LS_CMD_LINK:
        ok = LinkTools_ApplyCommand(p, n);
        break;
    case LS_CMD_OFFICE_SELECTION:
        ok = OfficeSelection_ApplyCommand(p, n);
        break;
    case LS_CMD_NEW_CITY:
        ok = NewCity_ApplyCommand(p, n);
        break;
    case LS_CMD_JOIN:
        ok = JoinTool_ApplyCommand(p, n);
        break;
    case LS_CMD_NET_CANCEL:
        ok = CancelTool_ApplyCommand(p, n);
        break;
    case LS_CMD_WORKERS:
        ok = WorldWorkers_ApplyCommand(p, n);
        break;
    case LS_CMD_CANCEL_PLANS:
        ok = CancelPlans_ApplyCommand(p, n);
        break;
    case LS_CMD_PLAN_REMOVE:
        ok = PlanRemove_ApplyCommand(p, n);
        break;
    case LS_CMD_CAR_OWNERS:
        ok = CarOwners_ApplyCommand(p, n);
        break;
    case LS_CMD_LINE_SPACING:
        ok = LineSpacing_ApplyCommand(p, n);
        break;
    case LS_CMD_CRIME_CANCEL:
        ok = CrimeCancel_ApplyCommand(p, n);
        break;
    case LS_CMD_CALL_HOME:
        ok = CallHome_ApplyCommand(p, n);
        break;
    case LS_CMD_POPUP:
        ok = Popups_ApplyCommand(p, n);
        break;
    case LS_CMD_BUOY:
        ok = BuoyTool_ApplyCommand(p, n);
        break;
    case LS_CMD_GO_THERE:
        ok = GoThere_ApplyCommand(p, n);
        break;
    case LS_CMD_LIGHTS:
        ok = ChristmasLights_ApplyCommand(p, n);
        break;
    case LS_CMD_SQUARE:
        ok = SquareTool_ApplyCommand(p, n);
        break;
    case LS_CMD_BUILD_MODE:
        ok = BuildModes_ApplyCommand(c.player, p, n);
        break;
    }
    return ok;
}

// Makes one command through the module of its kind. A payload whose size does
// not match the kind's message refuses it.
static void Apply(const Command& c)
{
    if (g_areaRule && !InArea(c.kind, c.player, c.data.data(), c.data.size())) {
        Logf("republicmp  LOCKSTEP: #%u %s of player %d after tick %llu - refused: outside their area", c.seq,
             KindName(c.kind), c.player, c.afterTick);
        return;
    }
    std::vector<StorageRecord> recipes;
    bool recipeAudit = false;
    if (StateHash_GetStorageTrace())
        Guard_Call("the construction recipe report", [&] { recipeAudit = StateHash_RecipeRecords(recipes); });
    unsigned recipeLoad = Determinism_WorldLoads();
    bool ok = false;
    g_applyThread = GetCurrentThreadId();
    g_applyPlayer = c.player;
    g_applyRemote = c.player != LockstepNet_SelfNumber();
    if (g_applyRemote) RemoteSounds_CommandBegin();
    InterlockedIncrement(&g_applying);
    InterlockedIncrement64(&g_applyEpoch);
    Wallets_CommandBegin(c.player);
    BuildModes_CommandBegin(c.player);
    double rubBefore = *Rubles(), usdBefore = *Dollars();
    InputBlock_BeginNeutral();
    // A drag the local player has under way would share the tool manager with
    // the replay and change what it builds on this PC only: it is cancelled.
    if (c.kind == LS_CMD_SEGMENT || c.kind == LS_CMD_UPGRADE || c.kind == LS_CMD_NET_DEMOLISH || c.kind == LS_CMD_PLACE ||
        c.kind == LS_CMD_SQUARE)
        Networks_CancelLocalDrag();
    // This PC's open menus or a window under its cursor (+fbc/+fbe/+fc3) make
    // the tools return at once: a command is made with them closed, as the
    // replays are (menus_closed.h). A road from the host was refused on the
    // player only because its cursor was over a window.
    MenusClosedScope menus;
    // A module that throws (core/guard.h) leaves this command not made here,
    // logged, instead of ending the game.
    Determinism_CommandBegin(c.seq);
    Guard_Call(KindName(c.kind), [&] { ok = ApplyKind(c); });
    Determinism_CommandEnd();
    // What the local mouse hovered may be rebuilt or gone now, and a replay
    // leaves the sender's hover set: the local tool, later in this frame,
    // read a node whose edges were gone (2bda49) or a road edge without the
    // rail one (4fe681). Nothing hovered until the game picks again.
    Networks_ClearHover();
    InputBlock_EndNeutral();
    if (recipeAudit && recipeLoad == Determinism_WorldLoads())
        Guard_Call("the construction recipe report", [&] { StateHash_LogRecipeChanges(recipes, c.seq, c.afterTick); });
    InterlockedDecrement(&g_applying);
    if (g_applyRemote) RemoteSounds_CommandEnd();
    g_applyRemote = false;
    g_madeThisFrame = true;
    // What the command paid or got: compared between the PCs' logs when the money differs.
    double paidRub = *Rubles() - rubBefore, paidUsd = *Dollars() - usdBefore;
    Wallets_CommandEnd();
    BuildModes_CommandEnd();
    char money[96] = "";
    if (paidRub != 0.0 || paidUsd != 0.0)
        sprintf_s(money, " (money %+.4f rubles %+.4f dollars)", paidRub, paidUsd);
    if (!Frequent(c.kind) || !ok || (money[0] && Determinism_GetMoneyTrace()))
        Logf("republicmp  LOCKSTEP: #%u %s after tick %llu - %s%s", c.seq, KindName(c.kind), c.afterTick,
             ok ? "made" : "not made", money);
}

bool Commands_Due(unsigned long long completed)
{
    if (g_session == COMMANDS_PLAYER && g_replayAt < g_record.size() && g_record[g_replayAt].afterTick <= completed)
        return true;
    if (g_replaying) return g_replayAt < g_record.size() && g_record[g_replayAt].afterTick <= completed;
    Lock();
    bool any = !g_outbox.empty();
    Unlock();
    return any;
}

bool Commands_Applying()
{
    return InterlockedCompareExchange(&g_applying, 0, 0) != 0;
}

bool Commands_ApplyingRemote()
{
    return g_applyRemote && Commands_Applying() && GetCurrentThreadId() == g_applyThread;
}

// Player: its own actions go to the host; what the host made after this
// tick is made here now (TCP brought it before the grant of the next tick).
static void PlayerBoundary(unsigned long long completed)
{
    std::vector<Command> mine;
    Lock();
    mine.swap(g_outbox);
    Unlock();
    for (const Command& c : mine) LockstepNet_SendCommand(c.kind, 0, 0, c.data.data(), c.data.size());
    while (g_replayAt < g_record.size() && g_record[g_replayAt].afterTick <= completed) {
        const Command& c = g_record[g_replayAt++];
        if (c.afterTick < completed)
            Logf("republicmp  LOCKSTEP: #%u was made by the host after tick %llu, here after %llu - the games will differ",
                 c.seq, c.afterTick, completed);
        Apply(c);
    }
    if (g_replayAt > 256) {
        g_record.erase(g_record.begin(), g_record.begin() + g_replayAt);
        g_replayAt = 0;
    }
}

void Commands_Boundary(unsigned long long completed)
{
    // Loading the shared world enables capture before the lockstep sockets
    // are ready. Applying here earlier loses commands: the host changes its
    // world and broadcasts to nobody; the player drains requests into no link.
    // Keep both queues intact until the transport has finished its setup.
    if ((g_session == COMMANDS_HOST || g_session == COMMANDS_PLAYER) && !LockstepNet_CommandsReady()) return;
    if (g_session == COMMANDS_HOST || g_session == COMMANDS_PLAYER) Rename_LogCities();
    if (g_session == COMMANDS_PLAYER) {
        PlayerBoundary(completed);
        return;
    }
    if (g_replaying) {
        while (g_replayAt < g_record.size() && g_record[g_replayAt].afterTick <= completed) {
            const Command& c = g_record[g_replayAt++];
            if (c.afterTick < completed)
                Logf("republicmp  LOCKSTEP: #%u was recorded after tick %llu, made after %llu - the runs cannot match",
                     c.seq, c.afterTick, completed);
            Apply(c);
            g_replayed++;
        }
        return;
    }
    if (g_session == COMMANDS_HOST) Rename_ShareNewCities();
    std::vector<Command> due;
    Lock();
    due.swap(g_outbox);
    Unlock();
    for (Command& c : due) {
        c.afterTick = completed;
        c.seq = ++g_seq;
        Apply(c);
        if (g_session == COMMANDS_HOST) LockstepNet_SendCommand(c.kind, c.afterTick, c.player, c.data.data(), c.data.size());
        else if (g_capturing) g_record.push_back(std::move(c));
    }
}

// ---------------------------------------------------------------------------
// Changes found by comparing with a record
// ---------------------------------------------------------------------------

static bool g_haveFrameRecord = false;
static double g_frameRubles = 0, g_frameDollars = 0;
static bool g_inLateRecord = false;
static double g_lateRubles = 0, g_lateDollars = 0;

// Balances changed since (rub, usd): the new ones become a command, the
// recorded ones are written back exactly (not by subtracting).
static void TakeMoney(double rub, double usd, const char* where)
{
    double nowRub = *Rubles(), nowUsd = *Dollars();
    if (nowRub == rub && nowUsd == usd) return;
    LsMoneyDelta d = { nowRub - rub, nowUsd - usd };
    *Rubles() = rub;
    *Dollars() = usd;
    static int s_logged = 0;
    if (s_logged < 40) {
        ++s_logged;
        Logf("republicmp  LOCKSTEP: money changed during %s on this PC (%+.4f rubles, %+.4f dollars) - made a command", where,
             d.rubles, d.dollars);
    }
    Commands_Submit(LS_CMD_MONEY, &d, sizeof(d));
}

// Selected tools whose actions are not commands yet act on one PC only; they
// are put away (world +d428 = 0, as the game's own cancel does). Prefixes of
// the build menu names; the carried route tools (route_tools.cpp) by full name.
static const char* const kCarriedTools[] = { "vehicle_addstop", "line_addstop", "line_addstop_copy", "line_addstop_copy2",
                                              "vehicle_godepo", "vehicle_godepo_batch",
                                              // the second-road drags (a road sync category; second_road_cancel stays put away)
                                              "second_road_airplane", "second_road_basicroad", "second_road_l2",
                                              "second_road_l3", "second_road_l4" };
// Copying the weights and the fire station's dispatch are carried by
// link_tools.cpp; put away only when their routines differ.
static const char* const kUncarriedTools[] = {
    "vehicle_", "worker_relocate", "building_addsource", "line_addstop", "second_road", "water_square",
    "bojka", "christmas_", "join_hub", "terrain_go_there", "building_addstation", "building_copyweights",
    "firestation_addfire",
};
// The single-tree tools and the tree demolition brush (brush_tools.cpp): put away only when their routine differs.
static const char* const kTreeTools[] = { "tree_", "palma", "jungle_", "fjord_", "afgan_", "afghan_", "tallkriak", "leafkriak" };

static void PutAwayUncarriedTool()
{
    __int64* tool = (__int64*)(g_exeBase + 0x9D4F10 + 0xD428);
    char name[64] = {};
    __try {
        if (!*tool || !*(char**)tool) return;
        const char* s = *(const char**)tool;
        for (int i = 0; i < 63 && s[i]; i++) name[i] = s[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    for (const char* carried : kCarriedTools)
        if (strcmp(name, carried) == 0 && (RouteTools_Ready() || strncmp(name, "second_road", 11) == 0)) return;
    if (strcmp(name, "building_addsource") == 0 && SourceSlots_Ready()) return; // source_slots.cpp
    if ((strcmp(name, "building_addstation") == 0 || strcmp(name, "building_addstation_dist") == 0) && LinkTools_Ready())
        return; // link_tools.cpp
    if ((strcmp(name, "building_copyweights") == 0 || strcmp(name, "firestation_addfire") == 0) && LinkTools_PickReady())
        return; // link_tools.cpp
    if (strcmp(name, "join_hub_all") == 0 && JoinTool_Ready()) return; // join_tool.cpp
    if (strcmp(name, "vehicle_tovehicle") == 0 || strcmp(name, "vehicle_replace") == 0) return; // their clicks are game calls (game_calls.cpp)
    if (strncmp(name, "worker_relocate", 15) == 0 && LinkTools_RelocateReady()) return; // link_tools.cpp
    if ((strcmp(name, "vehicle_replace_auto") == 0 || strcmp(name, "vehicle_addstop_copy") == 0 ||
         strcmp(name, "vehicle_sendconstruction") == 0) && LinkTools_VehicleReady())
        return; // link_tools.cpp
    if (strncmp(name, "bojka", 5) == 0 && BuoyTool_Ready()) return; // buoy_tool.cpp
    if (strcmp(name, "terrain_go_there") == 0 && GoThere_Ready()) return; // go_there.cpp
    if (strncmp(name, "christmas_", 10) == 0 && ChristmasLights_Ready()) return; // christmas_lights.cpp
    size_t nameLength = strlen(name);
    if (nameLength > 7 && strcmp(name + nameLength - 7, "_square") == 0 && SquareTool_Ready()) return; // square_tool.cpp
    bool trees = false;
    for (const char* prefix : kTreeTools) trees |= strncmp(name, prefix, strlen(prefix)) == 0;
    if ((trees && !BrushTools_Ready(3)) || (strcmp(name, "terrain_demolish_trees") == 0 && !BrushTools_Ready(1))) {
        *tool = 0;
        Logf("republicmp  LOCKSTEP: tool [%s] put away - the game's routine for it differs", name);
        return;
    }
    for (const char* prefix : kUncarriedTools) {
        if (strncmp(name, prefix, strlen(prefix)) != 0) continue;
        *tool = 0;
        Logf("republicmp  LOCKSTEP: tool [%s] put away - its actions are not shared in lockstep yet", name);
        return;
    }
}

// The game rules (the game menu's settings page: realistic mode, traffic,
// research and the rest): the page edits a copy (world +c81c..+c852) and
// "Save settings", closing the window or the "Change settings?" popup apply it
// with 337170(world) (game_calls takes those three calls). In a session the
// copy travels as a command and every PC writes it and calls 337170, so the
// rules and their side effects (road lanes, citizens, buildings) change alike.
// The popup's save-and-reload for a critical rule (+14d3c..+14d3f, made by the
// next Late body) is not made in a session. The cheats switch (world +9: cheat
// page, cheat keys and window actions on one PC) stays off. (by SoylentGreed)
struct GameRulePair {
    unsigned copy, rule, bytes;
};
static const GameRulePair kGameRules[] = {
    { 0xC81C, 0x5B0, 4 }, { 0xC844, 0x5B4, 4 }, { 0xC848, 0x5B8, 4 }, { 0xC84C, 0x5BC, 4 }, { 0xC820, 0x5C0, 4 },
    { 0xC824, 0x5C4, 4 }, { 0xC828, 0x5C8, 4 }, { 0xC82C, 0x5CC, 4 }, { 0xC830, 0x5D0, 4 }, { 0xC834, 0x5D4, 4 },
    { 0xC838, 0x5D8, 4 }, { 0xC83C, 0x5DC, 4 }, { 0xC840, 0x5E0, 4 }, { 0xC850, 0x14451, 1 }, { 0xC851, 0x14453, 1 },
    { 0xC852, 0x14452, 1 },
};
#define CHEATS_OFFSET 0x9

#define RELOAD_FLAGS_OFFSET 0x14D3C // 4 bytes: a critical rule's save and reload
#define GAME_RULES_APPLY_RVA 0x337170

#pragma pack(push, 1)
struct LsGameRules {
    BYTE copy[sizeof(kGameRules) / sizeof(kGameRules[0])][4];
};
#pragma pack(pop)

void Commands_GuardSettings()
{
    static int s_logged = 0;
    if (!g_capturing) return;
    BYTE* world = g_exeBase + 0x9D4F10;
    bool reload = false, cheats = false;
    __try {
        if (world[CHEATS_OFFSET]) {
            world[CHEATS_OFFSET] = 0;
            cheats = true;
        }
        if (*(unsigned*)(world + RELOAD_FLAGS_OFFSET)) {
            *(unsigned*)(world + RELOAD_FLAGS_OFFSET) = 0;
            reload = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    // The reload popup accepted: not this PC's own save and reload, but the page's rules as a
    // command; applying them, the host re-syncs everyone (ApplyGameRules).
    if (reload) Commands_CaptureGameRules();
    if ((reload || cheats) && s_logged < 8) {
        ++s_logged;
        Logf("republicmp  LOCKSTEP: %s", reload ? "a rule that needs a reload: made as a command, then a re-sync of everyone"
                                                : "cheats stay off in a session");
    }
}

// game_calls, the interface's 337170 call: the page's copy as a command.
void Commands_CaptureGameRules()
{
    LsGameRules m = {};
    BYTE* world = g_exeBase + 0x9D4F10;
    bool changed = false;
    __try {
        for (size_t i = 0; i < sizeof(kGameRules) / sizeof(kGameRules[0]); i++) {
            memcpy(m.copy[i], world + kGameRules[i].copy, kGameRules[i].bytes);
            if (memcmp(world + kGameRules[i].copy, world + kGameRules[i].rule, kGameRules[i].bytes) == 0) continue;
            changed = true;
            unsigned from = 0, to = 0;
            memcpy(&from, world + kGameRules[i].rule, kGameRules[i].bytes);
            memcpy(&to, world + kGameRules[i].copy, kGameRules[i].bytes);
            Logf("republicmp  LOCKSTEP:   rule +0x%X changes from %u to %u", kGameRules[i].rule, from, to);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  LOCKSTEP: the game rules could not be read - not changed");
        return;
    }
    if (changed) Commands_Submit(LS_CMD_GAME_RULES, &m, sizeof(m)); // the page closed with nothing changed: nothing to make
}

static bool ApplyGameRules(const void* data, size_t size)
{
    if (size != sizeof(LsGameRules)) return false;
    const LsGameRules& m = *(const LsGameRules*)data;
    BYTE* world = g_exeBase + 0x9D4F10;
    bool critical = false;
    __try {
        // The page's gate (336e50, 337450, 7fe6d0): energy, water, waste on or off, seasons,
        // pollution and day/night take effect only through a save and a reload.
        auto copy = [&](int i) { int v = 0; memcpy(&v, m.copy[i], 4); return v; };
        auto live = [&](unsigned at) { return *(int*)(world + at); };
        critical = live(0x5B0) != copy(0) || live(0x5B4) != copy(1) || live(0x5C0) != copy(4) ||
                   live(0x5D4) != copy(9) || live(0x5C4) != copy(5) ||
                   (live(0x5B8) != copy(2) && (live(0x5B8) == 0 || copy(2) == 0));
        for (size_t i = 0; i < sizeof(kGameRules) / sizeof(kGameRules[0]); i++)
            memcpy(world + kGameRules[i].copy, m.copy[i], kGameRules[i].bytes);
        ((void (*)(void*))(g_exeBase + GAME_RULES_APPLY_RVA))(world);
        *(unsigned*)(world + RELOAD_FLAGS_OFFSET) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // Every PC has the new rules; the host saves them and everyone loads that save, as the
    // game's own reload would (the load rebuilds every building's connections).
    if (critical) {
        Logf("republicmp  LOCKSTEP:   a rule that needs a reload changed - %s", SaveSync_IsHostRole() ? "re-syncing everyone" : "the host re-syncs");
        // shortcut: the game saves and reloads at once; here a tick or two may still run (the same
        // on every PC) before the host's pause stops them, with nothing rebuilt yet.
        bool paused = true;
        int speed = 0;
        Determinism_SessionSpeed(&paused, &speed);
        if (SaveSync_IsHostRole() && !paused) Determinism_RequestPauseToggle();
        SaveSync_RequestResync();
    }
    return true;
}

void Commands_Collect()
{
    WorldWorkers_Collect(); // also releases the session's value outside a session
    BuildModes_Collect();
    if (!g_capturing) return;
    Commands_GuardSettings();
    PutAwayUncarriedTool();
    TerrainHook_LockstepFlush();
    if (!g_haveFrameRecord) return;
    BuildingSettings_LockstepDetect();
    TakeMoney(g_frameRubles, g_frameDollars, "the interface");
}

void Commands_AfterFrame(bool tickRan)
{
    if (!g_capturing) {
        g_haveFrameRecord = false;
        return;
    }
    // The settings record is only taken again when something could have
    // changed them for every PC: a tick, a command, or the first frame.
    if (tickRan || g_madeThisFrame || !g_haveFrameRecord) BuildingSettings_LockstepBaseline();
    g_frameRubles = *Rubles();
    g_frameDollars = *Dollars();
    g_haveFrameRecord = true;
    g_madeThisFrame = false;
}

void Commands_RecordMoney()
{
    if (!g_capturing || !g_haveFrameRecord) return;
    g_frameRubles = *Rubles();
    g_frameDollars = *Dollars();
}

void Commands_CheckMoney(const char* where)
{
    if (!g_capturing || !g_haveFrameRecord) return;
    TakeMoney(g_frameRubles, g_frameDollars, where);
}

void Commands_LateBegin()
{
    g_inLateRecord = g_capturing;
    if (!g_inLateRecord) return;
    Commands_CheckMoney("the held frame before Late");
    BuildingSettings_LockstepBaseline();
    Networks_LockstepDirectionsBaseline();
    g_lateRubles = *Rubles();
    g_lateDollars = *Dollars();
}

void Commands_LateEnd()
{
    if (!g_inLateRecord) return;
    g_inLateRecord = false;
    BuildingSettings_LockstepDetect();
    Networks_LockstepDirectionsDetect();
    TakeMoney(g_lateRubles, g_lateDollars, "a held frame's Late (tools)");
}

void Commands_SetSession(int role)
{
    g_haveFrameRecord = false;
    Lock();
    g_outbox.clear();
    Unlock();
    g_record.clear();
    g_replayAt = g_replayed = 0;
    g_replaying = false;
    g_seq = 0;
    g_session = role;
    Rename_ResetCities();
    g_capturing = role != COMMANDS_OFF;
    Logf("republicmp  LOCKSTEP: commands %s", role == COMMANDS_HOST ? "ordered here (host)"
                                              : role == COMMANDS_PLAYER ? "ordered by the host" : "off");
}

void Commands_SubmitRemote(int kind, int player, const void* data, size_t size)
{
    if (g_session != COMMANDS_HOST) return;
    Command c;
    c.kind = kind;
    c.player = player;
    c.afterTick = 0;
    c.seq = 0;
    c.data.assign((const BYTE*)data, (const BYTE*)data + size);
    {
        CommandsLockScope lock;
        g_outbox.push_back(std::move(c));
    }
    Logf("republicmp  LOCKSTEP: %s from the player - made at the next boundary", KindName(kind));
}

void Commands_Receive(int kind, int player, unsigned long long afterTick, const void* data, size_t size)
{
    if (g_session != COMMANDS_PLAYER) return;
    Command c;
    c.kind = kind;
    c.player = player;
    c.afterTick = afterTick;
    c.seq = ++g_seq;
    c.data.assign((const BYTE*)data, (const BYTE*)data + size);
    g_record.push_back(std::move(c));
}

void Commands_ProbeRun(int run, bool record)
{
    g_haveFrameRecord = false;
    Lock();
    g_outbox.clear();
    Unlock();
    g_replaying = record && run == 1;
    g_capturing = record && run == 0;
    if (run == 0) {
        g_record.clear();
        g_seq = 0;
    }
    g_replayAt = 0;
    g_replayed = 0;
    if (record)
        Logf("republicmp  LOCKSTEP: probe run %d - %s", run + 1,
             run == 0 ? "player actions become commands and are recorded"
                      : "the recorded commands are made again after the same ticks");
}

void Commands_ProbeEnd(size_t* recorded, size_t* replayed)
{
    g_capturing = false;
    g_replaying = false;
    if (recorded) *recorded = g_record.size();
    if (replayed) *replayed = g_replayed;
    Lock();
    g_outbox.clear();
    Unlock();
    std::vector<Command>().swap(g_record);
    g_replayAt = g_replayed = 0;
}
