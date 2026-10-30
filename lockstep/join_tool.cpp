// SPDX-License-Identifier: GPL-3.0-only
// See join_tool.h. Follows SoylentGreed's join route (RepublicLAN, read
// against the disassembly of 1.1.1.9).
#include "join_tool.h"
#include "commands.h"
#include "../world/view_list_scope.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define HANDLER_RVA 0x3C9A00
#define PRESS_RVA 0xA54E91        // byte: left button pressed this frame
#define DOWN_RVA 0xA54E90         // byte: left button held
#define CAMERA_RVA 0x9E9D10       // float x, y, z: the camera the handler's ray starts at
#define CURSOR_OFFSET 0xF6C       // world: the cursor on the terrain
#define RAY_END_OFFSET 0xF60      // world: the end of the cursor's ray
#define TABLE_COUNT_OFFSET 0x14034 // world: int, the networks in the table
#define TABLE_FIRST_OFFSET 0x14038 // world: 0x50-byte entries, the network first
#define TABLE_STRIDE 0x50
#define MAX_NETWORKS 64
#define NODES 0x298               // network: committed nodes (begin, end)
#define EDGES 0x2B0               // network: committed edges
#define NODE_EDGES 0x28           // node: its edges (begin, end)
#define NODE_POS 4                // node: float x, y, z
#define REFUSAL_OFFSET 0x318      // network: int refusal (3 = cannot join) and +31c its text
#define MAX_LIST (1u << 20)
#define CAMERA_HEIGHT 50.0f
#define SCOPE_RADIUS 1.0f
#define MAGIC 0x3142484A
static const unsigned kHover[3] = { 0xF648, 0xF650, 0xF6A0 };
static const unsigned kMenus[3] = { 0xFBC, 0xFBE, 0xFC3 };
static const unsigned char kEntry[16] = { 0x88, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24,
                                          0x08, 0x55, 0x56, 0x57, 0x48, 0x8d, 0x6c, 0x24 };

#pragma pack(push, 1)
struct JoinMsg {
    unsigned magic;
    unsigned table, networks; // the network's place in the world's table, the table's size
    unsigned node, nodes;     // the node's place in the network's +298, the list's size
    float point[3];           // the node's position (bits compared)
};
#pragma pack(pop)

typedef U64 (*HandlerFn)(U64 world, U64 mode);

static int g_ready = -1;

void JoinTool_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool JoinTool_Ready()
{
    if (g_ready < 0) {
        g_ready = memcmp(g_exeBase + HANDLER_RVA, kEntry, sizeof(kEntry)) == 0 ? 1 : 0;
        if (!g_ready) Logf("republicmp  JOIN: the join tool's routine differs - the tool is put away in lockstep");
    }
    return g_ready == 1;
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

static unsigned Networks(U64 world, U64* out)
{
    int count = *(int*)(world + TABLE_COUNT_OFFSET);
    if (count <= 0 || count > MAX_NETWORKS) return 0;
    for (int i = 0; i < count; i++) out[i] = Q(world + TABLE_FIRST_OFFSET + (U64)i * TABLE_STRIDE);
    return (unsigned)count;
}

// A vector of pointers: begin and count (false: not one, or too long).
static bool List(U64 at, U64& begin, unsigned& count)
{
    U64 b = Q(at), e = Q(at + 8);
    begin = b;
    count = 0;
    if (!b && !e) return true;
    if (!b || e < b || (e - b) % 8 || (e - b) / 8 > MAX_LIST) return false;
    count = (unsigned)((e - b) / 8);
    return true;
}

static int IndexOf(U64 list, U64 value)
{
    U64 b = 0;
    unsigned n = 0;
    if (!value || !List(list, b, n)) return -1;
    for (unsigned i = 0; i < n; i++)
        if (Q(b + (U64)i * 8) == value) return (int)i;
    return -1;
}

// The node the handler picked (+f650) as a command; nullptr, or why not.
static const char* Encode(U64 world, JoinMsg& m)
{
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    U64 node = Q(world + kHover[1]);
    if (!node) return "no node is picked";
    U64 nets[MAX_NETWORKS] = {};
    unsigned n = Networks(world, nets);
    if (!n) return "the network table is unreadable";
    for (unsigned t = 0; t < n; t++) {
        if (!nets[t]) continue;
        int i = IndexOf(nets[t] + NODES, node);
        if (i < 0) continue;
        if (*(int*)(nets[t] + REFUSAL_OFFSET) == 3) return "the game refuses to join these edges";
        U64 eb = 0, nb = 0;
        unsigned ec = 0, nc = 0;
        if (!List(node + NODE_EDGES, eb, ec) || ec != 2) return "the node does not have two edges";
        if (!List(nets[t] + NODES, nb, nc)) return "the network's nodes are unreadable";
        memcpy(m.point, (const void*)(node + NODE_POS), 12);
        m.table = t;
        m.networks = n;
        m.node = (unsigned)i;
        m.nodes = nc;
        return nullptr;
    }
    return "the node is in no network of the table";
}

static const char* GuardedEncode(U64 world, JoinMsg& m)
{
    __try {
        return Encode(world, m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "a fault while reading the pick";
    }
}

U64 JoinTool_Run(U64 world, U64 mode)
{
    HandlerFn handler = (HandlerFn)(g_exeBase + HANDLER_RVA);
    BYTE* w = (BYTE*)world;
    BYTE* press = g_exeBase + PRESS_RVA;
    bool menus = w[kMenus[0]] || w[kMenus[1]] || w[kMenus[2]];
    if (!*press || menus || !Commands_TakeLocal() || !JoinTool_Ready()) {
        static int s_logged = 0;
        if (*press && !menus && Commands_Capturing() && s_logged < 16) {
            ++s_logged;
            Logf("republicmp  JOIN: a click made on this PC only - %s",
                 !JoinTool_Ready() ? "the tool is not taken over" : "a command or a tick was being made");
        }
        return handler(world, mode);
    }
    // The handler with the click hidden: its pick and highlight, nothing joined here.
    BYTE kept = *press;
    *press = 0;
    U64 r = handler(world, mode);
    *press = kept;
    JoinMsg m;
    const char* why = GuardedEncode(world, m);
    if (why) Logf("republicmp  JOIN: not shared - %s; nothing is done", why);
    else Commands_Submit(LS_CMD_JOIN, &m, sizeof(m));
    return r;
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

// The words the handler reads or writes, this PC's own (put back after).
struct OwnState {
    float cursor[3], rayEnd[3], camera[3];
    U64 hover[3];
    BYTE press, down, menus[3];
    int refusal[MAX_NETWORKS][2];
    unsigned networks;
    U64 nets[MAX_NETWORKS];
};

static void ReadOwn(U64 world, OwnState& s)
{
    BYTE* w = (BYTE*)world;
    s.networks = Networks(world, s.nets);
    for (unsigned i = 0; i < s.networks; i++)
        if (s.nets[i]) memcpy(s.refusal[i], (const void*)(s.nets[i] + REFUSAL_OFFSET), 8);
    memcpy(s.cursor, w + CURSOR_OFFSET, 12);
    memcpy(s.rayEnd, w + RAY_END_OFFSET, 12);
    memcpy(s.camera, g_exeBase + CAMERA_RVA, 12);
    s.press = g_exeBase[PRESS_RVA];
    s.down = g_exeBase[DOWN_RVA];
    for (int i = 0; i < 3; i++) s.menus[i] = w[kMenus[i]];
    for (int k = 0; k < 3; k++) s.hover[k] = Q(world + kHover[k]);
}

// A hover word that no longer names a node or an edge of the table is
// cleared (the join freed one). Returns how many were cleared.
static int WriteOwn(U64 world, const OwnState& s)
{
    BYTE* w = (BYTE*)world;
    memcpy(w + CURSOR_OFFSET, s.cursor, 12);
    memcpy(w + RAY_END_OFFSET, s.rayEnd, 12);
    memcpy(g_exeBase + CAMERA_RVA, s.camera, 12);
    g_exeBase[PRESS_RVA] = s.press;
    g_exeBase[DOWN_RVA] = s.down;
    for (int i = 0; i < 3; i++) w[kMenus[i]] = s.menus[i];
    for (unsigned i = 0; i < s.networks; i++)
        if (s.nets[i]) memcpy((void*)(s.nets[i] + REFUSAL_OFFSET), s.refusal[i], 8);
    int cleared = 0;
    for (int k = 0; k < 3; k++) {
        U64 v = s.hover[k];
        bool known = !v;
        for (unsigned i = 0; i < s.networks && !known; i++)
            known = s.nets[i] && (IndexOf(s.nets[i] + NODES, v) >= 0 || IndexOf(s.nets[i] + EDGES, v) >= 0);
        if (!known) {
            v = 0;
            ++cleared;
        }
        *(U64*)(world + kHover[k]) = v;
    }
    return cleared;
}

// The node the command names here: the same table, the node with the same
// position. The list's order can differ between PCs (one PC's own previews
// add and take nodes), so a node not at the sender's place is looked for by
// its position in the whole list; *moved is its place here then.
static const char* Decode(U64 world, const JoinMsg& m, U64& network, U64& node, int* moved)
{
    network = node = 0;
    *moved = -1;
    U64 nets[MAX_NETWORKS] = {};
    unsigned n = Networks(world, nets);
    if (n != m.networks || m.table >= n || !nets[m.table]) return "the network table differs here";
    U64 b = 0;
    unsigned c = 0;
    if (!List(nets[m.table] + NODES, b, c)) return "the network's nodes are unreadable here";
    U64 at = m.node < c ? Q(b + (U64)m.node * 8) : 0;
    if (!at || memcmp((const void*)(at + NODE_POS), m.point, 12) != 0) {
        at = 0;
        float nearest = 1.0e30f;
        for (unsigned i = 0; i < c; i++) {
            U64 p = Q(b + (U64)i * 8);
            if (!p) continue;
            const float* pos = (const float*)(p + NODE_POS);
            float dx = pos[0] - m.point[0], dy = pos[1] - m.point[1], dz = pos[2] - m.point[2];
            float d = dx * dx + dy * dy + dz * dz;
            if (d < nearest) nearest = d;
            if (memcmp(pos, m.point, 12) != 0) continue;
            if (at) return "two nodes are at that position here";
            at = p;
            *moved = (int)i;
        }
        if (!at) {
            static char why[96];
            _snprintf_s(why, sizeof(why), _TRUNCATE, "no node is at that position here (the nearest %.4f m)", sqrtf(nearest));
            return why;
        }
    }
    network = nets[m.table];
    node = at;
    return nullptr;
}

// The click frame on the node, then the handler (no C++ objects: SEH).
static bool RunJoin(U64 world, const float point[3])
{
    __try {
        BYTE* w = (BYTE*)world;
        float camera[3] = { point[0], point[1] + CAMERA_HEIGHT, point[2] };
        memcpy(w + CURSOR_OFFSET, point, 12);
        memcpy(w + RAY_END_OFFSET, point, 12);
        memcpy(g_exeBase + CAMERA_RVA, camera, 12);
        g_exeBase[PRESS_RVA] = 1;
        g_exeBase[DOWN_RVA] = 0;
        for (int i = 0; i < 3; i++) w[kMenus[i]] = 0;
        for (int k = 0; k < 3; k++) *(U64*)(world + kHover[k]) = 0;
        ((HandlerFn)(g_exeBase + HANDLER_RVA))(world, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void Counts(U64 network, unsigned* nodes, unsigned* edges)
{
    U64 b = 0;
    *nodes = *edges = 0;
    __try {
        List(network + NODES, b, *nodes);
        List(network + EDGES, b, *edges);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool JoinTool_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(JoinMsg)) return false;
    JoinMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || !m.networks || m.networks > MAX_NETWORKS || m.table >= m.networks || m.node >= m.nodes) return false;
    for (float f : m.point)
        if (!isfinite(f) || fabsf(f) > 1.0e6f) return false;
    if (!JoinTool_Ready()) return false;
    U64 world = (U64)World(), network = 0, node = 0;
    int moved = -1;
    const char* why;
    __try {
        why = Decode(world, m, network, node, &moved);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while resolving";
    }
    if (why) {
        Logf("republicmp  JOIN: node #%u of network %u at (%.2f,%.2f,%.2f): %s - not made", m.node, m.table, m.point[0],
             m.point[1], m.point[2], why);
        return false;
    }
    if (moved >= 0)
        Logf("republicmp  JOIN: node #%u of network %u is #%d here (the node lists' order differs)", m.node, m.table, moved);
    unsigned nodes = 0, edges = 0, nodesAfter = 0, edgesAfter = 0;
    Counts(network, &nodes, &edges);
    static OwnState own;
    bool readOwn = false;
    __try {
        ReadOwn(world, own);
        readOwn = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!readOwn) {
        Logf("republicmp  JOIN: this PC's tool state is unreadable - not made");
        return false;
    }
    // Only the edges at the node in every network's visible lists, alike on every PC.
    bool scoped = ViewListScope_Begin(m.point[0] - SCOPE_RADIUS, m.point[2] - SCOPE_RADIUS, m.point[0] + SCOPE_RADIUS,
                                      m.point[2] + SCOPE_RADIUS);
    bool ok = RunJoin(world, m.point);
    int cleared = 0;
    __try {
        cleared = WriteOwn(world, own);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (scoped) ViewListScope_End();
    Counts(network, &nodesAfter, &edgesAfter);
    bool joined = ok && edgesAfter < edges;
    Logf("republicmp  JOIN: node #%u of network %u at (%.2f,%.2f,%.2f): %s - nodes %u -> %u, edges %u -> %u%s", m.node,
         m.table, m.point[0], m.point[1], m.point[2], !ok ? "the tool faulted" : joined ? "made" : "nothing joined",
         nodes, nodesAfter, edges, edgesAfter, cleared ? " (this PC's highlight cleared)" : "");
    return joined;
}
