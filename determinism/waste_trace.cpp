// SPDX-License-Identifier: GPL-3.0-only
// See waste_trace.h.
#include "waste_trace.h"
#include "determinism.h"

typedef unsigned __int64 U64;

#define FIND_WASTE_RVA 0x19E330
#define FIND_UNLOAD_RVA 0x19E750
#define WORLD_RVA 0x9D4F10
#define RESOURCES_OFFSET 0xC2B0
#define RESOURCE_BYTES 0x340
#define OFFICE_SITES 0xF90       // the office's buildings
#define SLOTS 0x970              // a building's storage slots
#define SLOT_BYTES 0xE0
#define SLOT_KIND 0x90
#define SLOT_ENTRY_BYTES 0x10
#define WASTE_KIND 0x11
#define VEHICLE_HOME 0x4F8
#define VEHICLE_POSITION 0xD90
#define MAX_LISTED 4096
#define NOTES 8192
#define NOTES_DUMPED 130         // the games are compared every 120 ticks

typedef U64 (*FindWasteFn)(U64 world, U64 office, float capacity, U64 list, unsigned char flag, U64* resource, U64 list2);
typedef U64 (*FindUnloadFn)(U64 world, U64 vehicle, U64 resource, float radius, unsigned char flag);

struct Note {
    unsigned long long tick;
    int kind;     // 0 waste for an empty truck, 1 a place to unload
    int subject;  // kind 0 the office's id, kind 1 the truck's
    int answer;   // the building's id, -1 none
    int resource; // its place in the world's resources, -1 none
    unsigned wasteHash;
    float amount; // the waste the office's buildings hold, summed
    float x, z;   // kind 1: the truck
    float arg;    // kind 0 the truck's capacity, kind 1 the radius
    int flag;
};

static Note g_notes[NOTES];
static unsigned long long g_count = 0;

void WasteTrace_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

static U64 Q(U64 at) { return *(U64*)at; }

static int BuildingId(U64 building)
{
    __try {
        return building ? *(int*)building : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

static int ResourceIndex(U64 resource)
{
    __try {
        U64 first = Q((U64)(g_exeBase + WORLD_RVA + RESOURCES_OFFSET));
        if (!resource || resource < first || (resource - first) % RESOURCE_BYTES) return -1;
        return (int)((resource - first) / RESOURCE_BYTES);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// What the search reads: every waste slot's entries of the office's buildings, to the bit.
static void HashWaste(U64 office, unsigned* hash, float* amount)
{
    unsigned h = 2166136261u;
    float sum = 0;
    __try {
        U64 b = office ? Q(office + OFFICE_SITES) : 0, e = office ? Q(office + OFFICE_SITES + 8) : 0;
        for (unsigned i = 0; b && b + (U64)i * 8 < e && i < MAX_LISTED; i++) {
            U64 site = Q(b + (U64)i * 8);
            if (!site) continue;
            h = (h ^ *(unsigned*)site) * 16777619u;
            U64 slot = Q(site + SLOTS), last = Q(site + SLOTS + 8);
            for (; slot && slot < last; slot += SLOT_BYTES) {
                if (*(int*)(slot + SLOT_KIND) != WASTE_KIND) continue;
                for (U64 it = Q(slot), to = Q(slot + 8); it && it < to; it += SLOT_ENTRY_BYTES) {
                    unsigned bits = *(unsigned*)(it + 8);
                    h = (h ^ bits) * 16777619u;
                    sum += *(float*)(it + 8);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        h = 0;
    }
    *hash = h;
    *amount = sum;
}

// A load starts the ticks again: what was noted before it says nothing about this world.
static void Keep(const Note& n)
{
    if (g_count && n.tick < g_notes[(g_count - 1) % NOTES].tick) g_count = 0;
    g_notes[g_count++ % NOTES] = n;
}

static bool Tracing()
{
    return Determinism_IsActive() && Determinism_GetMoneyTrace();
}

U64 WasteTrace_FindWaste(U64 world, U64 office, float capacity, U64 list, unsigned char flag, U64* resource, U64 list2)
{
    Note n = {};
    bool trace = Tracing();
    if (trace) HashWaste(office, &n.wasteHash, &n.amount);
    U64 r = ((FindWasteFn)(g_exeBase + FIND_WASTE_RVA))(world, office, capacity, list, flag, resource, list2);
    if (trace) {
        n.tick = Determinism_StepCount();
        n.kind = 0;
        n.subject = BuildingId(office);
        n.answer = BuildingId(r);
        n.resource = resource ? ResourceIndex(*resource) : -1;
        n.arg = capacity;
        n.flag = flag;
        Keep(n);
    }
    return r;
}

U64 WasteTrace_FindUnload(U64 world, U64 vehicle, U64 resource, float radius, unsigned char flag)
{
    Note n = {};
    bool trace = Tracing();
    if (trace) {
        __try {
            n.subject = *(int*)(vehicle + 4);
            n.x = *(float*)(vehicle + VEHICLE_POSITION);
            n.z = *(float*)(vehicle + VEHICLE_POSITION + 8);
            HashWaste(Q(vehicle + VEHICLE_HOME), &n.wasteHash, &n.amount);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    U64 r = ((FindUnloadFn)(g_exeBase + FIND_UNLOAD_RVA))(world, vehicle, resource, radius, flag);
    if (trace) {
        n.tick = Determinism_StepCount();
        n.kind = 1;
        n.answer = BuildingId(r);
        n.resource = ResourceIndex(resource);
        n.arg = radius;
        n.flag = flag;
        Keep(n);
    }
    return r;
}

void WasteTrace_Dump(unsigned long long tick)
{
    if (!g_count) {
        Logf("republicmp  WASTE_TRACE: no search yet");
        return;
    }
    unsigned long long first = g_count > NOTES ? g_count - NOTES : 0;
    int written = 0;
    for (unsigned long long i = first; i < g_count; i++) {
        const Note& n = g_notes[i % NOTES];
        if (n.tick > tick || n.tick + NOTES_DUMPED <= tick) continue;
        ++written;
        if (n.kind == 0)
            Logf("republicmp  WASTE_TRACE: tick %llu office %d looks for waste (capacity %.9g, flag %d): building %d, resource %d; the office's waste %.9g hash %08X",
                 n.tick, n.subject, n.arg, n.flag, n.answer, n.resource, n.amount, n.wasteHash);
        else
            Logf("republicmp  WASTE_TRACE: tick %llu truck %d at (%.9g,%.9g) looks to unload resource %d (radius %.9g, flag %d): building %d; its office's waste %.9g hash %08X",
                 n.tick, n.subject, n.x, n.z, n.resource, n.arg, n.flag, n.answer, n.amount, n.wasteHash);
    }
    Logf("republicmp  WASTE_TRACE: %d search(es) in the ticks before %llu, %llu since the load", written, tick, g_count);
}
