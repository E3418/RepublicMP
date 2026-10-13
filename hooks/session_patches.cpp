// See session_patches.h.
#include "session_patches.h"
#include "../net/build_sync.h" // BuildSync_IsConnected
#include "determinism.h"        // Determinism_IsActive (the probe)
#include <string.h>
#include <limits.h>

struct SessionPatch {
    unsigned int rva;
    unsigned char size;
    unsigned char original[8];
    unsigned char patch[8];
    const char* what;
};

#define CITIZEN_INIT_RVA 0x8244f0    // FUN_1408244f0(owner, citizen): sets a citizen's +18 if not above 0
#define CITIZEN_INIT_GATED_CALL 0x2fd517 // its call in FUN_1402fd2e0 (Late body and interface)

// The last entry's patch (a call to the gate's relay) is filled in at Apply.
static SessionPatch kPatches[] = {
    // jne +15 -> 2-byte nop: always slow the timer by MultiplyFPS(1/3)
    { 0x13cb5b, 2, { 0x75, 0x15 }, { 0x66, 0x90 }, "building pass: frame-rate test, timer slowdown" },
    // jne +3a -> nop: always update only this pass's third of the buildings
    { 0x13dbc1, 2, { 0x75, 0x3a }, { 0x66, 0x90 }, "building pass: frame-rate test, building third" },
    // jne +15 -> nop: always restore the timer at the end
    { 0x13e594, 2, { 0x75, 0x15 }, { 0x66, 0x90 }, "building pass: frame-rate test, timer restore" },
    // movzx ecx,byte [rax+178] -> xor ecx,ecx; nop5
    { 0x13d963, 7, { 0x0f, 0xb6, 0x88, 0x78, 0x01, 0x00, 0x00 }, { 0x31, 0xc9, 0x0f, 0x1f, 0x44, 0x00, 0x00 },
      "construction site: camera visibility" },
    // Citizen init called from interface code -> nop5
    { 0x4a6aa6, 5, { 0xe8, 0x45, 0xda, 0x37, 0x00 }, { 0x0f, 0x1f, 0x44, 0x00, 0x00 }, "citizen init: building overlay" },
    { 0x7645ae, 5, { 0xe8, 0x3d, 0xff, 0x0b, 0x00 }, { 0x0f, 0x1f, 0x44, 0x00, 0x00 }, "citizen init: window 761300" },
    { 0x7bb8b3, 5, { 0xe8, 0x38, 0x8c, 0x06, 0x00 }, { 0x0f, 0x1f, 0x44, 0x00, 0x00 }, "citizen init: window 7bada0" },
    { 0x8243c2, 5, { 0xe8, 0x29, 0x01, 0x00, 0x00 }, { 0x0f, 0x1f, 0x44, 0x00, 0x00 }, "citizen init: getter 8243b0" },
    // The call shared by Late and the interface -> the gate's relay (simulation thread only)
    { CITIZEN_INIT_GATED_CALL, 5, { 0xe8, 0xd4, 0x6f, 0x52, 0x00 }, { 0 }, "citizen init: Late / interface" },
};
#define PATCH_COUNT (sizeof(kPatches) / sizeof(kPatches[0]))

static bool g_applied = false;
static bool g_refused = false; // the bytes didn't match - never tried again

void SessionPatches_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ---------------------------------------------------------------------------
// Citizen init gate
// ---------------------------------------------------------------------------
//
// A new citizen's +18 is set by FUN_1408244f0 the first time someone needs it:
// the citizen update (simulation), but also the interface on the render
// thread (an overlay, two windows, a getter) - so it got its value on
// whichever PC's interface reached it first, at different times. The four
// interface-only calls are nops in a session (they then read the value the
// simulation set, or 0), and the call shared by the Late body and the
// interface goes through a gate that lets only the simulation thread in.
// (by SoylentGreed)

typedef void (*CitizenInitFn)(void* owner, void* citizen);
static volatile DWORD g_simThreadId = 0;
static BYTE* g_gateRelay = nullptr;

static void CitizenInitGate(void* owner, void* citizen)
{
    if (GetCurrentThreadId() == g_simThreadId)
        ((CitizenInitFn)(g_exeBase + CITIZEN_INIT_RVA))(owner, citizen);
}

static void OnSimStep()
{
    g_simThreadId = GetCurrentThreadId(); // the main thread, which runs Early and Late
}

// jmp [rip+0] / the gate's address, near the exe so a rel32 call reaches it.
static bool PrepareGate()
{
    if (g_gateRelay) return true;
    static bool s_observing = false;
    if (!s_observing) {
        Determinism_AddStepObserver(OnSimStep);
        s_observing = true;
    }
    BYTE* relay = AllocNear(g_exeBase + CITIZEN_INIT_GATED_CALL, 4096);
    if (!relay) return false;
    long long rel = (long long)relay - (long long)(g_exeBase + CITIZEN_INIT_GATED_CALL + 5);
    if (rel < INT_MIN || rel > INT_MAX) return false;
    DWORD old;
    if (!VirtualProtect(relay, 16, PAGE_EXECUTE_READWRITE, &old)) return false;
    unsigned long long to = (unsigned long long)&CitizenInitGate;
    relay[0] = 0xff; relay[1] = 0x25; relay[2] = relay[3] = relay[4] = relay[5] = 0;
    memcpy(relay + 6, &to, 8);
    VirtualProtect(relay, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, 16);
    SessionPatch& p = kPatches[sizeof(kPatches) / sizeof(kPatches[0]) - 1];
    int rel32 = (int)rel;
    p.patch[0] = 0xe8;
    memcpy(p.patch + 1, &rel32, 4);
    g_gateRelay = relay;
    return true;
}

static bool BytesAre(const SessionPatch& p, const unsigned char* bytes)
{
    __try {
        return memcmp(g_exeBase + p.rva, bytes, p.size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool Write(const SessionPatch& p, const unsigned char* bytes)
{
    BYTE* at = g_exeBase + p.rva;
    DWORD old;
    if (!VirtualProtect(at, p.size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, p.size);
    VirtualProtect(at, p.size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, p.size);
    return true;
}

static void Apply()
{
    if (!PrepareGate()) {
        Logf("republicmp  SESSION_PATCHES: no relay page near the executable - none applied");
        g_refused = true;
        return;
    }
    for (size_t i = 0; i < PATCH_COUNT; i++) {
        if (!BytesAre(kPatches[i], kPatches[i].original)) {
            Logf("republicmp  SESSION_PATCHES: unexpected bytes at 0x%X (%s) - none applied",
                 kPatches[i].rva, kPatches[i].what);
            g_refused = true;
            return;
        }
    }
    for (size_t i = 0; i < PATCH_COUNT; i++) Write(kPatches[i], kPatches[i].patch);
    g_applied = true;
    Logf("republicmp  SESSION_PATCHES: %d applied (frame-rate tests, construction-site camera, citizen init)", (int)PATCH_COUNT);
}

static void Restore()
{
    for (size_t i = 0; i < PATCH_COUNT; i++)
        if (BytesAre(kPatches[i], kPatches[i].patch)) Write(kPatches[i], kPatches[i].original);
    g_applied = false;
    Logf("republicmp  SESSION_PATCHES: restored");
}

void SessionPatches_Tick()
{
    bool want = BuildSync_IsConnected() || Determinism_IsActive();
    if (want && !g_applied && !g_refused) Apply();
    else if (!want && g_applied) Restore();
}
