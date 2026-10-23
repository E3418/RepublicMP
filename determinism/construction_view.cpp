// SPDX-License-Identifier: GPL-3.0-only
// See construction_view.h.
#include "construction_view.h"
#include "../core/profile.h"
#include <vector>
#include <string.h>
#include <stdio.h>

#define LIST_OFFSET 0x11DD8           // world: the drawer's building list (begin, end)
#define TYPE_OFFSET 0x318             // building: its type (null for network pieces)
#define CATEGORY_OFFSET 0x360         // type: category
#define POS_OFFSET 0x350              // building: position
#define PROGRESS_OFFSET 0x604         // float: construction progress, 1 = finished
#define SITE_FLAG_OFFSET 0x761        // byte: drawn as a site even when finished (2df64e)
#define DRAW_FLAG_OFFSET 0x3DC        // byte: the drawer's first test (2dca5b)
#define INVERTED_OFFSET 0xEA8         // byte: inverted progress (demolition)
#define VISIBLE_OFFSET 0x115A         // byte: on the camera
#define DISTANCE_OFFSET 0x115C        // float: camera distance
#define STAGES_OFFSET 0x610           // vector of stage records (begin, end)
#define STAGE_SIZE 0x21B8
#define STAGE_KEEP_FROM 0x20B0        // the slot data 2dc470 and 167540 write (+20b4..+20cc)
#define STAGE_KEEP_BYTES (STAGE_SIZE - STAGE_KEEP_FROM)
#define MAX_STAGES 16
#define STAGE_INDEX_OFFSET 0x634      // int: cached stage index (159070)
#define STAGE_FRACTION_OFFSET 0x638   // float: its fraction
#define PIECE_INDEX_OFFSET 0x63C      // int: piece index
#define SCAFFOLD_OFFSET 0x640         // three vectors: +640, +658, +670 (167540)
#define PILES_OFFSET 0x718            // vector: the dirt piles' places
#define PARTS_OFFSET 0x6B8            // two vectors the drawer clears and refills: +6b8, +6d0
#define SHIPYARD_TIMER_OFFSET 0xFC0   // float: the shipyard branch adds the frame time (2dd2c5)
#define LOG_MS 2000
#define LOG_SITES 6

static bool g_draw = true;
static bool g_log = false;

void ConstructionView_Bind(const TsmHost* host)
{
    TsmBind(host);
    g_draw = Profile_GetInt("debug", "construction_draw", 1) != 0;
    g_log = Profile_GetInt("debug", "construction_log", 0) != 0;
}

bool ConstructionView_GetDraw() { return g_draw; }
bool ConstructionView_GetLog() { return g_log; }

void ConstructionView_SetDraw(bool on)
{
    g_draw = on;
    Profile_SetInt("debug", "construction_draw", on ? 1 : 0);
    Logf("republicmp  CONSTRUCTION: sites in a session are drawn %s", on ? "with this PC's view" : "as before (no stages)");
}

void ConstructionView_SetLog(bool on)
{
    g_log = on;
    Profile_SetInt("debug", "construction_log", on ? 1 : 0);
    Logf("republicmp  CONSTRUCTION: log %s", on ? "on (every 2 s, the drawer's sites)" : "off");
}

// ---------------------------------------------------------------------------
// Raw reads (SEH, no C++ objects)
// ---------------------------------------------------------------------------

static bool ReadList(BYTE* world, BYTE*** begin, BYTE*** end)
{
    __try {
        *begin = *(BYTE***)(world + LIST_OFFSET);
        *end = *(BYTE***)(world + LIST_OFFSET + 8);
        return *begin && *end >= *begin && *end - *begin < 200000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A building the drawer treats as a site: typed, and unfinished or flagged.
static bool IsSite(BYTE* b)
{
    __try {
        return b && *(void**)(b + TYPE_OFFSET) &&
               (*(float*)(b + PROGRESS_OFFSET) != 1.0f || b[SITE_FLAG_OFFSET] != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static int StageCount(BYTE* b, BYTE** records)
{
    __try {
        BYTE* begin = *(BYTE**)(b + STAGES_OFFSET);
        BYTE* end = *(BYTE**)(b + STAGES_OFFSET + 8);
        *records = begin;
        if (!begin || end < begin || (end - begin) % STAGE_SIZE) return 0;
        long long n = (end - begin) / STAGE_SIZE;
        return n > MAX_STAGES ? MAX_STAGES : (int)n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static long long VectorBytes(BYTE* b, unsigned offset)
{
    __try {
        BYTE* begin = *(BYTE**)(b + offset);
        BYTE* end = *(BYTE**)(b + offset + 8);
        return begin && end >= begin ? end - begin : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// What the drawer may write into a building: the shipyard timer and the
// stage records' slot data.
static unsigned long long KeptHash(BYTE* b)
{
    unsigned long long h = 0xcbf29ce484222325ull;
    __try {
        const BYTE* p = b + SHIPYARD_TIMER_OFFSET;
        for (int i = 0; i < 4; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
        BYTE* records = nullptr;
        int n = StageCount(b, &records);
        for (int s = 0; s < n; s++) {
            const BYTE* t = records + (size_t)s * STAGE_SIZE + STAGE_KEEP_FROM;
            for (int i = 0; i < STAGE_KEEP_BYTES; i++) { h ^= t[i]; h *= 0x100000001b3ull; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Keeping the fields the drawer writes
// ---------------------------------------------------------------------------

struct Kept {
    BYTE* building;
    float timer;
    BYTE* records;
    int stages;
    BYTE tail[MAX_STAGES][STAGE_KEEP_BYTES];
};
static std::vector<Kept> g_kept; // render thread only
static size_t g_keptCount = 0;

static bool KeepOne(BYTE* b, Kept& k)
{
    __try {
        k.building = b;
        k.timer = *(float*)(b + SHIPYARD_TIMER_OFFSET);
        k.stages = StageCount(b, &k.records);
        for (int s = 0; s < k.stages; s++)
            memcpy(k.tail[s], k.records + (size_t)s * STAGE_SIZE + STAGE_KEEP_FROM, STAGE_KEEP_BYTES);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void PutBackOne(const Kept& k)
{
    __try {
        *(float*)(k.building + SHIPYARD_TIMER_OFFSET) = k.timer;
        // The drawer does not add or remove stage records; a list that changed is left alone.
        BYTE* records = nullptr;
        if (StageCount(k.building, &records) != k.stages || records != k.records) return;
        for (int s = 0; s < k.stages; s++)
            memcpy(k.records + (size_t)s * STAGE_SIZE + STAGE_KEEP_FROM, k.tail[s], STAGE_KEEP_BYTES);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---------------------------------------------------------------------------
// The log
// ---------------------------------------------------------------------------

struct SiteLog {
    BYTE* building;
    int category;
    float pos[3];
    float progress;
    int siteFlag, drawFlag, inverted;
    int simVisible, viewVisible;
    float simDistance, viewDistance;
    int stages, stageIndex, pieceIndex;
    float stageFraction;
    long long scaffold[3], piles, parts[2];
    unsigned long long hashBefore, hashAfter;
};
static SiteLog g_sites[LOG_SITES];
static int g_siteCount = 0;
static int g_listed = 0, g_listedSites = 0;
static bool g_logging = false; // this call is logged
static ULONGLONG g_lastLogMs = 0;
static int g_callsSinceLog[2] = {};

static bool ReadSite(BYTE* b, SiteLog& s)
{
    __try {
        memset(&s, 0, sizeof(s));
        s.building = b;
        BYTE* type = *(BYTE**)(b + TYPE_OFFSET);
        s.category = type ? *(int*)(type + CATEGORY_OFFSET) : -1;
        memcpy(s.pos, b + POS_OFFSET, 12);
        s.progress = *(float*)(b + PROGRESS_OFFSET);
        s.siteFlag = b[SITE_FLAG_OFFSET];
        s.drawFlag = b[DRAW_FLAG_OFFSET];
        s.inverted = b[INVERTED_OFFSET];
        s.simVisible = s.viewVisible = b[VISIBLE_OFFSET];
        s.simDistance = s.viewDistance = *(float*)(b + DISTANCE_OFFSET);
        BYTE* records = nullptr;
        s.stages = StageCount(b, &records);
        s.stageIndex = *(int*)(b + STAGE_INDEX_OFFSET);
        s.stageFraction = *(float*)(b + STAGE_FRACTION_OFFSET);
        s.pieceIndex = *(int*)(b + PIECE_INDEX_OFFSET);
        for (int i = 0; i < 3; i++) s.scaffold[i] = VectorBytes(b, SCAFFOLD_OFFSET + 0x18 * i);
        s.piles = VectorBytes(b, PILES_OFFSET);
        for (int i = 0; i < 2; i++) s.parts[i] = VectorBytes(b, PARTS_OFFSET + 0x18 * i);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ConstructionView_Sample(BYTE* world, int pass)
{
    if (pass == 0 || pass == 1) g_callsSinceLog[pass]++;
    g_logging = false;
    if (!g_log || pass != 0) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_lastLogMs < LOG_MS) return;
    g_lastLogMs = now;
    g_logging = true;
    g_siteCount = g_listed = g_listedSites = 0;
    BYTE** begin;
    BYTE** end;
    if (!ReadList(world, &begin, &end)) return;
    g_listed = (int)(end - begin);
    for (BYTE** it = begin; it < end; ++it) {
        if (!IsSite(*it)) continue;
        g_listedSites++;
        if (g_siteCount < LOG_SITES && ReadSite(*it, g_sites[g_siteCount])) g_siteCount++;
    }
}

void ConstructionView_View()
{
    if (!g_logging) return;
    for (int i = 0; i < g_siteCount; i++) {
        __try {
            g_sites[i].viewVisible = g_sites[i].building[VISIBLE_OFFSET];
            g_sites[i].viewDistance = *(float*)(g_sites[i].building + DISTANCE_OFFSET);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

void ConstructionView_Keep(BYTE* world, bool keep)
{
    if (g_logging)
        for (int i = 0; i < g_siteCount; i++) g_sites[i].hashBefore = KeptHash(g_sites[i].building);
    g_keptCount = 0;
    if (!keep) return;
    BYTE** begin;
    BYTE** end;
    if (!ReadList(world, &begin, &end)) return;
    for (BYTE** it = begin; it < end; ++it) {
        if (!IsSite(*it)) continue;
        if (g_keptCount == g_kept.size()) g_kept.resize(g_kept.size() + 16);
        if (KeepOne(*it, g_kept[g_keptCount])) g_keptCount++;
    }
}

void ConstructionView_After(bool keep)
{
    if (g_logging)
        for (int i = 0; i < g_siteCount; i++) g_sites[i].hashAfter = KeptHash(g_sites[i].building);
    if (keep)
        for (size_t i = 0; i < g_keptCount; i++) PutBackOne(g_kept[i]);
    if (!g_logging) return;
    g_logging = false;
    Logf("republicmp  CONSTRUCTION: drawer list %d building(s), %d site(s) on it; calls %d (colour) %d (pass 1) in 2 s; %s",
         g_listed, g_listedSites, g_callsSinceLog[0], g_callsSinceLog[1],
         keep ? "drawn with this PC's view, kept fields put back" : "the simulation's view");
    g_callsSinceLog[0] = g_callsSinceLog[1] = 0;
    for (int i = 0; i < g_siteCount; i++) {
        const SiteLog& s = g_sites[i];
        Logf("republicmp  CONSTRUCTION:   cat %d at (%.0f,%.0f) progress %.3f site %d draw %d inv %d | sim vis %d dist %.0f, "
             "view vis %d dist %.0f | stages %d idx %d frac %.2f piece %d | scaffold %lld/%lld/%lld piles %lld parts %lld/%lld | "
             "drawer %s the kept fields",
             s.category, s.pos[0], s.pos[2], s.progress, s.siteFlag, s.drawFlag, s.inverted, s.simVisible, s.simDistance,
             s.viewVisible, s.viewDistance, s.stages, s.stageIndex, s.stageFraction, s.pieceIndex, s.scaffold[0],
             s.scaffold[1], s.scaffold[2], s.piles, s.parts[0], s.parts[1],
             s.hashBefore == s.hashAfter ? "did not change" : "CHANGED");
    }
}
