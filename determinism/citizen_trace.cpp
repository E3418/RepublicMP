// SPDX-License-Identifier: GPL-3.0-only
// See citizen_trace.h. The hooks only read: integers before the game's call
// (its float arguments stay untouched), no allocation or logging inside.
#include "citizen_trace.h"
#include "determinism.h"
#include <string.h>

#define BOARD_RVA 0x831120
#define JOB_SEARCH_RVA 0x824860
#define REMOVE_RVA 0x835D70
// mov [rsp+20],rbx; mov [rsp+10],rdx; push rbp, rsi, rdi, r12, r13
static const unsigned char kBoardExpect[17] = { 0x48, 0x89, 0x5c, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24,
                                                0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
// mov rax,rsp; mov [rax+18],r8; mov [rax+10],rdx; mov [rax+8],rcx; push rbp, rsi, rdi
static const unsigned char kJobSearchExpect[18] = { 0x48, 0x8b, 0xc4, 0x4c, 0x89, 0x40, 0x18, 0x48, 0x89,
                                                    0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56, 0x57 };
// mov rax,rsp; mov [rax+18],rbx; push rbp, rsi, rdi, r12, r13
static const unsigned char kRemoveExpect[14] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18,
                                                 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
#define PERSON_AGE_OFFSET 0xD4
#define PERSON_IN_BUILDING_OFFSET 0x58
#define PERSON_WORKPLACE_OFFSET 0x690
#define BUILDING_INDEX_OFFSET 0x00
#define VEHICLE_INDEX_OFFSET 0x04

enum { EV_BOARDED = 1, EV_HIRED = 2, EV_REMOVED = 3 };
struct Event {
    unsigned long long tick;
    unsigned age;   // the age's float bits
    int kind;
    int building;   // boarded / removed: the building it was in; hired: the workplace
    int vehicle;
};
#define EVENT_RING 16384 // a power of two
static Event g_events[EVENT_RING];
static volatile LONG64 g_next = 0;

typedef __int64 (*Fn3)(__int64, __int64, __int64, __int64);
typedef __int64 (*Fn6)(__int64, __int64, __int64, __int64, __int64, __int64);
static Fn3 g_origBoard = nullptr;
static Fn6 g_origJobSearch = nullptr;
static Fn6 g_origRemove = nullptr;

void CitizenTrace_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static int IndexAt(__int64 object, int offset)
{
    if (!object) return -1;
    __try {
        return *(int*)(object + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

static __int64 PointerAt(__int64 object, int offset)
{
    if (!object) return 0;
    __try {
        return *(__int64*)(object + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static unsigned AgeBits(__int64 person)
{
    if (!person) return 0;
    __try {
        return *(unsigned*)(person + PERSON_AGE_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The simulation's thread, or the "Filling" worker while the main thread
// waits for it - never both at once.
static void Note(int kind, unsigned age, int building, int vehicle)
{
    LONG64 n = InterlockedIncrement64(&g_next) - 1;
    Event& e = g_events[n & (EVENT_RING - 1)];
    e.tick = Determinism_StepCount();
    e.age = age;
    e.kind = kind;
    e.building = building;
    e.vehicle = vehicle;
}

static __int64 HookedBoard(__int64 world, __int64 person, __int64 vehicle, __int64 a4)
{
    bool on = Determinism_IsActive();
    __int64 in = on ? PointerAt(person, PERSON_IN_BUILDING_OFFSET) : 0;
    __int64 r = g_origBoard(world, person, vehicle, a4);
    if (on && (r & 0xFF)) Note(EV_BOARDED, AgeBits(person), IndexAt(in, BUILDING_INDEX_OFFSET), IndexAt(vehicle, VEHICLE_INDEX_OFFSET));
    return r;
}

static __int64 HookedJobSearch(__int64 world, __int64 person, __int64 a3, __int64 a4, __int64 a5, __int64 a6)
{
    bool on = Determinism_IsActive();
    __int64 before = on ? PointerAt(person, PERSON_WORKPLACE_OFFSET) : 0;
    __int64 r = g_origJobSearch(world, person, a3, a4, a5, a6);
    if (on) {
        __int64 after = PointerAt(person, PERSON_WORKPLACE_OFFSET);
        if (after != before) Note(EV_HIRED, AgeBits(person), IndexAt(after, BUILDING_INDEX_OFFSET), -1);
    }
    return r;
}

static __int64 HookedRemove(__int64 world, __int64 person, __int64 a3, __int64 a4, __int64 a5, __int64 a6)
{
    if (Determinism_IsActive())
        Note(EV_REMOVED, AgeBits(person), IndexAt(PointerAt(person, PERSON_IN_BUILDING_OFFSET), BUILDING_INDEX_OFFSET), -1);
    return g_origRemove(world, person, a3, a4, a5, a6);
}

bool CitizenTrace_Install()
{
    bool board = InstallInlineHook(g_exeBase + BOARD_RVA, (void*)HookedBoard, (void**)&g_origBoard, kBoardExpect,
                                   sizeof(kBoardExpect), "republicmp citizen trace: board");
    bool job = InstallInlineHook(g_exeBase + JOB_SEARCH_RVA, (void*)HookedJobSearch, (void**)&g_origJobSearch,
                                 kJobSearchExpect, sizeof(kJobSearchExpect), "republicmp citizen trace: job search");
    bool remove = InstallInlineHook(g_exeBase + REMOVE_RVA, (void*)HookedRemove, (void**)&g_origRemove, kRemoveExpect,
                                    sizeof(kRemoveExpect), "republicmp citizen trace: remove");
    return board && job && remove;
}

void CitizenTrace_Dump(unsigned long long fromTick, unsigned long long toTick)
{
    static const char* const kKinds[] = { "?", "boarded", "hired", "removed" };
    LONG64 end = g_next;
    LONG64 begin = end > EVENT_RING ? end - EVENT_RING : 0;
    int written = 0;
    for (LONG64 n = begin; n < end; n++) {
        Event e = g_events[n & (EVENT_RING - 1)];
        if (e.tick <= fromTick || e.tick > toTick) continue;
        float age;
        memcpy(&age, &e.age, sizeof(age));
        Logf("republicmp  CITIZEN_EVENT: tick %llu %s age %.9g building %d vehicle %d", e.tick,
             kKinds[e.kind >= 1 && e.kind <= 3 ? e.kind : 0], age, e.building, e.vehicle);
        written++;
    }
    Logf("republicmp  CITIZEN_EVENT: %d event(s) of ticks %llu..%llu%s", written, fromTick + 1, toTick,
         begin > 0 ? " (older ones were overwritten)" : "");
}
