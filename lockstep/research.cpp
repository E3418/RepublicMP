// SPDX-License-Identifier: GPL-3.0-only
// See research.h. Follows SoylentGreed's research route (RepublicLAN, read
// against the disassembly of 1.1.1.9).
#include "research.h"
#include "commands.h"
#include "world_refs.h"
#include <string.h>
#include <vector>

#define WORLD_RVA 0x9D4F10
#define LIST_OFFSET 0x11778   // the research list: begin, end
#define ENTRY_BYTES 0xE8
#define ENTRY_BUILDING 0xD8   // the building researching the entry, or 0
#define MAX_ENTRIES 2048
#define DOWN_RVA 0xA54E90
#define PRESS_RVA 0xA54E91
#define ENTER_RVA 0xA54CAC
#define KEYPAD_ENTER_RVA 0xA54D2C

#pragma pack(push, 1)
struct ResearchMsg {
    unsigned index;   // the entry
    unsigned count;   // the list's size (the shape must match)
    unsigned start;   // 1: the building starts it, 0: suspended
    RouteRef building;
};
#pragma pack(pop)

static std::vector<U64> g_held;
static bool g_taken = false;
static unsigned long long g_epoch = 0;

void Research_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static bool Entries(U64* begin, unsigned* count)
{
    *begin = 0;
    *count = 0;
    __try {
        U64* v = (U64*)(g_exeBase + WORLD_RVA + LIST_OFFSET);
        if (!v[0] && !v[1]) return true;
        if (!v[0] || v[1] < v[0] || (v[1] - v[0]) % ENTRY_BYTES || (v[1] - v[0]) / ENTRY_BYTES > MAX_ENTRIES) return false;
        *begin = v[0];
        *count = (unsigned)((v[1] - v[0]) / ENTRY_BYTES);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every entry's building (false: unreadable).
static bool ReadBuildings(std::vector<U64>& out)
{
    U64 begin = 0;
    unsigned count = 0;
    if (!Entries(&begin, &count)) return false;
    out.resize(count);
    __try {
        for (unsigned i = 0; i < count; i++) out[i] = *(U64*)(begin + (U64)i * ENTRY_BYTES + ENTRY_BUILDING);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool WriteBuilding(unsigned index, U64 building)
{
    U64 begin = 0;
    unsigned count = 0;
    if (!Entries(&begin, &count) || index >= count) return false;
    __try {
        *(U64*)(begin + (U64)index * ENTRY_BYTES + ENTRY_BUILDING) = building;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool GuardedRef(U64 building, RouteRef& r)
{
    __try {
        return Ref_Building(building, r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static U64 GuardedAt(const RouteRef& r)
{
    __try {
        return Ref_BuildingAt(r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool Clicked()
{
    return g_exeBase[DOWN_RVA] || g_exeBase[PRESS_RVA] || g_exeBase[ENTER_RVA] || g_exeBase[KEYPAD_ENTER_RVA];
}

// ---------------------------------------------------------------------------
// The clicking PC: the window
// ---------------------------------------------------------------------------

void Research_Before(bool simBusy)
{
    g_taken = false;
    if (simBusy || !Commands_TakeUi() || !Clicked()) return;
    if (!ReadBuildings(g_held) || g_held.empty()) return;
    g_epoch = Commands_ApplyEpoch();
    g_taken = true;
}

void Research_After(bool simBusy)
{
    if (!g_taken) return;
    g_taken = false;
    if (simBusy || !Commands_TakeUi() || Commands_ApplyEpoch() != g_epoch) return;
    std::vector<U64> now;
    if (!ReadBuildings(now) || now.size() != g_held.size()) return; // the list changed shape: not compared
    for (unsigned i = 0; i < now.size(); i++) {
        if (now[i] == g_held[i]) continue;
        // Put back at once: the command makes the change on every PC.
        WriteBuilding(i, g_held[i]);
        ResearchMsg m;
        memset(&m, 0, sizeof(m));
        m.index = i;
        m.count = (unsigned)now.size();
        m.start = now[i] ? 1 : 0;
        m.building = kNoRef;
        if (now[i] && !GuardedRef(now[i], m.building)) {
            Logf("republicmp  RESEARCH: entry %u was started by a building that is not in the world's list - nothing is done", i);
            continue;
        }
        Commands_Submit(LS_CMD_RESEARCH, &m, sizeof(m));
    }
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

bool Research_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(ResearchMsg)) return false;
    ResearchMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.start > 1 || !m.count || m.index >= m.count) return false;
    const char* why = nullptr;
    U64 begin = 0;
    unsigned count = 0;
    U64 building = 0;
    if (!Entries(&begin, &count) || count != m.count) why = "the research list differs here";
    else if (m.start && !(building = GuardedAt(m.building))) why = "the building is gone";
    else if (!WriteBuilding(m.index, building)) why = "a fault while writing";
    if (m.start)
        Logf("republicmp  RESEARCH: entry %u started by building #%d: %s", m.index, m.building.place, why ? why : "made");
    else
        Logf("republicmp  RESEARCH: entry %u suspended: %s", m.index, why ? why : "made");
    return why == nullptr;
}
