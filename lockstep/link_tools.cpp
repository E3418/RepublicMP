// SPDX-License-Identifier: GPL-3.0-only
// See link_tools.h. Follows SoylentGreed's link tool route (RepublicLAN, link
// tool kinds 0 to 9, read against the disassembly of 1.1.1.9).
#include "link_tools.h"
#include "commands.h"
#include "world_refs.h"
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define SOURCE_OFFSET 0xD298     // the building of the window the tool serves
#define PRESS_RVA 0xA54E91       // the left press, as the tools read it
#define LINKS_OFFSET 0xD38       // a building's link objects (pointers)
#define LINKED_OFFSET 0xD50      // byte, set by the tools
#define LINK_DELAY_OFFSET 0xFC0  // float, set to 3.0 by the tools
#define STATION_PARTNERS 0xD78   // the target's list of buildings linked to it
#define DIST_PARTNERS 0xD90
#define TYPE_OFFSET 0x318
#define TYPE_CATEGORY 0x360
#define LINK_BYTES 0x18
#define RECORD_BYTES 0x198
#define MAX_LINKS 65536
#define STATION_TOOL 0x2B6F30
#define DIST_TOOL 0x2B78B0
#define MARK_RVA 0x40BD60
#define ALLOC_RVA 0x84DECC       // operator new
#define PUSH_RVA 0x8E050         // vector<T*>::push_back
#define RECORD_RVA 0x1512A0      // the link record's constructor
#define SHARES_RVA 0x76A3A0      // the load/unload shares from the source's links
#define FIRE_TOOL 0x1D48A0
#define WEIGHTS_TOOL 0x1D71C0
#define READINESS_RVA 0x6C2430   // a vehicle's readiness value (the fire tool's test)
#define DISPATCH_RVA 0x2582E0    // a vehicle sent to a fire
#define FIRE_MODE_RVA 0x9D54C0   // int: the game's mode the fire tool reads
#define REALISM_OFFSET 0x5BC     // world: realism
#define WEIGHTS_OFFSET 0x11F4
#define WEIGHTS_BYTES 36
#define MAGIC 0x314B4E4C
#define RELOCATE_TOOL 0x2B8CE0
#define RELOCATE_WORKER_RVA 0x83C6A0
#define RELOCATE_CITIZEN_RVA 0x83CA50
#define PLACES_OFFSET 0xBC0      // a building's workplaces or flats' places
#define RESIDENTS_OFFSET 0xBD8
#define INFO_OFFSET 0x970
#define CAPACITY_OFFSET 0x8C
#define HOLDER_OFFSET 0xC8
#define RELOCATE_LIMIT 10
#define RELOCATE_EDUCATED_LIMIT 5
#define RELOCATE_INMATES 5
#define MODE_EDUCATED 1
#define MODE_INMATE 2
#define MAX_PEOPLE (1u << 20)
#define SOUND_RVA 0x3FFAD0
#define AUTO_TOOL 0x2B4680
#define AUTO_MANAGER 0xD440
#define AUTO_QUEUE 0x40
#define AUTO_TEST_RVA 0x6149C0
#define AUTO_ELIGIBLE_RVA 0x614310
#define AUTO_PUSH_RVA 0xB0E90
#define AUTO_BUSY 0x600
#define AUTO_FLAG 0x1A6C
#define COPY_TOOL 0x2B18C0
#define COPY_REFRESH_RVA 0x6BF1D0
#define COPY_STATIONS_RVA 0x1FEF00
#define COPY_RECORDS_RVA 0x44A180
#define COPY_RESET_RVA 0x67CD20
#define COPY_HALF_RVA 0x1E0FC0
#define COPY_PUSH_RECORD_RVA 0x1FF8F0
#define COPY_DROP_HALF_RVA 0x1513F0
#define COPY_LINES_RVA 0x497B10
#define SEND_TOOL 0x1D88A0
#define SEND_OPTIONS_RVA 0x14FE20
#define SEND_CANDIDATES_RVA 0x155440
#define SEND_PICK_RVA 0x155D00
#define SEND_RELEASE_RVA 0x1A50  // the typed-array release (not checked: an instrumented build's entry differs)
#define SEND_HOME_RVA 0x3FB8E0
#define SEND_WORK_RVA 0x151150
#define SEND_RETURN_RVA 0x3FBDC0
#define SEND_JOB_RVA 0x150960
#define SEND_COUPLED_RVA 0xF99C0
#define SEND_MODE_RVA 0x67CB30
#define SEND_ASSIGN_RVA 0x67D5D0
#define SEND_INDEX_OFFSET 0xD2A8 // the option a key cycles
#define SEND_INDEX_MAX 0xFFFF
#define SEND_OPTION_BYTES 0x40
#define SEND_SITES_OFFSET 0xF90

enum {
    KIND_STATION = 0,
    KIND_DISTRIBUTION = 1,
    KIND_FIRE = 2,
    KIND_WEIGHTS = 3,
    KIND_RELOCATE = 4,
    KIND_AUTO_REPLACE = 5,
    KIND_COPY_STOPS = 6,
    KIND_SEND = 7,
    KIND_LAST = KIND_SEND
};

struct RoutineEntry {
    unsigned rva;
    unsigned char entry[16];
};
static const RoutineEntry kRoutines[] = {
    { STATION_TOOL, { 0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48 } },
    { DIST_TOOL, { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41 } },
    { MARK_RVA, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xf8, 0xe5, 0xff, 0xff, 0xb8, 0x08, 0x1b } },
    { ALLOC_RVA, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xeb, 0x21, 0x48, 0x8b, 0xcb, 0xe8, 0xd5 } },
    { PUSH_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xfa, 0x48, 0x8b, 0xd9 } },
    { RECORD_RVA, { 0x33, 0xc0, 0x48, 0x89, 0x41, 0x08, 0x48, 0x89, 0x41, 0x10, 0x48, 0x89, 0x41, 0x18, 0x48, 0x89 } },
    { SHARES_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x7c, 0x24, 0x10, 0xf3, 0x0f, 0x10, 0x2d, 0x4e, 0x0f } },
};
static const RoutineEntry kPickRoutines[] = {
    { FIRE_TOOL, { 0x4c, 0x8b, 0xdc, 0x55, 0x41, 0x56, 0x49, 0x8d, 0x6b, 0xa8, 0x48, 0x81, 0xec, 0x48, 0x01, 0x00 } },
    { WEIGHTS_TOOL, { 0x4c, 0x8b, 0xdc, 0x53, 0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x80, 0xb9, 0xbc, 0x0f, 0x00 } },
    { MARK_RVA, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xf8, 0xe5, 0xff, 0xff, 0xb8, 0x08, 0x1b } },
    { READINESS_RVA, { 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x81, 0x98, 0x03, 0x00, 0x00, 0x48, 0x8b, 0xd1, 0x48, 0x85 } },
    { DISPATCH_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x57 } },
};

static const RoutineEntry kRelocateRoutines[] = {
    { RELOCATE_TOOL, { 0x4c, 0x8b, 0xdc, 0x55, 0x57, 0x49, 0x8d, 0x6b, 0xe8, 0x48, 0x81, 0xec, 0x08, 0x01, 0x00, 0x00 } },
    { RELOCATE_WORKER_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41 } },
    { RELOCATE_CITIZEN_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41 } },
    { MARK_RVA, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xf8, 0xe5, 0xff, 0xff, 0xb8, 0x08, 0x1b } },
    { SOUND_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x30, 0x80 } },
};
static const RoutineEntry kVehicleRoutines[] = {
    { AUTO_TOOL, { 0x4c, 0x8b, 0xdc, 0x55, 0x56, 0x49, 0x8d, 0xab, 0x48, 0xff, 0xff, 0xff, 0x48, 0x81, 0xec, 0xa8 } },
    { COPY_TOOL, { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xe8 } },
    { SEND_TOOL, { 0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48 } },
    { MARK_RVA, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xf8, 0xe5, 0xff, 0xff, 0xb8, 0x08, 0x1b } },
    { AUTO_TEST_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x40, 0x32, 0xff, 0x48, 0x8b, 0xda } },
    { AUTO_ELIGIBLE_RVA, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x9b, 0x00, 0x00, 0x00, 0x48, 0x83, 0xba, 0x00, 0x06, 0x00, 0x00 } },
    { AUTO_PUSH_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x41, 0x08, 0x48, 0x8b } },
    { COPY_REFRESH_RVA, { 0x48, 0x8b, 0xc4, 0x44, 0x88, 0x48, 0x20, 0x44, 0x88, 0x40, 0x18, 0x88, 0x50, 0x10, 0x48, 0x89 } },
    { COPY_STATIONS_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b, 0x41, 0x08, 0x48, 0x8b } },
    { COPY_RECORDS_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 } },
    { COPY_RESET_RVA, { 0x4c, 0x8b, 0x81, 0x40, 0x06, 0x00, 0x00, 0x4c, 0x8b, 0xca, 0x4c, 0x2b, 0x81, 0x38, 0x06, 0x00 } },
    { COPY_HALF_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x0f, 0xb6, 0x02, 0x48, 0x8b, 0xda } },
    { COPY_PUSH_RECORD_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0xfa } },
    { COPY_DROP_HALF_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x33, 0xff, 0x48 } },
    { COPY_LINES_RVA, { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xf0 } },
    { SEND_OPTIONS_RVA, { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xb9 } },
    { SEND_CANDIDATES_RVA, { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48 } },
    { SEND_PICK_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x40, 0x33, 0xdb } },
    { SEND_HOME_RVA, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b, 0x9a, 0xf0, 0x04, 0x00, 0x00, 0x48, 0x8b, 0xda } },
    { SEND_WORK_RVA, { 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x55, 0x53, 0x48, 0x8d, 0xac, 0x24, 0x08, 0xff, 0xff, 0xff, 0x48 } },
    { SEND_RETURN_RVA, { 0x48, 0x8b, 0xc4, 0x44, 0x88, 0x48, 0x20, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55 } },
    { SEND_JOB_RVA, { 0x48, 0x8b, 0xc4, 0x41, 0x56, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x40, 0xa8 } },
    { SEND_COUPLED_RVA, { 0x4c, 0x8b, 0xc9, 0x48, 0x85, 0xd2, 0x74, 0x27, 0x4c, 0x8b, 0x02, 0x33, 0xc0, 0x48, 0x8b, 0x52 } },
    { SEND_MODE_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x41, 0x0f, 0xb6, 0xf8, 0x48, 0x8b } },
    { SEND_ASSIGN_RVA, { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 } },
};

typedef U64 (*ToolFn)(U64 world, U64 a2);
typedef U64 (*MarkFn)(U64 world, U64 target, U64 a3, U64 refused, U64 a5);
typedef void* (*AllocFn)(size_t bytes);
typedef void (*PushFn)(U64 vector, const U64* value);
typedef U64 (*RecordFn)(U64 record);
typedef void (*SharesFn)(U64 world, U64 source, float* load, float* unload, unsigned char a5);
typedef double (*ReadinessFn)(U64 vehicle);
typedef void (*DispatchFn)(U64 a1, U64 fire, U64 vehicle);
typedef unsigned char (*RelocateWorkerFn)(U64 world, U64 source, U64 target, unsigned char test, unsigned char educated);
typedef void (*RelocateCitizenFn)(U64 world, U64 citizen, U64 target);
typedef void (*SoundFn)(U64 world, const char* name, unsigned char a3);
typedef unsigned char (*AutoTestFn)(U64 manager, U64 vehicle);
typedef unsigned char (*AutoEligibleFn)(U64 a1, U64 vehicle);
typedef void (*RefreshFn)(U64 vehicle, unsigned char a2, unsigned char a3, unsigned char a4);
typedef void (*ResizeFn)(U64 vector, size_t count);
typedef void (*ResetRecordFn)(U64 vehicle, U64 record);
typedef void (*CopyHalfFn)(U64 to, U64 from);
typedef void (*PushRecordFn)(U64 vector, U64 record);
typedef void (*DropHalfFn)(U64 half);
typedef void (*LinesFn)(U64 lines, U64 line, U64 vehicle, unsigned char a4, U64 a5);
typedef U64 (*SendOptionsFn)(U64 world, void* option, U64 vehicle, U64 home, U64 target, int index, int* count);
typedef U64 (*SendCandidatesFn)(U64 world, U64* vector, U64 list, unsigned char a4);
typedef U64 (*SendPickFn)(U64 a1, U64 vehicle, U64* vector, unsigned char a4);
typedef void (*SendReleaseFn)(U64 memory, size_t count, size_t size);
typedef unsigned char (*SendHomeFn)(U64 a1, U64 vehicle);
typedef unsigned char (*SendWorkFn)(U64 world, U64 vehicle, U64 found, U64 job);
typedef void (*SendReturnFn)(U64 world, U64 vehicle, U64 a3, U64 a4, unsigned char a5, unsigned char a6);
typedef unsigned char (*SendJobFn)(U64 world, void* option, U64 home);
typedef unsigned char (*SendCoupledFn)(U64 vehicle, U64 list);
typedef void (*SendModeFn)(U64 vehicle, unsigned char a2, unsigned char a3);
typedef void (*SendAssignFn)(U64 vehicle, U64 job, int a3, unsigned char a4);

#pragma pack(push, 1)
struct LinkMsg {
    unsigned magic;
    unsigned kind; // KIND_*
    RouteRef source;
    RouteRef target;
    unsigned mode;  // relocation: MODE_*; construction vehicles: the option's index
    unsigned start; // relocation: the inmates' first place, before the modulo (the tool's rand)
};
#pragma pack(pop)

static int g_ready = -1, g_pickReady = -1, g_relocateReady = -1, g_vehicleReady = -1;

void LinkTools_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

bool LinkTools_Ready()
{
    if (g_ready < 0) {
        g_ready = 1;
        for (const RoutineEntry& e : kRoutines) {
            if (memcmp(g_exeBase + e.rva, e.entry, sizeof(e.entry)) != 0) {
                Logf("republicmp  LINKS: game routine at 0x%X differs - the link tools are put away in lockstep", e.rva);
                g_ready = 0;
                break;
            }
        }
    }
    return g_ready == 1;
}

static bool Verify(const RoutineEntry* routines, size_t count, const char* what)
{
    for (size_t i = 0; i < count; i++)
        if (memcmp(g_exeBase + routines[i].rva, routines[i].entry, sizeof(routines[i].entry)) != 0) {
            Logf("republicmp  LINKS: game routine at 0x%X differs - %s put away in lockstep", routines[i].rva, what);
            return false;
        }
    return true;
}

bool LinkTools_PickReady()
{
    if (g_pickReady < 0)
        g_pickReady = Verify(kPickRoutines, sizeof(kPickRoutines) / sizeof(kPickRoutines[0]),
                             "the fire and copy weights tools are") ? 1 : 0;
    return g_pickReady == 1;
}

bool LinkTools_RelocateReady()
{
    if (g_relocateReady < 0)
        g_relocateReady = Verify(kRelocateRoutines, sizeof(kRelocateRoutines) / sizeof(kRelocateRoutines[0]),
                                 "the relocation tools are") ? 1 : 0;
    return g_relocateReady == 1;
}

bool LinkTools_VehicleReady()
{
    if (g_vehicleReady < 0)
        g_vehicleReady = Verify(kVehicleRoutines, sizeof(kVehicleRoutines) / sizeof(kVehicleRoutines[0]),
                                "the vehicle pick tools are") ? 1 : 0;
    return g_vehicleReady == 1;
}

static bool KindReady(unsigned kind)
{
    if (kind >= KIND_AUTO_REPLACE) return LinkTools_VehicleReady();
    if (kind == KIND_RELOCATE) return LinkTools_RelocateReady();
    return kind >= KIND_FIRE ? LinkTools_PickReady() : LinkTools_Ready();
}

static const char* KindText(unsigned kind)
{
    switch (kind) {
    case KIND_STATION: return "station link";
    case KIND_DISTRIBUTION: return "distribution link";
    case KIND_FIRE: return "fire station dispatch";
    case KIND_WEIGHTS: return "weights copy";
    case KIND_RELOCATE: return "relocation";
    case KIND_AUTO_REPLACE: return "auto-replacement";
    case KIND_COPY_STOPS: return "stops copy";
    case KIND_SEND: return "construction vehicle";
    }
    return "?";
}

static unsigned ToolRva(unsigned kind)
{
    switch (kind) {
    case KIND_DISTRIBUTION: return DIST_TOOL;
    case KIND_FIRE: return FIRE_TOOL;
    case KIND_WEIGHTS: return WEIGHTS_TOOL;
    case KIND_RELOCATE: return RELOCATE_TOOL;
    case KIND_AUTO_REPLACE: return AUTO_TOOL;
    case KIND_COPY_STOPS: return COPY_TOOL;
    case KIND_SEND: return SEND_TOOL;
    }
    return STATION_TOOL;
}

// ---------------------------------------------------------------------------
// The clicking PC
// ---------------------------------------------------------------------------

// While a hidden click runs, the tool's mark tells the pick.
static bool g_armed = false, g_seen = false, g_allowed = false;
static U64 g_target = 0;

U64 LinkTools_Mark(U64 world, U64 target, U64 a3, U64 refused, U64 a5)
{
    if (g_armed && !g_seen) {
        g_seen = true;
        g_target = target;
        g_allowed = (refused & 0xff) == 0;
    }
    return ((MarkFn)(g_exeBase + MARK_RVA))(world, target, a3, refused, a5);
}

static U64 Run(unsigned kind, U64 world, U64 a2)
{
    ToolFn tool = (ToolFn)(g_exeBase + ToolRva(kind));
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal() || !KindReady(kind)) return tool(world, a2);
    BYTE kept = *press;
    *press = 0;
    g_armed = true;
    g_seen = g_allowed = false;
    g_target = 0;
    U64 r = tool(world, a2);
    g_armed = false;
    *press = kept;
    if (!g_seen || !g_allowed || !g_target) return r;
    LinkMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    m.kind = kind;
    const char* why = nullptr;
    __try {
        U64 source = *(U64*)(world + SOURCE_OFFSET);
        if (kind == KIND_AUTO_REPLACE) {
            m.source = kNoRef;
            if (!Ref_Vehicle(g_target, m.target)) why = "the picked vehicle is not in the world's list";
        } else if (kind == KIND_COPY_STOPS || kind == KIND_SEND) {
            if (!source || !Ref_Vehicle(source, m.source)) why = "the window's vehicle is not in the world's list";
            else if (kind == KIND_COPY_STOPS ? !Ref_Vehicle(g_target, m.target) : !Ref_Building(g_target, m.target))
                why = "the picked object is not in the world's list";
            if (kind == KIND_SEND) {
                int index = *(int*)(world + SEND_INDEX_OFFSET); // the option the click would use
                m.mode = index < 0 || index > SEND_INDEX_MAX ? 0 : (unsigned)index;
            }
        } else {
            if (!source || !Ref_Building(source, m.source)) why = "the window's building is not in the world's list";
            else if (!Ref_Building(g_target, m.target)) why = "the picked building is not in the world's list";
            if (kind == KIND_RELOCATE) {
                // The tool's name decides the mode (2b8d64..2b8da6).
                const char* name = *(const char**)(world + 0xD428);
                m.mode = !name ? 0 : strcmp(name, "worker_relocate_educated") == 0 ? MODE_EDUCATED
                                   : strcmp(name, "worker_relocate_inmate") == 0   ? MODE_INMATE
                                                                                   : 0;
                m.start = GetTickCount();
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading it";
    }
    if (why)
        Logf("republicmp  LINKS: %s not shared - %s; nothing is done", KindText(kind), why);
    else {
        Commands_Submit(LS_CMD_LINK, &m, sizeof(m));
        if (kind == KIND_RELOCATE) {
            __try {
                ((SoundFn)(g_exeBase + SOUND_RVA))(world, "gui_building_assign_ok", 1);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    return r;
}

U64 LinkTools_Station(U64 world, U64 a2) { return Run(KIND_STATION, world, a2); }
U64 LinkTools_Distribution(U64 world, U64 a2) { return Run(KIND_DISTRIBUTION, world, a2); }
U64 LinkTools_Fire(U64 world, U64 a2) { return Run(KIND_FIRE, world, a2); }
U64 LinkTools_Weights(U64 world, U64 a2) { return Run(KIND_WEIGHTS, world, a2); }
U64 LinkTools_Relocate(U64 world, U64 a2) { return Run(KIND_RELOCATE, world, a2); }
U64 LinkTools_AutoReplace(U64 world, U64 a2) { return Run(KIND_AUTO_REPLACE, world, a2); }
U64 LinkTools_CopyStops(U64 world, U64 a2) { return Run(KIND_COPY_STOPS, world, a2); }
U64 LinkTools_SendConstruction(U64 world, U64 a2) { return Run(KIND_SEND, world, a2); }

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

static U64 Q(U64 at) { return *(U64*)at; }

// The source's links already hold one to the target.
static bool Linked(U64 source, U64 target)
{
    U64 begin = Q(source + LINKS_OFFSET), end = Q(source + LINKS_OFFSET + 8);
    if (!begin || end < begin || (end - begin) % 8 || (end - begin) / 8 > MAX_LINKS) return false;
    for (U64 at = begin; at < end; at += 8) {
        U64 link = Q(at);
        if (link && Q(link + 8) == target) return true;
    }
    return false;
}

// The tool's link steps (2b75fb..2b77ba station, 2b8492..2b8683 distribution).
static bool AddLink(U64 world, unsigned kind, U64 source, U64 target)
{
    __try {
        float load = 0, unload = 0;
        ((SharesFn)(g_exeBase + SHARES_RVA))(world, source, &load, &unload, 0);
        U64 object = (U64)((AllocFn)(g_exeBase + ALLOC_RVA))(LINK_BYTES);
        *(BYTE*)object = 0;
        *(U64*)(object + 8) = 0;
        *(U64*)(object + 0x10) = 0;
        ((PushFn)(g_exeBase + PUSH_RVA))(source + LINKS_OFFSET, &object);
        U64 link = Q(Q(source + LINKS_OFFSET + 8) - 8);
        U64 record = (U64)((AllocFn)(g_exeBase + ALLOC_RVA))(RECORD_BYTES);
        memset((void*)record, 0, RECORD_BYTES);
        record = ((RecordFn)(g_exeBase + RECORD_RVA))(record);
        *(U64*)(link + 0x10) = record;
        *(U64*)(link + 8) = target;
        *(BYTE*)link = 1;
        *(BYTE*)record = 1;
        *(BYTE*)(record + 0xc8) = 1;
        *(float*)(record + 4) = 1.0f;
        *(U64*)(record + 0x10) = Q(record + 8);
        *(float*)(record + 0xcc) = 1.0f;
        *(U64*)(record + 0xd8) = Q(record + 0xd0);
        *(U64*)(record + 0x190) = 0;
        *(U64*)(record + 0x20) = 0;
        *(BYTE*)(record + 0x28) = 1;
        *(U64*)(record + 0xe8) = 0;
        *(BYTE*)(record + 0xf0) = 1;
        U64 type = Q(target + TYPE_OFFSET);
        unsigned category = type ? *(unsigned*)(type + TYPE_CATEGORY) : 0xffffffffu;
        if (kind == 0) {
            if (type && category == 0) {
                *(float*)(record + 4) = 0.0f;
                *(float*)(record + 0xcc) = 1.0f;
            } else {
                *(float*)(record + 4) = load;
                *(float*)(record + 0xcc) = unload;
            }
        } else {
            if (type && category == 0x14) {
                *(float*)(record + 4) = 0.0f;
                *(unsigned*)(record + 0xcc) = 0x3f7d70a4u; // 0.99f
            } else {
                *(float*)(record + 4) = load;
                *(float*)(record + 0xcc) = unload;
            }
            if (!type || (category <= 8 && ((0x121u >> category) & 1u))) *(BYTE*)(record + 0xc8) = 0;
            else *(BYTE*)record = 0;
        }
        *(BYTE*)(source + LINKED_OFFSET) = 1;
        *(float*)(source + LINK_DELAY_OFFSET) = 3.0f;
        ((PushFn)(g_exeBase + PUSH_RVA))(target + (kind == 0 ? STATION_PARTNERS : DIST_PARTNERS), &source);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The fire tool's sending (1d4d61..1d5078): fire engines (+1708 +294 == 1) of
// the station while the fire (+1160) has fewer vehicles (+28..+30) than its
// capacity (+1168..+1170, 32-byte entries); then the other kind (+294 == 0xa,
// +1a04 not 1 or 3), each with +d39 cleared.
static bool FireReady(U64 vehicle, U64 type, int mode, int realism)
{
    if (mode == 2) {
        bool check = true;
        if (type && 1.0f > *(float*)(type + 0x8624)) {
            unsigned c = *(unsigned*)(type + 0x294);
            if (((c - 3u) & ~3u) != 0 || c == 5) check = false;
        }
        if (check && (Q(vehicle + 0x5e8) || 0.0005f >= *(float*)(vehicle + 0x5f0))) return false;
    }
    if (realism > 0) {
        if (!(1.0 > ((ReadinessFn)(g_exeBase + READINESS_RVA))(vehicle))) return false;
        if (*(BYTE*)(vehicle + 0x1ab0) || Q(vehicle + 0x1ad0)) return false;
        bool tested = Q(vehicle + 0x4e8) != 0;
        if (!tested) {
            U64 home = Q(vehicle + 0x4f8);
            if (home) {
                U64 homeType = Q(home + 0x318);
                tested = homeType && *(unsigned*)(homeType + 0x360) == 0x26;
            }
        }
        if (tested && *(unsigned*)(vehicle + 0x50c) == 0x15) return false;
    }
    return true;
}

static bool FireDispatch(U64 world, U64 station, U64 target, unsigned* sent)
{
    *sent = 0;
    __try {
        U64 fire = Q(target + 0x1160);
        if (!fire) return false;
        int mode = *(int*)(g_exeBase + FIRE_MODE_RVA), realism = *(int*)(world + REALISM_OFFSET);
        int capacity = (int)((__int64)(Q(target + 0x1170) - Q(target + 0x1168)) >> 5);
        int have = (int)((__int64)(Q(fire + 0x30) - Q(fire + 0x28)) >> 3);
        if (have >= capacity) return true;
        DispatchFn dispatch = (DispatchFn)(g_exeBase + DISPATCH_RVA);
        size_t count = (size_t)((Q(station + 0xc78) - Q(station + 0xc70)) >> 3);
        for (size_t i = 0; i < count; i++) {
            U64 v = Q(Q(station + 0xc70) + i * 8);
            U64 home = Q(v + 0x4f0);
            if (home && home != Q(v + 0x4f8)) continue;
            if (Q(v + 0x528) || Q(v + 0xc80) || Q(v + 0x500)) continue;
            U64 type = Q(v + 0x1708);
            if (type && *(unsigned*)(type + 0x294) != 1) continue;
            if (!FireReady(v, type, mode, realism)) continue;
            dispatch(0, Q(target + 0x1160), v);
            ++*sent;
            if (++have >= capacity) return true;
            count = (size_t)((Q(station + 0xc78) - Q(station + 0xc70)) >> 3);
        }
        for (size_t i = 0; i < count; i++) {
            U64 v = Q(Q(station + 0xc70) + i * 8);
            U64 type = Q(v + 0x1708);
            if (!type) continue;
            U64 home = Q(v + 0x4f0);
            if (home && home != station) continue;
            if (*(unsigned*)(type + 0x294) != 0xa || Q(v + 0x528) || Q(v + 0xc80) || Q(v + 0x500)) continue;
            if (((*(unsigned*)(v + 0x1a04) - 1u) & 0xfffffffdu) == 0) continue;
            if (!FireReady(v, type, mode, realism)) continue;
            dispatch(0, Q(target + 0x1160), v);
            *(BYTE*)(v + 0xd39) = 0;
            ++*sent;
            count = (size_t)((Q(station + 0xc78) - Q(station + 0xc70)) >> 3);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The copy weights tool's step (1d74e0..1d7546).
static bool CopyWeights(U64 source, U64 target)
{
    __try {
        memcpy((void*)(target + WEIGHTS_OFFSET), (const void*)(source + WEIGHTS_OFFSET), WEIGHTS_BYTES);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static size_t ListCount(U64 list)
{
    U64 b = Q(list), e = Q(list + 8);
    return !b || e < b || (e - b) % 8 || (e - b) / 8 > MAX_PEOPLE ? 0 : (size_t)((e - b) / 8);
}

// The relocation tool's click steps (2b9268..2b93c8). Both routines draw from the
// game's rand, which is the command's stream here.
static bool Relocate(U64 world, U64 source, U64 target, unsigned mode, unsigned start, unsigned* moved)
{
    *moved = 0;
    __try {
        if (mode == MODE_INMATE) {
            size_t count = ListCount(source + RESIDENTS_OFFSET);
            if (!count) return true;
            size_t at = start % count;
            for (size_t step = 0;;) {
                size_t now = ListCount(source + RESIDENTS_OFFSET);
                if (!now) break;
                if (at >= now) at = 0;
                ((RelocateCitizenFn)(g_exeBase + RELOCATE_CITIZEN_RVA))(world, Q(Q(source + RESIDENTS_OFFSET) + at * 8), target);
                if (++*moved >= RELOCATE_INMATES) break;
                if (++step >= ListCount(source + RESIDENTS_OFFSET)) break;
            }
            return true;
        }
        U64 info = Q(target + INFO_OFFSET);
        if (!info) return false;
        float capacity = *(float*)(info + CAPACITY_OFFSET);
        int vacant = 0;
        size_t places = ListCount(target + PLACES_OFFSET);
        for (size_t i = 0; i < places; i++)
            if (!Q(Q(Q(target + PLACES_OFFSET) + i * 8) + HOLDER_OFFSET)) ++vacant;
        if (!((float)vacant < capacity)) return true;
        int count = (int)(capacity - (float)vacant);
        int limit = mode == MODE_EDUCATED ? RELOCATE_EDUCATED_LIMIT : RELOCATE_LIMIT;
        if (limit <= count) count = limit;
        for (int i = 0; i < count; i++) {
            ((RelocateWorkerFn)(g_exeBase + RELOCATE_WORKER_RVA))(world, source, target, 0, mode == MODE_EDUCATED ? 1 : 0);
            ++*moved;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The auto-replacement tool's click steps. The tests decide on every PC.
static const char* AutoReplace(U64 world, U64 vehicle)
{
    __try {
        U64 manager = Q(world + AUTO_MANAGER);
        if (!manager) return "no manager";
        if (!((AutoTestFn)(g_exeBase + AUTO_TEST_RVA))(manager, vehicle) || Q(vehicle + AUTO_BUSY)) return "not allowed here";
        if (!((AutoEligibleFn)(g_exeBase + AUTO_ELIGIBLE_RVA))(0, vehicle)) return nullptr;
        U64 queue = manager + AUTO_QUEUE, b = Q(queue), e = Q(queue + 8);
        bool there = false;
        for (U64 at = b; b && at < e && !there; at += 8) there = Q(at) == vehicle;
        if (!there) ((PushFn)(g_exeBase + AUTO_PUSH_RVA))(queue, &vehicle);
        *(BYTE*)(vehicle + AUTO_FLAG) = 1;
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "a fault while queueing it";
    }
}

// The copy stops tool's steps (2b1976..2b1990, 2b1c97, 2b1dda..2b203b): the source's
// stops, less its last when +5fd is set; a loop's last stop of the target is kept.
static const char* CopyStops(U64 world, U64 source, U64 target)
{
    __try {
        U64 sb = Q(source + 0x680), se = Q(source + 0x688);
        if (se < sb || (se - sb) % 8 || (se - sb) / 8 > 4096) return "its stops are unreadable";
        int count = (int)((se - sb) / 8) - (*(BYTE*)(source + 0x5FD) ? 1 : 0);
        if (count < 0) return "it has no stops";
        RefreshFn refresh = (RefreshFn)(g_exeBase + COPY_REFRESH_RVA);
        ResizeFn stations = (ResizeFn)(g_exeBase + COPY_STATIONS_RVA);
        ResizeFn records = (ResizeFn)(g_exeBase + COPY_RECORDS_RVA);
        CopyHalfFn half = (CopyHalfFn)(g_exeBase + COPY_HALF_RVA);
        DropHalfFn drop = (DropHalfFn)(g_exeBase + COPY_DROP_HALF_RVA);
        refresh(source, 0, 0, 0);
        refresh(target, 0, 0, 0);
        U64 last = 0;
        if (*(BYTE*)(target + 0x5FD) && Q(target + 0x688) != Q(target + 0x680)) last = Q(Q(target + 0x688) - 8);
        stations(target + 0x680, (size_t)count);
        records(target + 0x638, (size_t)count);
        for (int i = 0; i < count; i++) {
            U64 at = (U64)i * RECORD_BYTES;
            *(U64*)(Q(target + 0x680) + (U64)i * 8) = Q(Q(source + 0x680) + (U64)i * 8);
            ((ResetRecordFn)(g_exeBase + COPY_RESET_RVA))(target, Q(target + 0x638) + at);
            half(Q(target + 0x638) + at, Q(source + 0x638) + at);
            half(Q(target + 0x638) + at + 0xC8, Q(source + 0x638) + at + 0xC8);
            *(U64*)(Q(target + 0x638) + at + 0x190) = Q(Q(source + 0x638) + at + 0x190);
        }
        if (*(int*)(target + 0x698) >= count) *(int*)(target + 0x698) = count - 1;
        if (last) {
            *(BYTE*)(target + 0x5FD) = 1;
            *(U64*)(target + 0x5E8) = 0;
            stations(target + 0x680, (size_t)((Q(target + 0x688) - Q(target + 0x680)) / 8) + 1);
            *(U64*)(Q(target + 0x688) - 8) = last;
            __declspec(align(16)) BYTE record[RECORD_BYTES + 0x38];
            memset(record, 0, sizeof(record));
            ((RecordFn)(g_exeBase + RECORD_RVA))((U64)record);
            *(U64*)(record + 0x190) = 0;
            ((PushRecordFn)(g_exeBase + COPY_PUSH_RECORD_RVA))(target + 0x638, (U64)record);
            *(int*)(target + 0x698) = (int)((Q(target + 0x688) - Q(target + 0x680)) / 8) - 1;
            *(unsigned short*)(target + 0xD3A) = 0x0101;
            drop((U64)(record + 0xC8));
            drop((U64)record);
        }
        ((LinesFn)(g_exeBase + COPY_LINES_RVA))(Q(world + 0x12800), Q(source + 0x650), target, 0, 0);
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "a fault while copying";
    }
}

// The send tool's click steps (1d8ef0..1d8f55 its own office, 1d919e..1d92a6 any other
// building). The tests are repeated, as a click is made later than it was seen.
static const char* SendConstruction(U64 world, U64 vehicle, U64 target, int index)
{
    __try {
        U64 home = Q(vehicle + 0x4F8), type = Q(vehicle + 0x1708);
        if (!home || Q(vehicle + 0x5E8)) return "it has no office here";
        bool special = type && *(unsigned*)(type + 0x8600) == 0x12;
        bool unpowered = type && *(float*)(type + 0x8624) <= 0.0f;
        U64 found = 0;
        if (special || unpowered) {
            U64 list[3] = {};
            ((SendCandidatesFn)(g_exeBase + SEND_CANDIDATES_RVA))(world, list, home + 0xC70, 1);
            found = ((SendPickFn)(g_exeBase + SEND_PICK_RVA))(0, vehicle, list, 1);
            if (list[0]) ((SendReleaseFn)(g_exeBase + SEND_RELEASE_RVA))(list[0], (size_t)((list[2] - list[0]) >> 3), 8);
        }
        SendWorkFn work = (SendWorkFn)(g_exeBase + SEND_WORK_RVA);
        SendReturnFn back = (SendReturnFn)(g_exeBase + SEND_RETURN_RVA);
        SendJobFn job = (SendJobFn)(g_exeBase + SEND_JOB_RVA);
        if (target == home) {
            if (Q(vehicle + 0x4F0) == home || Q(vehicle + 0xC80) == home || Q(vehicle + 0x4A0)) return "it is at its office";
            if (!unpowered) {
                if (((SendHomeFn)(g_exeBase + SEND_HOME_RVA))(0, vehicle)) return nullptr;
                if (!special) {
                    back(world, vehicle, 0, 0, 1, 1);
                    return nullptr;
                }
            }
            if (!work(world, vehicle, found, home)) back(world, vehicle, 0, 0, 1, 1);
            return nullptr;
        }
        if (!type || !(*(float*)(type + 0x8624) > 0.0f || Q(vehicle + 0x4F0))) return "it cannot be sent";
        // Only the office's own sites (1d8c25..1d8c67).
        U64 first = Q(home + SEND_SITES_OFFSET), last = Q(home + SEND_SITES_OFFSET + 8);
        bool listed = false;
        if (first && last >= first && (last - first) % 8 == 0 && (last - first) / 8 <= 4096)
            for (U64 at = first; at < last && !listed; at += 8) listed = Q(at) == target;
        if (!listed) return "the site is not on its office's list";
        __declspec(align(16)) BYTE option[SEND_OPTION_BYTES];
        memset(option, 0, sizeof(option));
        int options = 0;
        ((SendOptionsFn)(g_exeBase + SEND_OPTIONS_RVA))(world, option, vehicle, home, target, index, &options);
        U64 r = (U64)option, jobAt = Q(r), sent = Q(r + 0x10);
        if (!(*(float*)(r + 0x2C) > 0.0f) || (!*(unsigned*)(r + 0x28) && !Q(r + 8)) || !sent) return "no work there";
        U64 coupled = Q(sent + 0x4A0);
        if (coupled) {
            if (!jobAt || !((SendCoupledFn)(g_exeBase + SEND_COUPLED_RVA))(sent, coupled + 0x488)) return "its coupled vehicle cannot go";
            *(unsigned*)(sent + 0x50C) = 8;
            *(unsigned*)(coupled + 0x50C) = 8;
            ((SendModeFn)(g_exeBase + SEND_MODE_RVA))(coupled, 1, 1);
            ((SendAssignFn)(g_exeBase + SEND_ASSIGN_RVA))(coupled, jobAt, -1, 0);
            *(unsigned*)(coupled + 0x698) = 0;
            *(BYTE*)(coupled + 0xD3A) = 1;
            return nullptr;
        }
        bool ok;
        if (special || unpowered) {
            if (found && Q(sent + 0x4F8) == Q(sent + 0x4F0) && Q(sent + 0x4F0) == Q(found + 0x4F0)) {
                *(U64*)(r + 0x18) = found;
                ok = job(world, option, home) != 0;
            } else {
                ok = work(world, sent, found, jobAt) != 0;
                if (special && !ok) ok = job(world, option, home) != 0;
            }
        } else {
            ok = job(world, option, home) != 0;
        }
        return ok ? nullptr : "the game refused it";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "a fault while sending it";
    }
}

// Kinds 5 to 7: the vehicle tools.
static bool ApplyVehicleTool(const LinkMsg& m)
{
    U64 world = (U64)(g_exeBase + WORLD_RVA), source = 0, target = 0;
    const char* why = nullptr;
    __try {
        source = m.kind == KIND_AUTO_REPLACE ? 0 : Ref_VehicleAt(m.source);
        target = m.kind == KIND_SEND ? Ref_BuildingAt(m.target) : Ref_VehicleAt(m.target);
        if (!target || (m.kind != KIND_AUTO_REPLACE && !source)) why = "it is gone";
        else if (m.kind == KIND_SEND ? (!Commands_BuildingInArea(target) || !Commands_VehicleInArea(source))
                                     : (!Commands_VehicleInArea(target) || (source && !Commands_VehicleInArea(source))))
            why = "it is outside the player's area";
        else if (source == target) why = "it is the same vehicle";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading them";
    }
    if (!why) {
        why = m.kind == KIND_AUTO_REPLACE ? AutoReplace(world, target)
              : m.kind == KIND_COPY_STOPS ? CopyStops(world, source, target)
                                          : SendConstruction(world, source, target, (int)m.mode);
    }
    Logf("republicmp  LINKS: %s #%d to #%d: %s", KindText(m.kind), m.source.place, m.target.place, why ? why : "made");
    return why == nullptr;
}

bool LinkTools_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(LinkMsg)) return false;
    LinkMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.kind > KIND_LAST) return false;
    if (m.kind >= KIND_AUTO_REPLACE) return KindReady(m.kind) && ApplyVehicleTool(m);
    if (m.kind == KIND_RELOCATE && m.mode > MODE_INMATE) return false;
    const char* why = nullptr;
    U64 source = 0, target = 0;
    unsigned sent = 0, moved = 0;
    if (!KindReady(m.kind)) why = "a game routine differs";
    __try {
        if (!why) {
            source = Ref_BuildingAt(m.source);
            target = Ref_BuildingAt(m.target);
            if (!source || !target) why = "a building is gone";
            // Weights are only copied from the source; a link changes both buildings.
            else if (!Commands_BuildingInArea(target) || (m.kind != KIND_WEIGHTS && !Commands_BuildingInArea(source)))
                why = "a building is outside the player's area";
            else if (source == target) why = "it is the same building";
            else if (m.kind <= KIND_DISTRIBUTION && Linked(source, target)) why = "they are linked already";
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading the buildings";
    }
    U64 world = (U64)(g_exeBase + WORLD_RVA);
    if (!why) {
        if (m.kind == KIND_FIRE) {
            if (!FireDispatch(world, source, target, &sent)) why = "the building does not burn here";
        } else if (m.kind == KIND_WEIGHTS) {
            if (!CopyWeights(source, target)) why = "a fault while copying";
        } else if (m.kind == KIND_RELOCATE) {
            if (!Relocate(world, source, target, m.mode, m.start, &moved)) why = "a fault while moving people";
        } else if (!AddLink(world, m.kind, source, target)) {
            why = "a fault while linking";
        }
    }
    if (m.kind == KIND_RELOCATE && !why)
        Logf("republicmp  LINKS: %s (mode %u) from building #%d to #%d: %u moved", KindText(m.kind), m.mode, m.source.place,
             m.target.place, moved);
    else if (m.kind == KIND_FIRE && !why)
        Logf("republicmp  LINKS: %s from building #%d to #%d: %u vehicle(s) sent", KindText(m.kind), m.source.place,
             m.target.place, sent);
    else
        Logf("republicmp  LINKS: %s from building #%d to #%d: %s", KindText(m.kind), m.source.place, m.target.place,
             why ? why : "made");
    return why == nullptr;
}
