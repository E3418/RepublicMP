// SPDX-License-Identifier: GPL-3.0-only
// See link_tools.h. Follows SoylentGreed's link tool route (RepublicLAN, link
// tool kinds 0 and 1, read against the disassembly of 1.1.1.9).
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
#define MAGIC 0x314B4E4C

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

typedef U64 (*ToolFn)(U64 world, U64 a2);
typedef U64 (*MarkFn)(U64 world, U64 target, U64 a3, U64 refused, U64 a5);
typedef void* (*AllocFn)(size_t bytes);
typedef void (*PushFn)(U64 vector, const U64* value);
typedef U64 (*RecordFn)(U64 record);
typedef void (*SharesFn)(U64 world, U64 source, float* load, float* unload, unsigned char a5);

#pragma pack(push, 1)
struct LinkMsg {
    unsigned magic;
    unsigned kind; // 0 station, 1 distribution
    RouteRef source;
    RouteRef target;
};
#pragma pack(pop)

static int g_ready = -1;

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

static const char* KindText(unsigned kind) { return kind ? "distribution link" : "station link"; }

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
    ToolFn tool = (ToolFn)(g_exeBase + (kind ? DIST_TOOL : STATION_TOOL));
    BYTE* press = g_exeBase + PRESS_RVA;
    if (!*press || !Commands_TakeLocal() || !LinkTools_Ready()) return tool(world, a2);
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

U64 LinkTools_Station(U64 world, U64 a2) { return Run(0, world, a2); }
U64 LinkTools_Distribution(U64 world, U64 a2) { return Run(1, world, a2); }

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

bool LinkTools_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(LinkMsg)) return false;
    LinkMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.kind > 1) return false;
    const char* why = nullptr;
    U64 source = 0, target = 0;
    if (!LinkTools_Ready()) why = "a game routine differs";
    __try {
        if (!why) {
            source = Ref_BuildingAt(m.source);
            target = Ref_BuildingAt(m.target);
            if (!source || !target) why = "a building is gone";
            else if (source == target) why = "it is the same building";
            else if (Linked(source, target)) why = "they are linked already";
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading the buildings";
    }
    if (!why && !AddLink((U64)(g_exeBase + WORLD_RVA), m.kind, source, target)) why = "a fault while linking";
    Logf("republicmp  LINKS: %s from building #%d to #%d: %s", KindText(m.kind), m.source.place, m.target.place,
         why ? why : "made");
    return why == nullptr;
}
