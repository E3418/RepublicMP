// See determinism.h.
#include "determinism.h"
#include "state_hash.h"
#include "../game/save_load.h"
#include "../session/game_speed.h"
#include "../session/save_sync.h" // SaveSync_IsBusy - no probe during a session
#include <intrin.h>           // _ReturnAddress
#include <math.h>
#include <stdio.h>
#include <unordered_map>

#pragma intrinsic(_ReturnAddress)

#define GAME_TIMER_RVA 0x9D4EE0
#define SIM_STEP_RVA 0x30C790   // FUN_14030c790(world): one simulation step
#define LOAD_WORLD_RVA 0x430FC0 // FUN_140430fc0(): loads the save named in the load globals

#define DLL_C3D "C3DDLL64.dll"
#define DLL_CRT_UTILITY "api-ms-win-crt-utility-l1-1-0.dll"
#define DLL_CRT_MATH "api-ms-win-crt-math-l1-1-0.dll"

#define PROBE_SAVE_NAME "mp_probe"
#define PROBE_MAX_SAMPLES 4096
#define PROBE_SEED 0x5EED5EED12345678ull

// Prologues of the two inline-hooked functions (SOVIET64.exe, PE 6A3EB6AD):
// mov rax,rsp / push rdi / sub rsp,60h / mov [rax+10h],rbx / [rax+18h],rbp / [rax+20h],rsi
static const unsigned char kSimStepExpect[20] = {
    0x48, 0x8B, 0xC4, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x89,
    0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20,
};
// push rbp / push r12..r15 / lea rbp,[rsp-0C800h]
static const unsigned char kLoadWorldExpect[18] = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xAC, 0x24, 0x00, 0x38, 0xFF, 0xFF,
};

void Determinism_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static volatile bool g_active = false;
static volatile LONG g_epoch = 1;
static unsigned long long g_seed = 0;
static volatile unsigned long long g_stepCount = 0;
static StepObserverFn g_stepObservers[DETERMINISM_MAX_OBSERVERS] = {};
static int g_stepObserverCount = 0;
static bool g_mathRedirected = false;

// ---------------------------------------------------------------------------
// Call-site generator
// ---------------------------------------------------------------------------

struct SiteCounters {
    LONG epoch;
    std::unordered_map<unsigned, unsigned> calls;
};
static thread_local SiteCounters* t_sites = nullptr;

static inline unsigned long long SplitMix(unsigned long long x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// 0..32767, like the CRT's rand(). site = return address of the game's call.
static int SiteRand(void* returnAddress)
{
    unsigned site = (unsigned)((BYTE*)returnAddress - g_exeBase);
    if (!t_sites) t_sites = new SiteCounters{ 0, {} };
    LONG epoch = g_epoch;
    if (t_sites->epoch != epoch) {
        t_sites->calls.clear();
        t_sites->epoch = epoch;
    }
    unsigned n = t_sites->calls[site]++;
    unsigned long long x = SplitMix(g_seed ^ ((unsigned long long)site << 32) ^ n);
    return (int)((x >> 33) & 0x7FFF);
}

typedef int (__cdecl* RandFn)(void);
typedef void (__cdecl* SrandFn)(unsigned);
typedef unsigned (__cdecl* RandIntFn)(unsigned);
typedef float (__cdecl* RandFloatFn)(float, float);
typedef double (__cdecl* RandDoubleFn)(double, double);
typedef void (__cdecl* RandStartFn)(void);
static RandFn o_rand;
static SrandFn o_srand;
static RandIntFn o_randInt;
static RandFloatFn o_randFloat;
static RandDoubleFn o_randDouble;
static RandStartFn o_randStart;

// Same arithmetic as the engine's C3DRandom_* (C3DDLL64 0x77f10..0x77f80):
// Int = rand() % n, Float = rand() / 32767.0f * (b - a) + a, Double alike.
static __declspec(noinline) int __cdecl H_rand(void)
{
    if (!g_active) return o_rand();
    return SiteRand(_ReturnAddress());
}
static __declspec(noinline) void __cdecl H_srand(unsigned seed)
{
    if (!g_active) o_srand(seed);
}
static __declspec(noinline) unsigned __cdecl H_randInt(unsigned n)
{
    if (!g_active) return o_randInt(n);
    return (unsigned)SiteRand(_ReturnAddress()) % n;
}
static __declspec(noinline) float __cdecl H_randFloat(float a, float b)
{
    if (!g_active) return o_randFloat(a, b);
    return ((float)SiteRand(_ReturnAddress()) / 32767.0f) * (b - a) + a;
}
static __declspec(noinline) double __cdecl H_randDouble(double a, double b)
{
    if (!g_active) return o_randDouble(a, b);
    return ((double)SiteRand(_ReturnAddress()) / 32767.0) * (b - a) + a;
}
static __declspec(noinline) void __cdecl H_randStart(void)
{
    if (!g_active) o_randStart();
}

// ---------------------------------------------------------------------------
// Fixed timer
// ---------------------------------------------------------------------------

typedef void (*TimerStopFn)(void* timer);
static TimerStopFn o_timerStop;

static void H_timerStop(void* timer)
{
    o_timerStop(timer);
    if (g_active && (BYTE*)timer == g_exeBase + GAME_TIMER_RVA) {
        *(float*)timer = DETERMINISM_FIXED_FPS;
        *(float*)((BYTE*)timer + 4) = DETERMINISM_FIXED_FPS;
        *(int*)((BYTE*)timer + 8) = (int)DETERMINISM_FIXED_FPS;
    }
}

// ---------------------------------------------------------------------------
// Threads and local input inside the simulation
// ---------------------------------------------------------------------------

// RVA of the function containing `addr` (follows chained unwind entries back
// to the primary one); 0 if it is not in the exe.
static DWORD ExeFunctionRva(void* addr)
{
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)addr, &imageBase, nullptr);
    if (!rf || (BYTE*)imageBase != g_exeBase) return 0;
    for (int i = 0; i < 8; i++) {
        BYTE* info = (BYTE*)imageBase + rf->UnwindData;
        const BYTE kChainInfo = 0x4; // UNW_FLAG_CHAININFO
        if (!((info[0] >> 3) & kChainInfo)) break;
        BYTE codes = info[2];
        rf = (PRUNTIME_FUNCTION)(info + 4 + ((codes + 1) & ~1) * 2);
    }
    return rf->BeginAddress;
}

// FUN_140139a70 decides from a held key and the real FPS whether all objects
// update this step or a third of them with a tripled step; FUN_140699420
// also reads a key. Under the layer both see "not pressed".
#define KEY_SIM_FUNC1_RVA 0x139A70
#define KEY_SIM_FUNC2_RVA 0x699420

typedef bool (*GetKeyDownFn)(void* input, int key);
static GetKeyDownFn o_getKeyDown;

static __declspec(noinline) bool H_getKeyDown(void* input, int key)
{
    if (g_active) {
        DWORD fn = ExeFunctionRva(_ReturnAddress());
        if (fn == KEY_SIM_FUNC1_RVA || fn == KEY_SIM_FUNC2_RVA) return false;
    }
    return o_getKeyDown(input, key);
}

// The off-road pathfinder (FUN_1405d0ae0, started by FUN_1406e1930 through
// _beginthread) hands its route back on whichever step it finishes. Under the
// layer it runs to completion inside the call, so the vehicle picks the route
// up on the next step everywhere. Its only other effect is the handle the
// caller stores - a permanently signalled event stands in for it.
typedef uintptr_t (__cdecl* BeginThreadFn)(void (__cdecl*)(void*), unsigned, void*);
static BeginThreadFn o_beginThread;
static HANDLE g_doneThreadHandle = nullptr;

static uintptr_t __cdecl H_beginThread(void (__cdecl* start)(void*), unsigned stackSize, void* arg)
{
    if (!g_active) return o_beginThread(start, stackSize, arg);
    start(arg);
    return (uintptr_t)g_doneThreadHandle;
}

// The "Filling" worker (FUN_1402593d0) runs alongside the rest of the
// simulation step in FUN_14030c790 and both modify vehicles and citizens.
// Under the layer the main thread waits for the worker right after starting
// it, so the two never overlap and always run in the same order.
#define FILLING_OBJ_RVA 0x995290       // DAT_140995290: the worker's object
#define FILLING_DONE_OFFSET 0x59       // byte: 1 when the step's work is finished
#define FILLING_START_EVENT_OFFSET 0x60

typedef BOOL (WINAPI* SetEventFn)(HANDLE);
static SetEventFn o_setEvent;

static BOOL WINAPI H_setEvent(HANDLE h)
{
    BOOL r = o_setEvent(h);
    if (g_active) {
        BYTE* obj = *(BYTE**)(g_exeBase + FILLING_OBJ_RVA);
        if (obj && h == *(HANDLE*)(obj + FILLING_START_EVENT_OFFSET)) {
            volatile BYTE* done = obj + FILLING_DONE_OFFSET;
            while (!*done) YieldProcessor();
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

struct MathImport { const char* name; void* impl; };

// This DLL's static CRT (/MT): identical code on every machine.
static const MathImport kMath[] = {
    { "sin",    (void*)static_cast<double (*)(double)>(&sin) },
    { "cos",    (void*)static_cast<double (*)(double)>(&cos) },
    { "acos",   (void*)static_cast<double (*)(double)>(&acos) },
    { "log",    (void*)static_cast<double (*)(double)>(&log) },
    { "floor",  (void*)static_cast<double (*)(double)>(&floor) },
    { "sqrt",   (void*)static_cast<double (*)(double)>(&sqrt) },
    { "pow",    (void*)static_cast<double (*)(double, double)>(&pow) },
    { "fmod",   (void*)static_cast<double (*)(double, double)>(&fmod) },
    { "sinf",   (void*)static_cast<float (*)(float)>(&sinf) },
    { "cosf",   (void*)static_cast<float (*)(float)>(&cosf) },
    { "tanf",   (void*)static_cast<float (*)(float)>(&tanf) },
    { "asinf",  (void*)static_cast<float (*)(float)>(&asinf) },
    { "acosf",  (void*)static_cast<float (*)(float)>(&acosf) },
    { "atanf",  (void*)static_cast<float (*)(float)>(&atanf) },
    { "sqrtf",  (void*)static_cast<float (*)(float)>(&sqrtf) },
    { "floorf", (void*)static_cast<float (*)(float)>(&floorf) },
    { "ceilf",  (void*)static_cast<float (*)(float)>(&ceilf) },
    { "expf",   (void*)static_cast<float (*)(float)>(&expf) },
    { "log10f", (void*)static_cast<float (*)(float)>(&log10f) },
    { "atan2f", (void*)static_cast<float (*)(float, float)>(&atan2f) },
    { "powf",   (void*)static_cast<float (*)(float, float)>(&powf) },
    { "fmodf",  (void*)static_cast<float (*)(float, float)>(&fmodf) },
    { "lroundf", (void*)static_cast<long (*)(float)>(&lroundf) },
};

// Once, on first activation: both machines then run the same math code.
static void RedirectMath()
{
    if (g_mathRedirected) return;
    g_mathRedirected = true;
    _set_FMA3_enable(0);
    int patched = 0;
    for (const MathImport& m : kMath) {
        void* orig = nullptr;
        char label[48];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "math %s", m.name);
        if (FindIatSlot(g_exe, DLL_CRT_MATH, m.name) && PatchIat(g_exe, DLL_CRT_MATH, m.name, m.impl, &orig, label))
            patched++;
        if (g_engine && FindIatSlot(g_engine, DLL_CRT_MATH, m.name) &&
            PatchIat(g_engine, DLL_CRT_MATH, m.name, m.impl, &orig, label))
            patched++;
    }
    Logf("republicmp  DETERMINISM: %d math imports redirected to the built-in CRT (FMA3 off)", patched);
}

// ---------------------------------------------------------------------------
// Step and load hooks, probe state
// ---------------------------------------------------------------------------

typedef void (*SimStepFn)(__int64 world);
typedef void (*LoadWorldFn)(void);
static SimStepFn o_simStep;
static LoadWorldFn o_loadWorld;

enum ProbeState { PROBE_IDLE = 0, PROBE_WAIT_LOAD, PROBE_RUNNING, PROBE_RUN_DONE };
static volatile LONG g_probeState = PROBE_IDLE;
static volatile bool g_probeBusy = false;
static int g_probeSteps = 0, g_probeInterval = 60, g_probeRun = 0;
static int g_probeSpeed = 1;
static unsigned long long g_runStep = 0;

struct ProbeSample {
    unsigned long long step;
    StateSummary s;
};
static ProbeSample g_samples[2][PROBE_MAX_SAMPLES];
static int g_sampleCount[2];

static CRITICAL_SECTION g_statusLock;
static char g_probeStatus[320] = "not run yet";

static void SetProbeStatus(const char* fmt, ...)
{
    char body[320];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_statusLock);
    strcpy_s(g_probeStatus, body);
    LeaveCriticalSection(&g_statusLock);
    Logf("republicmp  PROBE: %s", body);
}

static void HookedLoadWorld(void)
{
    // A probe run starts its generator epoch exactly at the load, so the
    // random numbers the load itself draws are the same in both runs.
    if (g_probeState == PROBE_WAIT_LOAD) {
        Determinism_Restart();
        g_runStep = 0;
    }
    o_loadWorld();
    if (g_probeState == PROBE_WAIT_LOAD) {
        g_runStep = 0;
        InterlockedExchange(&g_probeState, PROBE_RUNNING);
    }
}

static void RecordSample()
{
    int run = g_probeRun;
    if (g_sampleCount[run] >= PROBE_MAX_SAMPLES) return;
    ProbeSample& p = g_samples[run][g_sampleCount[run]];
    p.step = g_runStep;
    if (StateHash_Compute(&p.s)) g_sampleCount[run]++;
}

static void HookedSimStep(__int64 world)
{
    o_simStep(world);
    for (int i = 0; i < g_stepObserverCount; i++) g_stepObservers[i](); // also without the layer
    if (!g_active) return;
    g_stepCount++;
    if (g_probeState == PROBE_RUNNING) {
        if (g_runStep == 0) WriteGameSpeed(g_probeSpeed); // same speed in both runs from here on
        g_runStep++;
        if (g_runStep % g_probeInterval == 0) RecordSample();
        if (g_runStep >= (unsigned long long)g_probeSteps) {
            WriteGameSpeed(0);
            InterlockedExchange(&g_probeState, PROBE_RUN_DONE);
        }
    }
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

bool Determinism_Install()
{
    InitializeCriticalSection(&g_statusLock);
    int ok = 0;
    ok += PatchIat(g_exe, DLL_CRT_UTILITY, "rand", (void*)&H_rand, (void**)&o_rand, "det rand");
    ok += PatchIat(g_exe, DLL_CRT_UTILITY, "srand", (void*)&H_srand, (void**)&o_srand, "det srand");
    ok += PatchIat(g_exe, DLL_C3D, "?C3DRandom_Int@@YAII@Z", (void*)&H_randInt, (void**)&o_randInt, "det C3DRandom_Int");
    ok += PatchIat(g_exe, DLL_C3D, "?C3DRandom_Float@@YAMMM@Z", (void*)&H_randFloat, (void**)&o_randFloat, "det C3DRandom_Float");
    ok += PatchIat(g_exe, DLL_C3D, "?C3DRandom_Double@@YANNN@Z", (void*)&H_randDouble, (void**)&o_randDouble, "det C3DRandom_Double");
    ok += PatchIat(g_exe, DLL_C3D, "?C3DRandom_Start@@YAXXZ", (void*)&H_randStart, (void**)&o_randStart, "det C3DRandom_Start");
    ok += PatchIat(g_exe, DLL_C3D, "?Stop@C3D_TIMER@@QEAAXXZ", (void*)&H_timerStop, (void**)&o_timerStop, "det timer Stop");
    ok += PatchIat(g_exe, DLL_C3D, "?GetKeyDown@C3D_INPUT@@QEAA_NH@Z", (void*)&H_getKeyDown, (void**)&o_getKeyDown, "det GetKeyDown");
    g_doneThreadHandle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    ok += PatchIat(g_exe, "api-ms-win-crt-runtime-l1-1-0.dll", "_beginthread", (void*)&H_beginThread, (void**)&o_beginThread, "det _beginthread");
    ok += PatchIat(g_exe, "KERNEL32.dll", "SetEvent", (void*)&H_setEvent, (void**)&o_setEvent, "det SetEvent");
    bool step = InstallInlineHook(g_exeBase + SIM_STEP_RVA, (void*)&HookedSimStep, (void**)&o_simStep,
                                  kSimStepExpect, sizeof(kSimStepExpect), "republicmp sim step");
    bool load = InstallInlineHook(g_exeBase + LOAD_WORLD_RVA, (void*)&HookedLoadWorld, (void**)&o_loadWorld,
                                  kLoadWorldExpect, sizeof(kLoadWorldExpect), "republicmp load world");
    Logf("republicmp  DETERMINISM: %d/10 imports redirected, sim step hook %d, load hook %d", ok, step, load);
    return ok == 10 && step && load;
}

void Determinism_SetActive(bool active, unsigned long long seed)
{
    if (active) {
        RedirectMath();
        g_seed = seed;
        g_stepCount = 0;
        Determinism_Restart();
    }
    g_active = active;
    Logf("republicmp  DETERMINISM: %s (seed %016llx)", active ? "on" : "off", seed);
}

bool Determinism_IsActive() { return g_active; }
void Determinism_Restart() { InterlockedIncrement(&g_epoch); }
unsigned long long Determinism_StepCount() { return g_stepCount; }
void Determinism_AddStepObserver(StepObserverFn fn)
{
    if (g_stepObserverCount < DETERMINISM_MAX_OBSERVERS) g_stepObservers[g_stepObserverCount++] = fn;
}

// ---------------------------------------------------------------------------
// Probe runner (worker thread)
// ---------------------------------------------------------------------------

static bool GetSaveDir(char* out, size_t outSize, const char* name)
{
    char exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(g_exe, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* slash = strrchr(exePath, '\\');
    if (!slash) return false;
    *slash = 0;
    return _snprintf_s(out, outSize, _TRUNCATE, "%s\\media_soviet\\save\\%s", exePath, name) >= 0;
}

static void WriteRunLog(int run)
{
    char dir[MAX_PATH], path[MAX_PATH];
    if (!GetSaveDir(dir, sizeof(dir), PROBE_SAVE_NAME)) return;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\republicmp_probe_run%d.txt", dir, run + 1);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return;
    fprintf(f, "step day year progress buildings vehicles citizens storage cargo production clock bld veh cit\n");
    for (int i = 0; i < g_sampleCount[run]; i++) {
        const ProbeSample& p = g_samples[run][i];
        fprintf(f, "%llu %d %d %.6f %d %d %d %.3f %.3f %.3f %016llx %016llx %016llx %016llx\n", p.step, p.s.day,
                p.s.year, p.s.dayProgress, p.s.buildings, p.s.vehicles, p.s.citizens, p.s.storageTotal,
                p.s.cargoTotal, p.s.productionTotal, p.s.hashClock, p.s.hashBuildings, p.s.hashVehicles,
                p.s.hashCitizens);
    }
    fclose(f);
}

static void CompareRuns()
{
    int n = g_sampleCount[0] < g_sampleCount[1] ? g_sampleCount[0] : g_sampleCount[1];
    for (int i = 0; i < n; i++) {
        const StateSummary& a = g_samples[0][i].s;
        const StateSummary& b = g_samples[1][i].s;
        if (StateHash_All(a) == StateHash_All(b)) continue;
        char parts[96] = "";
        if (a.hashClock != b.hashClock) strcat_s(parts, "clock/money ");
        if (a.hashBuildings != b.hashBuildings) strcat_s(parts, "buildings ");
        if (a.hashVehicles != b.hashVehicles) strcat_s(parts, "vehicles ");
        if (a.hashCitizens != b.hashCitizens) strcat_s(parts, "citizens ");
        SetProbeStatus("DIFFERENT from step %llu (sample %d/%d): %s- vehicles %d/%d, citizens %d/%d, storage %.1f/%.1f",
                       g_samples[0][i].step, i + 1, n, parts, a.vehicles, b.vehicles, a.citizens, b.citizens,
                       a.storageTotal, b.storageTotal);
        return;
    }
    if (n == 0) SetProbeStatus("no samples recorded - was the world loaded?");
    else SetProbeStatus("IDENTICAL: both runs match at all %d samples (%llu steps)", n, g_samples[0][n - 1].step);
}

static bool WaitState(LONG want, DWORD timeoutMs)
{
    ULONGLONG start = GetTickCount64();
    while (g_probeState != want) {
        if (GetTickCount64() - start > timeoutMs) return false;
        Sleep(50);
    }
    return true;
}

static DWORD WINAPI ProbeThread(LPVOID)
{
    do {
        char dir[MAX_PATH];
        if (!GetSaveDir(dir, sizeof(dir), PROBE_SAVE_NAME)) { SetProbeStatus("couldn't build the save path"); break; }
        CreateDirectoryA(dir, nullptr);
        SetProbeStatus("saving the current game as '%s'...", PROBE_SAVE_NAME);
        TriggerSaveGame(PROBE_SAVE_NAME);
        if (!WaitForSaveToFinish(120000)) { SetProbeStatus("saving timed out"); break; }

        // world+0xC turns on a pass sorted by distance to the local camera
        // (FUN_1400f7930 from FUN_140139a70) - worth knowing if runs differ.
        Logf("republicmp  PROBE: world+0xC = %d", (int)*(BYTE*)(g_exeBase + 0x9D4F10 + 0xC));
        Determinism_SetActive(true, PROBE_SEED);
        bool failed = false;
        for (g_probeRun = 0; g_probeRun < 2 && !failed; g_probeRun++) {
            g_sampleCount[g_probeRun] = 0;
            SetProbeStatus("run %d/2: loading '%s'...", g_probeRun + 1, PROBE_SAVE_NAME);
            InterlockedExchange(&g_probeState, PROBE_WAIT_LOAD);
            TriggerLoadSave(PROBE_SAVE_NAME);
            if (!WaitState(PROBE_RUNNING, 300000)) { SetProbeStatus("run %d: the load never happened", g_probeRun + 1); failed = true; break; }
            SetProbeStatus("run %d/2: simulating %d steps - please don't touch anything", g_probeRun + 1, g_probeSteps);
            // Generous: the steps run at the game's own frame rate.
            if (!WaitState(PROBE_RUN_DONE, (DWORD)g_probeSteps * 1000 + 60000)) {
                SetProbeStatus("run %d: timed out after %llu steps", g_probeRun + 1, g_runStep);
                failed = true;
                break;
            }
            WriteRunLog(g_probeRun);
        }
        InterlockedExchange(&g_probeState, PROBE_IDLE);
        Determinism_SetActive(false, 0);
        if (!failed) CompareRuns();
    } while (false);
    InterlockedExchange(&g_probeState, PROBE_IDLE);
    if (g_active) Determinism_SetActive(false, 0);
    g_probeBusy = false;
    return 0;
}

bool Probe_Start(int steps, int interval)
{
    if (g_probeBusy || SaveSync_IsBusy()) return false;
    if (steps < interval || interval < 1 || steps / interval > PROBE_MAX_SAMPLES) return false;
    StateSummary s;
    if (!StateHash_Compute(&s)) {
        SetProbeStatus("load a game first");
        return false;
    }
    g_probeBusy = true;
    g_probeSteps = steps;
    g_probeInterval = interval;
    g_probeSpeed = ReadGameSpeed() > 0 ? ReadGameSpeed() : 1;
    CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
    return true;
}

bool Probe_IsRunning() { return g_probeBusy; }

const char* Probe_Status()
{
    static char copy[320];
    EnterCriticalSection(&g_statusLock);
    strcpy_s(copy, g_probeStatus);
    LeaveCriticalSection(&g_statusLock);
    return copy;
}
