// See det_patches.h.
#include "det_patches.h"
#include "determinism.h"
#include <string.h>
#include <limits.h>
#include <vector>

#define WORLD_RVA 0x9D4F10

void DetPatches_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ---------------------------------------------------------------------------
// Byte patches
// ---------------------------------------------------------------------------

struct DetPatch {
    unsigned int rva;
    unsigned char size;
    unsigned char original[26];
    unsigned char patch[26];
    const char* what;
};

#define NOP2 0x66, 0x90
#define NOP5 0x0f, 0x1f, 0x44, 0x00, 0x00
#define NOP6 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00
#define NOP21 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, \
              0x90, 0x90, 0x90, 0x90

static const DetPatch kPatches[] = {
    // cmp +13d8 / je skip / cmp +13d9 / jne skip -> jmp skip
    { 0x6a5475, 26, { 0x80, 0xbb, 0xd8, 0x13, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xe6, 0x08, 0x00, 0x00,
                      0x80, 0xbb, 0xd9, 0x13, 0x00, 0x00, 0x00, 0x0f, 0x85, 0xd9, 0x08, 0x00, 0x00 },
      { 0xe9, 0xee, 0x08, 0x00, 0x00, NOP21 }, "vehicle became visible (6a5410)" },
    { 0x6ab9d4, 26, { 0x80, 0xbe, 0xd8, 0x13, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xbf, 0x03, 0x00, 0x00,
                      0x80, 0xbe, 0xd9, 0x13, 0x00, 0x00, 0x00, 0x0f, 0x85, 0xb2, 0x03, 0x00, 0x00 },
      { 0xe9, 0xc7, 0x03, 0x00, 0x00, NOP21 }, "vehicle became visible (6a7520)" },
    // jne camera branch -> nop6
    { 0x6af131, 6, { 0x0f, 0x85, 0x0d, 0x01, 0x00, 0x00 }, { NOP6 }, "road vehicle placement (6af0c0)" },
    { 0x6ada40, 6, { 0x0f, 0x85, 0x16, 0x01, 0x00, 0x00 }, { NOP6 }, "rail vehicle placement (6ad960)" },
    // movzx eax,byte [rbx+13d8] -> xor eax,eax; nop5
    { 0x66e74c, 7, { 0x0f, 0xb6, 0x83, 0xd8, 0x13, 0x00, 0x00 }, { 0x31, 0xc0, NOP5 }, "vehicle checked flag" },
    // jne (piece on the camera) -> nop
    { 0x548b36, 2, { 0x75, 0x10 }, { NOP2 }, "road sign zones" },
    // the camera's own update of a followed vehicle -> nop5; the worker's skip -> nop
    { 0x10e4f, 5, { 0xe8, 0xdc, 0xc6, 0x65, 0x00 }, { NOP5 }, "follow camera: train head" },
    { 0x10e87, 5, { 0xe8, 0xa4, 0xc6, 0x65, 0x00 }, { NOP5 }, "follow camera: wagon" },
    { 0x10ebd, 5, { 0xe8, 0x6e, 0xc6, 0x65, 0x00 }, { NOP5 }, "follow camera: vehicle" },
    { 0x10f22, 5, { 0xe8, 0x09, 0xc6, 0x65, 0x00 }, { NOP5 }, "ride camera: vehicle" },
    { 0x3fadbc, 2, { 0x75, 0x29 }, { NOP2 }, "follow camera: worker list" },
    // debug keys: je -> jmp (same target), jne -> nop
    { 0x13d81e, 2, { 0x74, 0x6a }, { 0xeb, 0x6a }, "building pass: Ctrl+N" },
    { 0x13d89e, 6, { 0x0f, 0x84, 0xe2, 0x00, 0x00, 0x00 }, { 0xe9, 0xe3, 0x00, 0x00, 0x00, 0x90 }, "building pass: Ctrl+M" },
    { 0x69e256, 6, { 0x0f, 0x85, 0x5c, 0x38, 0x00, 0x00 }, { NOP6 }, "vehicle routine: T" },
    { 0x31603a, 6, { 0x0f, 0x84, 0x06, 0x01, 0x00, 0x00 }, { 0xe9, 0x07, 0x01, 0x00, 0x00, 0x90 }, "Late body: H (networks)" },
    { 0x3161c3, 2, { 0x74, 0x48 }, { 0xeb, 0x48 }, "Late body: H (cursor)" },
    { 0x738e83, 6, { 0x0f, 0x84, 0x2b, 0x01, 0x00, 0x00 }, { 0xe9, 0x2c, 0x01, 0x00, 0x00, 0x90 }, "school window: Ctrl list cleanup" },
    { 0x14c6b5, 2, { 0x74, 0x6f }, { 0xeb, 0x6f }, "car building: Ctrl+I list cleanup" },
    { 0x1bd2ab, 2, { 0x74, 0x21 }, { 0xeb, 0x21 }, "depot: E+L+C slot word" },
    { 0xfa41, 2, { 0x74, 0x5a }, { 0xeb, 0x5a }, "render: debug calendar keys" },
};
#define PATCH_COUNT (sizeof(kPatches) / sizeof(kPatches[0]))

static volatile bool g_on = false;     // byte patches written, relays acting
static bool g_refused = false;         // a site didn't match - never tried again
static volatile DWORD g_simThread = 0; // the main thread (Early and Late)

static bool BytesAre(const DetPatch& p, const unsigned char* bytes)
{
    __try {
        return memcmp(g_exeBase + p.rva, bytes, p.size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void Write(BYTE* at, const unsigned char* bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

// ---------------------------------------------------------------------------
// Building visibility (GetNode at 13ce50)
// ---------------------------------------------------------------------------
//
// The Early building pass asks the camera's quadtree for a building's node
// and reads its visible byte (+0x25); the visible branch writes the
// building's visibility and distance (+115a/+115c), which decide how often
// its agents update. The call `call [GetNode]` (6 bytes) becomes nop + call
// to this relay: on the simulation thread it answers an all-zero node (off
// the camera), as for every building nobody looks at.

#define GETNODE_SITE 0x13CE50
#define GETNODE_IMPORT 0x86C560 // the import slot the site calls through
typedef void* (*GetNodeFn)(void* tree, int index);
static const unsigned char kGetNodeOriginal[6] = { 0xff, 0x15, 0x0a, 0xf7, 0x72, 0x00 };
static const unsigned char kOffscreenNode[72] = {};

static void* DetGetNode(void* tree, int index)
{
    void* node = (*(GetNodeFn*)(g_exeBase + GETNODE_IMPORT))(tree, index);
    if (g_on && node && GetCurrentThreadId() == g_simThread) return (void*)kOffscreenNode;
    return node;
}

// ---------------------------------------------------------------------------
// Saplings (2f8e10 from the render thread, 2f8f20 from Late)
// ---------------------------------------------------------------------------
//
// A new planting entry (world+0x11740, 0x34 bytes: name, timer +20, position
// +28) starts growing when 2f8e10 gives its timer 0.001 - called by the
// render routine every fourth rendered frame, so on each PC at its own tick.
// While the layer is on, Late starts the new entries itself right before the
// growth 2f8f20 (every tick, every PC) and queues the forest add, which the
// render thread still makes where it used to (adding trees on the simulation
// thread raced the forest's renderer).

#define SAPLING_START_RVA 0x2F8E10
#define SAPLING_GROWTH_RVA 0x2F8F20
#define SAPLING_RENDER_CALL 0x9E4D
#define SAPLING_LATE_CALL 0x30D9B2
#define SAPLING_ENGINE_RVA 0x9941F0  // engine object
#define SAPLING_ENGINE_MAP 0xEE0     // +ee0 map, its trees +2f8..+300
#define SAPLING_ENGINE_FOREST 0xFC0  // +fc0 forest
#define SAPLING_ADD_IMPORT 0x86D270  // C3D_FOREST::EditorAddCursorTreesSoviet
#define SAPLING_LIST 0x11740
#define SAPLING_ENTRY 0x34
#define SAPLING_TIMER 0x20
#define SAPLING_NAME 0x20
#define SAPLING_POSITION 0x28
#define SAPLING_START_BITS 0x3a83126f // 0.001f
static const unsigned char kSaplingRenderOriginal[5] = { 0xe8, 0xbe, 0xef, 0x2e, 0x00 };
static const unsigned char kSaplingLateOriginal[5] = { 0xe8, 0x69, 0xb5, 0xfe, 0xff };

typedef void (*SaplingStartFn)(__int64, __int64, __int64, __int64);
typedef void (*SaplingGrowthFn)(__int64 world);
typedef bool (*ForestAddFn)(void* forest, const float* position, char* name);

struct QueuedSapling {
    char name[SAPLING_NAME + 1];
    float position[3];
};
static SRWLOCK g_saplingLock = SRWLOCK_INIT;
static std::vector<QueuedSapling> g_saplingQueue;

static bool TreesLoaded()
{
    __try {
        __int64 engine = *(__int64*)(g_exeBase + SAPLING_ENGINE_RVA);
        if (!engine) return false;
        __int64 map = *(__int64*)(engine + SAPLING_ENGINE_MAP);
        if (!map) return false;
        __int64 begin = *(__int64*)(map + 0x2f8), end = *(__int64*)(map + 0x300);
        return ((end - begin) >> 4) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 2f8e70..2f8ed5 without the forest add: entries whose timer is 0.0 start.
// Returns how many were started (at most cap are written to out).
#define SAPLING_BATCH 64
static int CollectSaplings(__int64 world, QueuedSapling* out, int cap)
{
    int n = 0;
    __try {
        BYTE* begin = *(BYTE**)(world + SAPLING_LIST);
        BYTE* end = *(BYTE**)(world + SAPLING_LIST + 8);
        if (!begin || end < begin || (end - begin) % SAPLING_ENTRY) return 0;
        for (BYTE* e = begin; e < end && n < cap; e += SAPLING_ENTRY) {
            if (*(float*)(e + SAPLING_TIMER) != 0.0f) continue;
            *(unsigned int*)(e + SAPLING_TIMER) = SAPLING_START_BITS;
            memset(&out[n], 0, sizeof(out[n]));
            memcpy(out[n].name, e, SAPLING_NAME);
            memcpy(out[n].position, e + SAPLING_POSITION, 12);
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

static void StartSaplings(__int64 world)
{
    QueuedSapling found[SAPLING_BATCH];
    int n = CollectSaplings(world, found, SAPLING_BATCH); // more start on the next tick
    if (!n) return;
    AcquireSRWLockExclusive(&g_saplingLock);
    g_saplingQueue.insert(g_saplingQueue.end(), found, found + n);
    ReleaseSRWLockExclusive(&g_saplingLock);
}

static void AddToForest(const QueuedSapling* q, size_t n)
{
    __try {
        __int64 engine = *(__int64*)(g_exeBase + SAPLING_ENGINE_RVA);
        void* forest = engine ? *(void**)(engine + SAPLING_ENGINE_FOREST) : nullptr;
        ForestAddFn add = *(ForestAddFn*)(g_exeBase + SAPLING_ADD_IMPORT);
        if (!forest || !add) return;
        for (size_t i = 0; i < n; i++) {
            QueuedSapling copy = q[i]; // the engine takes a writable name
            add(forest, copy.position, copy.name);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Late, at its growth call (rcx = world).
static void DetSaplingLate(__int64 world)
{
    if (g_on && GetCurrentThreadId() == g_simThread && TreesLoaded()) StartSaplings(world);
    ((SaplingGrowthFn)(g_exeBase + SAPLING_GROWTH_RVA))(world);
}

// Render routine, at its sapling start call (registers passed through).
static void DetSaplingRender(__int64 a, __int64 b, __int64 c, __int64 d)
{
    if (!g_on) {
        ((SaplingStartFn)(g_exeBase + SAPLING_START_RVA))(a, b, c, d);
        return;
    }
    std::vector<QueuedSapling> work;
    AcquireSRWLockExclusive(&g_saplingLock);
    work.swap(g_saplingQueue);
    ReleaseSRWLockExclusive(&g_saplingLock);
    if (!work.empty()) AddToForest(work.data(), work.size());
}

// ---------------------------------------------------------------------------
// Population count, autosave (Late body 30d1a0)
// ---------------------------------------------------------------------------
//
// The simulation scales by min(1, population / K) (citizen update 256080,
// 2571e0 from the building pass and Late), but the count world+0xc720 is
// written only by the interface, every ~3 game seconds of its own frames.
// While the layer is on the simulation thread writes it itself before Early
// and before Late, with the interface's rule over the citizens +126a8: a
// citizen counts when +71c and +748 are 0 and it is not marked +2d in a
// building whose type has +8e8 set (its assignment's +c8 [+20], else its
// home +20).
//
// A save writes every vehicle and ages it on the way (6c6770 -> 6c1bd0); the
// autosave fires on each PC's own real time (world float +0, accumulated in
// Late). While the layer is on, every Late starts with that timer at 0.

#define LATE_RVA 0x30D1A0
#define POPULATION_OFFSET 0xC720
#define CITIZENS_OFFSET 0x126A8
#define AUTOSAVE_TIMER_OFFSET 0x0
// push rbp / push r12..r15 / lea rbp,[rsp-2420h]
static const unsigned char kLateExpect[18] = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xE0, 0xDB, 0xFF, 0xFF
};
typedef void (*LateFn)(__int64 world);
static LateFn o_late = nullptr;
static volatile int g_lastPopulation = -1;

static volatile bool g_populationLogged = false; // the first recount after Apply goes to the log
static volatile LONG g_populationFaults = 0;

// The interface's rule (31aee1..31b13c). Returns -1 when a read faulted.
static int CountPopulation(__int64 world, int* listed)
{
    *listed = 0;
    __try {
        __int64* begin = *(__int64**)(world + CITIZENS_OFFSET);
        __int64* end = *(__int64**)(world + CITIZENS_OFFSET + 8);
        int n = 0;
        if (begin && end >= begin) {
            *listed = (int)(end - begin);
            for (__int64* it = begin; it < end; ++it) {
                __int64 c = *it;
                if (!c || *(int*)(c + 0x71c) || *(int*)(c + 0x748)) continue;
                __int64 job = *(__int64*)(c + 0xc8);
                __int64 holder = job ? job : c;
                __int64 building = *(__int64*)(holder + 0x20);
                __int64 type = building ? *(__int64*)(building + 0x318) : 0;
                if (type && *(unsigned char*)(type + 0x8e8) == 1 && *(unsigned char*)(holder + 0x2d) == 1) continue;
                n++;
            }
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static void RecountPopulation(__int64 world)
{
    int listed = 0;
    int n = CountPopulation(world, &listed);
    int* count = (int*)(world + POPULATION_OFFSET);
    if (!g_populationLogged) {
        g_populationLogged = true;
        Logf("republicmp  DET_PATCHES: population - the game's %d, recounted %d of %d citizens listed", *count, n, listed);
    }
    if (n < 0) {
        if (InterlockedIncrement(&g_populationFaults) <= 3) Logf("republicmp  DET_PATCHES: population recount faulted");
        return;
    }
    *count = n;
    g_lastPopulation = n;
}

static void HookedLate(__int64 world)
{
    if (g_on && world == (__int64)(g_exeBase + WORLD_RVA)) {
        g_simThread = GetCurrentThreadId();
        *(float*)(world + AUTOSAVE_TIMER_OFFSET) = 0.0f;
        RecountPopulation(world);
    }
    o_late(world);
}

void DetPatches_BeforeEarly(__int64 world)
{
    g_simThread = GetCurrentThreadId();
    if (g_on) RecountPopulation(world);
}

int DetPatches_LastPopulation() { return g_lastPopulation; }

// ---------------------------------------------------------------------------
// Render thread before Early (frame loop 105df3)
// ---------------------------------------------------------------------------
//
// The main loop schedules the next render at the end of a frame (47d320:
// start set, finished reset) and waits for it only after the next frame's
// Early (105eb8). So the render thread draws frame N while frame N+1's
// Prepare and Early run; vehicles test the vehicle ahead through scene-node
// matrices (699420), which rendering touches too, and two runs from the
// same save moved a vehicle differently with every random number equal.
// While the layer is on, the frame waits for that render to finish before
// it takes the renderer lock (where vanilla only takes the lock), so the
// render and the simulation never overlap. Costs frame rate, not visuals.

#define FRAME_LOCK_SITE 0x105DF3   // call [EnterCriticalSection] on renderer+0x80
#define FRAME_LOCK_IMPORT 0x86D7F8
#define RENDERER_RVA 0x995268      // renderer object; +0x68 its "finished" event
#define RENDERER_FINISHED 0x68
#define RENDER_WAIT_MS 2000
static const unsigned char kFrameLockOriginal[6] = { 0xff, 0x15, 0xff, 0x79, 0x76, 0x00 };
typedef void (WINAPI* EnterCsFn)(LPCRITICAL_SECTION);
static volatile LONG g_renderWaitTimeouts = 0;

static void DetFrameLock(LPCRITICAL_SECTION cs)
{
    if (g_on) {
        BYTE* renderer = *(BYTE**)(g_exeBase + RENDERER_RVA);
        HANDLE finished = renderer ? *(HANDLE*)(renderer + RENDERER_FINISHED) : nullptr;
        if (finished && WaitForSingleObject(finished, RENDER_WAIT_MS) == WAIT_TIMEOUT &&
            InterlockedIncrement(&g_renderWaitTimeouts) <= 3)
            Logf("republicmp  DET_PATCHES: the render thread took over %d ms - frame went on without it", RENDER_WAIT_MS);
    }
    (*(EnterCsFn*)(g_exeBase + FRAME_LOCK_IMPORT))(cs);
}

// ---------------------------------------------------------------------------
// Install, apply, restore
// ---------------------------------------------------------------------------

// A relay page near the exe: one `jmp [rip+0]` per target, 16 bytes apart.
static BYTE* g_relay = nullptr;

static bool CallTo(unsigned int site, BYTE* target, unsigned char out[5])
{
    long long rel = (long long)target - (long long)(g_exeBase + site + 5);
    if (rel < INT_MIN || rel > INT_MAX) return false;
    int rel32 = (int)rel;
    out[0] = 0xe8;
    memcpy(out + 1, &rel32, 4);
    return true;
}

bool DetPatches_Install()
{
    bool late = InstallInlineHook(g_exeBase + LATE_RVA, (void*)&HookedLate, (void**)&o_late,
                                  kLateExpect, sizeof(kLateExpect), "republicmp late body");

    bool sitesOk = memcmp(g_exeBase + GETNODE_SITE, kGetNodeOriginal, 6) == 0 &&
                   memcmp(g_exeBase + SAPLING_RENDER_CALL, kSaplingRenderOriginal, 5) == 0 &&
                   memcmp(g_exeBase + SAPLING_LATE_CALL, kSaplingLateOriginal, 5) == 0 &&
                   memcmp(g_exeBase + FRAME_LOCK_SITE, kFrameLockOriginal, 6) == 0;
    bool relays = false;
    if (sitesOk) g_relay = AllocNear(g_exeBase, 4096);
    if (g_relay) {
        DWORD old;
        VirtualProtect(g_relay, 64, PAGE_EXECUTE_READWRITE, &old);
        void* targets[4] = { (void*)&DetGetNode, (void*)&DetSaplingRender, (void*)&DetSaplingLate, (void*)&DetFrameLock };
        for (int i = 0; i < 4; i++) {
            BYTE* r = g_relay + i * 16;
            r[0] = 0xff; r[1] = 0x25; r[2] = r[3] = r[4] = r[5] = 0;
            memcpy(r + 6, &targets[i], 8);
        }
        VirtualProtect(g_relay, 64, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), g_relay, 64);
        unsigned char getNode[6], render[5], lateCall[5], frameLock[6];
        getNode[0] = 0x90;
        frameLock[0] = 0x90;
        if (CallTo(GETNODE_SITE + 1, g_relay, getNode + 1) && CallTo(SAPLING_RENDER_CALL, g_relay + 16, render) &&
            CallTo(SAPLING_LATE_CALL, g_relay + 32, lateCall) && CallTo(FRAME_LOCK_SITE + 1, g_relay + 48, frameLock + 1)) {
            Write(g_exeBase + GETNODE_SITE, getNode, 6);
            Write(g_exeBase + SAPLING_RENDER_CALL, render, 5);
            Write(g_exeBase + SAPLING_LATE_CALL, lateCall, 5);
            Write(g_exeBase + FRAME_LOCK_SITE, frameLock, 6);
            relays = true;
        }
    }
    Logf("republicmp  DET_PATCHES: late hook %d, relays %d (building visibility, saplings, render before Early)%s", late, relays,
         sitesOk ? "" : " - unexpected bytes at a relay site");
    return late && relays;
}

void DetPatches_Apply()
{
    if (g_on || g_refused) return;
    for (size_t i = 0; i < PATCH_COUNT; i++) {
        if (!BytesAre(kPatches[i], kPatches[i].original)) {
            Logf("republicmp  DET_PATCHES: unexpected bytes at 0x%X (%s) - none applied", kPatches[i].rva,
                 kPatches[i].what);
            g_refused = true;
            return;
        }
    }
    for (size_t i = 0; i < PATCH_COUNT; i++) Write(g_exeBase + kPatches[i].rva, kPatches[i].patch, kPatches[i].size);
    g_populationLogged = false;
    g_populationFaults = 0;
    g_lastPopulation = -1;
    g_on = true;
    Logf("republicmp  DET_PATCHES: %d applied (vehicle visibility and placement, sign zones, follow camera, "
         "debug keys) + building visibility, saplings, population count, autosave hold", (int)PATCH_COUNT);
}

void DetPatches_Tick()
{
    if (!g_on || Determinism_IsActive()) return;
    g_on = false;
    for (size_t i = 0; i < PATCH_COUNT; i++)
        if (BytesAre(kPatches[i], kPatches[i].patch)) Write(g_exeBase + kPatches[i].rva, kPatches[i].original, kPatches[i].size);
    AcquireSRWLockExclusive(&g_saplingLock);
    g_saplingQueue.clear(); // the loads that follow rebuild the forest from the list anyway
    ReleaseSRWLockExclusive(&g_saplingLock);
    Logf("republicmp  DET_PATCHES: restored");
}
