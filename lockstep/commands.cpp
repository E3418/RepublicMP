// SPDX-License-Identifier: GPL-3.0-only
// See commands.h.
#include "commands.h"
#include "../determinism/determinism.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/replay_menus.h"
#include "../world_sync/local_builds.h"
#include "lockstep_net.h"
#include "../game/input_block.h"
#include "../session/settings_sync.h"
#include "../world_sync/terrain/terrain_hook.h"
#include "purchase.h"
#include "route_tools.h"
#include "game_calls.h"
#include "line_accept.h"
#include "world_refs.h"
#include "vehicle_switches.h"
#include <vector>
#include <string.h>

void Commands_Bind(const TsmHost* host)
{
    TsmBind(host);
    Purchase_Bind(host);
    RouteTools_Bind(host);
    GameCalls_Bind(host);
    LineAccept_Bind(host);
    WorldRefs_Bind(host);
    VehicleSwitches_Bind(host);
}

struct Command {
    int kind;
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
static bool g_replaying = false;
static volatile LONG g_applying = 0;
static DWORD g_applyThread = 0;
static unsigned g_seq = 0;
static int g_session = COMMANDS_OFF; // in a session the player keeps the host's commands in g_record

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

bool Commands_Capturing()
{
    return g_capturing;
}

bool Commands_TakeLocal()
{
    return g_capturing && g_applying == 0 && !Determinism_InStepFrame();
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
    }
    return "?";
}

// Terrain brushes and their cost come every frame a tool is used: not logged one by one.
static bool Frequent(int kind)
{
    return kind == LS_CMD_TERRAIN || kind == LS_CMD_MONEY;
}

void Commands_Submit(int kind, const void* data, size_t size)
{
    if (!g_capturing) return;
    Command c;
    c.kind = kind;
    c.afterTick = 0;
    c.seq = 0;
    c.data.assign((const BYTE*)data, (const BYTE*)data + size);
    Lock();
    g_outbox.push_back(std::move(c));
    Unlock();
    if (!Frequent(kind)) Logf("republicmp  LOCKSTEP: %s captured - made at the next boundary", KindName(kind));
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

// Makes one command through the replay of its kind. The payload layout is the
// wire layout of the replay multiplayer; a size that does not match refuses it.
static void Apply(const Command& c)
{
    const void* p = c.data.data();
    size_t n = c.data.size();
    bool ok = false;
    g_applyThread = GetCurrentThreadId();
    InterlockedIncrement(&g_applying);
    double rubBefore = *Rubles(), usdBefore = *Dollars();
    InputBlock_BeginNeutral();
    // A drag the local player has under way would share the tool manager with
    // the replay and change what it builds on this PC only: it is cancelled.
    if (c.kind == LS_CMD_SEGMENT || c.kind == LS_CMD_UPGRADE || c.kind == LS_CMD_NET_DEMOLISH || c.kind == LS_CMD_PLACE)
        RoadSync_CancelLocalDrag();
    // This PC's open menus or a window under its cursor (+fbc/+fbe/+fc3) make
    // the tools return at once: a command is made with them closed, as the
    // replays are (replay_menus.h). A road from the host was refused on the
    // player only because its cursor was over a window.
    ReplayUiScope menus;
    switch (c.kind) {
    case LS_CMD_SEGMENT:
        if (n == sizeof(RoadSyncMsg)) ok = RoadSync_ApplyCommandSegment(*(const RoadSyncMsg*)p);
        break;
    case LS_CMD_UPGRADE:
        if (n == sizeof(RoadSyncUpgradeMsg)) ok = RoadSync_ApplyCommandUpgrade(*(const RoadSyncUpgradeMsg*)p);
        break;
    case LS_CMD_NET_DEMOLISH:
        if (n == sizeof(RoadSyncDemolishMsg)) {
            ok = RoadSync_ApplyCommandDemolish(*(const RoadSyncDemolishMsg*)p);
            LocalBuilds_DemolishMade(*(const RoadSyncDemolishMsg*)p);
        }
        break;
    case LS_CMD_PLACE:
        if (n == sizeof(BuildSyncMsg)) ok = BuildSync_ApplyPlacementCommand(*(const BuildSyncMsg*)p);
        break;
    case LS_CMD_DEMOLISH:
        if (n == sizeof(BuildSyncMsg)) ok = BuildSync_ApplyDemolishCommand(*(const BuildSyncMsg*)p);
        break;
    case LS_CMD_SETTING:
        if (n == sizeof(SettingWire)) ok = SettingsSync_ApplyCommand(*(const SettingWire*)p);
        break;
    case LS_CMD_PLAN_START:
        if (n == sizeof(RoadSyncPlanStartMsg)) ok = RoadSync_ApplyCommandPlanStart(*(const RoadSyncPlanStartMsg*)p);
        break;
    case LS_CMD_GROUP_TOOL:
        if (n == sizeof(RoadSyncGroupToolMsg)) ok = RoadSync_ApplyCommandGroupTool(*(const RoadSyncGroupToolMsg*)p);
        break;
    case LS_CMD_DIRECTION:
        if (n == sizeof(RoadSyncDirectionMsg)) ok = RoadSync_ApplyCommandDirection(*(const RoadSyncDirectionMsg*)p);
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
    }
    InputBlock_EndNeutral();
    InterlockedDecrement(&g_applying);
    g_madeThisFrame = true;
    // What the command paid or got: compared between the PCs' logs when the money differs.
    double paidRub = *Rubles() - rubBefore, paidUsd = *Dollars() - usdBefore;
    char money[96] = "";
    if (paidRub != 0.0 || paidUsd != 0.0)
        sprintf_s(money, " (money %+.4f rubles %+.4f dollars)", paidRub, paidUsd);
    if (!Frequent(c.kind) || !ok)
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

// Player: its own actions go to the host; what the host made after this
// tick is made here now (TCP brought it before the grant of the next tick).
static void PlayerBoundary(unsigned long long completed)
{
    std::vector<Command> mine;
    Lock();
    mine.swap(g_outbox);
    Unlock();
    for (const Command& c : mine) LockstepNet_SendCommand(c.kind, 0, c.data.data(), c.data.size());
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
    std::vector<Command> due;
    Lock();
    due.swap(g_outbox);
    Unlock();
    for (Command& c : due) {
        c.afterTick = completed;
        c.seq = ++g_seq;
        Apply(c);
        if (g_session == COMMANDS_HOST) LockstepNet_SendCommand(c.kind, c.afterTick, c.data.data(), c.data.size());
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
                                              "vehicle_godepo", "vehicle_godepo_batch" };
static const char* const kUncarriedTools[] = {
    "vehicle_", "worker_relocate", "building_addsource", "line_addstop", "second_road", "water_square",
    "bojka", "christmas_", "join_hub", "crossroad_add", "terrain_go_there", "terrain_demolish_trees",
    "road_crossroad", "tree_", "palma", "jungle_", "fjord_", "afgan_", "tallkriak", "leafkriak",
};

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
    if (RouteTools_Ready())
        for (const char* carried : kCarriedTools)
            if (strcmp(name, carried) == 0) return;
    for (const char* prefix : kUncarriedTools) {
        if (strncmp(name, prefix, strlen(prefix)) != 0) continue;
        *tool = 0;
        Logf("republicmp  LOCKSTEP: tool [%s] put away - its actions are not shared in lockstep yet", name);
        return;
    }
}

void Commands_Collect()
{
    if (!g_capturing) return;
    PutAwayUncarriedTool();
    TerrainHook_LockstepFlush();
    if (!g_haveFrameRecord) return;
    SettingsSync_LockstepDetect();
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
    if (tickRan || g_madeThisFrame || !g_haveFrameRecord) SettingsSync_LockstepBaseline();
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
    SettingsSync_LockstepBaseline();
    RoadSync_LockstepDirectionsBaseline();
    g_lateRubles = *Rubles();
    g_lateDollars = *Dollars();
}

void Commands_LateEnd()
{
    if (!g_inLateRecord) return;
    g_inLateRecord = false;
    SettingsSync_LockstepDetect();
    RoadSync_LockstepDirectionsDetect();
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
    g_capturing = role != COMMANDS_OFF;
    Logf("republicmp  LOCKSTEP: commands %s", role == COMMANDS_HOST ? "ordered here (host)"
                                              : role == COMMANDS_PLAYER ? "ordered by the host" : "off");
}

void Commands_SubmitRemote(int kind, const void* data, size_t size)
{
    if (g_session != COMMANDS_HOST) return;
    Command c;
    c.kind = kind;
    c.afterTick = 0;
    c.seq = 0;
    c.data.assign((const BYTE*)data, (const BYTE*)data + size);
    Lock();
    g_outbox.push_back(std::move(c));
    Unlock();
    Logf("republicmp  LOCKSTEP: %s from the player - made at the next boundary", KindName(kind));
}

void Commands_Receive(int kind, unsigned long long afterTick, const void* data, size_t size)
{
    if (g_session != COMMANDS_PLAYER) return;
    Command c;
    c.kind = kind;
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
