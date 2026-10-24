// SPDX-License-Identifier: GPL-3.0-only
// See office_selection.h.
#include "office_selection.h"
#include "commands.h"
#include "world_refs.h"
#include "../determinism/determinism.h"
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define TOOL_OBJECT_OFFSET 0xD448 // the assign tool's object
#define MODE_OFFSET 0x8
#define SELECTED_OFFSET 0xD0      // vector of building pointers
#define CURRENT_SLOT_OFFSET 0x178 // the quick slot in use, or null
#define SLOTS_OFFSET 0x180        // vector of 0x20-byte slots
#define SLOT_BYTES 0x20
#define SLOT_LIST_OFFSET 0x8      // a slot's vector of building pointers
#define PUSH_RVA 0xB0E90          // vector<T*>::push_back(vector, &value)
#define MAX_SELECTED 64
#define MAX_SLOTS 10
#define MAX_SLOT_OFFICES 32
#define MAGIC 0x314C5341

struct RoutineEntry {
    unsigned rva;
    unsigned char entry[16];
};
static const RoutineEntry kRoutines[] = {
    { 0x5F2A20, { 0x40, 0x55, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x30, 0x4c, 0x8b, 0xfa, 0x48, 0x8b, 0xf1, 0x48 } },
    { 0x5EDC30, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xf1, 0x00, 0x00, 0x00, 0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b } },
    { PUSH_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x41, 0x08, 0x48, 0x8b } },
};

typedef void (*PushFn)(U64 vector, const U64* value);

struct State {
    int mode;
    int current; // -1: no slot in use
    unsigned selected, slots;
    U64 selection[MAX_SELECTED];
    unsigned slotCount[MAX_SLOTS];
    U64 slot[MAX_SLOTS][MAX_SLOT_OFFICES];
};

#pragma pack(push, 1)
struct SelectionMsg {
    unsigned magic;
    int mode;
    int current;
    unsigned selected, slots;
    RouteRef selection[MAX_SELECTED];
    unsigned slotCount[MAX_SLOTS];
    RouteRef slot[MAX_SLOTS][MAX_SLOT_OFFICES];
};
#pragma pack(pop)

static int g_ready = -1;
static State g_shared;           // the session's state as this PC last saw it agreed
static bool g_sharedValid = false;
static unsigned g_sharedLoads = 0; // Determinism_WorldLoads() when g_shared was taken

void OfficeSelection_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static bool Ready()
{
    if (g_ready < 0) {
        g_ready = 1;
        for (const RoutineEntry& e : kRoutines)
            if (memcmp(g_exeBase + e.rva, e.entry, sizeof(e.entry)) != 0) {
                Logf("republicmp  OFFICES: game routine at 0x%X differs - the assign tool's selection is not shared", e.rva);
                g_ready = 0;
                break;
            }
    }
    return g_ready == 1;
}

static U64 Q(U64 at) { return *(U64*)at; }

static U64 Object()
{
    return Q((U64)(g_exeBase + WORLD_RVA) + TOOL_OBJECT_OFFSET);
}

// A vector of building pointers (false: unreadable or longer than max).
static bool ReadList(U64 vector, U64* out, unsigned max, unsigned* count)
{
    U64 begin = Q(vector), end = Q(vector + 8);
    *count = 0;
    if (!begin && !end) return true;
    if (!begin || end < begin || (end - begin) % 8 || (end - begin) / 8 > max) return false;
    *count = (unsigned)((end - begin) / 8);
    memcpy(out, (const void*)begin, (size_t)*count * 8);
    return true;
}

static bool Read(State& s)
{
    memset(&s, 0, sizeof(s));
    __try {
        U64 obj = Object();
        if (!obj) return false;
        s.mode = *(int*)(obj + MODE_OFFSET);
        if (!ReadList(obj + SELECTED_OFFSET, s.selection, MAX_SELECTED, &s.selected)) return false;
        U64 begin = Q(obj + SLOTS_OFFSET), end = Q(obj + SLOTS_OFFSET + 8);
        if (end < begin || (end - begin) % SLOT_BYTES || (end - begin) / SLOT_BYTES > MAX_SLOTS) return false;
        s.slots = (unsigned)((end - begin) / SLOT_BYTES);
        for (unsigned i = 0; i < s.slots; i++)
            if (!ReadList(begin + (U64)i * SLOT_BYTES + SLOT_LIST_OFFSET, s.slot[i], MAX_SLOT_OFFICES, &s.slotCount[i])) return false;
        U64 current = Q(obj + CURRENT_SLOT_OFFSET);
        s.current = current && current >= begin && current < end && (current - begin) % SLOT_BYTES == 0
                        ? (int)((current - begin) / SLOT_BYTES)
                        : -1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool Same(const State& a, const State& b)
{
    if (a.mode != b.mode || a.current != b.current || a.selected != b.selected || a.slots != b.slots) return false;
    if (memcmp(a.selection, b.selection, (size_t)a.selected * 8)) return false;
    for (unsigned i = 0; i < a.slots; i++)
        if (a.slotCount[i] != b.slotCount[i] || memcmp(a.slot[i], b.slot[i], (size_t)a.slotCount[i] * 8)) return false;
    return true;
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

void OfficeSelection_Poll()
{
    if (!Commands_Capturing()) {
        g_sharedValid = false; // the next session takes its own start
        return;
    }
    if (!Ready()) return;
    // A load rebuilds the tool's object from the save: what was seen before
    // it is not a click. Capturing begins before the session's own load, so
    // the start is taken once it is done (the host sent its pre-load state
    // as a change, made on the host alone).
    if (Determinism_WorldLoading()) {
        g_sharedValid = false;
        return;
    }
    static State now;
    if (!Read(now)) return;
    unsigned loads = Determinism_WorldLoads();
    if (!g_sharedValid || loads != g_sharedLoads) {
        // The session's start: every PC loaded the same save.
        g_shared = now;
        g_sharedValid = true;
        g_sharedLoads = loads;
        return;
    }
    if (Same(now, g_shared)) return;
    static SelectionMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    m.mode = now.mode;
    m.current = now.current;
    m.slots = now.slots;
    unsigned lost = 0;
    for (unsigned i = 0; i < now.selected; i++)
        if (GuardedRef(now.selection[i], m.selection[m.selected])) m.selected++;
        else lost++;
    for (unsigned i = 0; i < now.slots; i++)
        for (unsigned k = 0; k < now.slotCount[i]; k++)
            if (GuardedRef(now.slot[i][k], m.slot[i][m.slotCount[i]])) m.slotCount[i]++;
            else lost++;
    g_shared = now;
    if (lost) Logf("republicmp  OFFICES: %u office(s) of the assign tool are not in the world's list - left out", lost);
    Commands_Submit(LS_CMD_OFFICE_SELECTION, &m, sizeof(m));
}

// A vector of building pointers set to `list` (the game's own push).
static void WriteList(U64 vector, const U64* list, unsigned count)
{
    *(U64*)(vector + 8) = Q(vector); // emptied, its memory kept
    for (unsigned i = 0; i < count; i++) ((PushFn)(g_exeBase + PUSH_RVA))(vector, &list[i]);
}

bool OfficeSelection_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(SelectionMsg)) return false;
    static SelectionMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.selected > MAX_SELECTED || m.slots > MAX_SLOTS) return false;
    const char* why = nullptr;
    unsigned gone = 0;
    if (!Ready()) why = "a game routine differs";
    U64 selection[MAX_SELECTED];
    unsigned selected = 0;
    static U64 slot[MAX_SLOTS][MAX_SLOT_OFFICES];
    unsigned slotCount[MAX_SLOTS] = {};
    for (unsigned i = 0; !why && i < m.selected; i++)
        if ((selection[selected] = GuardedAt(m.selection[i]))) selected++;
        else gone++;
    for (unsigned i = 0; !why && i < m.slots; i++) {
        if (m.slotCount[i] > MAX_SLOT_OFFICES) why = "a slot is too long";
        for (unsigned k = 0; !why && k < m.slotCount[i]; k++)
            if ((slot[i][slotCount[i]] = GuardedAt(m.slot[i][k]))) slotCount[i]++;
            else gone++;
    }
    if (!why) {
        __try {
            U64 obj = Object();
            U64 begin = obj ? Q(obj + SLOTS_OFFSET) : 0, end = obj ? Q(obj + SLOTS_OFFSET + 8) : 0;
            if (!obj) why = "no assign tool here";
            else if (end < begin || (end - begin) / SLOT_BYTES != m.slots) why = "its quick slots differ here";
            else {
                *(int*)(obj + MODE_OFFSET) = m.mode;
                WriteList(obj + SELECTED_OFFSET, selection, selected);
                for (unsigned i = 0; i < m.slots; i++)
                    WriteList(begin + (U64)i * SLOT_BYTES + SLOT_LIST_OFFSET, slot[i], slotCount[i]);
                *(U64*)(obj + CURRENT_SLOT_OFFSET) = m.current >= 0 && (unsigned)m.current < m.slots ? begin + (U64)m.current * SLOT_BYTES : 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            why = "a fault while writing";
        }
    }
    // What this PC now holds is the session's state (not a change of its own).
    g_sharedValid = Read(g_shared);
    Logf("republicmp  OFFICES: assign tool selection (%u office(s), mode %d, slot %d)%s: %s", selected, m.mode, m.current,
         gone ? " - some offices are gone" : "", why ? why : "made");
    return why == nullptr;
}
