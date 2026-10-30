// SPDX-License-Identifier: GPL-3.0-only
// See site_watch.h.
#include "site_watch.h"
#include "determinism.h"
#include "../core/profile.h"
#include "../core/guard.h"
#include <vector>
#include <unordered_map>
#include <math.h>

#define BUILDINGS_BEGIN_RVA 0x9E6A18
#define BUILDINGS_END_RVA 0x9E6A20
#define TYPE_OFFSET 0x318        // building: its type
#define CATEGORY_OFFSET 0x360    // type: category
#define POS_OFFSET 0x350         // building: position (x +350, z +358)
#define PROGRESS_OFFSET 0x604    // float: construction progress, 1 = finished
#define STAGES_OFFSET 0x610      // stage records (begin, end)
#define STAGE_SIZE 0x21B8
#define STAGE_OFFSET 0x634       // int: the cached current stage
#define SITE_FLAG_OFFSET 0x761   // byte: drawn as a site even when finished
#define ENTRY_BYTES 16           // a stage's material: definition, have +8, need +c
#define LOG_LIMIT 400            // lines per world load
#define JUMP 0.25f               // progress gained at once that is logged in a tick
#define EARLY_END 0.97f          // a site finished from below this is logged
#define DETAIL_ENTRIES 8         // materials listed for a jump
#define STACK_FRAMES 12
#define SITE_MAKE_RVA 0x25BAB0   // building constructor (on a 0x1420 allocation)
#define SITE_FREE_RVA 0x164BB0   // building destructor

struct SiteState {
    unsigned progress; // the float's bits: any change counts
    int stage;
    int stages;
    BYTE* records;
    float have, need;  // the current stage's materials, summed
    int category;
    float x, z;
};

static bool g_on = false;
static CRITICAL_SECTION g_watchLock;
static std::unordered_map<BYTE*, SiteState> g_sites;
static bool g_looked = false; // g_sites holds a look of this world
static int g_lines = 0;

void SiteWatch_Bind(const TsmHost* host)
{
    TsmBind(host);
    InitializeCriticalSection(&g_watchLock);
    g_on = Profile_GetInt("debug", "site_watch", 0) != 0;
}

bool SiteWatch_GetOn() { return g_on; }

void SiteWatch_SetOn(bool on)
{
    LockScope lock(g_watchLock);
    g_on = on;
    g_sites.clear();
    g_looked = false;
    Profile_SetInt("debug", "site_watch", on ? 1 : 0);
    Logf("republicmp  SITE_WATCH: %s", on ? "on - construction sites changed outside a tick are logged" : "off");
}

void SiteWatch_Reset()
{
    if (!g_on) return;
    LockScope lock(g_watchLock);
    g_sites.clear();
    g_looked = false;
    g_lines = 0;
}

// One building as a site; false: not a site (or unreadable). `any`: a
// finished building is read too.
static bool ReadSite(BYTE* b, SiteState& s, bool any = false)
{
    __try {
        float progress = *(float*)(b + PROGRESS_OFFSET);
        if (!any && !(progress < 1.0f) && !b[SITE_FLAG_OFFSET]) return false;
        s.progress = *(unsigned*)(b + PROGRESS_OFFSET);
        s.stage = *(int*)(b + STAGE_OFFSET);
        s.records = *(BYTE**)(b + STAGES_OFFSET);
        BYTE* end = *(BYTE**)(b + STAGES_OFFSET + 8);
        s.stages = s.records && end >= s.records ? (int)((end - s.records) / STAGE_SIZE) : 0;
        BYTE* type = *(BYTE**)(b + TYPE_OFFSET);
        // A network's site keeps other words there (a float seen as the pointer).
        s.category = type && ((unsigned long long)type >> 32) ? *(int*)(type + CATEGORY_OFFSET) : -1;
        s.x = *(float*)(b + POS_OFFSET);
        s.z = *(float*)(b + POS_OFFSET + 8);
        s.have = s.need = 0;
        if (s.records && s.stage >= 0 && s.stage < s.stages && s.stages <= 64) {
            BYTE* stage = s.records + (size_t)s.stage * STAGE_SIZE;
            BYTE* it = *(BYTE**)(stage + 8);
            BYTE* entriesEnd = *(BYTE**)(stage + 0x10);
            for (int n = 0; it && it < entriesEnd && n < 256; it += ENTRY_BYTES, n++) {
                s.have += *(float*)(it + 8);
                s.need += *(float*)(it + 0xC);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static float AsFloat(unsigned bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static bool Invalid(const SiteState& s)
{
    float p = AsFloat(s.progress);
    return !(p >= 0.0f && p <= 1.0f) || s.stage < -1 || (s.stages && s.stage >= s.stages);
}

static void Line(const char* phase, const char* what, BYTE* b, const SiteState* was, const SiteState& now)
{
    if (++g_lines > LOG_LIMIT) {
        if (g_lines == LOG_LIMIT + 1) Logf("republicmp  SITE_WATCH: %d lines - no more until the next load", LOG_LIMIT);
        return;
    }
    char from[160] = "";
    if (was)
        _snprintf_s(from, sizeof(from), _TRUNCATE, "progress %.9g stage %d of %d records %p materials %.6g of %.6g -> ",
                    AsFloat(was->progress), was->stage, was->stages, was->records, was->have, was->need);
    Logf("republicmp  SITE_WATCH: tick %llu %s (%s frame, thread %lu): site %p cat %d at (%.1f,%.1f) %s: %sprogress %.9g "
         "stage %d of %d records %p materials %.6g of %.6g",
         Determinism_StepCount(), phase, Determinism_IsStepFrame() ? "tick" : "held", GetCurrentThreadId(), b,
         now.category, now.x, now.z, what, from, AsFloat(now.progress), now.stage, now.stages, now.records, now.have,
         now.need);
}

static void Compare(const char* phase, bool tick)
{
    BYTE** it = *(BYTE***)(g_exeBase + BUILDINGS_BEGIN_RVA);
    BYTE** end = *(BYTE***)(g_exeBase + BUILDINGS_END_RVA);
    std::unordered_map<BYTE*, SiteState> now;
    for (; it && it < end; ++it) {
        SiteState s;
        if (*it && ReadSite(*it, s)) now.emplace(*it, s);
    }
    if (g_looked) {
        for (const auto& [b, s] : now) {
            auto was = g_sites.find(b);
            if (was == g_sites.end()) {
                if (!tick || Invalid(s)) Line(phase, "appeared", b, nullptr, s);
                continue;
            }
            const SiteState& w = was->second;
            bool changed = w.progress != s.progress || w.stage != s.stage || w.stages != s.stages ||
                           w.records != s.records || w.have != s.have || w.need != s.need;
            if (!changed) continue;
            if (!tick) Line(phase, "changed outside the tick", b, &w, s);
            else if (Invalid(s) && !Invalid(w)) Line(phase, "became invalid", b, &w, s);
            else if (AsFloat(s.progress) - AsFloat(w.progress) > JUMP) Line(phase, "jumped", b, &w, s);
            else if (s.stage < w.stage && s.records == w.records) Line(phase, "went back a stage", b, &w, s);
        }
        if (!tick)
            for (const auto& [b, w] : g_sites)
                if (!now.count(b)) {
                    if (++g_lines <= LOG_LIMIT)
                        Logf("republicmp  SITE_WATCH: tick %llu %s (%s frame, thread %lu): site %p cat %d at (%.1f,%.1f) "
                             "gone or finished outside the tick (progress was %.9g)",
                             Determinism_StepCount(), phase, Determinism_IsStepFrame() ? "tick" : "held",
                             GetCurrentThreadId(), b, w.category, w.x, w.z, AsFloat(w.progress));
                }
    }
    g_sites.swap(now);
    g_looked = true;
}

void SiteWatch_Check(const char* phase, bool tick)
{
    if (!g_on || !Determinism_IsActive()) return;
    LockScope lock(g_watchLock);
    Guard_Call("the construction site watch", [&] { Compare(phase, tick); });
}

// ---------------------------------------------------------------------------
// Inside the tick
// ---------------------------------------------------------------------------

// The exe's return addresses on the stack, as exe+rva: the direct caller
// first.
static void StackText(void* caller, char* out, size_t size)
{
    void* frames[STACK_FRAMES];
    USHORT n = RtlCaptureStackBackTrace(1, STACK_FRAMES, frames, nullptr);
    size_t len = 0;
    out[0] = 0;
    auto add = [&](void* at) {
        BYTE* p = (BYTE*)at;
        if (p < g_exeBase || p >= g_exeBase + g_exeSize || len + 16 >= size) return;
        int w = _snprintf_s(out + len, size - len, _TRUNCATE, " exe+%llX", (unsigned long long)(p - g_exeBase));
        if (w > 0) len += w;
    };
    add(caller);
    for (USHORT i = 0; i < n; i++)
        if (frames[i] != caller) add(frames[i]);
}

// The current stage's materials, each have/need.
static void MaterialsText(BYTE* b, char* out, size_t size)
{
    size_t len = 0;
    out[0] = 0;
    __try {
        BYTE* records = *(BYTE**)(b + STAGES_OFFSET);
        BYTE* end = *(BYTE**)(b + STAGES_OFFSET + 8);
        int stage = *(int*)(b + STAGE_OFFSET);
        int stages = records && end >= records ? (int)((end - records) / STAGE_SIZE) : 0;
        if (!records || stage < 0 || stage >= stages || stages > 64) return;
        BYTE* at = records + (size_t)stage * STAGE_SIZE;
        BYTE* it = *(BYTE**)(at + 8);
        BYTE* entriesEnd = *(BYTE**)(at + 0x10);
        for (int n = 0; it && it < entriesEnd && n < DETAIL_ENTRIES; it += ENTRY_BYTES, n++) {
            int w = _snprintf_s(out + len, size - len, _TRUNCATE, " %p:%.6g/%.6g", *(void**)it, *(float*)(it + 8),
                                *(float*)(it + 0xC));
            if (w <= 0) break;
            len += w;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool LineAllowed()
{
    if (++g_lines <= LOG_LIMIT) return true;
    if (g_lines == LOG_LIMIT + 1) Logf("republicmp  SITE_WATCH: %d lines - no more until the next load", LOG_LIMIT);
    return false;
}

static bool Watching()
{
    return g_on && Determinism_IsActive() && !Determinism_WorldLoading();
}

// One update at a time, on the main thread.
static BYTE* g_updated = nullptr;
static SiteState g_before;
static char g_beforeMaterials[DETAIL_ENTRIES * 48];

void SiteWatch_UpdateBefore(BYTE* building)
{
    g_updated = nullptr;
    if (!Watching() || !ReadSite(building, g_before)) return;
    MaterialsText(building, g_beforeMaterials, sizeof(g_beforeMaterials));
    g_updated = building;
}

void SiteWatch_UpdateAfter(BYTE* building)
{
    if (!g_updated || building != g_updated) return;
    g_updated = nullptr;
    SiteState now;
    if (!ReadSite(building, now, true)) return;
    float was = AsFloat(g_before.progress), is = AsFloat(now.progress);
    const char* what = !(is < 1.0f) && was < EARLY_END ? "finished at once in its construction update"
                       : is - was > JUMP                ? "jumped in its construction update"
                                                        : nullptr;
    if (!what) return;
    LockScope lock(g_watchLock);
    Line("construction update", what, building, &g_before, now);
    if (g_lines > LOG_LIMIT) return;
    char after[DETAIL_ENTRIES * 48], stack[STACK_FRAMES * 20];
    MaterialsText(building, after, sizeof(after));
    StackText(_ReturnAddress(), stack, sizeof(stack));
    Logf("republicmp  SITE_WATCH:   materials before:%s, after:%s; stack:%s", g_beforeMaterials[0] ? g_beforeMaterials : " none",
         after[0] ? after : " none", stack);
}

typedef BYTE* (*SiteMakeFn)(BYTE* memory);
typedef unsigned long long (*SiteFreeFn)(BYTE* building);
static SiteMakeFn g_origMake = nullptr;
static SiteFreeFn g_origFree = nullptr;

static BYTE* HookedMake(BYTE* memory)
{
    BYTE* b = g_origMake(memory);
    if (Watching()) {
        LockScope lock(g_watchLock);
        if (LineAllowed()) {
            char stack[STACK_FRAMES * 20];
            StackText(_ReturnAddress(), stack, sizeof(stack));
            Logf("republicmp  SITE_WATCH: tick %llu (%s frame, thread %lu): building object %p made; stack:%s",
                 Determinism_StepCount(), Determinism_IsStepFrame() ? "tick" : "held", GetCurrentThreadId(), b, stack);
        }
    }
    return b;
}

static unsigned long long HookedFree(BYTE* building)
{
    SiteState s;
    if (Watching() && building && ReadSite(building, s)) {
        LockScope lock(g_watchLock);
        if (LineAllowed()) {
            char stack[STACK_FRAMES * 20];
            StackText(_ReturnAddress(), stack, sizeof(stack));
            Logf("republicmp  SITE_WATCH: tick %llu (%s frame, thread %lu): site %p cat %d at (%.1f,%.1f) destroyed at progress "
                 "%.9g stage %d of %d materials %.6g of %.6g; stack:%s",
                 Determinism_StepCount(), Determinism_IsStepFrame() ? "tick" : "held", GetCurrentThreadId(), building,
                 s.category, s.x, s.z, AsFloat(s.progress), s.stage, s.stages, s.have, s.need, stack);
        }
    }
    return g_origFree(building);
}

// The first instructions (none RIP-relative) the loader's jump replaces.
static const BYTE kMakeExpect[19] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40,
                                      0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF };
static const BYTE kFreeExpect[18] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24,
                                      0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };

bool SiteWatch_Install()
{
    bool make = InstallInlineHook(g_exeBase + SITE_MAKE_RVA, (void*)HookedMake, (void**)&g_origMake, kMakeExpect,
                                  sizeof(kMakeExpect), "republicmp site watch: building made");
    bool destroy = InstallInlineHook(g_exeBase + SITE_FREE_RVA, (void*)HookedFree, (void**)&g_origFree, kFreeExpect,
                                     sizeof(kFreeExpect), "republicmp site watch: building destroyed");
    return make && destroy;
}
