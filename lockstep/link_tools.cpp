// SPDX-License-Identifier: GPL-3.0-only
// See link_tools.h. Follows SoylentGreed's link tool route (RepublicLAN, link
// tool kinds 0 to 3, read against the disassembly of 1.1.1.9).
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

enum { KIND_STATION = 0, KIND_DISTRIBUTION = 1, KIND_FIRE = 2, KIND_WEIGHTS = 3 };

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

typedef U64 (*ToolFn)(U64 world, U64 a2);
typedef U64 (*MarkFn)(U64 world, U64 target, U64 a3, U64 refused, U64 a5);
typedef void* (*AllocFn)(size_t bytes);
typedef void (*PushFn)(U64 vector, const U64* value);
typedef U64 (*RecordFn)(U64 record);
typedef void (*SharesFn)(U64 world, U64 source, float* load, float* unload, unsigned char a5);
typedef double (*ReadinessFn)(U64 vehicle);
typedef void (*DispatchFn)(U64 a1, U64 fire, U64 vehicle);

#pragma pack(push, 1)
struct LinkMsg {
    unsigned magic;
    unsigned kind; // KIND_*
    RouteRef source;
    RouteRef target;
};
#pragma pack(pop)

static int g_ready = -1, g_pickReady = -1;

void LinkTools_Bind(const TsmHost* host)
{
    TsmBind(host);
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

static bool KindReady(unsigned kind) { return kind >= KIND_FIRE ? LinkTools_PickReady() : LinkTools_Ready(); }

static const char* KindText(unsigned kind)
{
    switch (kind) {
    case KIND_STATION: return "station link";
    case KIND_DISTRIBUTION: return "distribution link";
    case KIND_FIRE: return "fire station dispatch";
    case KIND_WEIGHTS: return "weights copy";
    }
    return "?";
}

static unsigned ToolRva(unsigned kind)
{
    switch (kind) {
    case KIND_DISTRIBUTION: return DIST_TOOL;
    case KIND_FIRE: return FIRE_TOOL;
    case KIND_WEIGHTS: return WEIGHTS_TOOL;
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
        if (!source || !Ref_Building(source, m.source)) why = "the window's building is not in the world's list";
        else if (!Ref_Building(g_target, m.target)) why = "the picked building is not in the world's list";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading it";
    }
    if (why)
        Logf("republicmp  LINKS: %s not shared - %s; nothing is done", KindText(kind), why);
    else
        Commands_Submit(LS_CMD_LINK, &m, sizeof(m));
    return r;
}

U64 LinkTools_Station(U64 world, U64 a2) { return Run(KIND_STATION, world, a2); }
U64 LinkTools_Distribution(U64 world, U64 a2) { return Run(KIND_DISTRIBUTION, world, a2); }
U64 LinkTools_Fire(U64 world, U64 a2) { return Run(KIND_FIRE, world, a2); }
U64 LinkTools_Weights(U64 world, U64 a2) { return Run(KIND_WEIGHTS, world, a2); }

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

bool LinkTools_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(LinkMsg)) return false;
    LinkMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.kind > KIND_WEIGHTS) return false;
    const char* why = nullptr;
    U64 source = 0, target = 0;
    unsigned sent = 0;
    if (!KindReady(m.kind)) why = "a game routine differs";
    __try {
        if (!why) {
            source = Ref_BuildingAt(m.source);
            target = Ref_BuildingAt(m.target);
            if (!source || !target) why = "a building is gone";
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
        } else if (!AddLink(world, m.kind, source, target)) {
            why = "a fault while linking";
        }
    }
    if (m.kind == KIND_FIRE && !why)
        Logf("republicmp  LINKS: %s from building #%d to #%d: %u vehicle(s) sent", KindText(m.kind), m.source.place,
             m.target.place, sent);
    else
        Logf("republicmp  LINKS: %s from building #%d to #%d: %s", KindText(m.kind), m.source.place, m.target.place,
             why ? why : "made");
    return why == nullptr;
}
