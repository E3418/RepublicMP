// SPDX-License-Identifier: GPL-3.0-only
// See world_workers.h. The readers' spans and their replacements follow
// SoylentGreed's world settings route (RepublicLAN, read against 1.1.1.9).
#include "world_workers.h"
#include "commands.h"
#include "../session/save_sync.h"
#include <string.h>
#include <limits.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define WIDGET_OFFSET 0xC790   // world: the field's widget (heap; null before the menu has one)
#define WIDGET_GET_RVA 0xFF6A0 // float ff6a0(widget): its value
#define WIDGET_SET_RVA 0xFF520 // ff520(widget, float): sets it
#define MAGIC 0x31574B57

static const unsigned char kGetEntry[16] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0x2d, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x57, 0xc0 };
static const unsigned char kSetEntry[16] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x0f, 0x57, 0xdb, 0x4c, 0x8d, 0x05 };

// A reader: the checked bytes (the patched span first, then what follows it)
// and the span's replacement, with the relay call's rel32 at callAt + 1.
struct ReaderSite {
    unsigned rva, span, checked;
    unsigned char original[36];
    unsigned char patch[22];
    unsigned callAt;
    const char* name;
};
static const ReaderSite kReaders[] = {
    // cmp [r13+c790],0; je 2d4696; mov rcx,[r13+c790]; call ff6a0 | cvttss2si ecx,xmm0; mov rax,[rbp-80]; mov [rax+630],ecx
    // -> mov rcx,r13; call WorkersAt; test rax,rax; js 2d4696; mov ecx,eax; jmp 2d468c; nop5
    { 0x2D4672, 22, 36,
      { 0x49, 0x83, 0xbd, 0x90, 0xc7, 0x00, 0x00, 0x00, 0x74, 0x1a, 0x49, 0x8b, 0x8d, 0x90, 0xc7, 0x00, 0x00, 0xe8,
        0x18, 0xb0, 0xe2, 0xff, 0xf3, 0x0f, 0x2c, 0xc8, 0x48, 0x8b, 0x45, 0x80, 0x89, 0x88, 0x30, 0x06, 0x00, 0x00 },
      { 0x4c, 0x89, 0xe9, 0xe8, 0, 0, 0, 0, 0x48, 0x85, 0xc0, 0x78, 0x17, 0x89, 0xc1, 0xeb, 0x09, 0x90, 0x90, 0x90, 0x90, 0x90 },
      3, "building tool" },
    // mov rcx,[rsi+c790]; test rcx,rcx; je 16ab00; call ff6a0 | cvttss2si eax,xmm0; mov [rdi+630],eax
    // -> mov rcx,rsi; call WorkersAt; test rax,rax; js 16ab00; jmp 16aafa; nop2
    { 0x16AAE5, 17, 27,
      { 0x48, 0x8b, 0x8e, 0x90, 0xc7, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x0f, 0xe8, 0xaa, 0x4b, 0xf9, 0xff,
        0xf3, 0x0f, 0x2c, 0xc0, 0x89, 0x87, 0x30, 0x06, 0x00, 0x00 },
      { 0x48, 0x89, 0xf1, 0xe8, 0, 0, 0, 0, 0x48, 0x85, 0xc0, 0x78, 0x0e, 0xeb, 0x06, 0x90, 0x90 }, 3, "demolition" },
};

#pragma pack(push, 1)
struct WorkersMsg {
    unsigned magic;
    int present; // the field exists (0: the readers skip, as without a widget)
    int workers;
};
#pragma pack(pop)

typedef float (*GetFn)(U64 widget);
typedef void (*SetFn)(U64 widget, float value);

// The session's values, read by both readers on any thread: HELD | PRESENT | value.
// Slot 1 is the second player's own in a mode that keeps it per player (Republics).
#define HELD_BIT (1ull << 62)
#define PRESENT_BIT (1ull << 32)
static volatile LONG64 g_held[2] = {};
static const __int64 kSkip = -1; // the readers' "no widget": they leave +630 alone

// Main thread: the session's values, what this PC's field showed at the last
// frame (a change of it is the player's edit) and what was sent.
static bool g_started = false;
static WorkersMsg g_shared[2] = {}, g_seen = {}, g_sent = {};
static bool g_sentValid = false;

void WorldWorkers_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

static int Slot(int player) { return player > 0 && Commands_PerPlayer() ? 1 : 0; } // any player but the host, as the area rule
static int LocalSlot() { return Slot(SaveSync_IsHostRole() ? 0 : 1); }

static U64 Widget() { return *(U64*)(g_exeBase + WORLD_RVA + WIDGET_OFFSET); }

static bool ReadLocal(WorkersMsg& m)
{
    m = { MAGIC, 0, 0 };
    __try {
        U64 widget = Widget();
        if (!widget) return true;
        m.present = 1;
        m.workers = (int)((GetFn)(g_exeBase + WIDGET_GET_RVA))(widget);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void Hold(int slot, const WorkersMsg& m)
{
    InterlockedExchange64(&g_held[slot], (LONG64)(HELD_BIT | (m.present ? PRESENT_BIT | (unsigned)m.workers : 0)));
}

// Both readers: the value stored as the site's +630, or kSkip. A placement or
// demolition command takes its player's; the simulation's own demolitions the host's.
static __int64 __cdecl WorkersAt(U64 world)
{
    U64 v = (U64)InterlockedCompareExchange64(&g_held[Slot(Commands_ApplyingPlayer())], 0, 0);
    if (v & HELD_BIT) return (v & PRESENT_BIT) ? (__int64)(unsigned)v : kSkip;
    __try {
        U64 widget = world ? *(U64*)(world + WIDGET_OFFSET) : 0;
        if (!widget) return kSkip;
        return (__int64)(unsigned)(int)((GetFn)(g_exeBase + WIDGET_GET_RVA))(widget);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kSkip;
    }
}

// This PC's field shows this PC's player's value.
static void ShowShared()
{
    const WorkersMsg& own = g_shared[LocalSlot()];
    if (!own.present) return;
    __try {
        U64 widget = Widget();
        if (!widget) return;
        ((SetFn)(g_exeBase + WIDGET_SET_RVA))(widget, (float)own.workers);
        g_seen = own;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WorldWorkers_Collect()
{
    if (!Commands_Capturing()) {
        g_started = false;
        InterlockedExchange64(&g_held[0], 0);
        InterlockedExchange64(&g_held[1], 0);
        return;
    }
    WorkersMsg local;
    if (!ReadLocal(local)) return;
    if (!g_started) {
        // The session's start: every PC loaded the same save, every player starts with its value.
        // shortcut: the second player's own value is not saved; a load starts both from the save's.
        g_shared[0] = g_shared[1] = g_seen = local;
        g_sentValid = false;
        g_started = true;
        Hold(0, local);
        Hold(1, local);
        return;
    }
    const WorkersMsg& own = g_shared[LocalSlot()];
    if (local.present && g_seen.present) {
        if (local.workers != g_seen.workers && (local.workers != own.workers || !own.present) &&
            !(g_sentValid && g_sent.workers == local.workers)) {
            Commands_Submit(LS_CMD_WORKERS, &local, sizeof(local));
            g_sent = local;
            g_sentValid = true;
        }
    } else if (local.present && own.present && local.workers != own.workers) {
        // The field appeared (the menu made its widget): it shows the session's value.
        ShowShared();
        return;
    }
    g_seen = local;
}

bool WorldWorkers_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(WorkersMsg)) return false;
    WorkersMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.present < 0 || m.present > 1 || (!m.present && m.workers)) return false;
    int slot = Slot(Commands_ApplyingPlayer());
    g_shared[slot] = m;
    g_started = true;
    Hold(slot, m);
    if (slot == LocalSlot()) {
        g_sentValid = false;
        WorkersMsg local;
        if (ReadLocal(local) && local.present && (local.workers != m.workers)) ShowShared();
        else if (local.present) g_seen = local;
    }
    Logf("republicmp  WORKERS: max. workers without an office = %d%s%s", m.workers, m.present ? "" : " (no field)",
         slot ? " (the second player's)" : "");
    return true;
}

bool WorldWorkers_Install()
{
    if (memcmp(g_exeBase + WIDGET_GET_RVA, kGetEntry, sizeof(kGetEntry)) ||
        memcmp(g_exeBase + WIDGET_SET_RVA, kSetEntry, sizeof(kSetEntry))) {
        Logf("republicmp  WORKERS: the field's routines differ - its value stays each PC's own");
        return false;
    }
    for (const ReaderSite& s : kReaders)
        if (memcmp(g_exeBase + s.rva, s.original, s.checked)) {
            Logf("republicmp  WORKERS: the %s's reader differs - its value stays each PC's own", s.name);
            return false;
        }
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) return false;
    DWORD old;
    if (!VirtualProtect(relay, 16, PAGE_EXECUTE_READWRITE, &old)) return false;
    static const unsigned char kJump[6] = { 0xFF, 0x25, 0, 0, 0, 0 }; // jmp [rip]
    U64 target = (U64)&WorkersAt;
    memcpy(relay, kJump, sizeof(kJump));
    memcpy(relay + sizeof(kJump), &target, 8);
    VirtualProtect(relay, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, 16);
    for (const ReaderSite& s : kReaders) {
        BYTE* at = g_exeBase + s.rva;
        unsigned char code[22];
        memcpy(code, s.patch, sizeof(code));
        long long to = (long long)relay - (long long)(at + s.callAt + 5);
        if (to < INT_MIN || to > INT_MAX) return false;
        int rel32 = (int)to;
        memcpy(code + s.callAt + 1, &rel32, 4);
        if (!VirtualProtect(at, s.span, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(at, code, s.span);
        VirtualProtect(at, s.span, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, s.span);
    }
    return true;
}
