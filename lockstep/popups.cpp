// SPDX-License-Identifier: GPL-3.0-only
// See popups.h. The site, the thunk and the callbacks' arguments follow
// SoylentGreed's popup gate and popup values (RepublicLAN, read against 1.1.1.9).
#include "popups.h"
#include "commands.h"
#include "world_refs.h"
#include <string.h>
#include <limits.h>
#include <math.h>

#define POPUP_CALL_SITE 0x3BD94  // mov rdx,[rbx+70]; mov rcx,rax; call [rbx+78]
#define VALUES_DESTROY 0x42BA0   // destroys a values vector (each entry's text, then the buffer)
#define GAME_NEW 0x84DECC        // the game's operator new (the callbacks free with the matching delete)
#define VALUE_BYTES 0x18
#define VALUE_FLOAT 0x10
#define MAX_VALUES 64
#define LIST_ELEMENT 16          // a destinations list's elements (the percent writes their first float)
#define MAGIC 0x3150504F

static const unsigned char kSiteBytes[10] = { 0x48, 0x8b, 0x53, 0x70, 0x48, 0x8b, 0xc8, 0xff, 0x53, 0x78 };
static const unsigned char kDestroyEntry[16] = { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x48, 0x83,
                                                 0xec, 0x20, 0x48, 0x8b, 0x19, 0x48, 0x8b, 0xf1 };
static const unsigned char kNewEntry[16] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
                                             0xd9, 0xeb, 0x21, 0x48, 0x8b, 0xcb, 0xe8, 0xd5 };

enum { ARG_BUILDING = 0, ARG_LIST = 1 };
struct PopupCallback {
    unsigned rva;
    int arg;
    bool values;          // the entered values are read
    unsigned char entry[16]; // checked at startup (0: not checked)
    const char* name;
};
// Appended only: a command names its callback by index.
static const PopupCallback kCallbacks[] = {
    { 0x799550, ARG_BUILDING, false, {}, "production: next in production" },
    { 0x799600, ARG_BUILDING, false, {}, "production: cancel all production" },
    { 0x7723A0, ARG_BUILDING, false, {}, "cancel all source buildings" },
    { 0x745160, ARG_BUILDING, true, { 0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x53, 0x10, 0x49, 0x89, 0x4b, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54 },
      "link window: change order" },
    { 0x770640, ARG_BUILDING, true, { 0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x4b, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 },
      "construction office: order or copy" },
    { 0x778480, ARG_LIST, true, { 0x48, 0x8b, 0x02, 0x48, 0x89, 0x42, 0x08, 0xe9, 0x14, 0xa7, 0x8c, 0xff, 0xcc, 0xcc, 0xcc, 0xcc },
      "citizens' destinations: remove all connections" },
    { 0x778490, ARG_LIST, true, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x4c, 0x8b, 0x19, 0x48, 0x8b, 0xda, 0x4c, 0x8b, 0x41, 0x08 },
      "citizens' destinations: percent" },
};
#define CALLBACK_COUNT (sizeof(kCallbacks) / sizeof(kCallbacks[0]))
#define PERCENT_INDEX 6
static const unsigned kLists[] = { 0xF18, 0xF30, 0xF48, 0xF60 };

#pragma pack(push, 1)
struct PopupMsg {
    unsigned magic;
    unsigned index;    // kCallbacks
    RouteRef building;
    unsigned list;     // ARG_LIST: the list's offset in the building
    unsigned count;    // the values' floats
    unsigned values[MAX_VALUES];
};
#pragma pack(pop)

typedef void (*DestroyFn)(void* values);
typedef U64 (*NewFn)(size_t bytes);
typedef void (*CallbackFn)(U64 values, U64 argument);

void Popups_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

static U64 Q(U64 at) { return *(U64*)at; }

static int CallbackIndex(U64 callback)
{
    for (unsigned i = 0; i < CALLBACK_COUNT; i++)
        if (callback == (U64)(g_exeBase + kCallbacks[i].rva)) return (int)i;
    return -1;
}

// The floats of a values vector (false: unreadable, or more than MAX_VALUES).
static bool ReadValues(const void* values, PopupMsg& m)
{
    U64 begin = Q((U64)values), end = Q((U64)values + 8);
    if (!begin && !end) return true;
    if (!begin || end < begin || (end - begin) % VALUE_BYTES || (end - begin) / VALUE_BYTES > MAX_VALUES) return false;
    for (U64 at = begin; at < end; at += VALUE_BYTES) {
        unsigned bits = *(unsigned*)(at + VALUE_FLOAT);
        float f;
        memcpy(&f, &bits, 4);
        if (!isfinite(f) || fabsf(f) >= 1.0e9f) return false;
        m.values[m.count++] = bits;
    }
    return true;
}

// The argument as a building (and a list offset in it).
static bool ArgumentRef(const PopupCallback& c, U64 argument, PopupMsg& m)
{
    if (c.arg == ARG_BUILDING) return Ref_Building(argument, m.building);
    for (unsigned o : kLists)
        if (argument > o && Ref_Building(argument - o, m.building)) {
            m.list = o;
            return true;
        }
    return false;
}

// The thunk's question (the interface thread): true makes the original call;
// false: carried here, the values destroyed as the callback would.
static bool __cdecl Decide(void* values, void* argument, U64 callback)
{
    if (!Commands_TakeUi()) return true;
    int i = CallbackIndex(callback);
    if (i < 0) return true;
    const PopupCallback& c = kCallbacks[i];
    PopupMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    m.index = (unsigned)i;
    const char* why = nullptr;
    __try {
        if (!argument || !ArgumentRef(c, (U64)argument, m)) why = "its building is not in the world's list";
        else if (c.values && values && !ReadValues(values, m)) why = "its values cannot be read";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while reading it";
    }
    if (why) Logf("republicmp  POPUP: %s not shared - %s; nothing is done", c.name, why);
    else Commands_Submit(LS_CMD_POPUP, &m, sizeof(m));
    __try {
        if (values) ((DestroyFn)(g_exeBase + VALUES_DESTROY))(values);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool Popups_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(PopupMsg)) return false;
    PopupMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.index >= CALLBACK_COUNT || m.count > MAX_VALUES) return false;
    const PopupCallback& c = kCallbacks[m.index];
    if (!c.values && m.count) return false;
    bool listOk = c.arg == ARG_BUILDING ? m.list == 0 : false;
    for (unsigned o : kLists) listOk |= c.arg == ARG_LIST && m.list == o;
    if (!listOk) return false;
    const char* why = nullptr;
    __try {
        U64 building = Ref_BuildingAt(m.building);
        if (!building) {
            why = "the building is gone";
        } else if (!Commands_BuildingInArea(building)) {
            why = "the building is outside the player's area";
        } else {
            U64 argument = building + m.list;
            // The percent writes one share per value with no count check: the list must hold them.
            if (m.index == PERCENT_INDEX) {
                U64 begin = Q(argument), end = Q(argument + 8);
                if (end < begin || (end - begin) % LIST_ELEMENT || m.count > (end - begin) / LIST_ELEMENT)
                    why = "the list is shorter here";
            }
            if (!why) {
                U64 vec[3] = {};
                if (m.count) {
                    size_t bytes = (size_t)m.count * VALUE_BYTES;
                    U64 b = ((NewFn)(g_exeBase + GAME_NEW))(bytes);
                    if (!b) why = "no memory for the values";
                    else {
                        memset((void*)b, 0, bytes);
                        for (unsigned k = 0; k < m.count; k++) *(unsigned*)(b + (U64)k * VALUE_BYTES + VALUE_FLOAT) = m.values[k];
                        vec[0] = b;
                        vec[1] = vec[2] = b + bytes;
                    }
                }
                if (!why) ((CallbackFn)(g_exeBase + c.rva))((U64)vec, argument);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "the callback faulted";
    }
    Logf("republicmp  POPUP: %s: %s", c.name, why ? why : "made");
    return why == nullptr;
}

bool Popups_Install()
{
    BYTE* site = g_exeBase + POPUP_CALL_SITE;
    if (memcmp(site, kSiteBytes, sizeof(kSiteBytes)) || memcmp(g_exeBase + VALUES_DESTROY, kDestroyEntry, 16) ||
        memcmp(g_exeBase + GAME_NEW, kNewEntry, 16)) {
        Logf("republicmp  POPUP: the popup's callback call differs - popups act on the clicking PC only");
        return false;
    }
    static const unsigned char kNone[16] = {};
    for (const PopupCallback& c : kCallbacks)
        if (memcmp(c.entry, kNone, 16) && memcmp(g_exeBase + c.rva, c.entry, 16)) {
            Logf("republicmp  POPUP: the callback %s differs - popups act on the clicking PC only", c.name);
            return false;
        }
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) return false;
    // The thunk (rbx: the popup, rax: the values): asks Decide(values, [rbx+70], [rbx+78]),
    // then makes the original call with the original registers, or returns.
    static const unsigned char kHead[] = {
        0x50, 0x41, 0x50, 0x41, 0x51, // push rax; push r8; push r9
        0x48, 0x83, 0xec, 0x20,       // sub rsp,20
        0x48, 0x89, 0xc1,             // mov rcx,rax
        0x48, 0x8b, 0x53, 0x70,       // mov rdx,[rbx+70]
        0x4c, 0x8b, 0x43, 0x78,       // mov r8,[rbx+78]
        0x48, 0xb8,                   // mov rax,imm64
    };
    static const unsigned char kTail[] = {
        0xff, 0xd0,                   // call rax
        0x48, 0x83, 0xc4, 0x20,       // add rsp,20
        0x41, 0x59, 0x41, 0x58,       // pop r9; pop r8
        0x84, 0xc0,                   // test al,al
        0x58,                         // pop rax
        0x74, 0x12,                   // je done (past the next 18 bytes)
        0x48, 0x8b, 0x53, 0x70,       // mov rdx,[rbx+70]
        0x48, 0x89, 0xc1,             // mov rcx,rax
        0x48, 0x83, 0xec, 0x28,       // sub rsp,28
        0xff, 0x53, 0x78,             // call [rbx+78]
        0x48, 0x83, 0xc4, 0x28,       // add rsp,28
        0xc3,                         // done: ret
    };
    DWORD old;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READWRITE, &old)) return false;
    U64 decide = (U64)&Decide;
    size_t n = 0;
    memcpy(relay + n, kHead, sizeof(kHead));
    n += sizeof(kHead);
    memcpy(relay + n, &decide, 8);
    n += 8;
    memcpy(relay + n, kTail, sizeof(kTail));
    n += sizeof(kTail);
    VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, n);
    long long to = (long long)relay - (long long)(site + 5);
    if (to < INT_MIN || to > INT_MAX) return false;
    unsigned char code[10] = { 0xe8, 0, 0, 0, 0, 0x0f, 0x1f, 0x44, 0x00, 0x00 }; // call thunk; nop5
    int rel32 = (int)to;
    memcpy(code + 1, &rel32, 4);
    if (!VirtualProtect(site, sizeof(code), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site, code, sizeof(code));
    VirtualProtect(site, sizeof(code), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(code));
    return true;
}
