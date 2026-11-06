// SPDX-License-Identifier: GPL-3.0-only
// See buoy_tool.h. Follows SoylentGreed's buoy route (RepublicLAN, read against
// the disassembly of 1.1.1.9).
#include "buoy_tool.h"
#include "commands.h"
#include <math.h>
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define HANDLER_RVA 0x3C85B0
#define FIND_RVA 0x3C8BA0     // the first node closer than a radius, or -1
#define TEST_RVA 0x3C8C60     // nonzero: a buoy cannot stand here
#define REMOVE_RVA 0x3C9060
#define ALLOC_RVA 0x84DECC    // operator new
#define NODE_RVA 0xFF950
#define SIGNAL_RVA 0x419410
#define PUSH_RVA 0xB0E90
#define SOUND_RVA 0x3FFAD0
#define HEIGHT_IMPORT 0x86C058 // C3D_TERRAIN::CollisionTerrainHeightOverPoint
#define ENGINE_RVA 0x9941F0
#define ENGINE_TERRAIN 0xED8
#define PRESS_RVA 0xA54E91
#define CURSOR_OFFSET 0xF6C
#define NETWORK_OFFSET 0xFA28
#define NODES_OFFSET 0x298
#define NODE_BYTES 0xE8
#define NODE_KIND 0xB
#define MAX_NODES (1u << 20)
#define WATER_LINE -1.0f
#define PLACE_RADIUS 50.0f
#define REMOVE_RADIUS 25.0f
#define MAGIC 0x31595542
static const unsigned kMenus[3] = { 0xFBC, 0xFBE, 0xFC3 };

struct Routine {
    unsigned rva;
    unsigned char entry[16];
};
static const Routine kRoutines[] = {
    { HANDLER_RVA, { 0x40, 0x55, 0x53, 0x56, 0x48, 0x8d, 0xac, 0x24, 0xc0, 0xe5, 0xff, 0xff, 0xb8, 0x40, 0x1b, 0x00 } },
    { FIND_RVA, { 0x48, 0x83, 0xec, 0x18, 0x4c, 0x8b, 0x89, 0x28, 0xfa, 0x00, 0x00, 0x45, 0x33, 0xc0, 0x0f, 0x28 } },
    { TEST_RVA, { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1 } },
    { REMOVE_RVA, { 0x85, 0xd2, 0x0f, 0x88, 0xb9, 0x00, 0x00, 0x00, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x63, 0xc2 } },
    { ALLOC_RVA, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xeb, 0x21, 0x48, 0x8b, 0xcb, 0xe8, 0xd5 } },
    { NODE_RVA, { 0x48, 0x83, 0xec, 0x18, 0x33, 0xd2, 0x0f, 0x57, 0xc9, 0x48, 0x89, 0x51, 0x10, 0x0f, 0x28, 0xc1 } },
    { SIGNAL_RVA, { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8b, 0xec, 0x48 } },
    { PUSH_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x41, 0x08, 0x48, 0x8b } },
    { SOUND_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x30, 0x80 } },
};

typedef U64 (*HandlerFn)(U64 world, U64 mode);
typedef int (*FindFn)(U64 world, const float* point, float radius);
typedef unsigned char (*TestFn)(U64 world, const float* point, float radius);
typedef void (*RemoveFn)(U64 world, int index);
typedef U64 (*AllocFn)(size_t bytes);
typedef U64 (*NodeFn)(U64 node);
typedef void (*SignalFn)(U64 world, U64 node, unsigned char a3, unsigned char a4, unsigned char a5);
typedef void (*PushFn)(U64 vector, const U64* value);
typedef void (*SoundFn)(U64 world, const char* name, unsigned char a3);
typedef float (*HeightFn)(U64 terrain, const float* point);

static int g_ready = -1;

void BuoyTool_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

bool BuoyTool_Ready()
{
    if (g_ready < 0) {
        g_ready = 1;
        for (const Routine& r : kRoutines)
            if (memcmp(g_exeBase + r.rva, r.entry, sizeof(r.entry)) != 0) {
                Logf("republicmp  BUOY: game routine at 0x%X differs - the buoy tools are put away in lockstep", r.rva);
                g_ready = 0;
                break;
            }
    }
    return g_ready == 1;
}

static U64 Q(U64 at) { return *(U64*)at; }

// The buoy network's nodes: begin and count (false: not readable).
static bool Nodes(U64 world, U64& begin, unsigned& count)
{
    U64 network = Q(world + NETWORK_OFFSET);
    begin = 0;
    count = 0;
    if (!network) return false;
    U64 b = Q(network + NODES_OFFSET), e = Q(network + NODES_OFFSET + 8);
    if (e < b || (e - b) % 8 || (e - b) / 8 > MAX_NODES) return false;
    begin = b;
    count = (unsigned)((e - b) / 8);
    return true;
}

// The handler's decision for a press at the cursor; nullptr: nothing to send.
static bool Decide(U64 world, unsigned mode, BuoyMsg& m)
{
    __try {
        memset(&m, 0, sizeof(m));
        m.magic = MAGIC;
        m.mode = mode;
        m.index = -1;
        memcpy(m.point, (const void*)(world + CURSOR_OFFSET), 12);
        U64 begin = 0;
        if (!Nodes(world, begin, m.nodes)) return false;
        FindFn find = (FindFn)(g_exeBase + FIND_RVA);
        if (mode == 1) {
            int i = find(world, m.point, REMOVE_RADIUS);
            if (i < 0 || (unsigned)i >= m.nodes) return false;
            m.index = i;
            memcpy(m.point, (const void*)(Q(begin + (U64)i * 8) + 4), 12);
            return true;
        }
        U64 engine = Q((U64)(g_exeBase + ENGINE_RVA));
        U64 terrain = engine ? Q(engine + ENGINE_TERRAIN) : 0;
        if (!terrain) return false;
        HeightFn height = *(HeightFn*)(g_exeBase + HEIGHT_IMPORT);
        if (!(height(terrain, m.point) < WATER_LINE)) return false;
        if (find(world, m.point, PLACE_RADIUS) >= 0) return false;
        return ((TestFn)(g_exeBase + TEST_RVA))(world, m.point, PLACE_RADIUS) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void PlaySoundHere(U64 world)
{
    __try {
        ((SoundFn)(g_exeBase + SOUND_RVA))(world, "building_buoy", 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

U64 BuoyTool_Run(U64 world, U64 mode)
{
    HandlerFn handler = (HandlerFn)(g_exeBase + HANDLER_RVA);
    BYTE* w = (BYTE*)world;
    BYTE* press = g_exeBase + PRESS_RVA;
    unsigned m32 = (unsigned)mode;
    bool menus = w[kMenus[0]] || w[kMenus[1]] || w[kMenus[2]];
    bool refused = false;
    if (!*press || menus || m32 > 1 || !Commands_TakeLocal() || !BuoyTool_Ready()) {
        // Outside the lockstep (a Republics host alone) the area rule is checked here.
        refused = *press && !menus && Commands_PointRefusedHere((const float*)(w + CURSOR_OFFSET));
        if (!refused) return handler(world, mode);
    }
    if (!refused) {
        BuoyMsg m;
        if (Decide(world, m32, m)) {
            Commands_Submit(LS_CMD_BUOY, &m, sizeof(m));
            if (m32 == 0) PlaySoundHere(world);
        }
    }
    // The handler with the press hidden: its preview, nothing made here now.
    BYTE kept = *press;
    *press = 0;
    U64 r = handler(world, mode);
    *press = kept;
    return r;
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

// The handler's placement steps (3c88d2..), without the sound.
static bool Place(U64 world, const float point[3])
{
    __try {
        U64 network = Q(world + NETWORK_OFFSET);
        if (!network) return false;
        U64 node = ((NodeFn)(g_exeBase + NODE_RVA))(((AllocFn)(g_exeBase + ALLOC_RVA))(NODE_BYTES));
        if (!node) return false;
        memcpy((void*)(node + 4), point, 12);
        *(unsigned*)(node + 0xE0) = NODE_KIND;
        ((SignalFn)(g_exeBase + SIGNAL_RVA))(world, node, 0, 1, 0);
        *(U64*)(node + 0xD0) = 0;
        *(U64*)(node + 0xD8) = 0;
        ((PushFn)(g_exeBase + PUSH_RVA))(network + NODES_OFFSET, &node);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The node named by the command: at its place, else the one at its position
// (a removal the clicking PC had not seen yet shifts the list alike on every PC).
static int Named(U64 world, const BuoyMsg& m)
{
    __try {
        U64 begin = 0;
        unsigned count = 0;
        if (!Nodes(world, begin, count)) return -1;
        if ((unsigned)m.index < count && memcmp((const void*)(Q(begin + (U64)m.index * 8) + 4), m.point, 12) == 0)
            return m.index;
        for (unsigned i = 0; i < count; i++)
            if (memcmp((const void*)(Q(begin + (U64)i * 8) + 4), m.point, 12) == 0) return (int)i;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

static bool Remove(U64 world, int index)
{
    __try {
        ((RemoveFn)(g_exeBase + REMOVE_RVA))(world, index);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool BuoyTool_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(BuoyMsg)) return false;
    BuoyMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.mode > 1 || !BuoyTool_Ready()) return false;
    for (float f : m.point)
        if (!isfinite(f) || fabsf(f) > 1.0e6f) return false;
    U64 world = (U64)(g_exeBase + WORLD_RVA);
    const char* result;
    if (m.mode == 0) {
        result = Place(world, m.point) ? nullptr : "a fault while making it";
    } else {
        int index = Named(world, m);
        result = index < 0 ? "no buoy is at that position here" : Remove(world, index) ? nullptr : "a fault while removing it";
    }
    Logf("republicmp  BUOY: %s at (%.2f,%.2f,%.2f): %s", m.mode == 0 ? "placed" : "removed", m.point[0], m.point[1], m.point[2],
         result ? result : "made");
    return result == nullptr;
}
