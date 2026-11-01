// SPDX-License-Identifier: GPL-3.0-only
// See cancel_tool.h.
#include "cancel_tool.h"
#include "commands.h"
#include <math.h>
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define REMOVE_RVA 0x5434C0        // 5434c0(network, edge, 0, -1, 0): removes an edge
#define REMOVE_CALL 0x2144D2       // the cancel handler's call of it
#define TABLE_COUNT_OFFSET 0x14034 // world: int, the networks in the table
#define TABLE_FIRST_OFFSET 0x14038 // world: 0x50-byte entries, the network first
#define TABLE_STRIDE 0x50
#define MAX_NETWORKS 64
#define EDGES 0x2B0                // network: committed edges (begin, end)
#define EDGE_NODE0 0x20
#define EDGE_NODE1 0x28
#define NODE_POS 4                 // node: float x, y, z
#define MAX_LIST (1u << 20)
#define MAGIC 0x314E4143

#pragma pack(push, 1)
struct CancelMsg {
    unsigned magic;
    unsigned table, networks; // the network's place in the world's table, the table's size
    unsigned edge, edges;     // the edge's place in the network's +2b0, the list's size
    float ends[2][3];         // its nodes' positions (bits compared)
};
#pragma pack(pop)

typedef U64 (*RemoveFn)(U64 network, U64 edge, U64 a3, U64 a4, U64 a5);

static BYTE* g_relay = nullptr;

void CancelTool_Bind(const TsmHost* host)
{
    TsmBind(host);
}

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

static bool Ends(U64 edge, float out[2][3])
{
    U64 n0 = Q(edge + EDGE_NODE0), n1 = Q(edge + EDGE_NODE1);
    if (!n0 || !n1) return false;
    memcpy(out[0], (const void*)(n0 + NODE_POS), 12);
    memcpy(out[1], (const void*)(n1 + NODE_POS), 12);
    return true;
}

// The edge the handler is removing as a command; nullptr, or why not.
static const char* Encode(U64 network, U64 edge, CancelMsg& m)
{
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    U64 nets[MAX_NETWORKS] = {};
    unsigned n = Networks(WORLD_RVA + (U64)g_exeBase, nets);
    if (!n) return "the network table is unreadable";
    for (unsigned t = 0; t < n; t++) {
        if (nets[t] != network) continue;
        U64 b = 0;
        unsigned c = 0;
        if (!List(network + EDGES, b, c)) return "the network's edges are unreadable";
        for (unsigned i = 0; i < c; i++) {
            if (Q(b + (U64)i * 8) != edge) continue;
            if (!Ends(edge, m.ends)) return "the edge has no nodes";
            m.table = t;
            m.networks = n;
            m.edge = i;
            m.edges = c;
            return nullptr;
        }
        return "the edge is not among the network's edges";
    }
    return "the network is not in the world's table";
}

static const char* GuardedEncode(U64 network, U64 edge, CancelMsg& m)
{
    __try {
        return Encode(network, edge, m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "a fault while reading the edge";
    }
}

// The cancel handler's call of 5434c0 (through the relay).
static U64 RelayedRemove(U64 network, U64 edge, U64 a3, U64 a4, U64 a5)
{
    RemoveFn remove = (RemoveFn)(g_exeBase + REMOVE_RVA);
    if (!Commands_TakeLocal()) return remove(network, edge, a3, a4, a5);
    CancelMsg m;
    const char* why = GuardedEncode(network, edge, m);
    if (why) {
        Logf("republicmp  CANCEL: not shared - %s; nothing is removed", why);
        return 0;
    }
    Commands_Submit(LS_CMD_NET_CANCEL, &m, sizeof(m));
    return 0;
}

bool CancelTool_Install()
{
    BYTE* call = g_exeBase + REMOVE_CALL;
    int rel = 0;
    memcpy(&rel, call + 1, 4);
    if (call[0] != 0xE8 || REMOVE_CALL + 5 + rel != REMOVE_RVA) {
        Logf("republicmp  CANCEL: the cancel handler's call differs - the cancel tools are made on one PC only");
        return false;
    }
    BYTE* relay = AllocNear(g_exeBase + REMOVE_CALL, 4096);
    if (!relay) return false;
    long long to = (long long)relay - (long long)(call + 5);
    if (to < INT_MIN || to > INT_MAX) return false;
    DWORD old;
    if (!VirtualProtect(relay, 16, PAGE_EXECUTE_READWRITE, &old)) return false;
    static const unsigned char kJump[6] = { 0xFF, 0x25, 0, 0, 0, 0 }; // jmp [rip]
    U64 target = (U64)&RelayedRemove;
    memcpy(relay, kJump, sizeof(kJump));
    memcpy(relay + sizeof(kJump), &target, 8);
    VirtualProtect(relay, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, 16);
    int rel32 = (int)to;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(call + 1, &rel32, 4);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    g_relay = relay;
    return true;
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

// The edge the command names here: the same table, the edge at its place with the same ends.
static const char* Decode(const CancelMsg& m, U64& network, U64& edge)
{
    network = edge = 0;
    U64 nets[MAX_NETWORKS] = {};
    unsigned n = Networks(WORLD_RVA + (U64)g_exeBase, nets);
    if (n != m.networks || m.table >= n || !nets[m.table]) return "the network table differs here";
    U64 b = 0;
    unsigned c = 0;
    if (!List(nets[m.table] + EDGES, b, c) || m.edge >= c) return "the edge's place is not in the network here";
    U64 at = Q(b + (U64)m.edge * 8);
    float ends[2][3];
    if (!at || !Ends(at, ends) || memcmp(ends, m.ends, sizeof(ends)) != 0) return "another edge is at that place here";
    network = nets[m.table];
    edge = at;
    return nullptr;
}

static bool Remove(U64 network, U64 edge)
{
    __try {
        ((RemoveFn)(g_exeBase + REMOVE_RVA))(network, edge, 0, 0xFFFFFFFFull, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static unsigned EdgeCount(U64 network)
{
    U64 b = 0;
    unsigned c = 0;
    __try {
        List(network + EDGES, b, c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return c;
}

bool CancelTool_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(CancelMsg)) return false;
    CancelMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || !m.networks || m.networks > MAX_NETWORKS || m.table >= m.networks || m.edge >= m.edges)
        return false;
    for (int e = 0; e < 2; e++)
        for (float f : m.ends[e])
            if (!isfinite(f) || fabsf(f) > 1.0e6f) return false;
    U64 network = 0, edge = 0;
    const char* why;
    __try {
        why = Decode(m, network, edge);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while resolving";
    }
    if (why) {
        Logf("republicmp  CANCEL: edge #%u of network %u (%.2f,%.2f)-(%.2f,%.2f): %s - not made", m.edge, m.table,
             m.ends[0][0], m.ends[0][2], m.ends[1][0], m.ends[1][2], why);
        return false;
    }
    unsigned before = EdgeCount(network);
    bool ok = Remove(network, edge);
    unsigned after = EdgeCount(network);
    Logf("republicmp  CANCEL: edge #%u of network %u (%.2f,%.2f)-(%.2f,%.2f): %s - edges %u -> %u", m.edge, m.table,
         m.ends[0][0], m.ends[0][2], m.ends[1][0], m.ends[1][2], !ok ? "the removal faulted" : "removed", before, after);
    return ok;
}
