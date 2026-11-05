// SPDX-License-Identifier: GPL-3.0-only
// See car_owners.h. Follows SoylentGreed's car owners route (RepublicLAN, read
// against the disassembly of 1.1.1.9).
#include "car_owners.h"
#include "commands.h"
#include "world_refs.h"
#include <limits.h>
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define BLOCK_BEGIN 0x7A1A17 // the press and its loop (checked by a hash)
#define BLOCK_END 0x7A1ACB
#define BLOCK_FNV 0xd86075e84d8ad95dull
#define CLEAR_RVA 0x831C50   // 831c50(world, owner)
#define HOME_RVA 0x3FBDC0    // 3fbdc0(world, car, 0, 0, 1, 1)
#define CLEAR_SITE 0x7A1A88
#define HOME_SITE 0x7A1AAB
#define CARS 0xC70           // building: its cars (pointers)
#define CAR_HOME 0x4F8
#define CAR_OWNER 0x4E8
#define MAX_CARS (1u << 16)
#define SAME_PRESS_MS 250    // one press: its cars come in one interface frame
#define MAGIC 0x314E574F

static const unsigned char kClearEntry[16] = { 0x40, 0x55, 0x56, 0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0xf2, 0x48, 0x8b, 0xe9, 0x48, 0x8b, 0x92 };
static const unsigned char kHomeEntry[16] = { 0x48, 0x8b, 0xc4, 0x44, 0x88, 0x48, 0x20, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55 };

#pragma pack(push, 1)
struct OwnersMsg {
    unsigned magic;
    RouteRef building;
};
#pragma pack(pop)

typedef void (*ClearFn)(U64 world, U64 owner);
typedef BYTE (*HomeFn)(U64 world, U64 car, BYTE a, BYTE b, BYTE c, BYTE d);

static U64 g_lastBuilding = 0;
static ULONGLONG g_lastAt = 0;

void CarOwners_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

static U64 Q(U64 at) { return *(U64*)at; }

static void __cdecl RelayClear(U64 world, U64 owner)
{
    if (Commands_TakeUi()) return;
    ((ClearFn)(g_exeBase + CLEAR_RVA))(world, owner);
}

static BYTE __cdecl RelayHome(U64 world, U64 car, BYTE a, BYTE b, BYTE c, BYTE d)
{
    if (!Commands_TakeUi()) return ((HomeFn)(g_exeBase + HOME_RVA))(world, car, a, b, c, d);
    OwnersMsg m = {};
    m.magic = MAGIC;
    U64 building = 0;
    bool ok = false;
    __try {
        building = car ? Q(car + CAR_HOME) : 0;
        ok = building && Ref_Building(building, m.building);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ULONGLONG now = GetTickCount64();
    if (building == g_lastBuilding && now - g_lastAt < SAME_PRESS_MS) return 0; // the press's next car
    g_lastBuilding = building;
    g_lastAt = now;
    if (ok) Commands_Submit(LS_CMD_CAR_OWNERS, &m, sizeof(m));
    else Logf("republicmp  CAR OWNERS: the building is not in the world's list - nothing is done");
    return 0;
}

bool CarOwners_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(OwnersMsg)) return false;
    OwnersMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC) return false;
    const char* why = nullptr;
    int done = 0;
    __try {
        U64 world = (U64)(g_exeBase + WORLD_RVA), b = Ref_BuildingAt(m.building);
        if (!b) {
            why = "the building is gone";
        } else if (!Commands_BuildingInArea(b)) {
            why = "the building is outside the player's area";
        } else {
            // 7a1a48..7a1ac9 on this PC's building (the list's size re-read every pass).
            for (U64 i = 0; i < (Q(b + CARS + 8) - Q(b + CARS)) / 8 && i < MAX_CARS; ++i) {
                U64 car = Q(Q(b + CARS) + 8 * i);
                if (Q(car + CAR_HOME) != b) continue;
                U64 owner = Q(car + CAR_OWNER);
                if (!owner) continue;
                ((ClearFn)(g_exeBase + CLEAR_RVA))(world, owner);
                ((HomeFn)(g_exeBase + HOME_RVA))(world, Q(Q(b + CARS) + 8 * i), 0, 0, 1, 1);
                ++done;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    Logf("republicmp  CAR OWNERS: reset owners: %s (%d car(s))", why ? why : "made", done);
    return why == nullptr;
}

bool CarOwners_Install()
{
    unsigned long long h = 0xcbf29ce484222325ull;
    for (unsigned a = BLOCK_BEGIN; a < BLOCK_END; a++) h = (h ^ g_exeBase[a]) * 0x100000001b3ull;
    if (h != BLOCK_FNV || memcmp(g_exeBase + CLEAR_RVA, kClearEntry, 16) || memcmp(g_exeBase + HOME_RVA, kHomeEntry, 16)) {
        Logf("republicmp  CAR OWNERS: the car window's reset differs - it acts on the clicking PC only");
        return false;
    }
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) return false;
    const unsigned sites[2] = { CLEAR_SITE, HOME_SITE };
    void* to[2] = { (void*)&RelayClear, (void*)&RelayHome };
    DWORD old;
    VirtualProtect(relay, 32, PAGE_EXECUTE_READWRITE, &old);
    for (int i = 0; i < 2; i++) {
        BYTE* r = relay + 16 * i;
        r[0] = 0xFF; r[1] = 0x25; r[2] = r[3] = r[4] = r[5] = 0;
        memcpy(r + 6, &to[i], 8);
    }
    VirtualProtect(relay, 32, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, 32);
    for (int i = 0; i < 2; i++) {
        BYTE* at = g_exeBase + sites[i];
        long long to32 = (long long)(relay + 16 * i) - (long long)(at + 5);
        if (to32 < INT_MIN || to32 > INT_MAX) return false;
        unsigned char code[5] = { 0xE8 };
        int rel32 = (int)to32;
        memcpy(code + 1, &rel32, 4);
        DWORD prot;
        VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &prot);
        memcpy(at, code, 5);
        VirtualProtect(at, 5, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), at, 5);
    }
    return true;
}
