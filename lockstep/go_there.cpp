// SPDX-License-Identifier: GPL-3.0-only
// See go_there.h. Follows SoylentGreed's route (RepublicLAN, read against the
// disassembly of 1.1.1.9).
#include "go_there.h"
#include "commands.h"
#include <math.h>
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define HANDLER_RVA 0x3CA560
#define RECORD_RVA 0x1512A0
#define HALF_FREE_RVA 0x1513F0
#define RECORDS_CLEAR_RVA 0x1FF7A0
#define RECORDS_PUSH_RVA 0x1FF8F0
#define GROW_RVA 0xB1700
#define NODE_RVA 0xA556B0
#define PRESS_RVA 0xA54E91
#define OBJECT_OFFSET 0xD298
#define WINDOW_OFFSET 0xD2A0
#define WINDOWS_OFFSET 0xCAC0
#define CURSOR_OFFSET 0xF6C
#define VEHICLE_TARGET 0x728
#define VEHICLE_STATIONS 0x680
#define VEHICLE_RECORDS 0x638
#define VEHICLE_STOP 0x698
#define VEHICLE_FLAG 0xD3A
#define RECORD_BYTES 0x198
#define RECORD_HALF 0xC8
#define RECORD_NODE 0x190
#define MAGIC 0x31485447
static const unsigned kMenus[3] = { 0xFBC, 0xFBE, 0xFC3 };

struct Routine {
    unsigned rva;
    unsigned char entry[16];
};
static const Routine kRoutines[] = {
    { HANDLER_RVA, { 0x48, 0x8b, 0xc4, 0x41, 0x56, 0x48, 0x81, 0xec, 0xe0, 0x01, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24 } },
    { RECORD_RVA, { 0x33, 0xc0, 0x48, 0x89, 0x41, 0x08, 0x48, 0x89, 0x41, 0x10, 0x48, 0x89, 0x41, 0x18, 0x48, 0x89 } },
    { HALF_FREE_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x33, 0xff, 0x48 } },
    { RECORDS_CLEAR_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48 } },
    { RECORDS_PUSH_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0xfa } },
    { GROW_RVA, { 0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0x51, 0x10, 0x4c, 0x8b, 0xc9, 0x4c, 0x8b, 0x41, 0x08, 0x49 } },
};

typedef U64 (*HandlerFn)(U64 world);
typedef U64 (*OneFn)(U64 a);
typedef U64 (*TwoFn)(U64 a, U64 b);

static int g_ready = -1;

void GoThere_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

bool GoThere_Ready()
{
    if (g_ready < 0) {
        g_ready = 1;
        for (const Routine& r : kRoutines)
            if (memcmp(g_exeBase + r.rva, r.entry, sizeof(r.entry)) != 0) {
                Logf("republicmp  GO THERE: game routine at 0x%X differs - the tool is put away in lockstep", r.rva);
                g_ready = 0;
                break;
            }
    }
    return g_ready == 1;
}

static U64 Q(U64 at) { return *(U64*)at; }

// The handler's own test for a click it acts on.
static bool Encode(U64 world, GoThereMsg& m)
{
    __try {
        U64 vehicle = Q(world + OBJECT_OFFSET), window = Q(world + WINDOW_OFFSET);
        U64 first = Q(world + WINDOWS_OFFSET), last = Q(world + WINDOWS_OFFSET + 8);
        if (!vehicle || !window || last < first + 8 || Q(first) != window) return false;
        memset(&m, 0, sizeof(m));
        m.magic = MAGIC;
        memcpy(m.point, (const void*)(world + CURSOR_OFFSET), 12);
        if (!Ref_Vehicle(vehicle, m.vehicle)) {
            Logf("republicmp  GO THERE: the window's vehicle is not in the world's list - nothing is done");
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

U64 GoThere_Run(U64 world)
{
    HandlerFn handler = (HandlerFn)(g_exeBase + HANDLER_RVA);
    BYTE* w = (BYTE*)world;
    BYTE* press = g_exeBase + PRESS_RVA;
    bool menus = w[kMenus[0]] || w[kMenus[1]] || w[kMenus[2]];
    if (!*press || menus || !Commands_TakeLocal() || !GoThere_Ready()) return handler(world);
    GoThereMsg m;
    if (Encode(world, m)) Commands_Submit(LS_CMD_GO_THERE, &m, sizeof(m));
    BYTE kept = *press;
    *press = 0;
    U64 r = handler(world); // its tests only (it may put the tool away)
    *press = kept;
    return r;
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

// The handler's steps (3ca620..); 0 done, else the step that faulted.
static int Send(U64 vehicle, const float point[3])
{
    __declspec(align(16)) BYTE record[RECORD_BYTES + 0x38];
    memset(record, 0, sizeof(record));
    int step = 1;
    __try {
        ((OneFn)(g_exeBase + RECORD_RVA))((U64)record);
        *(U64*)(record + RECORD_NODE) = (U64)(g_exeBase + NODE_RVA);
        step = 2;
        memcpy((void*)(vehicle + VEHICLE_TARGET), point, 12);
        step = 3;
        U64* stations = (U64*)(vehicle + VEHICLE_STATIONS);
        stations[1] = stations[0];
        step = 4;
        ((OneFn)(g_exeBase + RECORDS_CLEAR_RVA))(vehicle + VEHICLE_RECORDS);
        step = 3;
        if (stations[1] == stations[2]) ((TwoFn)(g_exeBase + GROW_RVA))((U64)stations, 1);
        if (stations[1]) *(U64*)stations[1] = 0;
        stations[1] += 8;
        step = 4;
        ((TwoFn)(g_exeBase + RECORDS_PUSH_RVA))(vehicle + VEHICLE_RECORDS, (U64)record);
        step = 5;
        *(unsigned*)(vehicle + VEHICLE_STOP) = 0;
        *(BYTE*)(vehicle + VEHICLE_FLAG) = 1;
        step = 6;
        ((OneFn)(g_exeBase + HALF_FREE_RVA))((U64)(record + RECORD_HALF));
        ((OneFn)(g_exeBase + HALF_FREE_RVA))((U64)record);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return step;
    }
}

bool GoThere_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(GoThereMsg)) return false;
    GoThereMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || !GoThere_Ready()) return false;
    for (float f : m.point)
        if (!isfinite(f) || fabsf(f) > 1.0e6f) return false;
    U64 vehicle = 0;
    const char* why = nullptr;
    __try {
        vehicle = Ref_VehicleAt(m.vehicle);
        if (!vehicle) why = "the vehicle is gone";
        else if (!Commands_VehicleInArea(vehicle)) why = "the vehicle is outside the player's area";
        else if (!Commands_PointInArea(m.point)) why = "the point is outside the player's area";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading the vehicle";
    }
    int fault = why ? 0 : Send(vehicle, m.point);
    Logf("republicmp  GO THERE: vehicle #%d to (%.2f,%.2f,%.2f): %s", m.vehicle.place, m.point[0], m.point[1], m.point[2],
         why ? why : fault ? "a fault while sending it" : "made");
    return !why && !fault;
}
