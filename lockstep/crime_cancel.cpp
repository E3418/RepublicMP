// SPDX-License-Identifier: GPL-3.0-only
// See crime_cancel.h. Follows SoylentGreed's crime cancel route (RepublicLAN,
// read against the disassembly of 1.1.1.9).
#include "crime_cancel.h"
#include "commands.h"
#include "world_refs.h"
#include <initializer_list>
#include <limits.h>
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define BLOCK_SITE 0x768570  // the click test, its je last
#define JE_SITE 0x768584     // je 768681 (6 bytes)
#define SKIP_RVA 0x768681    // the row's normal drawing
#define NATIVE_RVA 0x76858A  // after the je: the hover and acted tests, then the effect
#define TEXT_SITE 0x768536   // mov edx,0x986 ("Cancel assignment")
#define ADVANCE_RVA 0x2571E0 // 2571e0(world, case, float)
#define CLOSE_RVA 0x2580D0   // 2580d0(world, case)
#define CASE_LIST 0x1388     // building: its cases (pointers)
#define CASE_TYPE 4
#define CASE_KIND 0x70
#define CASE_OWNER 8
#define OWNER_BUILDING 0x108
#define BUILDING_RECORD 0x1B8
#define MAX_CASES 4096
#define MAGIC 0x31434352

static const unsigned kRecordCount[4] = { 0x330, 0, 0x334, 0x338 };
static const unsigned kWorldCount[4] = { 0x11658, 0, 0x1165C, 0x11660 };

struct Pinned {
    unsigned rva, n;
    unsigned char bytes[26];
};
static const Pinned kPinned[] = {
    { TEXT_SITE, 5, { 0xba, 0x86, 0x09, 0x00, 0x00 } },
    { BLOCK_SITE, 26, { 0x38, 0x0d, 0x1b, 0xc9, 0x2e, 0x00, 0x0f, 0x28, 0x05, 0x43, 0x3f, 0x1a, 0x00, 0x0f, 0x11, 0x05, 0x88,
                        0x5d, 0x25, 0x00, 0x0f, 0x84, 0xf7, 0x00, 0x00, 0x00 } },
    // the hover and acted tests after the je, r12 from the list, the list from [rbp+170], r13 the world
    { NATIVE_RVA, 21, { 0x38, 0x4f, 0x20, 0x0f, 0x84, 0xee, 0x00, 0x00, 0x00, 0x38, 0x8d, 0x50, 0x01, 0x00, 0x00, 0x0f, 0x85, 0xe2, 0x00,
                        0x00, 0x00 } },
    { 0x767839, 7, { 0x48, 0x8b, 0xb5, 0x70, 0x01, 0x00, 0x00 } },
    { 0x767DC0, 4, { 0x4e, 0x8b, 0x24, 0xca } },
    { 0x7677F3, 3, { 0x4c, 0x8b, 0xe9 } },
    { ADVANCE_RVA, 16, { 0x48, 0x89, 0x54, 0x24, 0x10, 0x41, 0x56, 0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00, 0xf3, 0x0f } },
    { CLOSE_RVA, 16, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41 } },
};

#pragma pack(push, 1)
struct CrimeMsg {
    unsigned magic;
    RouteRef building;
    unsigned index, check; // the case's place in the building's +1388 and a check of its +4 and +70
};
#pragma pack(pop)

typedef void (*AdvanceFn)(U64 world, U64 c, float amount);
typedef void (*CloseFn)(U64 world, U64 c);

void CrimeCancel_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

static U64 Q(U64 at) { return *(U64*)at; }

static unsigned CaseCheck(U64 c)
{
    unsigned w[3] = { 0x43524d45u, *(unsigned*)(c + CASE_TYPE), *(unsigned*)(c + CASE_KIND) };
    unsigned h = 2166136261u;
    for (unsigned v : w)
        for (int b = 0; b < 4; b++) {
            h ^= (v >> (8 * b)) & 0xffu;
            h *= 16777619u;
        }
    return h | 1u;
}

// The relay's question (the interface thread, the press hovered on a row not acted yet):
// carried here, it marks the row acted ([rbp+150]) and nothing of the effect is made.
static void __cdecl Capture(U64 c, U64 world, U64 list, U64 rbp)
{
    if (!Commands_TakeUi()) return;
    CrimeMsg m = {};
    m.magic = MAGIC;
    const char* why = nullptr;
    __try {
        U64 begin = Q(list), end = Q(list + 8);
        m.index = UINT_MAX;
        if (world != (U64)(g_exeBase + WORLD_RVA) || !c) why = "registers";
        else if (end < begin || (end - begin) % 8 || (end - begin) / 8 > MAX_CASES) why = "the case list";
        else {
            for (U64 at = begin; at < end; at += 8)
                if (Q(at) == c) m.index = (unsigned)((at - begin) / 8);
            if (m.index == UINT_MAX) why = "the case is not in its list";
            else if (!Ref_Building(list - CASE_LIST, m.building)) why = "the building is not in the world's list";
            else m.check = CaseCheck(c);
        }
        *(BYTE*)(rbp + 0x150) = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    if (why) Logf("republicmp  CRIME: the cancel is not carried (%s) - nothing is done", why);
    else Commands_Submit(LS_CMD_CRIME_CANCEL, &m, sizeof(m));
}

bool CrimeCancel_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(CrimeMsg)) return false;
    CrimeMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.index >= MAX_CASES || !(m.check & 1)) return false;
    const char* why = nullptr;
    __try {
        U64 world = (U64)(g_exeBase + WORLD_RVA), b = Ref_BuildingAt(m.building), c = 0;
        if (b) {
            U64 begin = Q(b + CASE_LIST), end = Q(b + CASE_LIST + 8);
            if (end >= begin && m.index < (end - begin) / 8) c = Q(begin + (U64)m.index * 8);
        }
        if (!b) why = "the building is gone";
        else if (!Commands_BuildingInArea(b)) why = "the building is outside the player's area";
        else if (!c || CaseCheck(c) != m.check) why = "the case differs here";
        else {
            int kind = *(int*)(c + CASE_KIND);
            bool counted = kind == 0 || kind == 2 || kind == 3;
            U64 owner = Q(c + CASE_OWNER);
            U64 ob = owner ? Q(owner + OWNER_BUILDING) : 0;
            if (ob && counted) ++*(int*)(Q(ob + BUILDING_RECORD) + kRecordCount[kind]);
            if (counted) ++*(int*)(world + kWorldCount[kind]);
            ((AdvanceFn)(g_exeBase + ADVANCE_RVA))(world, c, 0.0f);
            ((CloseFn)(g_exeBase + CLOSE_RVA))(world, c);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    Logf("republicmp  CRIME: cancel assignment: %s", why ? why : "made");
    return why == nullptr;
}

bool CrimeCancel_Install()
{
    for (const Pinned& p : kPinned)
        if (memcmp(g_exeBase + p.rva, p.bytes, p.n)) {
            Logf("republicmp  CRIME: the cancel button's code at 0x%X differs - it acts on the clicking PC only", p.rva);
            return false;
        }
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) return false;
    // Entered by jmp with the flags of cmp [a54e91],cl and rsp 16-aligned (7677c0's body).
    unsigned char code[160];
    size_t n = 0;
    auto put = [&](std::initializer_list<unsigned char> b) {
        for (unsigned char x : b) code[n++] = x;
    };
    size_t jz = n;
    put({ 0x74, 0x00 });                                     // jz done (no press)
    put({ 0x80, 0x7f, 0x20, 0x00 });                         // cmp byte [rdi+20],0
    size_t je = n;
    put({ 0x74, 0x00 });                                     // je done (not hovered)
    put({ 0x80, 0xbd, 0x50, 0x01, 0x00, 0x00, 0x00 });       // cmp byte [rbp+150],0
    size_t jne = n;
    put({ 0x75, 0x00 });                                     // jne done (a row acted)
    put({ 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 }); // push rax,rcx,rdx,r8..r11
    put({ 0x48, 0x83, 0xec, 0x28 });                         // sub rsp,28 (aligned again)
    put({ 0x4c, 0x89, 0xe1, 0x4c, 0x89, 0xea });             // mov rcx,r12; mov rdx,r13
    put({ 0x4c, 0x8b, 0x85, 0x70, 0x01, 0x00, 0x00 });       // mov r8,[rbp+170]
    put({ 0x49, 0x89, 0xe9 });                               // mov r9,rbp
    U64 capture = (U64)&Capture;
    put({ 0x48, 0xb8 });                                     // mov rax,Capture
    memcpy(code + n, &capture, 8);
    n += 8;
    put({ 0xff, 0xd0 });                                     // call rax
    put({ 0x48, 0x83, 0xc4, 0x28, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58 }); // add rsp,28; pops
    put({ 0x80, 0xbd, 0x50, 0x01, 0x00, 0x00, 0x00 });       // cmp byte [rbp+150],0
    size_t taken = n;
    put({ 0x75, 0x00 });                                     // jne done (carried)
    U64 native = (U64)(g_exeBase + NATIVE_RVA), skip = (U64)(g_exeBase + SKIP_RVA);
    put({ 0xff, 0x25, 0, 0, 0, 0 });                         // jmp [native]: the window's own tests and effect
    memcpy(code + n, &native, 8);
    n += 8;
    size_t done = n;
    put({ 0xff, 0x25, 0, 0, 0, 0 });                         // done: jmp [768681]
    memcpy(code + n, &skip, 8);
    n += 8;
    code[jz + 1] = (unsigned char)(done - (jz + 2));
    code[je + 1] = (unsigned char)(done - (je + 2));
    code[jne + 1] = (unsigned char)(done - (jne + 2));
    code[taken + 1] = (unsigned char)(done - (taken + 2));
    DWORD old;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(relay, code, n);
    VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, n);
    BYTE* at = g_exeBase + JE_SITE;
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
