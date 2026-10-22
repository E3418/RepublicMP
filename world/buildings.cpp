// SPDX-License-Identifier: GPL-3.0-only
// See buildings.h.
#include "buildings.h"
#include "local_builds.h" // g_origUniversal, LocalBuilds_Begin/EndBuildingPlacement
#include "networks.h" // Networks_ClearHover
#include "demolish_hook.h" // DemolishHook_CallOriginal
#include "view_list_scope.h"
#include "menus_closed.h"
#include <vector>
#include <algorithm>
#include <stdio.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Engine addresses and offsets
// ---------------------------------------------------------------------------

#define UNIVERSAL_THIS_RVA 0x9D4F10      // placement god-object (thisPtr)
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18 // city buildings vector
#define VEC_BUILDINGS_END_RVA 0x9E6A20
#define OBJ_TYPE_OFFSET 0x318            // building type record
#define OBJ_PROGRESS_OFFSET 0x604        // float: construction progress, 0..1
#define TYPE_CATEGORY_OFFSET 0x360       // int category in the type record

// thisPtr placement inputs.
#define CURSOR_X_OFFSET 0xf6c
#define CURSOR_Y_OFFSET 0xf70
#define CURSOR_Z_OFFSET 0xf74
#define CURSOR_COPY_OFFSET 0xf78         // mirrors the cursor x/y/z
#define CURSOR_YAW_OFFSET 0xf94          // yaw in radians
#define MIRROR_TOGGLE_OFFSET 0x11db0     // byte: player's mirror toggle
#define PLACEMENT_RESULT_OFFSET 0x11a10  // 3 ints: validation result of the last call
// Pointer to the menu entry the local player has selected (name string
// first, e.g. "road_onelane"; +0x2b5 a flag, +0x48 the type record), 0 when
// nothing is selected. FUN_1403c4850 (the road routing a building's access
// road goes through) reads +0x2b5 without a null check.
#define SELECTED_ENTRY_OFFSET 0xd428
#define PLACE_SCOPE_MARGIN 150.0f        // view-list scope around a placement
#define REPLAY_ENTRY_SIZE 0x1000         // covers every field read off an entry

#define OBJ_POS_OFFSET 0x350

#define LMB_HELD_RVA 0xA54E90         // byte: left mouse button held
#define LMB_JUST_PRESSED_RVA 0xA54E91 // byte: pressed this frame

// ---------------------------------------------------------------------------
// Module state and helpers
// ---------------------------------------------------------------------------

static char g_replayEntry[REPLAY_ENTRY_SIZE]; // stand-in selected menu entry (main thread only)
static volatile LONG g_applyingRemote = 0;

void Buildings_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool Buildings_IsApplyingCommand()
{
    return g_applyingRemote != 0;
}

static __int64 GetPlacementManagerThisPtr()
{
    return (__int64)((uintptr_t)g_exe + UNIVERSAL_THIS_RVA);
}

// The buildings before a placement, sorted, so the new ones are found by
// identity - not as "the vector's tail", which a placement that also removes
// or reorders objects breaks.
static std::vector<void*> g_placeBefore;

// The buildings vector's bounds; false if unreadable.
static bool ReadBuildingsVector(void*** begin, void*** end)
{
    __try {
        *begin = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        *end = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        return *begin && *end >= *begin;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void SnapshotBeforePlacement()
{
    g_placeBefore.clear();
    void** begin;
    void** end;
    if (ReadBuildingsVector(&begin, &end)) g_placeBefore.assign(begin, end);
    std::sort(g_placeBefore.begin(), g_placeBefore.end());
}

// Plain binary search: no STL iterators, which can't sit in a __try.
static bool WasBeforePlacement(void* obj)
{
    void* const* a = g_placeBefore.data();
    size_t lo = 0, hi = g_placeBefore.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (a[mid] < obj) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_placeBefore.size() && a[lo] == obj;
}

// The new object (not in the snapshot) nearest (x, z); *newCount = how many
// new objects there are, *dist = its distance.
static void* PickMainNewObject(float x, float z, int* newCount, float* dist)
{
    void* best = nullptr;
    float bestD2 = 0.0f;
    *newCount = 0;
    __try {
        void** it = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        void** end = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        for (; it && it < end; ++it) {
            void* obj = *it;
            if (!obj || WasBeforePlacement(obj)) continue;
            ++*newCount;
            float dx = *(float*)((char*)obj + OBJ_POS_OFFSET) - x;
            float dz = *(float*)((char*)obj + OBJ_POS_OFFSET + 8) - z;
            float d2 = dx * dx + dz * dz;
            if (!best || d2 < bestD2) { best = obj; bestD2 = d2; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        best = nullptr;
    }
    *dist = sqrtf(bestD2);
    return best;
}

// For the log: the new objects of a placement - category (-1: no type, as a
// construction site's), offset from the cursor and construction progress - so
// the PCs' logs show which object one of them made and another did not.
static void DescribeNewObjects(float x, float z, char* out, size_t outSize)
{
    out[0] = 0;
    size_t used = 0;
    int shown = 0;
    __try {
        void** it = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        void** end = *(void***)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        for (; it && it < end && shown < 6; ++it) {
            char* obj = (char*)*it;
            if (!obj || WasBeforePlacement(obj)) continue;
            __int64 type = *(__int64*)(obj + OBJ_TYPE_OFFSET);
            int cat = type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
            float dx = *(float*)(obj + OBJ_POS_OFFSET) - x;
            float dz = *(float*)(obj + OBJ_POS_OFFSET + 8) - z;
            int n = _snprintf_s(out + used, outSize - used, _TRUNCATE, "%s[cat %d at %+.1f,%+.1f progress %.3f]",
                                shown ? " " : "", cat, dx, dz, *(float*)(obj + OBJ_PROGRESS_OFFSET));
            if (n < 0) break;
            used += (size_t)n;
            shown++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The local building at `pos` (0.5 m in x/z, 1 m in y) with this type
// category (-1 = any), nearest first; 0 if none.
static __int64 FindBuildingAt(float x, float y, float z, int category)
{
    __int64 found = 0;
    float bestD = 1e30f;
    __try {
        __int64* it = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        __int64* end = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        for (; it && it < end; ++it) {
            __int64 b = *it;
            if (!b) continue;
            const float* p = (const float*)(b + OBJ_POS_OFFSET);
            float dx = p[0] - x, dz = p[2] - z;
            if (fabsf(dx) >= 0.5f || fabsf(dz) >= 0.5f || fabsf(p[1] - y) >= 1.0f) continue;
            __int64 type = *(__int64*)(b + OBJ_TYPE_OFFSET);
            int cat = type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
            if (category >= 0 && cat != category) continue;
            float d = dx * dx + dz * dz;
            if (d < bestD) { bestD = d; found = b; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = 0;
    }
    return found;
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------
//
// The placement is the building tool's own click frame: 2bade0 as the tool
// calls it (param_4 = 0, param_8 = 0) with the button pressed this frame and
// the clicking PC's cursor, yaw and mirror. param_4 = 0 is what lays the
// building's access roads (the loop over the type's connection points at
// type+0x2f8, 2bb563), so each PC's engine routes the same roads itself;
// param_4 = 1 skips them. The no-click call with param_4 = 0, param_8 = 1
// (4e4cb0's second branch) returned before validating anything (result
// -1,-1,0), as the click path reads the mouse. When the click frame builds
// nothing, a second call with param_4 = 1, param_8 = 1 (confirm without a
// click) places the building without its roads - the same outcome on every
// PC either way. No rotation is written afterwards: every PC smooths the same
// yaw input alike.

bool Buildings_ApplyPlacementCommand(const BuildingMsg& msg)
{
    __int64 thisPtr = GetPlacementManagerThisPtr();
    bool placed = false;
    __try {
        float* cursor = (float*)(thisPtr + CURSOR_X_OFFSET);
        float* cursorCopy = (float*)(thisPtr + CURSOR_COPY_OFFSET);
        float* yaw = (float*)(thisPtr + CURSOR_YAW_OFFSET);
        unsigned char* mirror = (unsigned char*)(thisPtr + MIRROR_TOGGLE_OFFSET);
        unsigned char* held = (unsigned char*)((uintptr_t)g_exe + LMB_HELD_RVA);
        unsigned char* pressed = (unsigned char*)((uintptr_t)g_exe + LMB_JUST_PRESSED_RVA);
        __int64* selected = (__int64*)(thisPtr + SELECTED_ENTRY_OFFSET);
        float savedCursor[3] = { cursor[0], cursor[1], cursor[2] };
        float savedCopy[3] = { cursorCopy[0], cursorCopy[1], cursorCopy[2] };
        float savedYaw = *yaw;
        unsigned char savedMirror = *mirror, savedHeld = *held, savedPressed = *pressed;
        __int64 savedSelected = *selected;
        // The clicking PC's pick modes (see BuildingMsg::pickModes).
        unsigned char* pickModes[2] = { (unsigned char*)(thisPtr + 0xF600), (unsigned char*)(thisPtr + 0xF698) };
        unsigned char savedPickModes[2] = { *pickModes[0], *pickModes[1] };
        // This PC's open menus would make the placement return at once
        // (menus_closed.h); they read closed for the call.
        unsigned char* menus = (unsigned char*)thisPtr;
        unsigned char savedMenus[MENU_FLAG_COUNT];
        for (int i = 0; i < MENU_FLAG_COUNT; i++) savedMenus[i] = menus[kMenuFlagOffsets[i]];
        BuildMode mine = BuildMode_Read();
        char typeName[64];
        strncpy_s(typeName, msg.typeName, _TRUNCATE);
        __try {
            cursor[0] = cursorCopy[0] = msg.cursorX;
            cursor[1] = cursorCopy[1] = msg.cursorY;
            cursor[2] = cursorCopy[2] = msg.cursorZ;
            *yaw = msg.cursorRot;
            *mirror = msg.mirrored ? 1 : 0;
            *held = 0;
            *pressed = 0;
            *pickModes[0] = (msg.pickModes & 1) ? 1 : 0;
            *pickModes[1] = (msg.pickModes & 2) ? 1 : 0;
            BuildMode_Write(msg.mode);
            // The local player's selected menu entry must not leak into the
            // call - and the access-road routing reads it unchecked, so a
            // player with nothing selected crashed it. A neutral entry named
            // after the building stands in while the call runs. Not
            // strncpy_s: the debug CRT fills the rest of the buffer with
            // 0xFE, and the game follows +0x48 as a pointer when it is nonzero.
            memset(g_replayEntry, 0, sizeof(g_replayEntry));
            memcpy(g_replayEntry, typeName, strnlen(typeName, 63));
            *selected = (__int64)g_replayEntry;
            for (int i = 0; i < MENU_FLAG_COUNT; i++) menus[kMenuFlagOffsets[i]] = 0;
            SnapshotBeforePlacement();
            // The access road's planner reads what this PC's mouse hovers (the clicking PC's road under
            // the cursor, the other PC's whatever it points at): one PC laid two pieces, the other one.
            Networks_ClearHover();
            // The same "camera-visible" edges on every PC (view_list_scope.h).
            bool scoped = ViewListScope_Begin(msg.cursorX - PLACE_SCOPE_MARGIN, msg.cursorZ - PLACE_SCOPE_MARGIN,
                                              msg.cursorX + PLACE_SCOPE_MARGIN, msg.cursorZ + PLACE_SCOPE_MARGIN);
            LocalBuilds_BeginBuildingPlacement();
            InterlockedIncrement(&g_applyingRemote);
            const int* result = (const int*)(thisPtr + PLACEMENT_RESULT_OFFSET);
            int clickResult[3] = { 0, 0, 0 };
            int newCount = 0;
            float delta = 0.0f;
            bool withRoads = false;
            __try {
                // The original (not the hook), so the call is not taken as a new placement.
                *pressed = 1;
                g_origUniversal(thisPtr, typeName, 0, 0, msg.param5, 0xffffffff, 0, 0);
                *pressed = 0;
                memcpy(clickResult, result, sizeof(clickResult));
                placed = withRoads = PickMainNewObject(msg.cursorX, msg.cursorZ, &newCount, &delta) != nullptr;
                if (!placed) {
                    SnapshotBeforePlacement();
                    g_origUniversal(thisPtr, typeName, 0, 1, msg.param5, 0xffffffff, 0, 1);
                    placed = PickMainNewObject(msg.cursorX, msg.cursorZ, &newCount, &delta) != nullptr;
                }
            } __finally {
                *pressed = 0;
                InterlockedDecrement(&g_applyingRemote);
                LocalBuilds_EndBuildingPlacement();
                if (scoped) ViewListScope_End();
            }
            char modeText[96];
            char made[320];
            DescribeNewObjects(msg.cursorX, msg.cursorZ, made, sizeof(made));
            Logf("republicmp  BUILDINGS: command placement type=[%s] pay=%s cursor=(%.2f,%.2f,%.2f) - %s (objects %d, click result %d,%d,%d, result %d,%d,%d) %s",
                 typeName, BuildMode_Describe(msg.mode, modeText, sizeof(modeText)), msg.cursorX, msg.cursorY, msg.cursorZ,
                 withRoads ? "placed by the click frame" : placed ? "placed by the second call, without access roads" : "refused by the engine",
                 newCount, clickResult[0], clickResult[1], clickResult[2], result[0], result[1], result[2], made);
        } __finally {
            cursor[0] = savedCursor[0]; cursor[1] = savedCursor[1]; cursor[2] = savedCursor[2];
            cursorCopy[0] = savedCopy[0]; cursorCopy[1] = savedCopy[1]; cursorCopy[2] = savedCopy[2];
            *yaw = savedYaw;
            *mirror = savedMirror;
            *held = savedHeld;
            *pressed = savedPressed;
            *selected = savedSelected;
            *pickModes[0] = savedPickModes[0];
            *pickModes[1] = savedPickModes[1];
            for (int i = 0; i < MENU_FLAG_COUNT; i++) menus[kMenuFlagOffsets[i]] = savedMenus[i];
            BuildMode_Write(mine);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  BUILDINGS: <exception making a placement command>");
    }
    return placed;
}

// ---------------------------------------------------------------------------
// Demolition
// ---------------------------------------------------------------------------

// Demolishes the local building at the clicking PC's position. Returns false
// only if the engine call threw; a building that is not there (already gone)
// counts as done.
bool Buildings_ApplyDemolishCommand(const BuildingMsg& msg)
{
    __int64 found = FindBuildingAt(msg.x, msg.y, msg.z, msg.demolishCategory);
    if (!found) {
        Logf("republicmp  BUILDINGS: demolish at (%.2f,%.2f,%.2f) category=%d - no such building here (already gone), skipped",
             msg.x, msg.y, msg.z, msg.demolishCategory);
        return true;
    }

    InterlockedIncrement(&g_applyingRemote);
    bool ok = DemolishHook_CallOriginal(GetPlacementManagerThisPtr(), found, msg.demolishArgs);
    InterlockedDecrement(&g_applyingRemote);

    // Removed at once, or (realistic mode) turned into a demolition site
    // that stays in the list until workers finish.
    bool stillListed = false;
    __try {
        __int64* it = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
        __int64* end = *(__int64**)((uintptr_t)g_exeBase + VEC_BUILDINGS_END_RVA);
        for (; it && it < end; ++it)
            if (*it == found) { stillListed = true; break; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Logf("republicmp  BUILDINGS: demolish at (%.2f,%.2f,%.2f) category=%d args=[%llx,%llx,%llx,%llx,%llx] %s",
         msg.x, msg.y, msg.z, msg.demolishCategory,
         msg.demolishArgs[0] & 0xff, msg.demolishArgs[1] & 0xff, msg.demolishArgs[2] & 0xff,
         msg.demolishArgs[3] & 0xff, msg.demolishArgs[4] & 0xff,
         !ok ? "THREW" : stillListed ? "applied - building STILL in the list (demolition site?)" : "applied - building removed");
    return ok;
}
