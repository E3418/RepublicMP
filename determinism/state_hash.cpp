// SPDX-License-Identifier: GPL-3.0-only
// See state_hash.h.
#include "state_hash.h"
#include "../core/profile.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <map>
#include <tuple>

#define WORLD_RVA 0x9D4F10

#define WORLD_DAY_OFFSET 0x590
#define WORLD_YEAR_OFFSET 0x594
#define WORLD_PROGRESS_OFFSET 0x59c
#define WORLD_USD_OFFSET 0x580
#define WORLD_RUB_OFFSET 0x588
#define WORLD_BUILDINGS_OFFSET 0x11B08
#define WORLD_VEHICLES_OFFSET 0x12810
#define WORLD_CITIZENS_OFFSET 0x126A8

#define BLD_INDEX_OFFSET 0x00
#define BLD_PROGRESS_OFFSET 0x604
#define BLD_STORAGE_OFFSET 0x970
#define BLD_STORAGE_SLOT_SIZE 0xE0
#define BLD_WORKERS_OFFSET 0xBC0
#define BLD_PRODUCED_OFFSET 0xDE4

#define VEH_INDEX_OFFSET 0x04
#define VEH_CARGO_OFFSET 0x3B8
#define VEH_POS_OFFSET 0xDE0

#define CIT_HOME_OFFSET 0x20
#define CIT_AGE_OFFSET 0xD4
#define CIT_POS_OFFSET 0x620
#define CIT_PLACED_AGE 1.0f // a citizen in their first year may not be placed yet

#define ENTRY_SIZE 0x10        // {Resource*, float amount, float}
#define ENTRY_AMOUNT_OFFSET 0x08
#define MAX_OBJECTS 2000000    // sanity bound on any container
#define MAX_ENTRIES 4096
#define MAX_NAME 64

static void LoadSettings();

void StateHash_Bind(const TsmHost* host)
{
    TsmBind(host);
    LoadSettings();
}

// FNV-1a, 64 bit.
static inline void Mix(unsigned long long& h, const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
}
static inline void MixInt(unsigned long long& h, int v) { Mix(h, &v, sizeof(v)); }
static inline void MixFloat(unsigned long long& h, float v) { Mix(h, &v, sizeof(v)); }

static inline void MixName(unsigned long long& h, const char* s)
{
    for (int i = 0; i < MAX_NAME && s[i]; i++) Mix(h, &s[i], 1);
    Mix(h, "", 1);
}

static inline long long Count(BYTE* vec, long long elemSize)
{
    BYTE* b = *(BYTE**)vec;
    BYTE* e = *(BYTE**)(vec + 8);
    if (!b || e < b) return 0;
    long long n = (e - b) / elemSize;
    return n > MAX_OBJECTS ? 0 : n;
}

// Resource entries {Resource*, amount, x} of one vector: names and amounts.
static void MixEntries(unsigned long long& h, BYTE* vec, double& total)
{
    long long n = Count(vec, ENTRY_SIZE);
    if (n > MAX_ENTRIES) n = MAX_ENTRIES;
    BYTE* e = *(BYTE**)vec;
    MixInt(h, (int)n);
    for (long long i = 0; i < n; i++, e += ENTRY_SIZE) {
        const char* res = *(const char**)e;
        if (res) MixName(h, res);
        float amount = *(float*)(e + ENTRY_AMOUNT_OFFSET);
        MixFloat(h, amount);
        total += amount;
    }
}

bool StateHash_Compute(StateSummary* out)
{
    memset(out, 0, sizeof(*out));
    const unsigned long long seed = 0xcbf29ce484222325ull;
    out->hashClock = out->hashBuildings = out->hashVehicles = out->hashCitizens = seed;
    BYTE* w = g_exeBase + WORLD_RVA;
    __try {
        out->day = *(int*)(w + WORLD_DAY_OFFSET);
        out->year = *(int*)(w + WORLD_YEAR_OFFSET);
        out->dayProgress = *(float*)(w + WORLD_PROGRESS_OFFSET);
        out->usd = *(double*)(w + WORLD_USD_OFFSET);
        out->rub = *(double*)(w + WORLD_RUB_OFFSET);
        MixInt(out->hashClock, out->day);
        MixInt(out->hashClock, out->year);
        MixFloat(out->hashClock, out->dayProgress);
        Mix(out->hashClock, &out->usd, sizeof(double));
        Mix(out->hashClock, &out->rub, sizeof(double));

        BYTE* bvec = w + WORLD_BUILDINGS_OFFSET;
        long long nb = Count(bvec, 8);
        if (nb == 0 && *(void**)bvec == nullptr) return false; // no world
        out->buildings = (int)nb;
        BYTE** bl = *(BYTE***)bvec;
        for (long long i = 0; i < nb; i++) {
            BYTE* b = bl[i];
            if (!b) { MixInt(out->hashBuildings, -1); continue; }
            MixInt(out->hashBuildings, *(int*)(b + BLD_INDEX_OFFSET));
            MixFloat(out->hashBuildings, *(float*)(b + BLD_PROGRESS_OFFSET));
            float produced = *(float*)(b + BLD_PRODUCED_OFFSET);
            MixFloat(out->hashBuildings, produced);
            out->productionTotal += produced;
            MixInt(out->hashBuildings, (int)Count(b + BLD_WORKERS_OFFSET, 8));
            BYTE* slots = *(BYTE**)(b + BLD_STORAGE_OFFSET);
            long long ns = Count(b + BLD_STORAGE_OFFSET, BLD_STORAGE_SLOT_SIZE);
            if (ns > 64) ns = 64;
            for (long long s = 0; s < ns; s++)
                MixEntries(out->hashBuildings, slots + s * BLD_STORAGE_SLOT_SIZE, out->storageTotal);
        }

        long long nv = Count(w + WORLD_VEHICLES_OFFSET, 8);
        out->vehicles = (int)nv;
        BYTE** vl = *(BYTE***)(w + WORLD_VEHICLES_OFFSET);
        for (long long i = 0; i < nv; i++) {
            BYTE* v = vl[i];
            if (!v) { MixInt(out->hashVehicles, -1); continue; }
            MixInt(out->hashVehicles, *(int*)(v + VEH_INDEX_OFFSET));
            Mix(out->hashVehicles, v + VEH_POS_OFFSET, 12);
            MixEntries(out->hashVehicles, v + VEH_CARGO_OFFSET, out->cargoTotal);
        }

        long long nc = Count(w + WORLD_CITIZENS_OFFSET, 8);
        out->citizens = (int)nc;
        BYTE** cl = *(BYTE***)(w + WORLD_CITIZENS_OFFSET);
        for (long long i = 0; i < nc; i++) {
            BYTE* c = cl[i];
            if (!c) { MixInt(out->hashCitizens, -1); continue; }
            BYTE* home = *(BYTE**)(c + CIT_HOME_OFFSET);
            MixInt(out->hashCitizens, home ? *(int*)(home + BLD_INDEX_OFFSET) : -1);
            float age = *(float*)(c + CIT_AGE_OFFSET);
            MixFloat(out->hashCitizens, age);
            out->ageTotal += age;
            // A citizen without a home, or in their first year, may not be
            // placed yet: its position bytes are whatever the heap held (the
            // object is not zeroed when made).
            if (home && age >= CIT_PLACED_AGE) Mix(out->hashCitizens, c + CIT_POS_OFFSET, 12);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-object records (probe)
// ---------------------------------------------------------------------------

#define BLD_TYPE_OFFSET 0x318
#define BLD_POS_OFFSET 0x3A0
#define TYPE_CATEGORY_OFFSET 0x360

static long long ListCount(int listOffset)
{
    __try {
        return Count(g_exeBase + WORLD_RVA + listOffset, 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static bool FillBuildings(BuildingRecord* out, long long n)
{
    __try {
        BYTE** bl = *(BYTE***)(g_exeBase + WORLD_RVA + WORLD_BUILDINGS_OFFSET);
        for (long long i = 0; i < n; i++) {
            BuildingRecord& r = out[i];
            memset(&r, 0, sizeof(r));
            BYTE* b = bl[i];
            if (!b) { r.index = -1; r.category = -2; continue; }
            r.index = *(int*)(b + BLD_INDEX_OFFSET);
            BYTE* type = *(BYTE**)(b + BLD_TYPE_OFFSET);
            r.category = type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
            memcpy(r.pos, b + BLD_POS_OFFSET, 12);
            r.progress = *(float*)(b + BLD_PROGRESS_OFFSET);
            r.produced = *(float*)(b + BLD_PRODUCED_OFFSET);
            r.workers = (int)Count(b + BLD_WORKERS_OFFSET, 8);
            unsigned long long h = 0xcbf29ce484222325ull;
            double unused = 0;
            BYTE* slots = *(BYTE**)(b + BLD_STORAGE_OFFSET);
            long long ns = Count(b + BLD_STORAGE_OFFSET, BLD_STORAGE_SLOT_SIZE);
            if (ns > 64) ns = 64;
            for (long long s = 0; s < ns; s++) {
                double amount = 0;
                MixEntries(h, slots + s * BLD_STORAGE_SLOT_SIZE, amount);
                if (s < RECORD_SLOTS) {
                    r.slotEntries[s] = (int)Count(slots + s * BLD_STORAGE_SLOT_SIZE, ENTRY_SIZE);
                    r.slotAmount[s] = (float)amount;
                }
            }
            r.slots = (int)ns;
            r.storage = h;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool FillCitizens(CitizenRecord* out, long long n)
{
    __try {
        BYTE** cl = *(BYTE***)(g_exeBase + WORLD_RVA + WORLD_CITIZENS_OFFSET);
        for (long long i = 0; i < n; i++) {
            CitizenRecord& r = out[i];
            memset(&r, 0, sizeof(r));
            BYTE* c = cl[i];
            if (!c) { r.home = -2; continue; }
            BYTE* home = *(BYTE**)(c + CIT_HOME_OFFSET);
            r.home = home ? *(int*)(home + BLD_INDEX_OFFSET) : -1;
            r.age = *(float*)(c + CIT_AGE_OFFSET);
            if (home && r.age >= CIT_PLACED_AGE) memcpy(r.pos, c + CIT_POS_OFFSET, 12); // not placed yet: left 0
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool FillVehicles(VehicleRecord* out, long long n)
{
    __try {
        BYTE** vl = *(BYTE***)(g_exeBase + WORLD_RVA + WORLD_VEHICLES_OFFSET);
        for (long long i = 0; i < n; i++) {
            VehicleRecord& r = out[i];
            memset(&r, 0, sizeof(r));
            BYTE* v = vl[i];
            if (!v) { r.index = -1; continue; }
            r.index = *(int*)(v + VEH_INDEX_OFFSET);
            memcpy(r.pos, v + VEH_POS_OFFSET, 12);
            unsigned long long h = 0xcbf29ce484222325ull;
            double unused = 0;
            MixEntries(h, v + VEH_CARGO_OFFSET, unused);
            r.cargo = h;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool StateHash_VehicleRecords(std::vector<VehicleRecord>& out)
{
    long long n = ListCount(WORLD_VEHICLES_OFFSET);
    if (n < 0) return false;
    out.resize((size_t)n);
    return n == 0 || FillVehicles(out.data(), n);
}

bool StateHash_BuildingRecords(std::vector<BuildingRecord>& out)
{
    long long n = ListCount(WORLD_BUILDINGS_OFFSET);
    if (n < 0) return false;
    out.resize((size_t)n);
    return n == 0 || FillBuildings(out.data(), n);
}

bool StateHash_CitizenRecords(std::vector<CitizenRecord>& out)
{
    long long n = ListCount(WORLD_CITIZENS_OFFSET);
    if (n < 0) return false;
    out.resize((size_t)n);
    return n == 0 || FillCitizens(out.data(), n);
}

unsigned long long StateHash_All(const StateSummary& s)
{
    unsigned long long h = 0xcbf29ce484222325ull;
    Mix(h, &s.hashClock, 8);
    Mix(h, &s.hashBuildings, 8);
    Mix(h, &s.hashVehicles, 8);
    Mix(h, &s.hashCitizens, 8);
    return h;
}

// Stock changes outside the common tick are suspicious even when the resource
// window only appears to display data. Capture exact entries, not just slot
// sums, so a transfer between entries cannot cancel out in the diagnostic.
static volatile LONG g_storageTrace = 0;
static volatile LONG g_storageUiLogs = 0;
static volatile LONG g_recipeLogs = 0;
#define MAX_STORAGE_RECORDS 131072

// Read once at bind, like the money and construction diagnostics. A missing
// key keeps the expensive resource trace off; only an explicit toggle saves it.
static void LoadSettings()
{
    InterlockedExchange(&g_storageTrace, Profile_GetInt("debug", "storage_trace", 0) != 0 ? 1 : 0);
}

bool StateHash_GetStorageTrace()
{
    return InterlockedCompareExchange(&g_storageTrace, 0, 0) != 0;
}

void StateHash_SetStorageTrace(bool on)
{
    InterlockedExchange(&g_storageUiLogs, 0);
    InterlockedExchange(&g_recipeLogs, 0);
    InterlockedExchange(&g_storageTrace, on ? 1 : 0);
    Profile_SetInt("debug", "storage_trace", on ? 1 : 0);
}

// No C++ allocation inside the protected reader. The second pass refuses a
// changed list shape rather than leaving a partially filled snapshot usable.
// Refuse malformed diagnostic vectors without raising an access violation:
// the loader has a limited crash-report budget, which diagnostics must not use.
static bool RecordVector(BYTE* at, size_t stride, long long max, BYTE** begin, long long* count)
{
    uintptr_t v[2] = {};
    SIZE_T done = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), at, v, sizeof(v), &done) || done != sizeof(v)) return false;
    if (!v[0] && !v[1]) { *begin = nullptr; *count = 0; return true; }
    if (!v[0] || v[1] < v[0] || (v[1] - v[0]) % stride || (v[1] - v[0]) / stride > (unsigned long long)max) return false;
    if (v[1] > v[0] && !ReadablePtr((void*)v[0], v[1] - v[0])) return false;
    *begin = (BYTE*)v[0];
    *count = (long long)((v[1] - v[0]) / stride);
    return true;
}

static long long ResourceSize(bool recipe)
{
    __try {
        BYTE* raw = nullptr;
        long long nb = 0, total = 0;
        if (!RecordVector(g_exeBase + WORLD_RVA + WORLD_BUILDINGS_OFFSET, 8, MAX_OBJECTS, &raw, &nb)) return -1;
        BYTE** bl = (BYTE**)raw;
        for (long long i = 0; i < nb; i++) {
            BYTE* b = bl[i];
            if (!b) continue;
            const int offset = recipe ? 0x610 : BLD_STORAGE_OFFSET;
            const int stride = recipe ? 0x21B8 : BLD_STORAGE_SLOT_SIZE;
            BYTE* slots = nullptr;
            long long ns = 0;
            if (!RecordVector(b + offset, stride, recipe ? 16 : RECORD_SLOTS, &slots, &ns)) return -1;
            for (long long s = 0; s < ns; s++) {
                BYTE* entries = nullptr;
                long long n = 0;
                if (!RecordVector(slots + s * stride + (recipe ? 8 : 0), ENTRY_SIZE, MAX_ENTRIES, &entries, &n) ||
                    total + n > MAX_STORAGE_RECORDS) return -1;
                total += n;
            }
        }
        return total;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static bool FillResources(StorageRecord* out, size_t size, bool recipe)
{
    __try {
        BYTE* raw = nullptr;
        long long nb = 0;
        if (!RecordVector(g_exeBase + WORLD_RVA + WORLD_BUILDINGS_OFFSET, 8, MAX_OBJECTS, &raw, &nb)) return false;
        BYTE** bl = (BYTE**)raw;
        size_t used = 0;
        for (long long i = 0; i < nb; i++) {
            BYTE* b = bl[i];
            if (!b) continue;
            const int offset = recipe ? 0x610 : BLD_STORAGE_OFFSET;
            const int stride = recipe ? 0x21B8 : BLD_STORAGE_SLOT_SIZE;
            BYTE* slots = nullptr;
            long long ns = 0;
            if (!RecordVector(b + offset, stride, recipe ? 16 : RECORD_SLOTS, &slots, &ns)) return false;
            for (long long s = 0; s < ns; s++) {
                BYTE* slot = slots + s * stride + (recipe ? 8 : 0);
                long long n = 0;
                BYTE* entries = nullptr;
                if (!RecordVector(slot, ENTRY_SIZE, MAX_ENTRIES, &entries, &n)) return false;
                for (long long e = 0; e < n; e++) {
                    if (used >= size) return false;
                    StorageRecord& v = out[used++];
                    memset(&v, 0, sizeof(v));
                    v.object = (unsigned long long)(uintptr_t)b;
                    v.building = *(int*)(b + BLD_INDEX_OFFSET);
                    v.ordinal = (int)i;
                    v.slot = (int)s;
                    v.entry = (int)e;
                    const char* name = *(const char**)(entries + e * ENTRY_SIZE);
                    if (name) {
                        if (!ReadablePtr(name, MAX_NAME)) return false;
                        for (int k = 0; k < MAX_NAME && name[k]; k++) {
                            unsigned char c = (unsigned char)name[k];
                            v.resource[k] = c >= 32 && c < 127 ? (char)c : '?';
                        }
                    }
                    memcpy(&v.amount, entries + e * ENTRY_SIZE + ENTRY_AMOUNT_OFFSET, 4);
                    if (recipe) memcpy(&v.required, entries + e * ENTRY_SIZE + 12, 4);
                }
            }
        }
        return used == size;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool StateHash_StorageRecords(std::vector<StorageRecord>& out)
{
    long long n = ResourceSize(false);
    if (n < 0) { out.clear(); return false; }
    out.resize((size_t)n);
    if (FillResources(out.data(), out.size(), false)) return true;
    out.clear();
    return false;
}

void StateHash_LogStorageUi(const std::vector<StorageRecord>& before, unsigned long long tick)
{
    if (!StateHash_GetStorageTrace() || InterlockedCompareExchange(&g_storageUiLogs, 0, 0) >= 32) return;
    std::vector<StorageRecord> after;
    if (!StateHash_StorageRecords(after)) return;
    if (before.size() != after.size()) {
        if (InterlockedIncrement(&g_storageUiLogs) <= 32)
            Logf("republicmp  STORAGE_UI: after tick %llu entry count changed %zu -> %zu", tick, before.size(), after.size());
        return;
    }
    for (size_t i = 0; i < before.size(); i++) {
        const StorageRecord& a = before[i];
        const StorageRecord& b = after[i];
        if (a.object != b.object || a.slot != b.slot || a.entry != b.entry || strcmp(a.resource, b.resource)) {
            if (InterlockedIncrement(&g_storageUiLogs) <= 32)
                Logf("republicmp  STORAGE_UI: after tick %llu entry identity/order changed at record %zu", tick, i);
            return;
        }
        unsigned oldBits = 0, newBits = 0;
        memcpy(&oldBits, &a.amount, 4);
        memcpy(&newBits, &b.amount, 4);
        if (oldBits == newBits) continue;
        LONG line = InterlockedIncrement(&g_storageUiLogs);
        if (line > 32) return;
        Logf("republicmp  STORAGE_UI: after tick %llu building %d slot %d entry %d [%s] %.9g (%08X) -> %.9g (%08X)%s",
             tick, a.building, a.slot, a.entry, a.resource, a.amount, oldBits, b.amount, newBits,
             line == 32 ? " - UI report limit reached; toggle storage trace to reset" : "");
    }
}

// Network construction objects keep a recipe in +610 even without a building
// type. 159070 later charges from each material entry's +c; equal progress
// hashes do not prove these requirements match. Capture command-side changes
// before paid construction consumes them (by SoylentGreed, stage layout).
bool StateHash_RecipeRecords(std::vector<StorageRecord>& out)
{
    long long n = ResourceSize(true);
    if (n < 0) { out.clear(); return false; }
    out.resize((size_t)n);
    if (FillResources(out.data(), out.size(), true)) return true;
    out.clear();
    return false;
}

// Saving can leave multiple native building indices at zero until the next
// simulation pass. Compare before/after snapshots by this process's object
// identity instead; log the list ordinal so both PCs can name those objects.
// No saved pointer is dereferenced after the snapshot or used across PCs.
using RecipeIdentity = std::tuple<unsigned long long, int, int>;
static RecipeIdentity RecipeKey(const StorageRecord& r)
{
    return std::make_tuple(r.object, r.slot, r.entry);
}

void StateHash_LogRecipeChanges(const std::vector<StorageRecord>& before, unsigned seq, unsigned long long tick)
{
    if (!StateHash_GetStorageTrace() || InterlockedCompareExchange(&g_recipeLogs, 0, 0) >= 512) return;
    std::vector<StorageRecord> after;
    if (!StateHash_RecipeRecords(after)) return;
    std::map<RecipeIdentity, const StorageRecord*> old;
    for (const StorageRecord& r : before) old.emplace(RecipeKey(r), &r);
    for (const StorageRecord& r : after) {
        const StorageRecord* was = nullptr;
        auto found = old.find(RecipeKey(r));
        if (found != old.end() && !strcmp(found->second->resource, r.resource)) was = found->second;
        unsigned bits = 0, oldBits = 0;
        memcpy(&bits, &r.required, 4);
        if (was) memcpy(&oldBits, &was->required, 4);
        if (was && bits == oldBits) continue;
        LONG line = InterlockedIncrement(&g_recipeLogs);
        if (line > 512) return;
        Logf("republicmp  CONSTRUCTION_RECIPE: command #%u after tick %llu building %d list #%d stage %d entry %d [%s] required %.9g (%08X), %s %.9g (%08X)%s",
             seq, tick, r.building, r.ordinal, r.slot, r.entry, r.resource, r.required, bits, was ? "previous" : "new entry; previous",
             was ? was->required : 0.0f, oldBits, line == 512 ? " - recipe report limit reached" : "");
    }
}
