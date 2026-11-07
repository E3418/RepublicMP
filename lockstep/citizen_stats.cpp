// SPDX-License-Identifier: GPL-3.0-only
// See citizen_stats.h.
#include "citizen_stats.h"
#include "wallets.h"
#include "economy_stats.h"
#include "../game/world.h" // GAME_BUILDING_POS_OFFSET
#include "../determinism/determinism.h"
#include "../save/mod_save.h"
#include "../session/save_sync.h"
#include <limits.h>
#include <algorithm>
#include <vector>

#define HUD_SITE_RVA 0x31AEE1    // 317db0's pass: mov r10d,[rdx+71c], rdx the citizen
#define RECORD_SITE_RVA 0x2FD3DB // 2fd2e0's loop: cmp dword [rdx+71c],0, rbx the city or 0
#define GRAPH_SITE_RVA 0x34C5ED  // 34b470's graphs: mov r8d,[rbx+4] / mov rcx,rdi, rbx the record drawn
#define SITE_SIZE 7
#define RECORD_STATS_RVA 0x2FD2E0 // (world, record, city): a record's citizens
#define HISTORY_PUSH_RVA 0x4488D0 // (vector, record): the history's push, copies the record
#define WORLD_RVA 0x9D4F10
#define STAT_CURRENT 0x11328     // the current statistics record
#define STAT_HISTORY 0x11728     // the history: a vector of records {begin, end, cap}
#define RECORD_SIZE 0x400        // a record: +0 day of the year, +4 year
#define CITIZEN_VEHICLE 0x60
#define VEHICLE_POS 0xDE0        // float x, y, z

typedef unsigned long long U64;

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
    // shl rbx,10 / add rbx,[rdi+11728] / mov r8d,[rbx+4] / mov rcx,rdi / mov edx,[rbx]
    { 0x34C5E2, 20, { 0x48, 0xC1, 0xE3, 0x0A, 0x48, 0x03, 0x9F, 0x28, 0x17, 0x01, 0x00,
                      0x44, 0x8B, 0x43, 0x04, 0x48, 0x8B, 0xCF, 0x8B, 0x13 } },
};

// Each relay keeps every volatile register (xmm0-5 too: the loops keep sums in
// xmm registers) around its fn, then runs its site's tail.
static const unsigned char kSave[] = {
    0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, // push rax rcx rdx r8 r9 r10 r11
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,                         // sub rsp, 80h
    0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20,                               // movdqu [rsp+20h], xmm0
    0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,                               // movdqu [rsp+30h], xmm1
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,                               // movdqu [rsp+40h], xmm2
    0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,                               // movdqu [rsp+50h], xmm3
    0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60,                               // movdqu [rsp+60h], xmm4
    0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70 };                             // movdqu [rsp+70h], xmm5
// No instruction here changes the flags.
static const unsigned char kRestore[] = {
    0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20,                               // movdqu xmm0, [rsp+20h]
    0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,                               // movdqu xmm1, [rsp+30h]
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,                               // movdqu xmm2, [rsp+40h]
    0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,                               // movdqu xmm3, [rsp+50h]
    0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60,                               // movdqu xmm4, [rsp+60h]
    0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,                               // movdqu xmm5, [rsp+70h]
    0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00,                   // lea rsp, [rsp+80h]
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58 }; // pop r11 r10 r9 r8 rdx rcx rax

struct Relay {
    const unsigned char* args;  // before the call: the fn's arguments
    size_t argsSize;
    const void* fn;
    const unsigned char* after; // right after the call
    size_t afterSize;
    const unsigned char* tail;  // after the registers are back
    size_t tailSize;
};

// The loops: keep = fn(citizen = rdx, city = rbx); kept, the replaced
// instruction; skipped, the loop's own way past the citizen.
static const unsigned char kLoopArgs[] = {
    0x48, 0x89, 0xD1,                                                 // mov rcx, rdx
    0x48, 0x89, 0xDA };                                               // mov rdx, rbx
static const unsigned char kLoopAfter[] = { 0x85, 0xC0 };            // test eax, eax
// 317db0 skips with r10d = 1 and r9b = 0: its jnz goes to 31b15d, which takes
// r9b = 0 to the loop's continue.
static const unsigned char kHudTail[] = {
    0x74, 0x08,                                                       // jz skip
    0x44, 0x8B, 0x92, 0x1C, 0x07, 0x00, 0x00, 0xC3,                   // mov r10d, [rdx+71Ch]; ret
    0x41, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x41, 0xB1, 0x00, 0xC3 };     // skip: mov r10d, 1; mov r9b, 0; ret
// 2fd2e0 skips with ZF = 0: its jnz goes to the loop's tail.
static const unsigned char kRecordTail[] = {
    0x74, 0x08,                                                       // jz skip
    0x83, 0xBA, 0x1C, 0x07, 0x00, 0x00, 0x00, 0xC3,                   // cmp dword [rdx+71Ch], 0; ret
    0x48, 0x85, 0xE4, 0xC3 };                                         // skip: test rsp, rsp; ret
// The graphs: rbx = fn(rbx), then the two replaced instructions.
static const unsigned char kGraphArgs[] = { 0x48, 0x89, 0xD9 };      // mov rcx, rbx
static const unsigned char kGraphAfter[] = { 0x48, 0x89, 0xC3 };     // mov rbx, rax
static const unsigned char kGraphTail[] = {
    0x44, 0x8B, 0x43, 0x04,                                           // mov r8d, [rbx+4]
    0x48, 0x8B, 0xCF,                                                 // mov rcx, rdi
    0xC3 };                                                           // ret

typedef U64 (*PushFn)(U64 vector, U64 record);
typedef void (*RecordStatsFn)(U64 world, U64 record, U64 city);
static PushFn o_push = nullptr;
// push rdi / sub rsp,30h / mov qword [rsp+20h],-2
static const BYTE kPushExpect[15] = {
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF
};

// The fields of a record's citizens the graphs draw (2fd2e0 fills them): the
// needs' averages +25c.., then lifespan, age and the counts +2ac..
struct Span {
    int offset, count;
};
static const Span kSpans[] = { { 0x25C, 9 }, { 0x2AC, 16 } };
#define SAMPLE_DWORDS 25

// Each half's citizens when the game pushed the history record `index`.
struct Sample {
    int index, day, year;
    unsigned int half[2][SAMPLE_DWORDS];
};

static CitizenStatsHalfRule g_halfRule = nullptr;
static volatile DWORD g_simThread = 0; // the thread of the steps, which runs the day tick
static std::vector<Sample> g_history;  // by index, ascending; under g_lock
static int g_sampleHalf = -1;          // the push's own counts, on g_sampleThread
static DWORD g_sampleThread = 0;
static BYTE g_sampled[RECORD_SIZE];    // what those counts fill
static BYTE g_drawn[RECORD_SIZE];      // the record a graph draws (render thread)

static void WriteHistory();
static void ReadHistory(bool found);

void CitizenStats_Bind(const ModHost* host)
{
    ModHost_Bind(host);
    ModSave_Add("citizen_history", WriteHistory, ReadHistory);
}

void CitizenStats_SetHalfRule(CitizenStatsHalfRule rule)
{
    g_halfRule = rule;
}

static void OnStep()
{
    g_simThread = GetCurrentThreadId();
}

static U64 World()
{
    return (U64)(g_exeBase + WORLD_RVA);
}

static bool On()
{
    return g_halfRule && Wallets_On() && Determinism_IsActive();
}

static int LocalHalf()
{
    int hostHalf = SaveSync_GetHostSide();
    return SaveSync_IsHostRole() ? hostHalf : 1 - hostHalf;
}

// 0 or 1; -1 when the citizen is nowhere or the cut is not known.
static int CitizenHalf(U64 citizen)
{
    for (int at : kCitizenBuildings)
        if (U64 b = *(U64*)(citizen + at)) return g_halfRule((const float*)(b + GAME_BUILDING_POS_OFFSET));
    U64 v = *(U64*)(citizen + CITIZEN_VEHICLE);
    return v ? g_halfRule((const float*)(v + VEHICLE_POS)) : -1;
}

// Counted in `half`: one of its citizens, or one with no known half (both count it).
static bool InHalf(U64 citizen, int half)
{
    __try {
        int h = CitizenHalf(citizen);
        return h < 0 || h == half;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

static int KeepInHud(U64 citizen, U64)
{
    return !On() || InHalf(citizen, LocalHalf());
}

static int KeepInRecord(U64 citizen, U64 city)
{
    if (g_sampleHalf >= 0 && GetCurrentThreadId() == g_sampleThread) return InHalf(citizen, g_sampleHalf);
    DWORD sim = g_simThread;
    return city || !sim || GetCurrentThreadId() == sim || !On() || InHalf(citizen, LocalHalf());
}

static const Sample* FindSample(int index)
{
    auto it = std::lower_bound(g_history.begin(), g_history.end(), index,
                               [](const Sample& s, int i) { return s.index < i; });
    return it != g_history.end() && it->index == index ? &*it : nullptr;
}

// The day tick pushes the current record, its citizens just counted: count them
// again for each half, with the game's own count. On the simulation thread, the
// same on every PC; the citizens it initializes the whole count did already.
static void SampleHalves(U64 record)
{
    U64 history = World() + STAT_HISTORY;
    Sample s = {};
    s.index = (int)((*(U64*)(history + 8) - *(U64*)history) / RECORD_SIZE);
    s.day = *(int*)record;
    s.year = *(int*)(record + 4);
    g_sampleThread = GetCurrentThreadId();
    for (int h = 0; h < 2; h++) {
        memset(g_sampled, 0, sizeof(g_sampled));
        g_sampleHalf = h;
        ((RecordStatsFn)(g_exeBase + RECORD_STATS_RVA))(World(), (U64)g_sampled, 0);
        g_sampleHalf = -1;
        int n = 0;
        for (const Span& span : kSpans) {
            memcpy(&s.half[h][n], g_sampled + span.offset, (size_t)span.count * 4);
            n += span.count;
        }
    }
    EnterCriticalSection(&g_lock);
    while (!g_history.empty() && g_history.back().index >= s.index) g_history.pop_back(); // records the game dropped
    g_history.push_back(s);
    LeaveCriticalSection(&g_lock);
}

static U64 H_Push(U64 vector, U64 record)
{
    U64 history = World() + STAT_HISTORY;
    if (vector == history && record == World() + STAT_CURRENT) {
        if (On() && Determinism_IsStepFrame()) {
            SampleHalves(record);
            int index = (int)((*(U64*)(history + 8) - *(U64*)history) / RECORD_SIZE);
            EconomyStats_HistoryPushed(index, *(int*)record, *(int*)(record + 4));
        } else if (*(U64*)(history + 8) == *(U64*)history) {
            // The first record of a new game, which reads no save (a load reads its own after this).
            EnterCriticalSection(&g_lock);
            g_history.clear();
            LeaveCriticalSection(&g_lock);
            EconomyStats_HistoryCleared();
        }
    }
    return o_push(vector, record);
}

// The record a graph is about to draw: a copy with this PC's half when one was
// sampled for it. The current record is already this half's (2fd2e0 counted it
// for the window just before, on this thread).
static U64 GraphRecord(U64 record)
{
    U64 history = World() + STAT_HISTORY;
    U64 begin = *(U64*)history;
    if (!On() || record < begin || record >= *(U64*)(history + 8)) return record;
    int index = (int)((record - begin) / RECORD_SIZE);
    int half = LocalHalf();
    EnterCriticalSection(&g_lock);
    const Sample* s = FindSample(index);
    bool found = s && s->day == *(int*)record && s->year == *(int*)(record + 4);
    if (found) {
        memcpy(g_drawn, (const void*)record, RECORD_SIZE);
        int n = 0;
        for (const Span& span : kSpans) {
            memcpy(g_drawn + span.offset, &s->half[half][n], (size_t)span.count * 4);
            n += span.count;
        }
    }
    LeaveCriticalSection(&g_lock);
    return found ? (U64)g_drawn : record;
}

// [citizen_history] index = day year, then each half's fields as hex words.
static void WriteHistory()
{
    char key[16], value[32 + 2 * SAMPLE_DWORDS * 9];
    EnterCriticalSection(&g_lock);
    for (const Sample& s : g_history) {
        sprintf_s(key, "%d", s.index);
        int n = sprintf_s(value, "%d %d", s.day, s.year);
        for (int h = 0; h < 2; h++)
            for (int i = 0; i < SAMPLE_DWORDS; i++) n += sprintf_s(value + n, sizeof(value) - n, " %08x", s.half[h][i]);
        ModSave_Put(key, value);
    }
    LeaveCriticalSection(&g_lock);
}

static void ReadSample(const char* key, const char* value, void*)
{
    Sample s = {};
    char* end;
    s.index = (int)strtol(key, &end, 10);
    if (*end || end == key || s.index < 0) return;
    s.day = (int)strtol(value, &end, 10);
    s.year = (int)strtol(end, &end, 10);
    for (int h = 0; h < 2; h++) {
        for (int i = 0; i < SAMPLE_DWORDS; i++) {
            char* next;
            s.half[h][i] = (unsigned int)strtoul(end, &next, 16);
            if (next == end) return;
            end = next;
        }
    }
    g_history.push_back(s);
}

static void ReadHistory(bool found)
{
    EnterCriticalSection(&g_lock);
    g_history.clear();
    if (found) {
        ModSave_ForEach(ReadSample, nullptr);
        std::sort(g_history.begin(), g_history.end(), [](const Sample& a, const Sample& b) { return a.index < b.index; });
    }
    LeaveCriticalSection(&g_lock);
}

static BYTE* PutRelay(BYTE* at, const Relay& r)
{
    U64 to = (U64)r.fn;
    static const unsigned char kMovRax[] = { 0x48, 0xB8 }; // mov rax, imm64
    static const unsigned char kCallRax[] = { 0xFF, 0xD0 };
    const struct { const void* p; size_t n; } parts[] = {
        { kSave, sizeof(kSave) }, { r.args, r.argsSize }, { kMovRax, sizeof(kMovRax) }, { &to, 8 },
        { kCallRax, sizeof(kCallRax) }, { r.after, r.afterSize }, { kRestore, sizeof(kRestore) },
        { r.tail, r.tailSize },
    };
    for (const auto& part : parts) {
        memcpy(at, part.p, part.n);
        at += part.n;
    }
    return at;
}

// call rel32 to the relay and a 2-byte nop over the site's 7 bytes.
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
    const Relay hud = { kLoopArgs, sizeof(kLoopArgs), (const void*)&KeepInHud, kLoopAfter, sizeof(kLoopAfter),
                        kHudTail, sizeof(kHudTail) };
    const Relay record = { kLoopArgs, sizeof(kLoopArgs), (const void*)&KeepInRecord, kLoopAfter, sizeof(kLoopAfter),
                           kRecordTail, sizeof(kRecordTail) };
    const Relay graph = { kGraphArgs, sizeof(kGraphArgs), (const void*)&GraphRecord, kGraphAfter, sizeof(kGraphAfter),
                          kGraphTail, sizeof(kGraphTail) };
    const SIZE_T size = 4096;
    BYTE* page = AllocNear(g_exeBase + HUD_SITE_RVA, size);
    DWORD old;
    if (!page || !VirtualProtect(page, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    BYTE* hudAt = page;
    BYTE* recordAt = PutRelay(hudAt, hud);
    BYTE* graphAt = PutRelay(recordAt, record);
    PutRelay(graphAt, graph);
    VirtualProtect(page, size, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, size);
    Determinism_AddStepObserver(OnStep);
    // The mod starts before the game's first instruction: no thread runs these loops yet.
    if (!PatchSite(HUD_SITE_RVA, hudAt) || !PatchSite(RECORD_SITE_RVA, recordAt) || !PatchSite(GRAPH_SITE_RVA, graphAt))
        return false;
    return InstallInlineHook(g_exeBase + HISTORY_PUSH_RVA, (void*)&H_Push, (void**)&o_push,
                             kPushExpect, sizeof(kPushExpect), "History_Push");
}
