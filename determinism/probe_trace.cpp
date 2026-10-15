// See probe_trace.h.
#include "probe_trace.h"
#include <string.h>
#include <unordered_map>
#include <vector>

#define WORLD_RVA 0x9D4F10

#define BLD_STORAGE_OFFSET 0x970
#define BLD_TYPE_OFFSET 0x318
#define BLD_POS_OFFSET 0x3A0
#define TYPE_CATEGORY_OFFSET 0x360
#define CIT_AGE_OFFSET 0xD4
#define CIT_POS_OFFSET 0x620
#define VEH_ID_OFFSET 0x4
#define VEH_POS_OFFSET 0xDE0
#define SLOT_SIZE 0xE0
#define ENTRY_SIZE 0x10

#define MAX_OBJECTS 100000
#define MAX_SLOTS 32
#define MAX_ENTRIES 64

#define TRACE_BUDGET (640ull << 20)
#define REPORT_OBJECTS 6
#define REPORT_WORDS 16
#define NOISE_OFFSETS 12
#define WATCH_LINES 60

void ProbeTrace_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// Bytes left out: written by the renderer, the camera or a window on its own
// schedule, never read by the simulation (from SoylentGreed's vehicle and
// building notes).
struct Mask {
    unsigned offset, size;
};
static const Mask kBuildingMasks[] = {
    { 0x121C, 0x14 }, // recomputed by the building window every frame
};
static const Mask kVehicleMasks[] = {
    { 0x8, 0x20 },    // model name buffer
    { 0xED8, 0xBC },  // render transform
    { 0x115C, 4 },    // interpolant of the render pass
    { 0x13D4, 4 },    // animation start
    { 0x13D8, 2 },    // visible now / last frame
    { 0x13E0, 4 },    // camera distance
    { 0x142C, 4 },  { 0x1468, 4 }, { 0x153C, 4 }, { 0x16FC, 4 }, { 0x1768, 4 }, { 0x1A18, 4 }, // render words
    { 0x1878, 8 },    // sound object
    { 0x1958, 4 },    // sound volume
};

static const Mask kWorldMasks[] = {
    { 0xF60, 0x18 }, // mouse ray and cursor point
};

// A list kind is a world vector of object pointers; the world kind is the
// world object itself. A watched kind never stops the trace: each offset that
// starts to differ is logged once.
struct Kind {
    const char* name;
    unsigned listOffset; // 0: the world object
    unsigned size;
    const Mask* masks;
    int maskCount;
    bool storage;
    bool watch;
};
static const Kind kKinds[] = {
    { "vehicle", 0x12810, 0x1AD8, kVehicleMasks, (int)(sizeof(kVehicleMasks) / sizeof(Mask)), false, false },
    { "building", 0x11B08, 0x1420, kBuildingMasks, (int)(sizeof(kBuildingMasks) / sizeof(Mask)), true, false },
    { "citizen", 0x126A8, 0x750, nullptr, 0, false, false },
    { "world", 0, 0x14800, kWorldMasks, (int)(sizeof(kWorldMasks) / sizeof(Mask)), false, true },
};
#define KIND_CITIZEN 2
#define KINDS ((int)(sizeof(kKinds) / sizeof(kKinds[0])))

// One object after a step, as one blob: the object's bytes (masks zeroed),
// then for a building its storage: per slot the entry count (u32), the slot's
// 0xE0 bytes and its entries' 16 bytes each.
typedef std::vector<BYTE> Blob;

// ---------------------------------------------------------------------------
// Reading the world
// ---------------------------------------------------------------------------

static long long Count(const BYTE* vec, long long elemSize)
{
    BYTE* b = *(BYTE* const*)vec;
    BYTE* e = *(BYTE* const*)(vec + 8);
    if (!b || e < b) return 0;
    return (e - b) / elemSize;
}

static bool CopyRaw(const BYTE* from, BYTE* out, size_t n)
{
    __try {
        memcpy(out, from, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Appends a building's storage to `blob` (`cap` bytes free); bytes used, -1 if
// a read faulted.
static long long CopyStorage(const BYTE* raw, BYTE* blob, size_t cap)
{
    size_t used = 0;
    __try {
        BYTE* slots = *(BYTE* const*)(raw + BLD_STORAGE_OFFSET);
        long long ns = Count(raw + BLD_STORAGE_OFFSET, SLOT_SIZE);
        if (ns > MAX_SLOTS) ns = MAX_SLOTS;
        for (long long s = 0; s < ns; s++) {
            BYTE* slot = slots + s * SLOT_SIZE;
            long long ne = Count(slot, ENTRY_SIZE);
            if (ne > MAX_ENTRIES) ne = MAX_ENTRIES;
            size_t need = 4 + SLOT_SIZE + (size_t)ne * ENTRY_SIZE;
            if (used + need > cap) break;
            *(unsigned*)(blob + used) = (unsigned)ne;
            memcpy(blob + used + 4, slot, SLOT_SIZE);
            if (ne) memcpy(blob + used + 4 + SLOT_SIZE, *(BYTE**)slot, (size_t)ne * ENTRY_SIZE);
            used += need;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return (long long)used;
}

static bool ReadList(unsigned listOffset, BYTE*** list, long long* n)
{
    __try {
        BYTE* vec = g_exeBase + WORLD_RVA + listOffset;
        *list = *(BYTE***)vec;
        *n = Count(vec, 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return *n >= 0 && *n <= MAX_OBJECTS;
}

static void CopyAll(const Kind& k, std::vector<Blob>& out)
{
    if (!k.listOffset) {
        out.resize(1);
        out[0].resize(k.size);
        if (!CopyRaw(g_exeBase + WORLD_RVA, out[0].data(), k.size)) memset(out[0].data(), 0, k.size);
        for (int m = 0; m < k.maskCount; m++) memset(out[0].data() + k.masks[m].offset, 0, k.masks[m].size);
        return;
    }
    BYTE** list = nullptr;
    long long n = 0;
    if (!ReadList(k.listOffset, &list, &n)) n = 0;
    out.resize((size_t)n);
    static BYTE storage[MAX_SLOTS * (4 + SLOT_SIZE + MAX_ENTRIES * ENTRY_SIZE)];
    for (long long i = 0; i < n; i++) {
        Blob& b = out[(size_t)i];
        b.resize(k.size);
        BYTE* p = list[i];
        if (!p || !CopyRaw(p, b.data(), k.size)) {
            memset(b.data(), 0, k.size);
            continue;
        }
        for (int m = 0; m < k.maskCount; m++) memset(b.data() + k.masks[m].offset, 0, k.masks[m].size);
        if (k.storage) {
            long long used = CopyStorage(b.data(), storage, sizeof(storage));
            if (used > 0) b.insert(b.end(), storage, storage + used);
        }
    }
}

// Identity across the two runs: a vehicle's id, a building's position bits,
// a citizen's (and the world's) place in its list.
static unsigned long long Key(int ki, const Blob& b, size_t i)
{
    const Kind& k = kKinds[ki];
    if (ki == KIND_CITIZEN || !k.listOffset) return i;
    if (!k.storage) return *(const unsigned*)(b.data() + VEH_ID_OFFSET);
    unsigned long long h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 12; i++) h = (h ^ b[BLD_POS_OFFSET + i]) * 0x100000001b3ull;
    return h;
}

// A qword that looks like a user-mode heap pointer: different on every load.
static bool PointerLike(unsigned long long q)
{
    unsigned hi = (unsigned)(q >> 32);
    return hi >= 0x1 && hi <= 0x7fff && (unsigned)q != 0;
}

// The dword at `o` is a half of a pointer in either blob.
static bool InPointer(const Blob& a, const Blob& b, size_t o)
{
    size_t q = o & ~(size_t)7;
    if (q + 8 > a.size() || q + 8 > b.size()) return false;
    return PointerLike(*(const unsigned long long*)(a.data() + q)) ||
           PointerLike(*(const unsigned long long*)(b.data() + q));
}

// ---------------------------------------------------------------------------
// Run 1: the stream
// ---------------------------------------------------------------------------
//
// Per step and kind: u32 object count, then per object either
//   u32 index, u32 dword changes n, n x {u32 offset, u32 value}   (same size)
//   u32 index, 0xFFFFFFFF, u32 bytes, the bytes                    (size changed)
// ending with index 0xFFFFFFFF. A count change writes every object in full.

static std::vector<BYTE> g_stream;
static std::vector<size_t> g_stepAt; // step (1-based) - 1 -> offset in g_stream
static std::vector<Blob> g_last[KINDS];
static bool g_full = false;

static void Put(unsigned v)
{
    const BYTE* p = (const BYTE*)&v;
    g_stream.insert(g_stream.end(), p, p + 4);
}

static void PutFull(unsigned index, const Blob& b)
{
    Put(index);
    Put(0xFFFFFFFFu);
    Put((unsigned)b.size());
    g_stream.insert(g_stream.end(), b.begin(), b.end());
}

static void Record(int ki, std::vector<Blob>& cur)
{
    std::vector<Blob>& last = g_last[ki];
    Put((unsigned)cur.size());
    bool all = cur.size() != last.size();
    for (size_t i = 0; i < cur.size(); i++) {
        const Blob& c = cur[i];
        if (all || c.size() != last[i].size()) {
            PutFull((unsigned)i, c);
            continue;
        }
        const Blob& l = last[i];
        unsigned changes = 0;
        size_t mark = 0;
        for (size_t o = 0; o + 4 <= c.size(); o += 4) {
            unsigned v = *(const unsigned*)(c.data() + o);
            if (v == *(const unsigned*)(l.data() + o)) continue;
            if (!changes) {
                Put((unsigned)i);
                mark = g_stream.size();
                Put(0);
            }
            Put((unsigned)o);
            Put(v);
            changes++;
        }
        if (changes) memcpy(g_stream.data() + mark, &changes, 4);
    }
    Put(0xFFFFFFFFu);
    last.swap(cur);
}

static unsigned Get(size_t& at)
{
    unsigned v;
    memcpy(&v, g_stream.data() + at, 4);
    at += 4;
    return v;
}

// Applies step `step`'s records to `mirror` (run 1 as it was after that step).
static void Replay(unsigned long long step, std::vector<Blob>* mirror)
{
    size_t at = g_stepAt[(size_t)(step - 1)];
    for (int ki = 0; ki < KINDS; ki++) {
        std::vector<Blob>& m = mirror[ki];
        unsigned n = Get(at);
        m.resize(n);
        for (;;) {
            unsigned index = Get(at);
            if (index == 0xFFFFFFFFu) break;
            unsigned changes = Get(at);
            Blob& b = m[index];
            if (changes == 0xFFFFFFFFu) {
                unsigned bytes = Get(at);
                b.assign(g_stream.data() + at, g_stream.data() + at + bytes);
                at += bytes;
                continue;
            }
            for (unsigned c = 0; c < changes; c++) {
                unsigned o = Get(at), v = Get(at);
                memcpy(b.data() + o, &v, 4);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Run 2: comparing
// ---------------------------------------------------------------------------

static std::vector<Blob> g_mirror[KINDS], g_cur[KINDS], g_prev[KINDS];
// Per object (by identity), the dwords that already differed the first time
// it was compared: left over from the load (heap contents the game never
// initialised), not simulation.
static std::unordered_map<unsigned long long, std::vector<bool>> g_noise[KINDS];
static bool g_reported = false, g_stopped = false;
static unsigned long long g_steps2 = 0;

static void LogNoise(int ki)
{
    const Kind& k = kKinds[ki];
    std::unordered_map<unsigned, int> perOffset;
    int objects = 0;
    for (auto& e : g_noise[ki]) {
        bool any = false;
        for (size_t d = 0; d < e.second.size(); d++)
            if (e.second[d]) {
                perOffset[(unsigned)(d * 4)]++;
                any = true;
            }
        objects += any;
    }
    Logf("republicmp  PROBE TRACE: %ss: %d of %zu differ already at their first comparison (load leftovers, left out), "
         "%zu offset(s)", k.name, objects, g_noise[ki].size(), perOffset.size());
    for (int line = 0; line < NOISE_OFFSETS && !perOffset.empty(); line++) {
        auto best = perOffset.begin();
        for (auto it = perOffset.begin(); it != perOffset.end(); ++it)
            if (it->second > best->second || (it->second == best->second && it->first < best->first)) best = it;
        Logf("republicmp  PROBE TRACE:   +0x%X in %d %s(s)", best->first, best->second, k.name);
        perOffset.erase(best);
    }
}

static void Where(const Kind& k, const Blob& b, size_t o, char* out, size_t outSize)
{
    if (o < k.size) {
        _snprintf_s(out, outSize, _TRUNCATE, "+0x%X", (unsigned)o);
        return;
    }
    size_t at = k.size;
    for (int s = 0; at + 4 + SLOT_SIZE <= b.size(); s++) {
        unsigned ne = *(const unsigned*)(b.data() + at);
        size_t end = at + 4 + SLOT_SIZE + (size_t)ne * ENTRY_SIZE;
        if (o < end) {
            if (o < at + 4) _snprintf_s(out, outSize, _TRUNCATE, "slot %d count", s);
            else if (o < at + 4 + SLOT_SIZE) _snprintf_s(out, outSize, _TRUNCATE, "slot %d +0x%X", s, (unsigned)(o - at - 4));
            else {
                size_t eo = o - at - 4 - SLOT_SIZE;
                char name[32] = "?";
                const char* res = *(const char* const*)(b.data() + at + 4 + SLOT_SIZE + (eo & ~(size_t)15));
                __try {
                    if (res) strncpy_s(name, res, _TRUNCATE);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                _snprintf_s(out, outSize, _TRUNCATE, "slot %d entry %u (%s) +0x%X", s, (unsigned)(eo / ENTRY_SIZE), name,
                            (unsigned)(eo % ENTRY_SIZE));
            }
            return;
        }
        at = end;
    }
    _snprintf_s(out, outSize, _TRUNCATE, "storage +0x%X", (unsigned)(o - k.size));
}

static void Describe(int ki, size_t i, const Blob& b)
{
    const Kind& k = kKinds[ki];
    if (ki == KIND_CITIZEN) {
        const float* pos = (const float*)(b.data() + CIT_POS_OFFSET);
        Logf("republicmp  PROBE TRACE:   citizen #%zu age %.6f at (%.2f,%.2f,%.2f):", i,
             *(const float*)(b.data() + CIT_AGE_OFFSET), pos[0], pos[1], pos[2]);
    } else if (k.storage) {
        int category = -1;
        __try {
            BYTE* type = *(BYTE* const*)(b.data() + BLD_TYPE_OFFSET);
            if (type) category = *(int*)(type + TYPE_CATEGORY_OFFSET);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        const float* pos = (const float*)(b.data() + BLD_POS_OFFSET);
        Logf("republicmp  PROBE TRACE:   building #%zu index %d category %d at (%.2f,%.2f,%.2f):", i, *(const int*)b.data(),
             category, pos[0], pos[1], pos[2]);
    } else {
        const float* pos = (const float*)(b.data() + VEH_POS_OFFSET);
        Logf("republicmp  PROBE TRACE:   vehicle #%zu id %d at (%.3f,%.3f,%.3f):", i, *(const int*)(b.data() + VEH_ID_OFFSET),
             pos[0], pos[1], pos[2]);
    }
}

static void LogDword(const char* where, unsigned r1, unsigned r2, const unsigned* before)
{
    float f1, f2, fb = 0;
    memcpy(&f1, &r1, 4);
    memcpy(&f2, &r2, 4);
    if (before) memcpy(&fb, before, 4);
    if (before)
        Logf("republicmp  PROBE TRACE:     %s: run 1 %08x run 2 %08x, step before %08x (float %.9g / %.9g, before %.9g; "
             "int %d / %d)", where, r1, r2, *before, f1, f2, fb, (int)r1, (int)r2);
    else
        Logf("republicmp  PROBE TRACE:     %s: run 1 %08x run 2 %08x (float %.9g / %.9g; int %d / %d)", where, r1, r2, f1, f2,
             (int)r1, (int)r2);
}

// Compares kind `ki` (mirror = run 1, cur = run 2); with `report`, logs it.
// Returns the number of differing objects.
static int Compare(int ki, bool report, unsigned long long step)
{
    const Kind& k = kKinds[ki];
    std::vector<Blob>& m = g_mirror[ki];
    std::vector<Blob>& c = g_cur[ki];
    std::vector<Blob>& p = g_prev[ki];
    size_t n = m.size() < c.size() ? m.size() : c.size();
    int differing = 0;
    for (size_t i = 0; i < n; i++) {
        const Blob& a = m[i];
        const Blob& b = c[i];
        unsigned long long key = Key(ki, b, i);
        auto it = g_noise[ki].find(key);
        if (it == g_noise[ki].end()) { // first comparison: whatever differs now is a load leftover
            std::vector<bool> noise((b.size() > a.size() ? b.size() : a.size()) / 4, false);
            size_t len = (a.size() < b.size() ? a.size() : b.size());
            for (size_t o = 0; o + 4 <= len; o += 4)
                if (*(const unsigned*)(a.data() + o) != *(const unsigned*)(b.data() + o)) noise[o / 4] = true;
            g_noise[ki].emplace(key, std::move(noise));
            continue;
        }
        const std::vector<bool>& noise = it->second;
        bool sizeDiffers = a.size() != b.size();
        size_t len = (a.size() < b.size() ? a.size() : b.size());
        int words = 0;
        for (size_t o = 0; o + 4 <= len; o += 4) {
            unsigned r1 = *(const unsigned*)(a.data() + o), r2 = *(const unsigned*)(b.data() + o);
            if (r1 == r2) continue;
            if (o / 4 < noise.size() && noise[o / 4]) continue;
            if (InPointer(a, b, o)) continue;
            if (!words && !sizeDiffers) {
                differing++;
                if (report && differing <= REPORT_OBJECTS) Describe(ki, i, b);
            }
            words++;
            if (report && differing <= REPORT_OBJECTS && words <= REPORT_WORDS) {
                char where[96];
                Where(k, b, o, where, sizeof(where));
                const unsigned* before = i < p.size() && o + 4 <= p[i].size() ? (const unsigned*)(p[i].data() + o) : nullptr;
                LogDword(where, r1, r2, before);
            }
        }
        if (sizeDiffers) {
            differing++;
            if (report && differing <= REPORT_OBJECTS) {
                Describe(ki, i, b);
                Logf("republicmp  PROBE TRACE:     storage size differs: run 1 %zu bytes, run 2 %zu", a.size(), b.size());
            }
        }
        if (report && differing <= REPORT_OBJECTS && words > REPORT_WORDS)
            Logf("republicmp  PROBE TRACE:     ... %d more word(s)", words - REPORT_WORDS);
    }
    if (m.size() != c.size()) differing++;
    if (report && differing)
        Logf("republicmp  PROBE TRACE: %ss first differ after step %llu: %d of them, %zu/%zu listed", k.name, step,
             differing, m.size(), c.size());
    return differing;
}

// A watched kind: logs each offset the first time it differs (after the
// object's load leftovers), and goes on.
static std::unordered_map<unsigned, bool> g_watched[KINDS];
static int g_watchLines[KINDS];

static void Watch(int ki, unsigned long long step)
{
    const Kind& k = kKinds[ki];
    std::vector<Blob>& m = g_mirror[ki];
    std::vector<Blob>& c = g_cur[ki];
    std::vector<Blob>& p = g_prev[ki];
    size_t n = m.size() < c.size() ? m.size() : c.size();
    for (size_t i = 0; i < n; i++) {
        const Blob& a = m[i];
        const Blob& b = c[i];
        unsigned long long key = Key(ki, b, i);
        size_t len = (a.size() < b.size() ? a.size() : b.size());
        auto it = g_noise[ki].find(key);
        if (it == g_noise[ki].end()) {
            std::vector<bool> noise(len / 4, false);
            for (size_t o = 0; o + 4 <= len; o += 4)
                if (*(const unsigned*)(a.data() + o) != *(const unsigned*)(b.data() + o)) noise[o / 4] = true;
            g_noise[ki].emplace(key, std::move(noise));
            continue;
        }
        const std::vector<bool>& noise = it->second;
        for (size_t o = 0; o + 4 <= len; o += 4) {
            unsigned r1 = *(const unsigned*)(a.data() + o), r2 = *(const unsigned*)(b.data() + o);
            if (r1 == r2 || (o / 4 < noise.size() && noise[o / 4]) || InPointer(a, b, o)) continue;
            if (g_watched[ki].count((unsigned)o)) continue;
            g_watched[ki][(unsigned)o] = true;
            if (g_watchLines[ki] >= WATCH_LINES) continue;
            g_watchLines[ki]++;
            char where[64];
            _snprintf_s(where, sizeof(where), _TRUNCATE, "%s +0x%X after step %llu", k.name, (unsigned)o, step);
            const unsigned* before = i < p.size() && o + 4 <= p[i].size() ? (const unsigned*)(p[i].data() + o) : nullptr;
            LogDword(where, r1, r2, before);
        }
    }
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void ProbeTrace_Reset()
{
    std::vector<BYTE>().swap(g_stream);
    std::vector<size_t>().swap(g_stepAt);
    for (int ki = 0; ki < KINDS; ki++) {
        std::vector<Blob>().swap(g_last[ki]);
        std::vector<Blob>().swap(g_mirror[ki]);
        std::vector<Blob>().swap(g_cur[ki]);
        std::vector<Blob>().swap(g_prev[ki]);
        g_noise[ki].clear();
        g_watched[ki].clear();
        g_watchLines[ki] = 0;
    }
    g_full = g_reported = g_stopped = false;
    g_steps2 = 0;
}

void ProbeTrace_Step(int run, unsigned long long step)
{
    if (run == 0) {
        if (g_full) return;
        if (g_stream.size() > TRACE_BUDGET) {
            g_full = true;
            Logf("republicmp  PROBE TRACE: kept for the first %llu steps only (memory budget)", step - 1);
            return;
        }
        g_stepAt.push_back(g_stream.size());
        for (int ki = 0; ki < KINDS; ki++) {
            std::vector<Blob> cur;
            CopyAll(kKinds[ki], cur);
            Record(ki, cur);
        }
        return;
    }
    if (g_stopped) return;
    if (step == 1)
        for (int ki = 0; ki < KINDS; ki++) {
            std::vector<Blob>().swap(g_last[ki]); // run 1's working copies are not needed any more
            g_mirror[ki].clear();
            g_prev[ki].clear();
        }
    if (step - 1 >= g_stepAt.size()) {
        g_stopped = true;
        return;
    }
    g_steps2 = step;
    Replay(step, g_mirror);
    int differing = 0;
    for (int ki = 0; ki < KINDS; ki++) {
        CopyAll(kKinds[ki], g_cur[ki]);
        if (kKinds[ki].watch) Watch(ki, step);
        else if (!g_reported) differing += Compare(ki, false, step);
    }
    if (step == 1)
        for (int ki = 0; ki < KINDS; ki++) LogNoise(ki);
    if (differing) {
        for (int ki = 0; ki < KINDS; ki++)
            if (!kKinds[ki].watch) Compare(ki, true, step);
        g_reported = true; // the watched kinds go on to the end
    }
    for (int ki = 0; ki < KINDS; ki++) g_prev[ki].swap(g_cur[ki]);
}

void ProbeTrace_Finish()
{
    if (!g_reported)
        Logf("republicmp  PROBE TRACE: vehicles, buildings and citizens identical after every one of %llu traced step(s)",
             g_steps2);
    ProbeTrace_Reset();
}
