#include "held_frame.h"
#include "determinism.h"
#include <vector>
#include <new>
#include <unordered_set>
#include <string.h>

void HeldFrame_Bind(const TsmHost* host) { TsmBind(host); }

// 2593d0 draws world+12810 through 3fa850 (6712b0, 67c280, 66edd0).
// Those builders recurse through parent +398, children +3a0 and link +4a0,
// and write render words back into vehicles. Copy every reachable object,
// then restore changed words after the worker joins. Never restore a changed
// heap pointer: the old allocation may have been freed. (by SoylentGreed)
static bool Read(const void* at, void* out, size_t n)
{
    SIZE_T done = 0;
    return !n || (ReadProcessMemory(GetCurrentProcess(), at, out, n, &done) && done == n);
}

static bool HeapPointer(unsigned long long v)
{
    if (v < (1ull << 32) || v >= 0x800000000000ull || (v & 7)) return false;
    MEMORY_BASIC_INFORMATION m = {};
    return VirtualQuery((void*)v, &m, sizeof(m)) == sizeof(m) && m.State == MEM_COMMIT;
}

class VehicleGuard {
    static const size_t Bytes = 0x1AD8;
    std::vector<BYTE*> targets_;
    std::unordered_set<BYTE*> seen_;
    std::vector<BYTE> copies_;
    std::vector<std::pair<BYTE*, BYTE>> flags_;

    void Add(BYTE* p) { if (p && seen_.insert(p).second) targets_.push_back(p); }
public:
    bool Begin(BYTE* listAt)
    {
        targets_.clear(); seen_.clear(); flags_.clear();
        BYTE** v[2] = {};
        if (!Read(listAt, v, sizeof(v)) || (uintptr_t)v[1] < (uintptr_t)v[0] ||
            ((uintptr_t)v[1] - (uintptr_t)v[0]) % 8 || ((uintptr_t)v[1] - (uintptr_t)v[0]) / 8 > 65536) return false;
        std::vector<BYTE*> list(((uintptr_t)v[1] - (uintptr_t)v[0]) / 8);
        if (!Read(v[0], list.data(), list.size() * 8)) return false;
        for (BYTE* p : list) {
            if (!p) continue;
            BYTE visible = 0, linked = 0; BYTE* link = nullptr;
            if (!Read(p + 0x13D8, &visible, 1) || !Read(p + 0x4A0, &link, 8) ||
                (link && !Read(link + 0x13D8, &linked, 1))) return false;
            if (visible || linked) { Add(p); Add(link); }
        }
        for (size_t i = 0; i < targets_.size(); i++) {
            if (targets_.size() > (64ull << 20) / Bytes) return false;
            BYTE* parent = nullptr; BYTE** children[2] = {};
            if (!Read(targets_[i] + 0x398, &parent, 8) || !Read(targets_[i] + 0x3A0, children, 16)) return false;
            Add(parent);
            uintptr_t begin = (uintptr_t)children[0], end = (uintptr_t)children[1];
            if (end < begin || (end - begin) % 8 || (end - begin) / 8 > 1024) return false;
            std::vector<BYTE*> c((end - begin) / 8);
            if (!Read(children[0], c.data(), c.size() * 8)) return false;
            for (BYTE* child : c) Add(child);
        }
        copies_.resize(targets_.size() * Bytes);
        for (size_t i = 0; i < targets_.size(); i++)
            if (!Read(targets_[i], copies_.data() + i * Bytes, Bytes)) return false;
        // Validate all flags before changing any of them; +d41 would ask the
        // worker to run the vehicle's 6bc9a0 simulation pass on this held frame.
        for (BYTE* p : list) {
            BYTE f = 0;
            if (p && !Read(p + 0xD41, &f, 1)) return false;
            if (f) flags_.push_back({ p, f });
        }
        for (const auto& f : flags_) f.first[0xD41] = 0;
        return true;
    }

    bool End()
    {
        for (const auto& f : flags_) f.first[0xD41] = f.second;
        flags_.clear();
        bool ok = true;
        unsigned long long current[Bytes / 8];
        for (size_t i = 0; i < targets_.size(); i++) {
            BYTE* p = targets_[i];
            if (!Read(p, current, Bytes)) { ok = false; continue; }
            for (size_t o = 0; o < Bytes; o += 8) {
                unsigned long long before, now;
                memcpy(&before, copies_.data() + i * Bytes + o, 8);
                now = current[o / 8];
                if (now == before) continue;
                if (HeapPointer(before) && HeapPointer(now)) {
                    Logf("republicmp  HELD FRAME: vehicle pointer changed at +0x%zX - probe refused", o);
                    ok = false;
                } else memcpy(p + o, &before, 8);
            }
        }
        return ok;
    }
};

bool HeldFrame_Verify()
{
    struct Site { unsigned rva; unsigned char bytes[24]; size_t size; };
    static const Site sites[] = {
        { 0x83CEA0, { 0x48,0x8b,0xc4,0x48,0x89,0x48,0x08,0x55,0x53,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x8d,0xa8,0x98,0xfc,0xff,0xff }, 22 },
        { 0x2595AE, { 0x48,0x8d,0x0d,0x5b,0xb9,0x77,0x00,0xe8,0x96,0x12,0x1a,0x00 }, 12 },
        { 0x3FA907, { 0x49,0x8b,0x86,0x18,0x28,0x01,0x00,0x49,0x2b,0x86,0x10,0x28,0x01,0x00 }, 14 },
        { 0x3FAA13, { 0x41,0x0f,0xb6,0xd7,0xe8,0x94,0x68,0x27,0x00 }, 9 },
        { 0x67135B, { 0x80,0xb9,0xd8,0x13,0x00,0x00,0x00 }, 7 },
        { 0x671368, { 0x48,0x8b,0x89,0x98,0x03,0x00,0x00 }, 7 },
        { 0x6713D0, { 0x48,0x8b,0x9e,0xa8,0x03,0x00,0x00,0x48,0x2b,0x9e,0xa0 }, 11 },
        { 0x66EDF9, { 0x80,0xb9,0xd8,0x13,0x00,0x00,0x00 }, 7 },
        { 0x67C2AE, { 0x48,0x8b,0xb1,0xa0,0x04,0x00,0x00,0x49,0x8b,0xd8,0x44,0x0f,0xb6,0xe2,0x4c,0x8b,0xf1,0x80,0xbe,0xd8,0x13,0x00,0x00,0x00 }, 24 },
    };
    for (const auto& s : sites) {
        if (memcmp(g_exeBase + s.rva, s.bytes, s.size)) {
            Logf("republicmp  HELD FRAME: unexpected bytes at exe+0x%X", s.rva);
            return false;
        }
    }
    return true;
}

// Early (30cb74..30cf4e) normally starts and joins this worker. On a held
// frame its simulation vector exe+9e7738 and the queues at [9e7710]+60 and
// [9e2350]+40 are empty. The vehicle render vector stays visible under the
// guard; when the guard cannot capture it, the vector world+12810 and the
// realism setting exe+9d54cc are hidden for the pass instead (vehicles are
// not drawn on that frame). Both threads draw from private streams. Without
// a usable worker the entities are still drawn. (by SoylentGreed)
bool HeldFrame_Draw(bool entitiesReady)
{
    BYTE* worker = *(BYTE**)(g_exeBase + 0x995290);
    bool workerOk = worker && !worker[0x28] && *(HANDLE*)(worker + 0x60) && *(DWORD*)(worker + 0xC8);
    static VehicleGuard guard;
    static bool hiddenLogged = false;
    bool guarded = false;
    uintptr_t hidden[2] = {};
    int realism = 0;
    if (workerOk) {
        try {
            guarded = guard.Begin(g_exeBase + 0x9E7720);
        } catch (const std::bad_alloc&) {
            guarded = false;
        }
        if (!guarded) {
            if (!hiddenLogged) {
                hiddenLogged = true;
                Logf("republicmp  HELD FRAME: vehicle graph could not be captured - vehicles hidden on such frames");
            }
            hidden[0] = *(uintptr_t*)(g_exeBase + 0x9E7728);
            *(uintptr_t*)(g_exeBase + 0x9E7728) = *(uintptr_t*)(g_exeBase + 0x9E7720);
            hidden[1] = 1;
            realism = *(int*)(g_exeBase + 0x9D54CC);
            *(int*)(g_exeBase + 0x9D54CC) = 0;
        }
    }
    BYTE* queue = *(BYTE**)(g_exeBase + 0x9E7710);
    BYTE* paths = *(BYTE**)(g_exeBase + 0x9E2350);
    BYTE* headers[] = { g_exeBase + 0x9E7738, queue ? queue + 0x60 : nullptr, paths ? paths + 0x40 : nullptr };
    uintptr_t ends[3] = {};
    bool started = false;
    if (workerOk) {
        for (int i = 0; i < 3; i++) if (headers[i]) {
            ends[i] = *(uintptr_t*)(headers[i] + 8);
            *(uintptr_t*)(headers[i] + 8) = *(uintptr_t*)headers[i];
        }
        Determinism_SetPrivateWorker(*(DWORD*)(worker + 0xC8));
        worker[0x59] = 0;
        started = SetEvent(*(HANDLE*)(worker + 0x60)) != FALSE;
    }
    if (entitiesReady) {
        Determinism_PrivateScope privateRng;
        BYTE* world = g_exeBase + 0x9D4F10;
        BYTE* list = world + (g_exeBase[0xA54BB5] ? 0x126A8 : 0x126C0);
        ((void (*)(void*, void*))(g_exeBase + 0x83CEA0))(nullptr, list);
    }
    if (started) {
        auto* lock = (CRITICAL_SECTION*)(worker + 0x70);
        for (;;) {
            EnterCriticalSection(lock);
            bool done = worker[0x59] != 0;
            LeaveCriticalSection(lock);
            if (done) break;
            SwitchToThread();
        }
    }
    bool clean = true;
    if (workerOk) {
        Determinism_SetPrivateWorker(0);
        if (guarded) clean = guard.End();
        for (int i = 0; i < 3; i++) if (headers[i]) *(uintptr_t*)(headers[i] + 8) = ends[i];
        if (hidden[1]) {
            *(uintptr_t*)(g_exeBase + 0x9E7728) = hidden[0];
            *(int*)(g_exeBase + 0x9D54CC) = realism;
        }
    }
    return clean;
}
