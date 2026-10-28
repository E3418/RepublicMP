// SPDX-License-Identifier: GPL-3.0-only
#include "office_guard.h"
#include "determinism.h"
#include <stdint.h>

#define WORLD_RVA 0x9D4F10
#define BUILDINGS_OFFSET 0x11B08
#define VEHICLES_OFFSET 0x12810
#define LINKED_OFFICES_OFFSET 0xDA8
#define OFFICE_VEHICLES_OFFSET 0xC70
#define LOOKUP_RVA 0x19F5F0
#define MAX_LIST 1000000

static volatile DWORD g_stepThread = 0;
void OfficeGuard_Bind(const TsmHost* host) { TsmBind(host); }
void OfficeGuard_BeginStep() { g_stepThread = GetCurrentThreadId(); }
void OfficeGuard_EndStep() { g_stepThread = 0; }

static bool Read(const void* at, void* out, size_t bytes)
{
    SIZE_T done = 0;
    return !bytes || (ReadProcessMemory(GetCurrentProcess(), at, out, bytes, &done) && done == bytes);
}

struct List { uintptr_t begin, end, capacity; };
static bool ReadList(const void* at, size_t stride, List& list)
{
    if (!Read(at, &list, sizeof(list))) return false;
    if (!list.begin && !list.end && !list.capacity) return true;
    if (!list.begin || list.begin > list.end || list.end > list.capacity ||
        ((list.begin | list.end | list.capacity) & 7) ||
        (list.end - list.begin) % stride || (list.capacity - list.begin) % stride ||
        (list.end - list.begin) / stride > MAX_LIST) return false;
    return list.begin == list.end || ReadablePtr((void*)list.begin, list.end - list.begin);
}

static bool Contains(const List& list, const void* object)
{
    uintptr_t value = (uintptr_t)object;
    for (uintptr_t p = list.begin; p < list.end; p += 8)
        if (*(const uintptr_t*)p == value) return true;
    return false;
}

// 19f5f0 follows building +da8 -> office +c70 -> vehicle +1708. The
// failing client reached 19f660 with an invalid +c70 begin (BB8080803F7FA298).
// Check live registry membership before following each building/vehicle link;
// readable freed memory alone does not establish that an object still exists.
// No exception handler, list rewriting or old heap-header restoration is used.
static const char* Validate(BYTE* building, BYTE** badOffice)
{
    *badOffice = nullptr;
    BYTE* world = g_exeBase + WORLD_RVA;
    List buildings{}, vehicles{}, offices{};
    if (!ReadList(world + BUILDINGS_OFFSET, 8, buildings) ||
        !ReadList(world + VEHICLES_OFFSET, 8, vehicles)) return "world registry invalid";
    if (!building || !Contains(buildings, building)) return "target building is no longer listed";
    if (!ReadList(building + LINKED_OFFICES_OFFSET, 8, offices)) return "linked-office vector invalid";
    for (uintptr_t p = offices.begin; p < offices.end; p += 8) {
        BYTE* office = *(BYTE**)p;
        *badOffice = office;
        if (!office || !Contains(buildings, office)) return "linked office is no longer listed";
        List owned{};
        if (!ReadList(office + OFFICE_VEHICLES_OFFSET, 8, owned)) return "office vehicle vector invalid";
        for (uintptr_t q = owned.begin; q < owned.end; q += 8) {
            BYTE* vehicle = *(BYTE**)q;
            if (!vehicle || !Contains(vehicles, vehicle)) return "office vehicle is no longer listed";
            BYTE* type = nullptr;
            int kind = 0, state = 0;
            if (!Read(vehicle + 0x1708, &type, 8)) return "vehicle type unreadable";
            if (!type) continue; // the native lookup skips this vehicle too
            if (!Read(type + 0x8600, &kind, 4)) return "vehicle class unreadable";
            if (kind != 0x11) continue;
            if (!Read(vehicle + 0x698, &state, 4)) return "vehicle job state unreadable";
            if (state) continue;
            List schedule{};
            if (!ReadList(vehicle + 0x680, 8, schedule)) return "vehicle schedule invalid";
            if (schedule.begin == schedule.end || *(BYTE**)schedule.begin != building) continue;
            // The native lookup reads the first stop and its resource filter
            // at stop +8/+10 (16-byte entries), without checking a stop exists.
            List stops{}, resources{};
            if (!ReadList(vehicle + 0x638, 0x198, stops) || stops.begin == stops.end)
                return "vehicle stop records invalid";
            if (!ReadList((void*)(stops.begin + 8), 0x10, resources)) return "vehicle stop resources invalid";
        }
    }
    *badOffice = nullptr;
    return nullptr;
}

typedef bool (*LookupFn)(void*, BYTE*, void*);
static LookupFn o_lookup = nullptr;
static LONG g_reports = 0;

static bool HookedLookup(void* context, BYTE* building, void* resource)
{
    if (!Determinism_IsActive() || !Determinism_InStepFrame() || g_stepThread != GetCurrentThreadId())
        return o_lookup(context, building, resource);
    BYTE* office = nullptr;
    if (const char* reason = Validate(building, &office)) {
        if (InterlockedIncrement(&g_reports) <= 8) {
            int target = -1, source = -1;
            Read(building, &target, 4);
            Read(office, &source, 4);
            Logf("republicmp  OFFICE_GUARD: tick %llu target %d linked office %d: %s - assignment refused; existing links need inspection",
                 Determinism_StepCount(), target, source, reason);
        }
        // Native true means another vehicle already owns this job (19e727):
        // conservatively refuse this candidate instead of following bad links.
        // This contains the crash; it does not repair the corrupted source list.
        return true;
    }
    return o_lookup(context, building, resource);
}

bool OfficeGuard_Install()
{
    // Four stack saves and push r14, complete instructions, no RIP operands.
    static const BYTE expected[22] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,
                                      0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x41,0x56 };
    return InstallInlineHook(g_exeBase + LOOKUP_RVA, (void*)&HookedLookup, (void**)&o_lookup,
                             expected, sizeof(expected), "republicmp linked office lookup");
}
