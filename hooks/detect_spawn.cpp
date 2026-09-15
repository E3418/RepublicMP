// Detects real player-placed buildings via the single universal placement
// hook, plus a debug synthetic spawn (F9) and a heartbeat log.
#include "detect_spawn.h"
#include "present_hook.h"
#include "game_speed.h"
#include "window_focus.h"
#include "../net/build_sync.h"
#include "../net/road_sync.h"
#include "../ui/sync_block.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <winnt.h>

#pragma comment(lib, "User32.lib")

void DetectSpawn_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// FUN_1402bade0 fires for every building placement (and cursor preview).
// We don't trust the call itself - we check whether the city-objects
// vector SIZE grew during the call. Size must be (end-begin)/pointer size,
// not a raw address diff (a mid-call reallocation breaks that).
#define VEC_BUILDINGS_END_RVA 0x9E6A20
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18

const unsigned char kUniversalExpect[21] = {
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xD8,
    0x53, 0xFF, 0xFF
};

UniversalFn g_origUniversal = nullptr;

static volatile long long g_universalCallCount = 0;
static volatile long long g_universalMultiPushCount = 0;

DWORD WINAPI HeartbeatThread(LPVOID)
{
    long long lastUniversalCalls = 0;
    long long lastPresentCalls = 0;
    for (;;) {
        Sleep(2000);
        long long curUniversalCalls = g_universalCallCount;
        long long curPresentCalls = g_presentCallCount;
        int curSpeed = ReadGameSpeed();
        int curSpeedSaved = ReadGameSpeedSaved();
        Logf("republicmp  heartbeat: universalCalls total=%lld (+%lld) universalMultiPush total=%lld presentCalls total=%lld (+%lld) gameSpeed=%d gameSpeedSaved=%d",
             curUniversalCalls, curUniversalCalls - lastUniversalCalls,
             g_universalMultiPushCount,
             curPresentCalls, curPresentCalls - lastPresentCalls,
             curSpeed, curSpeedSaved);
        lastUniversalCalls = curUniversalCalls;
        lastPresentCalls = curPresentCalls;
    }
    return 0;
}

// param_2 points directly at the ASCII building-type name.
static void SafeReadCStringDirect(const char* ptr, char* outBuf, size_t outBufSize)
{
    outBuf[0] = 0;
    __try {
        if (!ptr) {
            strncpy(outBuf, "<null>", outBufSize - 1);
            return;
        }
        size_t len = 0;
        while (len < outBufSize - 1) {
            char c = ptr[len];
            if (c == 0) break;
            if (c < 0x20 || c >= 0x7F) {
                strncpy(outBuf, "<non-ascii>", outBufSize - 1);
                return;
            }
            len++;
        }
        strncpy(outBuf, ptr, len);
        outBuf[len] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strncpy(outBuf, "<crash reading param_2>", outBufSize - 1);
    }
}

static bool LooksLikeValidType(const char* s)
{
    return strcmp(s, "<null>") != 0 &&
           strcmp(s, "<non-ascii>") != 0 &&
           strcmp(s, "<crash reading param_2>") != 0 &&
           strlen(s) >= 2;
}

// Patching the finished object's sin/cos fields AFTER g_origUniversal
// creates it (net/build_sync.cpp's ApplyOneRemotePlacement) fixes the
// VISUAL rotation, but the engine's own placement logic (collision test,
// road/pipe/wire auto-connection, path generation) already ran during that
// call using whatever DEFAULT orientation it placed the object at -
// patching afterwards can't retroactively fix any of that. The real INPUT
// field is needed instead - same idea as the known x/y/z cursor fields
// (param_1+0xf6c/0xf70/0xf74, DEBUG_SPAWN_OFFSET_* below) that get read
// INTO the object when g_origUniversal runs. param_3..param_8 are not it
// (see HookedUniversal below), so rotation must be cursor/thisPtr state set
// some other way (mouse-drag/R-key rotation while the placement ghost is
// up) - this dumps a wide range around the known x/y/z cluster on param_1,
// taken right BEFORE calling g_origUniversal on a real click (so it
// reflects whatever the player had actually set), to diff against the
// post-creation sin/cos (logged after the call) across several
// differently-rotated placements.
static void LogCursorRotHexDump(__int64 thisPtr)
{
    unsigned char* bytes = (unsigned char*)thisPtr;
    for (int row = 0; row < 10; row++) {
        int base = 0xf20 + row * 16;
        char hexLine[3 * 16 + 1] = {0};
        for (int col = 0; col < 16; col++) {
            char piece[4];
            _snprintf_s(piece, sizeof(piece), _TRUNCATE, "%02X ", bytes[base + col]);
            strcat_s(hexLine, sizeof(hexLine), piece);
        }
        Logf("republicmp  ROTIN_DIAG: this+0x%03x: %s", base, hexLine);
    }
}

// net/build_sync.cpp's BuildSyncMsg carries x/y/z plus rotation (rotSin/
// rotCos) - a remote placement without it would land with whatever
// rotation g_origUniversal defaults to, not the rotation the placing
// player actually chose. A hexdump of obj+0x330..0x38f on a newly created
// building confirms obj+0x340/obj+0x348 are a sin/cos pair (sin^2+cos^2==1
// holds across sampled placements) - that's the yaw, atan2(sin,cos)
// recovers the angle. See HookedUniversal below for the read-side wiring.

// Sends the placement over the build-sync connection (net/build_sync.*), if
// one is currently active. No-op otherwise - useful without a second player.
static void NotifyBuildingPlaced(float x, float y, float z, float rotSin, float rotCos, const char* typeName)
{
    BuildSync_SendPlacement(x, y, z, rotSin, rotCos, typeName);
}

// Debug synthetic spawn config/offsets.
#define DEBUG_SPAWN_VK VK_F9
#define DEBUG_SPAWN_POLL_MS 50
#define DEBUG_SPAWN_OFFSET_X 0xf6c
#define DEBUG_SPAWN_OFFSET_Y 0xf70
#define DEBUG_SPAWN_OFFSET_Z 0xf74
#define DEBUG_SPAWN_OFFSET_VALIDATION 0x11a10
#define DEBUG_SPAWN_TEST_TYPE_NAME "residental_brick_1"
#define DEBUG_SPAWN_OFFSET_METERS_X 5.0f

static volatile LONG g_debugSpawnPending = 0;
static volatile LONG g_thisPtrDiagPending = 0; // F9 also requests a fresh RTTI_DIAG dump - see below

// Same fixed placement-manager address net/build_sync.cpp uses - see the
// comment there for how it was identified. Duplicated here rather than
// shared, per this codebase's existing convention (each .cpp keeps its own
// g_exe/g_exeBase copy already, see tesmio_plugin.h).
#define UNIVERSAL_THIS_RVA 0x9D4F10

static __int64 GetPlacementManagerThisPtr()
{
    return (__int64)((uintptr_t)g_exe + UNIVERSAL_THIS_RVA);
}

static bool IsWorldLoaded()
{
    void** vecBeginPtr = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
    void** vecEndPtr   = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
    __try {
        return (*vecBeginPtr != nullptr) && (*vecEndPtr != nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Runs on the game thread (called from DetectSpawn_Tick, off the Present
// hook) to avoid a game-state race. thisPtr comes from the fixed
// placement-manager address (see DetectSpawn_Tick below), not from a real
// HookedUniversal call.
static void RunDebugSpawn(__int64 thisPtr)
{
    __try {
        float* xField = (float*)(thisPtr + DEBUG_SPAWN_OFFSET_X);
        float* yField = (float*)(thisPtr + DEBUG_SPAWN_OFFSET_Y);
        float* zField = (float*)(thisPtr + DEBUG_SPAWN_OFFSET_Z);

        float savedX = *xField;
        float savedY = *yField;
        float savedZ = *zField;

        float testX = savedX + DEBUG_SPAWN_OFFSET_METERS_X;
        float testY = savedY;
        float testZ = savedZ;

        *xField = testX;
        *yField = testY;
        *zField = testZ;

        char testTypeName[] = DEBUG_SPAWN_TEST_TYPE_NAME;

        void** vecEndPtr   = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        void** vecBeginPtr = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        void* beginBefore = *vecBeginPtr;
        void* endBefore = *vecEndPtr;
        long long countBefore = (beginBefore && endBefore)
            ? (long long)(((uintptr_t)endBefore - (uintptr_t)beginBefore) / sizeof(void*))
            : -1;

        Logf("republicmp  DEBUG_SPAWN: calling FUN_1402bade0, this=%p type=[%s] x=%.2f y=%.2f z=%.2f",
             (void*)thisPtr, testTypeName, testX, testY, testZ);

        g_origUniversal(thisPtr, testTypeName, 0, 1, 0, 0xffffffff, 0, 1);

        int validationCode = *(int*)(thisPtr + DEBUG_SPAWN_OFFSET_VALIDATION);

        void* beginAfter = *vecBeginPtr;
        void* endAfter = *vecEndPtr;
        long long countAfter = (beginAfter && endAfter)
            ? (long long)(((uintptr_t)endAfter - (uintptr_t)beginAfter) / sizeof(void*))
            : -1;
        long long countDelta = (countBefore >= 0 && countAfter >= 0) ? (countAfter - countBefore) : 0;

        Logf("republicmp  DEBUG_SPAWN: param_1+0x11a10=%d after call, buildings vector %lld -> %lld (delta=%lld)",
             validationCode, countBefore, countAfter, countDelta);

        if (countDelta > 0) {
            void* newObjPtr = *((void**)endAfter - 1);
            if (newObjPtr) {
                float placedX = *(float*)((char*)newObjPtr + 0x350);
                float placedY = *(float*)((char*)newObjPtr + 0x354);
                float placedZ = *(float*)((char*)newObjPtr + 0x358);
                Logf("republicmp  DEBUG_SPAWN: SUCCESS obj=%p x=%.2f y=%.2f z=%.2f",
                     newObjPtr, placedX, placedY, placedZ);
            }
        } else {
            Logf("republicmp  DEBUG_SPAWN: no building created (code=%d)", validationCode);
        }

        *xField = savedX;
        *yField = savedY;
        *zField = savedZ;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  DEBUG_SPAWN: <exception during synthetic call>");
    }
}

DWORD WINAPI DebugSpawnThread(LPVOID)
{
    bool keyWasDown = false;
    for (;;) {
        Sleep(DEBUG_SPAWN_POLL_MS);

        bool keyIsDown = IsGameWindowFocused() && (GetAsyncKeyState(DEBUG_SPAWN_VK) & 0x8000) != 0;
        bool keyJustPressed = keyIsDown && (keyWasDown == false);
        keyWasDown = keyIsDown;

        if (keyJustPressed == false) {
            continue;
        }

        InterlockedExchange(&g_debugSpawnPending, 1);
        InterlockedExchange(&g_thisPtrDiagPending, 1);
        Logf("republicmp  DEBUG_SPAWN: request queued (F9)");
    }
    return 0;
}

// Diagnostic dump of the placement-manager object ("thisPtr"), triggered by
// F9 (piggybacking on the same key as the debug spawn - see
// g_thisPtrDiagPending above). Useful for solo testing: press F9 once you're
// in build mode (so param_1 is valid, same requirement as the debug spawn)
// and get a fresh dump on demand, as many times as you like.
//
// This object has no vtable at offset 0 (the first 8 bytes there are a
// small float + int that change between calls, not a stable pointer), so a
// standard RTTI walk doesn't apply. Instead, this scans the whole loaded
// SOVIET64.exe module's memory image for any 8-byte-aligned qword equal to
// thisPtr. If the game keeps this object's address in an ordinary static
// variable (like every other DAT_... global already used for save/load),
// that variable's own address shows up as a hit here - RVA = hit_address -
// module_base, directly usable as g_exe+RVA like everything else in this
// codebase. Uses VirtualQuery to skip any non-readable/guard pages instead
// of assuming the whole image is safely readable.
static void ScanForPointerToThis(__int64 thisPtr)
{
    BYTE* base = (BYTE*)g_exeBase;
    SIZE_T imageSize = 0;

    __try {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            Logf("republicmp  PTR_SCAN: bad DOS header at module base, aborting");
            return;
        }
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            Logf("republicmp  PTR_SCAN: bad NT header, aborting");
            return;
        }
        imageSize = nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  PTR_SCAN: <exception reading PE headers, aborting>");
        return;
    }

    Logf("republicmp  PTR_SCAN: scanning module base=%p size=0x%zx for qword == %p",
         (void*)base, imageSize, (void*)thisPtr);

    BYTE* end = base + imageSize;
    BYTE* cursor = base;
    int hits = 0;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0) {
            Logf("republicmp  PTR_SCAN: VirtualQuery failed at %p, stopping", (void*)cursor);
            break;
        }

        BYTE* regionEnd = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
        if (regionEnd > end) regionEnd = end;

        bool readable = (mbi.State == MEM_COMMIT) &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0 &&
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;

        if (readable) {
            __try {
                for (BYTE* p = cursor; p + 8 <= regionEnd; p += 8) {
                    if (*(__int64*)p == thisPtr) {
                        DWORD rva = (DWORD)(p - base);
                        Logf("republicmp  PTR_SCAN: hit at RVA=0x%x (addr=%p)", rva, (void*)p);
                        hits++;
                        if (hits >= 50) {
                            Logf("republicmp  PTR_SCAN: too many hits (>=50), stopping early");
                            cursor = end;
                            goto scanDone;
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Logf("republicmp  PTR_SCAN: <exception scanning region at %p, skipping rest of it>", (void*)cursor);
            }
        }

        cursor = regionEnd;
    }

scanDone:
    Logf("republicmp  PTR_SCAN: done, %d hit(s)", hits);
}

static void LogThisPtrDiagnostics(__int64 thisPtr)
{
    __try {
        Logf("republicmp  RTTI_DIAG: thisPtr=%p", (void*)thisPtr);

        unsigned char* bytes = (unsigned char*)thisPtr;
        for (int row = 0; row < 8; row++) {
            char hexLine[3 * 16 + 1] = {0};
            for (int col = 0; col < 16; col++) {
                char piece[4];
                _snprintf_s(piece, sizeof(piece), _TRUNCATE, "%02X ", bytes[row * 16 + col]);
                strcat_s(hexLine, sizeof(hexLine), piece);
            }
            Logf("republicmp  RTTI_DIAG: +0x%02x: %s", row * 16, hexLine);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  RTTI_DIAG: <exception during hexdump>");
    }

    ScanForPointerToThis(thisPtr);
}

// decompile_of_1403d00f0.c (road) and decompile_of_1403b0e60.c (pipe) - the
// per-tool wrapper functions the giant per-frame FUN_14030d1a0 calls while
// that tool is selected - both manage their drag (press-drag-release)
// state through fields on the SAME world god-object (thisPtr, same one
// everywhere else in this file), not on a separate object:
//   thisPtr+0xd2ac  - drag-active flag (bool), shared across every path tool
//   thisPtr+0xd2c4  - accumulated drag duration (float)
//   thisPtr+0x350a  - char* to the currently active tool's name (the same
//                     string FUN_14030d1a0 strcmp's against "road_asphalt"/
//                     "pipe_underground"/etc.)
// The actual waypoint list + committed type/variant, though, live on a
// separate per-tool "manager" object fetched off thisPtr: road's manager is
// *(void**)(thisPtr+0xf7a0), pipe's is *(void**)(thisPtr+0xf858) - both
// passed as the real param_1 into the shared engine primitives
// (FUN_1404fc040/FUN_1404fcaf0/FUN_140518a80) inside those two wrappers.
// FUN_140518a80 (the actual commit-to-world call) is shared with ~20
// unrelated call sites (including FUN_1402bade0, the building hook
// itself) - too broad/risky to inline-hook directly. Polling thisPtr's
// drag-active flag every frame (the same per-frame tick function used for
// F9) and dumping the manager's waypoint array on the 1->0 transition
// (drag just finished) gets the same answer with zero new hooks.
#define TOOL_DRAG_ACTIVE_OFFSET 0xd2ac
#define TOOL_DRAG_DURATION_OFFSET 0xd2c4
#define TOOL_NAME_PTR_OFFSET 0x350a
#define ROAD_MANAGER_OFFSET 0xf7a0
#define PIPE_MANAGER_OFFSET 0xf858
// Waypoint array + committed type/variant, read off the per-tool manager
// object (NOT thisPtr) - matches FUN_140518a80's own field reads.
#define TOOL_WAYPOINTS_BEGIN_OFFSET 0x410
#define TOOL_WAYPOINTS_END_OFFSET 0x418
#define TOOL_TYPE_OFFSET 0x528
#define TOOL_VARIANT_OFFSET 0x52c

static bool g_toolDragWasActive = false;
static char g_toolDragLastName[64] = "<never captured>";
static float g_toolDragLastDuration = 0.0f;

// The offsets match the "road_asphalt" -> FUN_1403d00f0(this,1,0) mapping
// in decompile_of_1403d00f0.c (ROAD's type=1/variant=0 lands on the live
// manager). Reading the manager at the 1->0 transition itself is always one
// frame too late: FUN_140518a80 (the commit call) clears the manager's own
// waypoint array (and the game clears thisPtr's active-tool-name pointer)
// as part of finishing, and that already happens on the frame BEFORE the
// poll below ever sees d2ac go back to 0 - by the time it looks, the data
// is gone. Fix: snapshot the manager + tool name EVERY frame while the drag
// is active (not just once at the end), so there is always a fresh copy
// from the frame just before the commit wiped it.
//
// FUN_1404fcaf0 (drag-tick) has an explicit "road curve" accumulator on the
// manager, driven by mouse wheel ("ZOOM" axis reused) or
// GAMEPAD_ROAD_CURVE_ADD/SUB - manager+0x48 is the current curve value
// (float), clamped every frame between manager+0x40 (min) and manager+0x44
// (max). The waypoint array isn't a raw mouse trail - it's a route computed
// each frame from the drag-start point to the current cursor point,
// constrained by this curve value. That's why the wire protocol sends
// (start, end, curveValue, typeId, variant) rather than the whole point
// array (see RoadSyncMsg in net/road_sync.h): the far side reproduces the
// identical path by re-running FUN_1404fc040->FUN_1404fcaf0->FUN_140518a80
// itself. curveMin/curveMax/curveValue are captured alongside the existing
// point snapshot below.
struct ToolWaypointSnapshot {
    bool valid;
    int typeId;
    int variant;
    long long count; // -1 = unreadable/garbage manager, -2 = read exception
    float points[64][3];
    float curveMin;
    float curveMax;
    float curveValue;
};
static ToolWaypointSnapshot g_lastRoadSnapshot = {};
static ToolWaypointSnapshot g_lastPipeSnapshot = {};

// g_lastRoadSnapshot/g_lastPipeSnapshot above get overwritten EVERY active
// frame for BOTH categories at once, regardless of which tool is actually
// being dragged (see ToolDragPoll_Tick below - it snapshots both managers
// unconditionally whenever ANY drag is active). So building one road then
// one pipe would clobber the road's "last known good" data with the road
// manager's now-empty state while the pipe tool is active, and the
// self-test button would find nothing valid even though a perfectly good
// road was built moments earlier. g_lastGoodRoadSnapshot/
// g_lastGoodPipeSnapshot below are a SEPARATE cache that's only ever
// overwritten at the drag-end transition, and only when the count is
// actually plausible (2..64) - i.e. it holds the last REAL committed
// segment, not whatever the manager happened to contain on the most recent
// active frame of ANY tool.
static ToolWaypointSnapshot g_lastGoodRoadSnapshot = {};
static ToolWaypointSnapshot g_lastGoodPipeSnapshot = {};

static void SnapshotToolManager(ToolWaypointSnapshot* out, __int64 managerHandle)
{
    if (!managerHandle) {
        out->valid = false;
        return;
    }
    __try {
        out->valid = true;
        out->typeId = *(int*)(managerHandle + TOOL_TYPE_OFFSET);
        out->variant = *(int*)(managerHandle + TOOL_VARIANT_OFFSET);
        char* beginPtr = *(char**)(managerHandle + TOOL_WAYPOINTS_BEGIN_OFFSET);
        char* endPtr   = *(char**)(managerHandle + TOOL_WAYPOINTS_END_OFFSET);
        long long count = (beginPtr && endPtr && endPtr >= beginPtr)
            ? (long long)(endPtr - beginPtr) / 0x18 : -1;
        out->count = count;
        for (long long i = 0; i < count && i < 64; i++) {
            float* p = (float*)(beginPtr + i * 0x18);
            out->points[i][0] = p[0];
            out->points[i][1] = p[1];
            out->points[i][2] = p[2];
        }
        // See the ToolWaypointSnapshot comment above - manager+0x40/0x44/0x48,
        // straight from FUN_1404fcaf0's own pointer arithmetic (it addresses
        // the manager as C3DVECTOR3**, so param_1+8/+9 there are byte offsets
        // 0x40/0x48; the max bound at +0x44 is a literal byte offset in that
        // decompile, i.e. the same field family, 4 bytes apart from the min).
        out->curveMin = *(float*)(managerHandle + 0x40);
        out->curveMax = *(float*)(managerHandle + 0x44);
        out->curveValue = *(float*)(managerHandle + 0x48);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->valid = true;
        out->count = -2;
    }
}

static void LogToolSnapshot(const char* label, const ToolWaypointSnapshot* s)
{
    if (!s->valid) {
        Logf("republicmp  TOOL_COMMIT: %s manager=<null> (never touched this session)", label);
        return;
    }
    Logf("republicmp  TOOL_COMMIT: %s type=%d variant=%d waypoints=%lld curve=%.3f (min=%.3f max=%.3f) (snapshot from last active frame)",
         label, s->typeId, s->variant, s->count, s->curveValue, s->curveMin, s->curveMax);
    for (long long i = 0; i < s->count && i < 64; i++) {
        Logf("republicmp  TOOL_COMMIT:   [%lld] x=%.2f y=%.2f z=%.2f",
             i, s->points[i][0], s->points[i][1], s->points[i][2]);
    }
}

// Exposed for ui/debugui.cpp's self-test button - lets a "replay my own last
// captured segment, offset, against my own local manager" test run WITHOUT
// touching the network at all (see RoadSync_ApplySegment's comment in
// road_sync.cpp for why that matters - this is meant to be the very first
// time that replay code runs, before ever trusting it with remote data).
static bool GetLastToolSnapshot(const ToolWaypointSnapshot* s, int* outTypeId, int* outVariant,
                                float outPoints[][3], int* outCount, float* outCurveValue)
{
    if (!s->valid || s->count < 2 || s->count > 64) return false;
    *outTypeId = s->typeId;
    *outVariant = s->variant;
    *outCount = (int)s->count;
    *outCurveValue = s->curveValue;
    for (long long i = 0; i < s->count; i++) {
        outPoints[i][0] = s->points[i][0];
        outPoints[i][1] = s->points[i][1];
        outPoints[i][2] = s->points[i][2];
    }
    return true;
}

bool DetectSpawn_GetLastRoadSnapshot(int* outTypeId, int* outVariant, float outPoints[][3], int* outCount, float* outCurveValue)
{
    // g_lastGoodRoadSnapshot, NOT g_lastRoadSnapshot - see the comment above
    // g_lastGoodRoadSnapshot's declaration for why (the plain per-frame
    // cache gets clobbered by whichever tool was dragged most recently,
    // regardless of category).
    return GetLastToolSnapshot(&g_lastGoodRoadSnapshot, outTypeId, outVariant, outPoints, outCount, outCurveValue);
}

bool DetectSpawn_GetLastPipeSnapshot(int* outTypeId, int* outVariant, float outPoints[][3], int* outCount, float* outCurveValue)
{
    return GetLastToolSnapshot(&g_lastGoodPipeSnapshot, outTypeId, outVariant, outPoints, outCount, outCurveValue);
}

// Called every frame (see DetectSpawn_Tick below). While a drag is active,
// caches manager/tool-name state every frame (see comment above); on the
// active->inactive transition, logs whatever was cached on the LAST active
// frame - deliberately not what's readable right now, since that's already
// been cleared by the engine's own commit by this point.
static void ToolDragPoll_Tick(__int64 thisPtr)
{
    __try {
        bool active = *(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) != 0;

        if (active) {
            char toolName[64];
            char* namePtr = *(char**)(thisPtr + TOOL_NAME_PTR_OFFSET);
            SafeReadCStringDirect(namePtr, toolName, sizeof(toolName));
            strncpy_s(g_toolDragLastName, sizeof(g_toolDragLastName), toolName, _TRUNCATE);
            g_toolDragLastDuration = *(float*)(thisPtr + TOOL_DRAG_DURATION_OFFSET);
            SnapshotToolManager(&g_lastRoadSnapshot, *(__int64*)(thisPtr + ROAD_MANAGER_OFFSET));
            SnapshotToolManager(&g_lastPipeSnapshot, *(__int64*)(thisPtr + PIPE_MANAGER_OFFSET));
        }

        if (g_toolDragWasActive && !active) {
            Logf("republicmp  TOOL_COMMIT: drag just ended, activeTool=[%s] duration=%.3f",
                 g_toolDragLastName, g_toolDragLastDuration);
            LogToolSnapshot("ROAD", &g_lastRoadSnapshot);
            LogToolSnapshot("PIPE", &g_lastPipeSnapshot);

            // Sends over the network AND updates the "last GOOD" self-test
            // cache (see the comment above g_lastGoodRoadSnapshot) - same
            // "valid + sane point count" guard either way. Garbage manager
            // reads (rail/sidewalk/water don't map to these two offsets
            // yet) always come back with count<0, never a plausible 2..64,
            // so both naturally stay a no-op for anything not yet identified.
            if (g_lastRoadSnapshot.valid && g_lastRoadSnapshot.count >= 2 && g_lastRoadSnapshot.count <= 64) {
                g_lastGoodRoadSnapshot = g_lastRoadSnapshot;
                RoadSync_SendSegment(ROAD_SYNC_CATEGORY_ROAD, g_lastRoadSnapshot.typeId, g_lastRoadSnapshot.variant,
                                     g_lastRoadSnapshot.points, (int)g_lastRoadSnapshot.count,
                                     g_lastRoadSnapshot.curveValue);
            }
            if (g_lastPipeSnapshot.valid && g_lastPipeSnapshot.count >= 2 && g_lastPipeSnapshot.count <= 64) {
                g_lastGoodPipeSnapshot = g_lastPipeSnapshot;
                RoadSync_SendSegment(ROAD_SYNC_CATEGORY_PIPE, g_lastPipeSnapshot.typeId, g_lastPipeSnapshot.variant,
                                     g_lastPipeSnapshot.points, (int)g_lastPipeSnapshot.count,
                                     g_lastPipeSnapshot.curveValue);
            }
        }
        g_toolDragWasActive = active;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  TOOL_COMMIT: <exception polling drag state>");
    }
}

// Exposed for net/road_sync.cpp's RoadSync_Tick(). TOOL_DRAG_ACTIVE_OFFSET
// (thisPtr+0xd2ac) is ONE flag "shared across every path tool" (see its
// comment above), not per-manager, per-category, or per-player - it just
// means "the LOCAL player currently has a road/pipe/rail/etc drag in
// progress". RoadSync_ApplySegment drives FUN_1404fc040/FUN_1404fcaf0
// directly on the SAME manager object (thisPtr+0xf7a0/+0xf858) a real
// local drag uses - if a network segment gets applied while the local
// player is mid-drag on that manager, the two stomp each other's state
// (fc040 resets the manager's drag-start/curve out from under the live
// drag: the local drag's snap point jumps to the remote point, and the
// curve accumulator gets stuck at its max bound for several drags
// afterward). RoadSync_Tick() checks this before applying each queued
// segment and defers (leaves it queued, retries next frame) instead of
// applying into a manager the local player is actively driving. Cheap -
// reads the value ToolDragPoll_Tick already polls every frame, no extra
// memory read.
bool DetectSpawn_IsToolDragActive()
{
    return g_toolDragWasActive;
}

// Called every frame from the Present hook (game thread, same spot
// build_sync.cpp's BuildSync_Tick() is called from) - lets F9 fire the debug
// spawn / thisPtr diagnostics directly off the fixed placement-manager
// address (g_exe+0x9D4F10), independent of the local player ever having
// triggered a real HookedUniversal call. This is what makes F9 usable for
// solo testing without a second player: RunDebugSpawn/LogThisPtrDiagnostics
// only need a valid thisPtr, not an actual placement click.
void DetectSpawn_Tick()
{
    if (!IsWorldLoaded()) {
        // Keep any F9 request pending rather than dropping it - same
        // "hold until the world exists" idea as build_sync.cpp's g_pending.
        return;
    }

    __int64 thisPtr = GetPlacementManagerThisPtr();

    // Road/pipe drag-commit watch (see ToolDragPoll_Tick comment above) -
    // unconditional, every frame, independent of the F9 pending flags below.
    ToolDragPoll_Tick(thisPtr);

    if (g_debugSpawnPending == 0 && g_thisPtrDiagPending == 0) return;

    if (InterlockedExchange(&g_thisPtrDiagPending, 0) != 0) {
        LogThisPtrDiagnostics(thisPtr);
    }
    if (InterlockedExchange(&g_debugSpawnPending, 0) != 0) {
        RunDebugSpawn(thisPtr);
    }
}

void HookedUniversal(__int64 param_1, char* param_2, char param_3, char param_4,
                      unsigned __int64 param_5, unsigned int param_6,
                      char param_7, char param_8)
{
    // While a save-sync gray screen is up (ui/sync_block.h), refuse every
    // placement outright - don't even call through to the real engine
    // function. The mouse/keyboard swallow in hooks/imgui_hook.cpp's
    // HookedWndProc does NOT stop the player from placing/clicking during a
    // sync on its own, since the game reads at least some input some other
    // way than window messages (raw/polled - exactly how is unconfirmed).
    // This function is the single choke point every real placement funnels
    // through regardless of what triggered it, so refusing it here blocks
    // new buildings completely no matter how the click got through.
    // Doesn't affect ApplyOneRemotePlacement() in net/build_sync.cpp - that
    // calls g_origUniversal directly, bypassing this hook entirely.
    if (SyncBlock_IsActive()) {
        return;
    }

    g_universalCallCount++;

    char param2Direct[64];
    SafeReadCStringDirect(param_2, param2Direct, sizeof(param2Direct));

    // Unconditional entry log, independent of whether the buildings vector
    // below grows. Roads/pipes/fences/power lines go through this SAME
    // single placement hook, same as every building type - but the
    // buildings-vector growth check further down only catches placements
    // that push into that specific vector, so a road/pipe placement could
    // otherwise fire this hook and never produce a single "UNIVERSAL fired"
    // line below. This line makes that visible regardless: it prints before
    // g_origUniversal is even called, with the raw param_2 type name and
    // all of param_3..param_8.
    Logf("republicmp  HOOK_UNIVERSAL entry: type=[%s] this=%p param3=%d param4=%d param5=%llu param6=%u param7=%d param8=%d",
         param2Direct, (void*)param_1, (int)param_3, (int)param_4,
         (unsigned long long)param_5, param_6, (int)param_7, (int)param_8);

    void** vecEndPtr   = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
    void** vecBeginPtr = (void**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);

    void* beginBefore = nullptr;
    void* endBefore = nullptr;
    __try { beginBefore = *vecBeginPtr; endBefore = *vecEndPtr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { beginBefore = nullptr; endBefore = nullptr; }

    // See the comment above LogCursorRotHexDump - taken BEFORE the real
    // call so it reflects the player's actual pre-click rotation state, to
    // be diffed against the post-creation sin/cos logged further below.
    __try { LogCursorRotHexDump(param_1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Logf("republicmp  ROTIN_DIAG: <exception dumping cursor state>"); }

    g_origUniversal(param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8);

    __try {
        void* beginAfter = *vecBeginPtr;
        void* endAfter = *vecEndPtr;

        long long countBefore = (beginBefore && endBefore)
            ? (long long)(((uintptr_t)endBefore - (uintptr_t)beginBefore) / sizeof(void*))
            : -1;
        long long countAfter = (beginAfter && endAfter)
            ? (long long)(((uintptr_t)endAfter - (uintptr_t)beginAfter) / sizeof(void*))
            : -1;
        long long countDelta = (countBefore >= 0 && countAfter >= 0) ? (countAfter - countBefore) : 0;
        bool reallocated = (beginBefore != beginAfter);

        bool growthValid = endAfter && countDelta > 0;
        if (growthValid && countDelta > 1) {
            g_universalMultiPushCount++;
            Logf("republicmp  UNIVERSAL multi-push: %lld new element(s) (param2Direct=[%s], reallocated=%d)",
                 countDelta, param2Direct, reallocated ? 1 : 0);
        }

        if (growthValid) {
            void* newObjPtr = *((void**)endAfter - 1);
            if (newObjPtr) {
                float x = *(float*)((char*)newObjPtr + 0x350);
                float y = *(float*)((char*)newObjPtr + 0x354);
                float z = *(float*)((char*)newObjPtr + 0x358);

                const char* chosenType = LooksLikeValidType(param2Direct) ? param2Direct : "<unknown>";

                // param_3..param_8 stay BYTE-IDENTICAL (p3=0 p4=0
                // p6=0xffffffff p7=0 p8=0) across differently-rotated
                // placements; p5 only ever takes 2 values (0 once, then a
                // constant 3.0f-as-bits for the rest) with no relation to
                // the continuously-varying angle below - rotation is not
                // one of the engine's own placement params.
                // obj+0x340/obj+0x348 (sin/cos, sin^2+cos^2==1 holds) is the
                // only known place rotation lives, read off the object
                // AFTER creation. Since there's no input field to set
                // beforehand at this call site, net/build_sync.cpp's
                // ApplyOneRemotePlacement instead patches the remote side's
                // newly-created object's own sin/cos fields directly, same
                // offsets, right after g_origUniversal creates it.
                float rotSin = *(float*)((char*)newObjPtr + 0x340);
                float rotCos = *(float*)((char*)newObjPtr + 0x348);
                double rotDeg = atan2(rotSin, rotCos) * 180.0 / 3.14159265358979323846;

                Logf("republicmp  UNIVERSAL fired: type=[%s] x=%.2f y=%.2f z=%.2f obj=%p rotDeg=%.2f (sin=%.4f cos=%.4f)",
                     chosenType, x, y, z, newObjPtr, rotDeg, rotSin, rotCos);

                NotifyBuildingPlaced(x, y, z, rotSin, rotCos, chosenType);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  UNIVERSAL fired: <exception reading vector/object after call>");
    }
}
