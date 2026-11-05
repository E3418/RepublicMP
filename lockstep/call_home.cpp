// SPDX-License-Identifier: GPL-3.0-only
// See call_home.h. Follows SoylentGreed's call home route (RepublicLAN, read
// against the disassembly of 1.1.1.9).
#include "call_home.h"
#include "commands.h"
#include "world_refs.h"
#include <initializer_list>
#include <limits.h>
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define PRESS_OFF_SLOT 0x86C8B0 // C3D_INPUT::GetMouseLeftPressAOff
#define PRESS_SITE 0x701A2C     // the window's call [import] (6 bytes)
#define BLOCK_BEGIN 0x701A49    // the inline move (checked by a hash)
#define BLOCK_END 0x701D20
#define BLOCK_FNV 0xdbbff36e6fb49962ull
#define HOME_RVA 0x523C70       // 523c70(network, object, building): the vehicles parked there go home
#define RESERVE_RVA 0xB1700     // b1700(vector, 1): room for one more
#define NETWORK 0xFA40          // world: the network 523c70 takes
#define TYPE 0x318
#define TYPE_SUB 0x364
#define TYPE_KIND 0x360
#define PARTNER 0xD08
#define RECORDS 0xA10           // building: 0x60-byte records
#define RECORD_BYTES 0x60
#define RECORD_KIND 9
#define RECORD_OBJECT 0x30
#define VEHICLES 0xC70          // building: its vehicles (pointers)
#define VEHICLE_HOME 0x4F0
#define VEHICLE_OWNER 0x4F8
#define WINDOW_SLOT 0x18        // [rbp+18]: the window; +20 hovered
#define WINDOW_HOVER 0x20
#define MAX_RECORDS 4096
#define MAX_VEHICLES 4096
#define MAGIC 0x31504843

static const unsigned char kHomeEntry[16] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x53, 0x41, 0x54, 0x48, 0x83 };
static const unsigned char kReserveEntry[16] = { 0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0x51, 0x10, 0x4c, 0x8b, 0xc9, 0x4c, 0x8b, 0x41, 0x08, 0x49 };

#pragma pack(push, 1)
struct CallHomeMsg {
    unsigned magic;
    RouteRef building, partner;
};
#pragma pack(pop)

typedef bool (*GetterFn)(void* input);
typedef void (*HomeFn)(U64 network, U64 object, U64 building);
typedef void (*ReserveFn)(U64 vector, U64 count);

static GetterFn g_getter = nullptr;
static bool g_ready = false;

void CallHome_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

static U64 Q(U64 at) { return *(U64*)at; }

static bool PartnerBuilding(U64 b, U64& partner)
{
    partner = 0;
    U64 type = b ? Q(b + TYPE) : 0;
    if (!type) return false;
    partner = Q(b + PARTNER);
    return *(unsigned*)(type + TYPE_SUB) == 0x20 && *(unsigned*)(type + TYPE_KIND) <= 1 && partner;
}

static __declspec(noinline) bool RelayPress(void* input)
{
    CONTEXT c = {};
    RtlCaptureContext(&c);
    bool press = g_getter ? g_getter(input) : false;
    if (!press || !g_ready || !Commands_TakeUi()) return press;
    // One frame up: the window routine at the press site.
    DWORD64 base = 0;
    RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
    if (!f) return false;
    void* data = nullptr;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &data, &frame, nullptr);
    if (c.Rip != (DWORD64)(g_exeBase + PRESS_SITE + 5)) return false;
    CallHomeMsg m = {};
    m.magic = MAGIC;
    const char* why = nullptr;
    __try {
        U64 b = c.R12, partner = 0;
        if (!PartnerBuilding(b, partner)) return true; // the window sends each vehicle home (window calls)
        U64 window = c.Rbp ? Q(c.Rbp + WINDOW_SLOT) : 0;
        if (!window) why = "the window";
        else if (!*(BYTE*)(window + WINDOW_HOVER)) return true; // the window's hover test then does nothing
        else if (!Ref_Building(b, m.building) || !Ref_Building(partner, m.partner)) why = "a building is not in the world's list";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    if (why) {
        Logf("republicmp  CALL HOME: the press is not carried (%s) - nothing is done", why);
        return false;
    }
    Commands_Submit(LS_CMD_CALL_HOME, &m, sizeof(m));
    return false;
}

bool CallHome_Install()
{
    BYTE* at = g_exeBase + PRESS_SITE;
    int rel = 0;
    memcpy(&rel, at + 2, 4);
    if (at[0] != 0xFF || at[1] != 0x15 || PRESS_SITE + 6 + rel != PRESS_OFF_SLOT) {
        Logf("republicmp  CALL HOME: the press at 0x%X differs - a partner building's call home acts on one PC", PRESS_SITE);
        return false;
    }
    unsigned long long h = 0xcbf29ce484222325ull;
    for (unsigned a = BLOCK_BEGIN; a < BLOCK_END; a++) h = (h ^ g_exeBase[a]) * 0x100000001b3ull;
    if (h != BLOCK_FNV || memcmp(g_exeBase + HOME_RVA, kHomeEntry, 16) || memcmp(g_exeBase + RESERVE_RVA, kReserveEntry, 16)) {
        Logf("republicmp  CALL HOME: the window's move differs - a partner building's call home acts on one PC");
        return false;
    }
    g_getter = *(GetterFn*)(g_exeBase + PRESS_OFF_SLOT);
    if (!g_getter) return false;
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) return false;
    void* target = (void*)&RelayPress;
    DWORD old;
    VirtualProtect(relay, 16, PAGE_EXECUTE_READWRITE, &old);
    relay[0] = 0xFF; relay[1] = 0x25; relay[2] = relay[3] = relay[4] = relay[5] = 0;
    memcpy(relay + 6, &target, 8);
    VirtualProtect(relay, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, 16);
    long long to = (long long)relay - (long long)(at + 5);
    if (to < INT_MIN || to > INT_MAX) return false;
    unsigned char code[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    int rel32 = (int)to;
    memcpy(code + 1, &rel32, 4);
    DWORD prot;
    VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &prot);
    memcpy(at, code, 6);
    VirtualProtect(at, 6, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), at, 6);
    g_ready = true;
    return true;
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary: the window's four steps
// ---------------------------------------------------------------------------

static const char* Move(U64 world, U64 b, U64 p, unsigned* moved)
{
    HomeFn home = (HomeFn)(g_exeBase + HOME_RVA);
    ReserveFn reserve = (ReserveFn)(g_exeBase + RESERVE_RVA);
    for (U64 owner : { b, p }) {
        for (U64 i = 0;; ++i) {
            U64 begin = Q(owner + RECORDS), end = Q(owner + RECORDS + 8);
            if (end < begin || (end - begin) % RECORD_BYTES || (end - begin) / RECORD_BYTES > MAX_RECORDS) return "a records list";
            if (i >= (end - begin) / RECORD_BYTES) break;
            U64 rec = begin + i * RECORD_BYTES;
            if (*(unsigned*)rec == RECORD_KIND && Q(rec + RECORD_OBJECT)) home(Q(world + NETWORK), Q(rec + RECORD_OBJECT), b);
        }
    }
    U64 begin = Q(b + VEHICLES), end = Q(b + VEHICLES + 8);
    if (end < begin || (end - begin) % 8 || (end - begin) / 8 > MAX_VEHICLES) return "the building's vehicles";
    for (U64 at = begin; at < end; at += 8) *(U64*)(Q(at) + VEHICLE_OWNER) = b;
    for (unsigned guard = 0;; ++guard) {
        U64 pl = p + VEHICLES, pb = Q(pl), pe = Q(pl + 8);
        if (pe < pb || (pe - pb) % 8 || guard > MAX_VEHICLES) return "the partner's vehicles";
        if (pb == pe) break;
        U64 v = Q(pb);
        *(U64*)(v + VEHICLE_HOME) = b;
        *(U64*)(v + VEHICLE_OWNER) = b;
        U64 bl = b + VEHICLES;
        if (Q(bl + 8) == Q(bl + 16)) reserve(bl, 1);
        if (Q(bl + 8)) *(U64*)Q(bl + 8) = v;
        *(U64*)(bl + 8) += 8;
        memmove((void*)pb, (void*)(pb + 8), (size_t)(pe - (pb + 8)));
        *(U64*)(pl + 8) -= 8;
        ++*moved;
    }
    return nullptr;
}

bool CallHome_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(CallHomeMsg)) return false;
    CallHomeMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC) return false;
    const char* why = nullptr;
    unsigned moved = 0;
    __try {
        U64 world = (U64)(g_exeBase + WORLD_RVA);
        U64 b = Ref_BuildingAt(m.building), p = Ref_BuildingAt(m.partner), partner = 0;
        if (!b || !p) why = "a building is gone";
        else if (!Commands_BuildingInArea(b) || !Commands_BuildingInArea(p)) why = "a building is outside the player's area";
        else if (!PartnerBuilding(b, partner) || partner != p) why = "the partner differs here";
        else why = Move(world, b, p, &moved);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    Logf("republicmp  CALL HOME: a partner building's vehicles: %s (%u moved)", why ? why : "made", moved);
    return why == nullptr;
}
