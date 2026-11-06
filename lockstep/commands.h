// SPDX-License-Identifier: GPL-3.0-only
// Lockstep commands: player actions made at a tick boundary on every PC.
//
// While the determinism layer runs the simulation in whole ticks
// (determinism/), an action of the local player must not change the world
// in the middle of a frame on this PC only. The hooks that see the local
// actions (world/local_builds: building placement and every drag tool's
// commit; world/demolish_hook: building demolition) hand them
// here instead of letting the game make them, and the action is made later
// from its data on every PC (world/buildings, world/networks, ...) - on this
// PC as well, at the start of a frame, before the next tick runs
// (Determinism_BeforePrepare). Every PC that makes the same commands after the
// same tick reaches the same world.
//
// Tools act privately on every frame (a held Late, and the tool scope around
// Prepare and the tool handlers on a tick's frame; see sim_patches.cpp); the
// probe still waits while one is selected ("tool hold", by SoylentGreed).
//
// In a lockstep session (lockstep/lockstep_net) the host orders the commands
// of both players: it makes each one after its current tick and sends it on;
// the player sends its own to the host and makes the ones it receives after
// the tick they name. The determinism probe uses them on one PC: it records
// them in run 1 (the player builds while it runs) and makes them again in
// run 2 after the same ticks, with no input; both runs must stay identical.
#pragma once

#include "host.h"

enum LockstepCommandKind {
    LS_CMD_SEGMENT = 1,      // NetSegmentMsg: a drag tool's finished segment
    LS_CMD_UPGRADE = 2,      // NetUpgradeMsg: re-typing existing pieces
    LS_CMD_NET_DEMOLISH = 3, // NetDemolishMsg: the demolition tool on a network
    LS_CMD_PLACE = 4,        // BuildingMsg: building placement inputs
    LS_CMD_DEMOLISH = 5,     // BuildingMsg: building demolition
    LS_CMD_SETTING = 6,      // BuildingSettingMsg: a building window setting, pay or stop flags
    LS_CMD_PLAN_START = 7,   // NetPlanStartMsg: start planned construction
    LS_CMD_GROUP_TOOL = 8,   // NetGroupToolMsg: a group tool's commit
    LS_CMD_DIRECTION = 9,    // NetDirectionMsg: a factory connection's direction
    LS_CMD_TERRAIN = 10,     // terrain edits of one frame (terrain_hook)
    LS_CMD_MONEY = 11,       // LsMoneyDelta: what a local action paid or got
    LS_CMD_PURCHASE = 12,    // purchase.cpp: a vehicle, licence or blueprint bought in a purchase window
    LS_CMD_ROUTE = 13,       // route_tools.cpp: a stop added to a schedule, a vehicle put on a line, sent to a depot
    LS_CMD_GAME_CALL = 14,   // game_calls.cpp: a window button's routine call
    LS_CMD_LINE_ACCEPT = 15, // line_accept.cpp: the line window's Accept with its working copy
    LS_CMD_VEHICLE_SWITCH = 16, // vehicle_switches.cpp: vehicle words a window click changed
    LS_CMD_BUILDING_SWITCH = 17, // building_switches.cpp: building, storage slot or world setting words a click changed
    LS_CMD_BRUSH = 18,       // brush_tools.cpp: one frame of a terrain brush or a single-tree tool
    LS_CMD_HOVER = 19,       // hover_tools.cpp: a click of a road sign, waypoint or rail signal tool
    LS_CMD_JUNCTION = 20,    // junction_accept.cpp: the junction window's Accept with its working copy
    LS_CMD_GAME_RULES = 21,  // commands.cpp: the game menu's settings page applied (its copy of the rules)
    LS_CMD_CARGO_SETTINGS = 22, // cargo_settings.cpp: a stop's or link's loading or unloading half
    LS_CMD_SOURCE_SLOTS = 23,   // source_slots.cpp: a building's specific sources (add-source tool, windows)
    LS_CMD_RENAME = 24,         // rename.cpp: a city's or a crossroad's new name
    LS_CMD_LEDGER = 25,         // brush_tools.cpp: the construction ledger row a terrain stroke wrote
    LS_CMD_INVITE = 26,         // immigrant_invite.cpp: a residential window's immigrant invitation
    LS_CMD_SAVE = 27,           // shared_save.cpp: a save of the shared game, on every PC
    LS_CMD_RESEARCH = 28,       // research.cpp: research started or suspended in a research building's window
    LS_CMD_LINK = 29,           // link_tools.cpp: a station or distribution link added with a link pick tool
    LS_CMD_OFFICE_SELECTION = 30, // office_selection.cpp: the construction offices assign tool's selection
    LS_CMD_NEW_CITY = 31,       // new_city.cpp: a city made with the city tool (position and name)
    LS_CMD_JOIN = 32,           // join_tool.cpp: the join tool's node (two edges made one)
    LS_CMD_NET_CANCEL = 33,     // cancel_tool.cpp: an edge a network's cancel tool removes
    LS_CMD_WORKERS = 34,        // world_workers.cpp: the construction menu's "Max. workers without CO"
    LS_CMD_POPUP = 35,          // popups.cpp: a confirm or value popup's callback (its building and values)
    LS_CMD_CALL_HOME = 36,      // call_home.cpp: a partner building's "Call all vehicles home" (both buildings)
    LS_CMD_CRIME_CANCEL = 37,   // crime_cancel.cpp: the crime list's "Cancel assignment" (building, case)
    LS_CMD_LINE_SPACING = 38,   // line_spacing.cpp: the line window's line spacing button (the line)
    LS_CMD_CAR_OWNERS = 39,     // car_owners.cpp: the car building window's "Reset owners" (the building)
    LS_CMD_PLAN_REMOVE = 40,    // plan_remove.cpp: the factory window's "Remove from plan" (factory, entry)
    LS_CMD_CANCEL_PLANS = 41,   // cancel_plans.cpp: "Cancel all plans" (the networks by world field)
    LS_CMD_SPAWN_BUILDING = 42, // BuildingMsg: a mode's building, placed free of charge (the host's only)
    LS_CMD_BUOY = 43,           // buoy_tool.cpp: a buoy placed or removed
    LS_CMD_GO_THERE = 44,       // go_there.cpp: "Go there" (the vehicle, the point)
    LS_CMD_LIGHTS = 45,         // christmas_lights.cpp: a Christmas light line made, or lines freed
    LS_CMD_SQUARE = 46,         // square_tool.cpp: a pavement square (type, corners, yaw)
    LS_CMD_BUILD_MODE = 47,     // build_modes.cpp: a player chose rubles, dollars or realistic
};

// Money a local action changed on this PC (an interface purchase, a terrain
// tool's cost): put back here and added on every PC by the command.
struct LsMoneyDelta {
    double rubles;
    double dollars;
};

void Commands_Bind(const ModHost* host);

// True while local actions become commands instead of acting at once.
bool Commands_Capturing();

// For a hook: should the local action seen now become a command? False on
// the simulation's own calls (anything during a step frame - tools are
// hidden then) and while a command is being made.
bool Commands_TakeLocal();

// For a window's call (the interface, any thread): should it become a command?
// False only while this thread makes a command.
bool Commands_TakeUi();

// game_calls: the settings page applies its copy of the game rules (337170).
void Commands_CaptureGameRules();
// In a session: cheats off, and no save and reload after a critical rule
// (called before the frame's commands and after the interface).
void Commands_GuardSettings();

// Counts the commands made so far (any thread): a comparison around the
// interface that saw it change knows a command, not the interface, wrote.
unsigned long long Commands_ApplyEpoch();

// Any thread: queues a captured action; it is made at the next boundary.
void Commands_Submit(int kind, const void* data, size_t size);

// Who made a command: 0 the host, 1.. a player by its connection on the host.
// The host stamps it; what a player says of itself is never read.
// A mode's rule on where a player may act (modes/): false for a point the
// player may not build on or demolish refuses the whole command, on every PC.
// Main thread, while a command is made; null: no rule. Called with pos null,
// it says whether it applies at all.
// notify: false when the caller only filters (nothing is refused), so the
// mode shows no notice.
typedef bool (*CommandAreaFn)(int player, const float pos[3], bool notify);
void Commands_SetAreaRule(CommandAreaFn fn);
// A mode's rule on who may change the game's rules (the rules page): false
// refuses the command on every PC. Main thread, while a command is made;
// null: everyone may.
typedef bool (*CommandGlobalFn)(int player);
void Commands_SetGlobalRule(CommandGlobalFn fn);
// A mode's rule on the game's check that a border building crosses the border
// (411270): true lets this one stand where the game said no. corners: its model's
// box on the ground, in the world. It must depend on nothing but these and the
// world, as it runs in the preview and in a command on every PC. Inside the game's
// own call; null: the game's answer stands.
typedef bool (*CommandBorderFn)(unsigned long long type, const float corners[4][3]);
void Commands_SetBorderRule(CommandBorderFn fn);
bool Commands_BorderAllows(unsigned long long type, const float corners[4][3]);

// A mode hears how its LS_CMD_SPAWN_BUILDING ended, on every PC.
typedef void (*CommandSpawnedFn)(bool placed);
void Commands_SetSpawned(CommandSpawnedFn fn);
// Outside the lockstep (a Republics host alone): the same placement, here at once.
// Main thread. Its save carries the building to whoever joins.
bool Commands_SpawnBuildingNow(const struct BuildingMsg& m);

// Outside the lockstep (a Republics host alone) the game makes the local
// player's actions itself: the hooks that would make a command ask here
// first and skip the game's call when the rule refuses it. All false while
// commands are captured or when no rule applies. The rule's player is then
// COMMANDS_LOCAL_PLAYER, this PC's own.
#define COMMANDS_LOCAL_PLAYER -1
bool Commands_ChecksHere();
bool Commands_RefusedHere(int kind, const void* data, size_t size);
bool Commands_PointRefusedHere(const float pos[3]);
// While this thread makes a command: false when the area rule keeps the
// command's player off this building or point (Republics: the other
// republic's buildings are not theirs to act on). A module checks the
// buildings its command changes - not those it only reads - before it changes
// anything, and refuses the command when one is false. Logged; the clicking
// player's notice is shown. True outside a command or without a rule.
bool Commands_BuildingInArea(unsigned long long building);
bool Commands_PointInArea(const float pos[3], bool notify = true);
// The same for a vehicle (a wagon or trailer by the vehicle at its head), a
// line (none of its stations outside; border connections count as anyone's)
// and a station of a line.
bool Commands_VehicleInArea(unsigned long long vehicle);
// What decides a vehicle's side: the building it belongs to (0: none), else
// where it is. False when the vehicle could not be read. Any thread.
bool Commands_VehicleSide(unsigned long long vehicle, unsigned long long* building, float pos[3]);
bool Commands_LineInArea(unsigned long long line);
bool Commands_StationInArea(unsigned long long station);
// While this thread makes a command: the mode's global rule for its player.
bool Commands_GlobalAllowed();
// The player of the command this thread is making (0 the host), or -1 outside
// one. A mode with a rule active (Republics) keeps some values per player by it.
int Commands_ApplyingPlayer();
bool Commands_PerPlayer();

// Changes made outside the hooks, found by comparing with a record (main
// thread, while commands are captured): what the interface changed between
// the end of a frame and the start of the next (AfterFrame records, Collect
// compares - building settings, construction pay/stop flags, money), and what
// a tool changed inside a held frame's Late body (LateBegin records, LateEnd
// compares - the same plus factory connection directions). Each change is put
// back on this PC and becomes a command. Collect also hands over the terrain
// edits a tool made and puts away a selected tool whose actions are not
// carried yet.
void Commands_Collect();
void Commands_AfterFrame(bool tickRan);
void Commands_LateBegin();
void Commands_LateEnd();

// Money between the frame start and Late (main thread): RecordMoney takes the
// balances once the frame's commands are made; CheckMoney turns a change since
// then (this PC's view preparation or held drawing) into a command.
void Commands_RecordMoney();
void Commands_CheckMoney(const char* where);

// Main thread, at the start of a frame, with `completed` ticks run: queued
// actions get this boundary, and every command due here is made.
void Commands_Boundary(unsigned long long completed);
// True when Commands_Boundary has something to make at `completed`.
bool Commands_Due(unsigned long long completed);
// True while a command is being made (its replay runs).
bool Commands_Applying();
// True on the thread making a command of another player (lockstep/remote_sounds).
bool Commands_ApplyingRemote();

// Probe (main thread, right after a run's load): run 0 with `record` captures
// and records; run 1 makes the recorded commands again. record = false: off.
void Commands_ProbeRun(int run, bool record);
// After the probe: capture off, record dropped. Returns the commands made in
// run 2 that were recorded in run 1 (all of them when the replay was complete).
void Commands_ProbeEnd(size_t* recorded, size_t* replayed);

// Lockstep session (main thread, after the session's load): 0 off, 1 host,
// 2 player. Capture is on while a session runs.
enum { COMMANDS_OFF = 0, COMMANDS_HOST = 1, COMMANDS_PLAYER = 2 };
void Commands_SetSession(int role);
// Host, game thread: a command `player` sent; ordered at the next boundary.
void Commands_SubmitRemote(int kind, int player, const void* data, size_t size);
// Player, game thread: a command of `player` the host made after `afterTick`.
void Commands_Receive(int kind, int player, unsigned long long afterTick, const void* data, size_t size);
