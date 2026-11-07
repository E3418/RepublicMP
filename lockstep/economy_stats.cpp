// SPDX-License-Identifier: GPL-3.0-only
// See economy_stats.h.
#include "economy_stats.h"
#include "wallets.h"
#include "../save/mod_save.h"
#include <algorithm>
#include <math.h>
#include <string>
#include <vector>

#define WORLD_RVA 0x9D4F10
#define STAT_CURRENT 0x11328     // the current record
#define LIST_FIRST 0x08          // its 16 resource lists {begin, end, cap}, 0x18 apart
#define LIST_STRIDE 0x18
#define LIST_COUNT 16
#define LIST_IMPORT_RUB 2        // +0x38
#define LIST_SPEND_CONSTRUCTIONS 8 // +0xC8
#define LIST_PRODUCED 12         // +0x128
#define STATS_ADD_RVA 0x2FDB00
#define STATS_ADD_LIST_RVA 0x2FDF40
#define RECORD_RESET_RVA 0x2FC290
#define RESOURCE_GET_RVA 0x2AA830 // (world, name): the resource of that name, or null
#define RESOURCE_NAME_BYTES 64    // a Resource starts with its name, NUL-terminated
#define RESET_LOG_LINES 6

typedef unsigned long long U64;

// The game's list entry.
struct Entry {
    U64 res;
    float qty;
    float money;
};

// East minus West, of one resource in one list.
struct Flow {
    U64 res;
    double qty, money;
};
typedef std::vector<Flow> Lists[LIST_COUNT];

struct Past {
    int index, day, year;
    Lists lists;
};

// (world, source {resource, a, b}, list, price, use b, multiplier, money = b)
typedef U64 (*StatsAddFn)(U64, U64, U64, float, U64, float, U64);
// (unused, source list, list, price, unused, multiplier, unused, mode)
typedef U64 (*StatsAddListFn)(U64, U64, U64, float, U64, float, U64, U64);
typedef U64 (*ResourceGetFn)(U64 world, const char* name);

static StatsAddFn o_statsAdd = nullptr;
static StatsAddListFn o_statsAddList = nullptr;
static void* o_recordReset = nullptr;

static const BYTE kStatsAddExpect[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30
};
static const BYTE kStatsAddListExpect[14] = {
    0x40, 0x53, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0x42, 0x08
};
// xor ecx,ecx / mov [rdx],rcx / mov [rdx+188h],rcx / mov [rdx+190h],rcx (a leaf)
static const BYTE kRecordResetExpect[19] = {
    0x33, 0xC9, 0x48, 0x89, 0x0A, 0x48, 0x89, 0x8A, 0x88, 0x01, 0x00, 0x00,
    0x48, 0x89, 0x8A, 0x90, 0x01, 0x00, 0x00
};

// Under g_lock: the Filling worker adds while the main thread waits, the
// interface from the render thread.
static Lists g_current;          // the current record's
static std::vector<Past> g_past; // each history record's, by index, ascending
static int g_resetsLogged = 0;

static void WriteShares();
static void ReadShares(bool found);

void EconomyStats_Bind(const ModHost* host)
{
    ModHost_Bind(host);
    ModSave_Add("economy", WriteShares, ReadShares);
}

static U64 World()
{
    return (U64)(g_exeBase + WORLD_RVA);
}

static U64 Current()
{
    return World() + STAT_CURRENT;
}

// Which of the current record's lists `list` is, or -1.
static int ListIndex(U64 list)
{
    U64 first = Current() + LIST_FIRST;
    if (list < first || (list - first) % LIST_STRIDE) return -1;
    U64 i = (list - first) / LIST_STRIDE;
    return i < LIST_COUNT ? (int)i : -1;
}

static Entry* Begin(U64 list) { return *(Entry**)list; }
static Entry* End(U64 list) { return *(Entry**)(list + 8); }

static void AddFlow(std::vector<Flow>& flows, U64 res, double qty, double money)
{
    for (Flow& f : flows) {
        if (f.res == res) {
            f.qty += qty;
            f.money += money;
            return;
        }
    }
    flows.push_back({ res, qty, money });
}

// Both only add to an entry in place or append one (a resource can have two, its
// positive and negative amounts), so what a call added is entry i after minus
// entry i before.
static thread_local std::vector<Entry> t_before;

static void AddChanges(int half, int index, U64 list)
{
    double sign = half == 0 ? 1.0 : -1.0;
    size_t i = 0;
    EnterCriticalSection(&g_lock);
    for (Entry* it = Begin(list); it < End(list); ++it, ++i) {
        Entry was = i < t_before.size() ? t_before[i] : Entry{ it->res, 0.0f, 0.0f };
        double qty = (double)it->qty - was.qty, money = (double)it->money - was.money;
        if (qty != 0.0 || money != 0.0) AddFlow(g_current[index], it->res, sign * qty, sign * money);
    }
    LeaveCriticalSection(&g_lock);
}

static U64 H_StatsAdd(U64 world, U64 src, U64 list, float price, U64 useB, float mult, U64 direct)
{
    int index = ListIndex(list);
    int half = index < 0 ? -1 : Wallets_Half();
    if (half < 0) return o_statsAdd(world, src, list, price, useB, mult, direct);
    t_before.assign(Begin(list), End(list));
    U64 r = o_statsAdd(world, src, list, price, useB, mult, direct);
    AddChanges(half, index, list);
    return r;
}

static U64 H_StatsAddList(U64 a, U64 src, U64 list, float price, U64 e, float mult, U64 g, U64 mode)
{
    int index = ListIndex(list);
    int half = index < 0 ? -1 : Wallets_Half();
    if (half < 0) return o_statsAddList(a, src, list, price, e, mult, g, mode);
    t_before.assign(Begin(list), End(list));
    U64 r = o_statsAddList(a, src, list, price, e, mult, g, mode);
    AddChanges(half, index, list);
    return r;
}

// ---------------------------------------------------------------------------
// The record's reset and push
// ---------------------------------------------------------------------------

static double ListMoney(U64 list)
{
    double sum = 0.0;
    for (Entry* e = Begin(list); e < End(list); ++e) sum += e->money;
    return sum;
}

static double FlowMoney(const std::vector<Flow>& flows)
{
    double sum = 0.0;
    for (const Flow& f : flows) sum += f.money;
    return sum;
}

// The game is about to empty the current record (the day tick after its push, a load).
static void OnCurrentReset()
{
    EnterCriticalSection(&g_lock);
    if (Wallets_On() && g_resetsLogged < RESET_LOG_LINES) {
        ++g_resetsLogged;
        static const int kLogged[] = { LIST_PRODUCED, LIST_SPEND_CONSTRUCTIONS, LIST_IMPORT_RUB };
        static const char* kNames[] = { "produced", "constructions", "imports in rubles" };
        char line[512];
        int n = sprintf_s(line, "republicmp  ECONOMY: the record closes - money total / East / West:");
        for (int i = 0; i < 3 && n > 0; i++) {
            double total = ListMoney(Current() + LIST_FIRST + (U64)kLogged[i] * LIST_STRIDE);
            double d = FlowMoney(g_current[kLogged[i]]);
            n += sprintf_s(line + n, sizeof(line) - n, " %s %.0f / %.0f / %.0f%s", kNames[i], total,
                           (total + d) / 2, (total - d) / 2, i < 2 ? "," : "");
        }
        Logf("%s", line);
    }
    for (auto& flows : g_current) flows.clear();
    LeaveCriticalSection(&g_lock);
}

static void OnRecordReset(U64 record)
{
    if (record == Current()) OnCurrentReset();
}

void EconomyStats_HistoryPushed(int index, int day, int year)
{
    EnterCriticalSection(&g_lock);
    while (!g_past.empty() && g_past.back().index >= index) g_past.pop_back(); // records the game dropped
    g_past.emplace_back();
    Past& p = g_past.back();
    p.index = index;
    p.day = day;
    p.year = year;
    for (int i = 0; i < LIST_COUNT; i++) p.lists[i] = g_current[i];
    LeaveCriticalSection(&g_lock);
}

void EconomyStats_HistoryCleared()
{
    EnterCriticalSection(&g_lock);
    g_past.clear();
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------------------
// The save
// ---------------------------------------------------------------------------
//
// [economy]  <index> = day year, then that history record's lists' entries:
// list:qty:money:name, ';' between. The current record's are not kept: a load
// empties the game's own (433daa, after reading the save), and so the reset hook ours.

static std::vector<std::pair<U64, std::string>> g_names; // WriteShares': each resource's name, once

static const char* NameOf(U64 res)
{
    for (const auto& n : g_names)
        if (n.first == res) return n.second.c_str();
    char name[RESOURCE_NAME_BYTES];
    g_names.emplace_back(res, SafeReadStr((const void*)res, name, sizeof(name)) ? name : "");
    return g_names.back().second.c_str();
}

static void AppendLists(std::string& out, const Lists& lists)
{
    char one[96 + RESOURCE_NAME_BYTES];
    bool first = true;
    for (int i = 0; i < LIST_COUNT; i++) {
        for (const Flow& f : lists[i]) {
            const char* name = NameOf(f.res);
            if (!*name || !isfinite(f.qty) || !isfinite(f.money) || (f.qty == 0.0 && f.money == 0.0)) continue;
            sprintf_s(one, "%s%d:%.17g:%.17g:%s", first ? "" : ";", i, f.qty, f.money, name);
            out += one;
            first = false;
        }
    }
}

static void WriteShares()
{
    std::string value;
    char key[16], date[32];
    EnterCriticalSection(&g_lock);
    g_names.clear();
    for (const Past& p : g_past) {
        sprintf_s(key, "%d", p.index);
        sprintf_s(date, "%d %d ", p.day, p.year);
        value = date;
        AppendLists(value, p.lists);
        ModSave_Put(key, value.c_str());
    }
    LeaveCriticalSection(&g_lock);
}

static std::vector<std::pair<std::string, U64>> g_resources; // ReadShares': each name's resource (0: none), once

static U64 ResourceNamed(const std::string& name)
{
    for (const auto& r : g_resources)
        if (r.first == name) return r.second;
    U64 res = ((ResourceGetFn)(g_exeBase + RESOURCE_GET_RVA))(World(), name.c_str());
    g_resources.emplace_back(name, res);
    return res;
}

// Entries as AppendLists writes them; a resource this game does not have is left out.
static void ParseLists(const char* text, Lists& lists)
{
    std::string all = text;
    for (size_t at = 0, next; at < all.size(); at = next) {
        size_t semi = all.find(';', at);
        next = semi == std::string::npos ? all.size() : semi + 1;
        std::string token = all.substr(at, (semi == std::string::npos ? all.size() : semi) - at);
        int list = -1, used = 0;
        double qty = 0.0, money = 0.0;
        if (sscanf_s(token.c_str(), "%d:%lf:%lf:%n", &list, &qty, &money, &used) != 3 || used == 0) continue;
        if (list < 0 || list >= LIST_COUNT || !isfinite(qty) || !isfinite(money)) continue;
        U64 res = ResourceNamed(token.substr((size_t)used));
        if (res) AddFlow(lists[list], res, qty, money);
    }
}

static void ReadOne(const char* key, const char* value, void*)
{
    char* end;
    long index = strtol(key, &end, 10);
    if (*end || end == key || index < 0) return;
    Past p = {};
    p.index = (int)index;
    p.day = (int)strtol(value, &end, 10);
    p.year = (int)strtol(end, &end, 10);
    while (*end == ' ') ++end;
    ParseLists(end, p.lists);
    g_past.push_back(std::move(p));
}

// Right after the load (whose own reset emptied the current shares).
static void ReadShares(bool found)
{
    EnterCriticalSection(&g_lock);
    for (auto& flows : g_current) flows.clear();
    g_past.clear();
    g_resources.clear();
    if (found) {
        ModSave_ForEach(ReadOne, nullptr);
        std::sort(g_past.begin(), g_past.end(), [](const Past& a, const Past& b) { return a.index < b.index; });
    }
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

// 2fc290 is a leaf: its callers keep r8 and rdx across it (a new city's record,
// 32f50d and 33381e). So its hook is a relay that keeps every volatile register
// around OnRecordReset(rdx) and jumps on to the trampoline.
static const unsigned char kResetHead[] = {
    0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, // push rax rcx rdx r8 r9 r10 r11
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,                         // sub rsp, 80h
    0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20,                               // movdqu [rsp+20h], xmm0
    0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,                               // movdqu [rsp+30h], xmm1
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,                               // movdqu [rsp+40h], xmm2
    0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,                               // movdqu [rsp+50h], xmm3
    0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60,                               // movdqu [rsp+60h], xmm4
    0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,                               // movdqu [rsp+70h], xmm5
    0x48, 0x89, 0xD1,                                                 // mov rcx, rdx
    0x48, 0xB8 };                                                     // mov rax, imm64 (OnRecordReset)
static const unsigned char kResetTail[] = {
    0xFF, 0xD0,                                                       // call rax
    0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20,                               // movdqu xmm0, [rsp+20h]
    0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,                               // movdqu xmm1, [rsp+30h]
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,                               // movdqu xmm2, [rsp+40h]
    0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,                               // movdqu xmm3, [rsp+50h]
    0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60,                               // movdqu xmm4, [rsp+60h]
    0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,                               // movdqu xmm5, [rsp+70h]
    0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00,                   // lea rsp, [rsp+80h]
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, // pop r11 r10 r9 r8 rdx rcx rax
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };                             // jmp [rip+0] (the trampoline, next)

static bool InstallResetHook()
{
    const size_t size = sizeof(kResetHead) + 8 + sizeof(kResetTail) + 8;
    BYTE* relay = AllocNear(g_exeBase + RECORD_RESET_RVA, size);
    DWORD old;
    if (!relay || !VirtualProtect(relay, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    U64 fn = (U64)&OnRecordReset;
    BYTE* at = relay;
    memcpy(at, kResetHead, sizeof(kResetHead));
    at += sizeof(kResetHead);
    memcpy(at, &fn, 8);
    at += 8;
    memcpy(at, kResetTail, sizeof(kResetTail));
    at += sizeof(kResetTail);
    // Before any game thread runs: the trampoline's address is written right after.
    bool ok = InstallInlineHook(g_exeBase + RECORD_RESET_RVA, relay, &o_recordReset,
                                kRecordResetExpect, sizeof(kRecordResetExpect), "StatRecord_Reset");
    U64 trampoline = (U64)o_recordReset;
    memcpy(at, &trampoline, 8);
    VirtualProtect(relay, size, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, size);
    return ok;
}

bool EconomyStats_Install()
{
    BYTE* exe = g_exeBase;
    return InstallInlineHook(exe + STATS_ADD_RVA, (void*)&H_StatsAdd, (void**)&o_statsAdd,
                             kStatsAddExpect, sizeof(kStatsAddExpect), "Stats_Add")
        && InstallInlineHook(exe + STATS_ADD_LIST_RVA, (void*)&H_StatsAddList, (void**)&o_statsAddList,
                             kStatsAddListExpect, sizeof(kStatsAddListExpect), "Stats_AddList")
        && InstallResetHook();
}
