// SPDX-License-Identifier: GPL-3.0-only
// See citizen_stats.h.
#include "citizen_stats.h"
#include "wallets.h"
#include "../game/world.h" // GAME_BUILDING_POS_OFFSET
#include "../determinism/determinism.h"
#include "../session/save_sync.h"
#include <limits.h>

#define HUD_SITE_RVA 0x31AEE1    // 317db0's pass: mov r10d,[rdx+71c], rdx the citizen
#define RECORD_SITE_RVA 0x2FD3DB // 2fd2e0's loop: cmp dword [rdx+71c],0, rbx the city or 0
#define SITE_SIZE 7
#define CITIZEN_VEHICLE 0x60
#define VEHICLE_POS 0xDE0        // float x, y, z

// Home first, then where the game's own position of a citizen (824660) looks.
static const int kCitizenBuildings[] = { 0x20, 0x58, 0x78, 0x80 };

struct Expect {
    unsigned int rva;
    unsigned char size;
    unsigned char bytes[24];
};

// What the relays rely on; a game build where any differs gets no patch.
static const Expect kExpect[] = {
    // mov rdx,[rax+r11*8] / mov r9b,1 / mov r10d,[rdx+71c] / test r10d,r10d / jnz 31b15d
    { 0x31AEDA, 23, { 0x4A, 0x8B, 0x14, 0xD8, 0x41, 0xB1, 0x01, 0x44, 0x8B, 0x92, 0x1C, 0x07, 0x00, 0x00,
                      0x45, 0x85, 0xD2, 0x0F, 0x85, 0x6C, 0x02, 0x00, 0x00 } },
    // 31b15d: test r9b,r9b / jz 31b408, the loop's continue past every counter
    { 0x31B15D, 9, { 0x45, 0x84, 0xC9, 0x0F, 0x84, 0xA2, 0x02, 0x00, 0x00 } },
    // mov rdx,[rax+rcx*8] / cmp dword [rdx+71c],0 / jnz 2fd656, the loop's tail past the count
    { 0x2FD3D7, 17, { 0x48, 0x8B, 0x14, 0xC8, 0x83, 0xBA, 0x1C, 0x07, 0x00, 0x00, 0x00,
                      0x0F, 0x85, 0x6E, 0x02, 0x00, 0x00 } },
    // 2fd3f3: test rbx,rbx, the city filter
    { 0x2FD3F3, 3, { 0x48, 0x85, 0xDB } },
};

// Each relay keeps every volatile register (xmm0-5 too: the loops keep sums in
// xmm registers) around keep = fn(citizen = rdx, city = rbx), then runs its
// site's tail: the replaced instruction when kept, the skip when not.
static const unsigned char kHead[] = {
    0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, // push rax rcx rdx r8 r9 r10 r11
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,                         // sub rsp, 80h
    0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20,                               // movdqu [rsp+20h], xmm0
    0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,                               // movdqu [rsp+30h], xmm1
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,                               // movdqu [rsp+40h], xmm2
    0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,                               // movdqu [rsp+50h], xmm3
    0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60,                               // movdqu [rsp+60h], xmm4
    0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,                               // movdqu [rsp+70h], xmm5
    0x48, 0x89, 0xD1,                                                 // mov rcx, rdx
    0x48, 0x89, 0xDA,                                                 // mov rdx, rbx
    0x48, 0xB8 };                                                     // mov rax, imm64 (fn)
static const unsigned char kMiddle[] = {
    0xFF, 0xD0,                                                       // call rax
    0x85, 0xC0,                                                       // test eax, eax
    0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20,                               // movdqu xmm0, [rsp+20h]
    0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,                               // movdqu xmm1, [rsp+30h]
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,                               // movdqu xmm2, [rsp+40h]
    0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,                               // movdqu xmm3, [rsp+50h]
    0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60,                               // movdqu xmm4, [rsp+60h]
    0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,                               // movdqu xmm5, [rsp+70h]
    0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00,                   // lea rsp, [rsp+80h] (keeps the flags)
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, // pop r11 r10 r9 r8 rdx rcx rax
    0x74, 0x08 };                                                     // jz skip (past the 8 bytes kept)
// 317db0 skips with r10d = 1 and r9b = 0: its jnz goes to 31b15d, which takes
// r9b = 0 to the loop's continue.
static const unsigned char kHudTail[] = {
    0x44, 0x8B, 0x92, 0x1C, 0x07, 0x00, 0x00, 0xC3,                   // mov r10d, [rdx+71Ch]; ret
    0x41, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x41, 0xB1, 0x00, 0xC3 };     // mov r10d, 1; mov r9b, 0; ret
// 2fd2e0 skips with ZF = 0: its jnz goes to the loop's tail.
static const unsigned char kRecordTail[] = {
    0x83, 0xBA, 0x1C, 0x07, 0x00, 0x00, 0x00, 0xC3,                   // cmp dword [rdx+71Ch], 0; ret
    0x48, 0x85, 0xE4, 0xC3 };                                         // test rsp, rsp; ret

static CitizenStatsHalfRule g_halfRule = nullptr;
static volatile DWORD g_simThread = 0; // the thread of the steps, which runs the day tick

void CitizenStats_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

void CitizenStats_SetHalfRule(CitizenStatsHalfRule rule)
{
    g_halfRule = rule;
}

static void OnStep()
{
    g_simThread = GetCurrentThreadId();
}

static int LocalHalf()
{
    int hostHalf = SaveSync_GetHostSide();
    return SaveSync_IsHostRole() ? hostHalf : 1 - hostHalf;
}

// 0 or 1; -1 when the citizen is nowhere or the cut is not known.
static int CitizenHalf(unsigned long long citizen)
{
    for (int at : kCitizenBuildings)
        if (unsigned long long b = *(unsigned long long*)(citizen + at)) return g_halfRule((const float*)(b + GAME_BUILDING_POS_OFFSET));
    unsigned long long v = *(unsigned long long*)(citizen + CITIZEN_VEHICLE);
    return v ? g_halfRule((const float*)(v + VEHICLE_POS)) : -1;
}

static bool Keep(unsigned long long citizen)
{
    if (!g_halfRule || !Wallets_On() || !Determinism_IsActive()) return true;
    __try {
        int half = CitizenHalf(citizen);
        return half < 0 || half == LocalHalf();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

static int KeepInHud(unsigned long long citizen, unsigned long long)
{
    return Keep(citizen);
}

static int KeepInRecord(unsigned long long citizen, unsigned long long city)
{
    DWORD sim = g_simThread;
    return city || !sim || GetCurrentThreadId() == sim || Keep(citizen);
}

static BYTE* PutRelay(BYTE* at, int (*fn)(unsigned long long, unsigned long long),
                      const unsigned char* tail, size_t tailSize)
{
    unsigned long long to = (unsigned long long)fn;
    memcpy(at, kHead, sizeof(kHead));
    at += sizeof(kHead);
    memcpy(at, &to, 8);
    at += 8;
    memcpy(at, kMiddle, sizeof(kMiddle));
    at += sizeof(kMiddle);
    memcpy(at, tail, tailSize);
    return at + tailSize;
}

// call rel32 to the relay and a 2-byte nop over the site's 7-byte instruction.
static bool PatchSite(unsigned int rva, BYTE* relay)
{
    BYTE* site = g_exeBase + rva;
    long long rel = (long long)relay - (long long)(site + 5);
    if (rel < INT_MIN || rel > INT_MAX) return false;
    unsigned char code[SITE_SIZE] = { 0xE8, 0, 0, 0, 0, 0x66, 0x90 };
    int rel32 = (int)rel;
    memcpy(code + 1, &rel32, 4);
    DWORD old;
    if (!VirtualProtect(site, SITE_SIZE, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site, code, SITE_SIZE);
    VirtualProtect(site, SITE_SIZE, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, SITE_SIZE);
    return true;
}

static bool Matches(const Expect& e)
{
    __try {
        return memcmp(g_exeBase + e.rva, e.bytes, e.size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CitizenStats_Install()
{
    for (const Expect& e : kExpect) {
        if (!Matches(e)) {
            Logf("republicmp  CITIZEN_STATS: the code at %X is not as expected - wrong game build, not patched", e.rva);
            return false;
        }
    }
    const SIZE_T size = 2 * (sizeof(kHead) + 8 + sizeof(kMiddle) + sizeof(kHudTail));
    BYTE* page = AllocNear(g_exeBase + HUD_SITE_RVA, size);
    DWORD old;
    if (!page || !VirtualProtect(page, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    BYTE* hud = page;
    BYTE* record = PutRelay(hud, KeepInHud, kHudTail, sizeof(kHudTail));
    PutRelay(record, KeepInRecord, kRecordTail, sizeof(kRecordTail));
    VirtualProtect(page, size, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, size);
    Determinism_AddStepObserver(OnStep);
    // The mod starts before the game's first instruction: no thread runs these loops yet.
    return PatchSite(HUD_SITE_RVA, hud) && PatchSite(RECORD_SITE_RVA, record);
}
