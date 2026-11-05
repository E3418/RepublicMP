// SPDX-License-Identifier: GPL-3.0-only
// See plan_remove.h. Follows SoylentGreed's plan remove route (RepublicLAN,
// read against the disassembly of 1.1.1.9).
#include "plan_remove.h"
#include "commands.h"
#include "world_refs.h"
#include <initializer_list>
#include <limits.h>
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define PRESS_RVA 0x798CC2  // the press test, start of the checked block (0x6a bytes)
#define BLOCK_BYTES 0x6A
#define BLOCK_FNV 0x06d121336b9b8345ull
#define CLEAR_RVA 0x798CCA  // mov [a54e91],cl (6 bytes)
#define ERASE_RVA 0x798CD5
#define DONE_RVA 0x798CFB
#define SKIP_RVA 0x798D2C
#define PRESS_FLAG 0xA54E91 // the mouse press byte
#define QUEUE 0x11D8        // factory: the production queue (16-byte entries)
#define ENTRY_BYTES 0x10
#define MAX_ENTRIES 4096
#define TYPES 0x12840       // world: vehicle types (0x9f08 each), the name at +200
#define TYPE_STRIDE 0x9F08
#define TYPE_NAME 0x200
#define MAGIC 0x31515250

#pragma pack(push, 1)
struct PlanMsg {
    unsigned magic;
    RouteRef factory;
    unsigned index;     // the entry's place in the queue
    unsigned typeIndex; // its type's place in world +12840
    unsigned typeCheck; // a hash of the type's name
    int variant;
};
#pragma pack(pop)

void PlanRemove_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

static U64 Q(U64 at) { return *(U64*)at; }

static unsigned TypeCheck(U64 type)
{
    unsigned h = 2166136261u;
    const BYTE* name = (const BYTE*)(type + TYPE_NAME);
    for (int i = 0; i < 64 && name[i]; i++) {
        h ^= name[i];
        h *= 16777619u;
    }
    return h | 1u;
}

static bool Entry(U64 factory, unsigned index, U64& at)
{
    U64 begin = Q(factory + QUEUE), end = Q(factory + QUEUE + 8);
    if (end < begin || (end - begin) % ENTRY_BYTES || (end - begin) / ENTRY_BYTES > MAX_ENTRIES || index >= (end - begin) / ENTRY_BYTES)
        return false;
    at = begin + (U64)index * ENTRY_BYTES;
    return true;
}

static bool TypeIndex(U64 type, unsigned& index)
{
    U64 begin = Q((U64)(g_exeBase + WORLD_RVA) + TYPES), end = Q((U64)(g_exeBase + WORLD_RVA) + TYPES + 8);
    if (!type || !begin || type < begin || type >= end || (type - begin) % TYPE_STRIDE) return false;
    index = (unsigned)((type - begin) / TYPE_STRIDE);
    return true;
}

// The relay's question (the interface thread, the row hovered): 1 taken.
static BYTE __cdecl Capture(U64 factory, U64 offset, U64 window)
{
    (void)window;
    if (!Commands_TakeUi()) return 0;
    PlanMsg m = {};
    m.magic = MAGIC;
    const char* why = nullptr;
    __try {
        U64 at = 0;
        if (offset % ENTRY_BYTES || !Ref_Building(factory, m.factory)) why = "the factory is not in the world's list";
        else if (!Entry(factory, (unsigned)(offset / ENTRY_BYTES), at)) why = "the entry";
        else if (!TypeIndex(Q(at), m.typeIndex)) why = "the entry's type";
        else {
            m.index = (unsigned)(offset / ENTRY_BYTES);
            m.typeCheck = TypeCheck(Q(at));
            m.variant = *(int*)(at + 8);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    if (why) Logf("republicmp  PLAN: the removal is not carried (%s) - nothing is done", why);
    else Commands_Submit(LS_CMD_PLAN_REMOVE, &m, sizeof(m));
    return 1;
}

bool PlanRemove_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(PlanMsg)) return false;
    PlanMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.index >= MAX_ENTRIES) return false;
    const char* why = nullptr;
    __try {
        U64 factory = Ref_BuildingAt(m.factory), at = 0;
        unsigned typeIndex = 0;
        if (!factory) why = "the factory is gone";
        else if (!Commands_BuildingInArea(factory)) why = "the factory is outside the player's area";
        else if (!Entry(factory, m.index, at) || !TypeIndex(Q(at), typeIndex) || typeIndex != m.typeIndex ||
                 TypeCheck(Q(at)) != m.typeCheck || *(int*)(at + 8) != m.variant)
            why = "the entry differs here";
        else {
            // 798cd5..798cf3: the entry erased (its count may have moved with production).
            memmove((void*)at, (void*)(at + ENTRY_BYTES), (size_t)(Q(factory + QUEUE + 8) - (at + ENTRY_BYTES)));
            *(U64*)(factory + QUEUE + 8) -= ENTRY_BYTES;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    Logf("republicmp  PLAN: remove from plan: %s", why ? why : "made");
    return why == nullptr;
}

bool PlanRemove_Install()
{
    unsigned long long h = 0xcbf29ce484222325ull;
    for (unsigned a = PRESS_RVA; a < PRESS_RVA + BLOCK_BYTES; a++) h = (h ^ g_exeBase[a]) * 0x100000001b3ull;
    if (h != BLOCK_FNV) {
        Logf("republicmp  PLAN: the factory window's removal differs - it acts on the clicking PC only");
        return false;
    }
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) return false;
    // Entered by jmp with rsp 16-aligned (the window routine's body); cl is 0.
    unsigned char code[160];
    size_t n = 0;
    auto put = [&](std::initializer_list<unsigned char> b) {
        for (unsigned char x : b) code[n++] = x;
    };
    auto abs = [&](U64 v) {
        memcpy(code + n, &v, 8);
        n += 8;
    };
    put({ 0x48, 0xb8 });                                                 // mov rax,press
    abs((U64)(g_exeBase + PRESS_FLAG));
    put({ 0x88, 0x08 });                                                 // mov [rax],cl (the patched instruction)
    put({ 0x38, 0x4f, 0x20 });                                           // cmp [rdi+20],cl
    size_t je = n;
    put({ 0x74, 0x00 });                                                 // je skip (not hovered)
    put({ 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 }); // push rcx,rdx,r8..r11
    put({ 0x48, 0x83, 0xec, 0x20 });                                     // sub rsp,20
    put({ 0x4c, 0x89, 0xf9, 0x4c, 0x89, 0xf2, 0x49, 0x89, 0xf8 });       // mov rcx,r15; mov rdx,r14; mov r8,rdi
    put({ 0x48, 0xb8 });                                                 // mov rax,Capture
    abs((U64)&Capture);
    put({ 0xff, 0xd0 });                                                 // call rax
    put({ 0x48, 0x83, 0xc4, 0x20, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59 }); // add rsp,20; pops
    put({ 0x84, 0xc0 });                                                 // test al,al
    size_t jnz = n;
    put({ 0x75, 0x00 });                                                 // jnz taken
    put({ 0xff, 0x25, 0, 0, 0, 0 });                                     // jmp [798cd5] (the game's erase)
    abs((U64)(g_exeBase + ERASE_RVA));
    size_t taken = n;
    put({ 0xff, 0x25, 0, 0, 0, 0 });                                     // taken: jmp [798cfb]
    abs((U64)(g_exeBase + DONE_RVA));
    size_t skip = n;
    put({ 0xff, 0x25, 0, 0, 0, 0 });                                     // skip: jmp [798d2c]
    abs((U64)(g_exeBase + SKIP_RVA));
    code[je + 1] = (unsigned char)(skip - (je + 2));
    code[jnz + 1] = (unsigned char)(taken - (jnz + 2));
    DWORD old;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(relay, code, n);
    VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, n);
    BYTE* at = g_exeBase + CLEAR_RVA;
    long long to = (long long)relay - (long long)(at + 5);
    if (to < INT_MIN || to > INT_MAX) return false;
    unsigned char jmp[6] = { 0xe9, 0, 0, 0, 0, 0x90 };
    int rel32 = (int)to;
    memcpy(jmp + 1, &rel32, 4);
    if (!VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, jmp, 6);
    VirtualProtect(at, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 6);
    return true;
}
