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

// One building as a site; false: not a site (or unreadable).
static bool ReadSite(BYTE* b, SiteState& s)
{
    __try {
        float progress = *(float*)(b + PROGRESS_OFFSET);
        if (!(progress < 1.0f) && !b[SITE_FLAG_OFFSET]) return false;
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
