// SPDX-License-Identifier: GPL-3.0-only
// See cancel_plans.h. Follows SoylentGreed's cancel plans route (RepublicLAN,
// read against the disassembly of 1.1.1.9).
#include "cancel_plans.h"
#include "commands.h"
#include <initializer_list>
#include <limits.h>
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define PANEL_SITE 0x39F2AB  // call [GetMouseLeftPress] (6 bytes): the network panel
#define MENU_SITE 0x0883C8   // the construction menu
#define PRESS_SLOT 0x86C8B8  // C3D_INPUT::GetMouseLeftPress
#define REMOVE_RVA 0x5434C0  // 5434c0(network, edge, 0, -1, 0)
#define AREA_A_RVA 0x12D990  // (world, min, max): x and z read
#define AREA_B_RVA 0x12F010
#define WATER_RVA 0x592990   // 592990([world+fa38], 0)
#define BOX_MIN_RVA 0x90B1DC // 1e5
#define BOX_MAX_RVA 0x90BD40 // -1e5
#define SCENARIO 0x13178     // world: -1 no scenario (otherwise the press opens a window)
#define WATER_FIELD 0xFA38
#define NETWORK_FIRST 0xF700 // world: the network fields
#define NETWORK_END 0xFC00
#define EDGES 0x2B0
#define EDGE_PLANNED 0x140
#define EDGE_A 0x20
#define EDGE_B 0x28
#define NODE_X 4
#define NODE_Z 0xC
#define NETWORK_MODE 0x258
#define LIST_BEGIN (-0x10)   // the menu's frame: its networks (begin, end) and count [rbp+18]
#define LIST_COUNT 0x18
#define MAX_NETWORKS 16
#define MAX_EDGES (1u << 20)
#define MAGIC 0x31504143

struct Checked {
    unsigned begin, end;
    unsigned long long fnv;
};
static const Checked kBlocks[] = {
    { 0x39F2A4, 0x39F531, 0xa43ad673688f2271ull }, // the panel's press and loop
    { 0x0883C1, 0x08876A, 0x1b55793a216a2305ull }, // the menu's
    { 0x0874A5, 0x0874E9, 0xc0e7ee451e19f9f0ull }, // the menu's list count
};
static const unsigned char kEntries[4][16] = {
    { 0x48, 0x8b, 0xc4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d },
    { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1 },
    { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xc8 },
    { 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 },
};
static const unsigned kEntryRvas[4] = { REMOVE_RVA, AREA_A_RVA, AREA_B_RVA, WATER_RVA };

#pragma pack(push, 1)
struct CancelPlansMsg {
    unsigned magic;
    unsigned scope; // 0 the panel (one network), 1 the menu
    unsigned count;
    unsigned short field[MAX_NETWORKS]; // each network's world field, in the button's order
};
#pragma pack(pop)

struct Vector {
    float x, y, z, w;
};

typedef BYTE (*PressFn)(void* input);
typedef void (*RemoveFn)(U64 network, U64 edge, U64 a3, int a4, BYTE a5);
typedef void (*AreaFn)(U64 world, const Vector* min, const Vector* max);
typedef void (*WaterFn)(U64 field, unsigned a2);

static PressFn g_press = nullptr;

void CancelPlans_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

static U64 Q(U64 at) { return *(U64*)at; }

static unsigned NetworkField(U64 world, U64 network)
{
    for (unsigned f = NETWORK_FIRST; f < NETWORK_END; f += 8)
        if (network && Q(world + f) == network) return f;
    return 0;
}

// The press (the interface thread): a held press answers "no press".
static BYTE Take(void* input, unsigned scope, U64 context, U64 world)
{
    BYTE pressed = g_press ? g_press(input) : 0;
    if (!pressed || !Commands_TakeUi()) return pressed;
    CancelPlansMsg m = {};
    m.magic = MAGIC;
    m.scope = scope;
    const char* why = nullptr;
    __try {
        if (world != (U64)(g_exeBase + WORLD_RVA)) return pressed;
        if (*(int*)(world + SCENARIO) != -1) return pressed; // the scenario's window: interface only
        U64 networks[MAX_NETWORKS] = {};
        if (scope == 0) {
            m.count = 1;
            networks[0] = context;
        } else {
            U64 begin = Q(context + LIST_BEGIN), end = Q(context + LIST_BEGIN + 8), count = Q(context + LIST_COUNT);
            if (!begin || end < begin || (end - begin) % 8 || (end - begin) / 8 != count || !count || count > MAX_NETWORKS)
                why = "the menu's networks";
            else {
                m.count = (unsigned)count;
                memcpy(networks, (void*)begin, (size_t)count * 8);
            }
        }
        for (unsigned i = 0; i < m.count && !why; i++) {
            unsigned f = NetworkField(world, networks[i]);
            if (!f) why = "a network is not one of the world's";
            m.field[i] = (unsigned short)f;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    if (why) Logf("republicmp  CANCEL PLANS: the press is not carried (%s) - nothing is done", why);
    else Commands_Submit(LS_CMD_CANCEL_PLANS, &m, sizeof(m));
    return 0;
}

static __declspec(noinline) BYTE DetourPanel(void* input, U64 network, U64 world) { return Take(input, 0, network, world); }
static __declspec(noinline) BYTE DetourMenu(void* input, U64 frame, U64 world) { return Take(input, 1, frame, world); }

bool CancelPlans_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(CancelPlansMsg)) return false;
    CancelPlansMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.scope > 1 || !m.count || m.count > MAX_NETWORKS || (m.scope == 0 && m.count != 1)) return false;
    for (unsigned i = 0; i < m.count; i++)
        if (m.field[i] < NETWORK_FIRST || m.field[i] >= NETWORK_END || m.field[i] % 8) return false;
    const char* why = nullptr;
    unsigned removed = 0;
    __try {
        U64 world = (U64)(g_exeBase + WORLD_RVA);
        RemoveFn remove = (RemoveFn)(g_exeBase + REMOVE_RVA);
        float boxMin = *(float*)(g_exeBase + BOX_MIN_RVA), boxMax = *(float*)(g_exeBase + BOX_MAX_RVA);
        // One box for the press; the networks from last to first, each one's plans from last to first.
        Vector mn = { boxMin, 0.f, boxMin, 0.f }, mx = { boxMax, 0.f, boxMax, 0.f };
        for (int k = (int)m.count - 1; k >= 0 && !why; --k) {
            U64 net = Q(world + m.field[k]);
            if (!net) continue;
            U64 begin = Q(net + EDGES), end = Q(net + EDGES + 8);
            if (end < begin || (end - begin) % 8 || (end - begin) / 8 > MAX_EDGES) {
                why = "a network's edges";
                break;
            }
            for (long long i = (long long)((end - begin) / 8) - 1; i >= 0; --i) {
                begin = Q(net + EDGES);
                end = Q(net + EDGES + 8);
                if ((U64)i >= (end - begin) / 8) continue;
                U64 edge = Q(begin + (U64)i * 8);
                if (!edge || !*(BYTE*)(edge + EDGE_PLANNED)) continue;
                // The other republic's plans are theirs (Republics): left as they are.
                if (!Commands_PointInArea((const float*)(Q(edge + EDGE_A) + NODE_X), false) ||
                    !Commands_PointInArea((const float*)(Q(edge + EDGE_B) + NODE_X), false))
                    continue;
                for (unsigned e : { (unsigned)EDGE_A, (unsigned)EDGE_B }) {
                    U64 node = Q(edge + e);
                    float x = *(float*)(node + NODE_X), z = *(float*)(node + NODE_Z);
                    if (mn.x > x) mn.x = x;
                    if (mn.z > z) mn.z = z;
                    if (x > mx.x) mx.x = x;
                    if (z > mx.z) mx.z = z;
                }
                remove(net, edge, 0, -1, 0);
                ++removed;
            }
            unsigned mode = *(unsigned*)(net + NETWORK_MODE);
            if (!(mode & ~4u)) {
                Vector a = mn, b = mx, c = mn, d = mx;
                ((AreaFn)(g_exeBase + AREA_A_RVA))(world, &a, &b);
                ((AreaFn)(g_exeBase + AREA_B_RVA))(world, &c, &d);
            }
            if (mode == 0xD) ((WaterFn)(g_exeBase + WATER_RVA))(Q(world + WATER_FIELD), 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    Logf("republicmp  CANCEL PLANS: %s (%u planned piece(s) removed)", why ? why : "made", removed);
    return why == nullptr;
}

bool CancelPlans_Install()
{
    for (const Checked& c : kBlocks) {
        unsigned long long h = 0xcbf29ce484222325ull;
        for (unsigned a = c.begin; a < c.end; a++) h = (h ^ g_exeBase[a]) * 0x100000001b3ull;
        if (h != c.fnv) {
            Logf("republicmp  CANCEL PLANS: the button's code at 0x%X differs - it acts on the clicking PC only", c.begin);
            return false;
        }
    }
    for (int i = 0; i < 4; i++)
        if (memcmp(g_exeBase + kEntryRvas[i], kEntries[i], 16)) {
            Logf("republicmp  CANCEL PLANS: the routine 0x%X differs - the button acts on the clicking PC only", kEntryRvas[i]);
            return false;
        }
    const unsigned sites[2] = { PANEL_SITE, MENU_SITE };
    for (unsigned site : sites) {
        BYTE* at = g_exeBase + site;
        int rel = 0;
        memcpy(&rel, at + 2, 4);
        if (at[0] != 0xFF || at[1] != 0x15 || site + 6 + rel != PRESS_SLOT) {
            Logf("republicmp  CANCEL PLANS: the press at 0x%X differs - the button acts on the clicking PC only", site);
            return false;
        }
    }
    if (*(float*)(g_exeBase + BOX_MIN_RVA) != 1e5f || *(float*)(g_exeBase + BOX_MAX_RVA) != -1e5f) return false;
    g_press = *(PressFn*)(g_exeBase + PRESS_SLOT);
    if (!g_press) return false;
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) return false;
    DWORD old;
    if (!VirtualProtect(relay, 64, PAGE_EXECUTE_READWRITE, &old)) return false;
    // mov rdx,<context>; mov r8,<world>; mov rax,detour; jmp rax
    static const unsigned char kPanel[6] = { 0x4c, 0x89, 0xea, 0x49, 0x89, 0xf0 }; // rdx = r13 (the network), r8 = rsi
    static const unsigned char kMenu[6] = { 0x48, 0x89, 0xea, 0x4d, 0x89, 0xe8 };  // rdx = rbp (the frame), r8 = r13
    const unsigned char* heads[2] = { kPanel, kMenu };
    U64 detours[2] = { (U64)&DetourPanel, (U64)&DetourMenu };
    for (int i = 0; i < 2; i++) {
        BYTE* r = relay + 32 * i;
        memcpy(r, heads[i], 6);
        r[6] = 0x48;
        r[7] = 0xb8;
        memcpy(r + 8, &detours[i], 8);
        r[16] = 0xff;
        r[17] = 0xe0;
    }
    VirtualProtect(relay, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, 64);
    for (int i = 0; i < 2; i++) {
        BYTE* at = g_exeBase + sites[i];
        long long to = (long long)(relay + 32 * i) - (long long)(at + 5);
        if (to < INT_MIN || to > INT_MAX) return false;
        unsigned char code[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        int rel32 = (int)to;
        memcpy(code + 1, &rel32, 4);
        DWORD prot;
        VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &prot);
        memcpy(at, code, 6);
        VirtualProtect(at, 6, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), at, 6);
    }
    return true;
}
