// SPDX-License-Identifier: GPL-3.0-only
// See demolish_hook.h.
#include "demolish_hook.h"
#include "local_builds.h"          // LocalBuilds_InBuildingPlacement
#include "terrain_hook.h"          // g_terrainApplyingCommand
#include "buildings.h"     // BuildingMsg, Buildings_IsApplyingCommand
#include "networks.h"      // Networks_IsApplyingCommand
#include "../lockstep/commands.h"

#define WORLD_THIS_RVA 0x9D4F10
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18 // = thisPtr+0x11b08, the list terrain_demolish hovers over
#define VEC_BUILDINGS_END_RVA 0x9E6A20
#define OBJ_POS_OFFSET 0x350
#define OBJ_TYPE_OFFSET 0x318          // building type record
#define TYPE_CATEGORY_OFFSET 0x360     // int category in the type record
#define OBJ_DEMOLISH_FLAG_OFFSET 0xeb1 // zeroed by both player paths before the call

// mov rax,rsp / mov [rax+20],r9b / mov [rax+18],r8b / mov [rax+10],rdx -
// 15 bytes, none RIP-relative (SOVIET64.exe, 2026-09-07).
static const unsigned char kDemolishExpect[15] = {
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x44,
    0x88, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10
};

typedef void (*DemolishFn)(__int64 thisPtr, __int64 building,
                           unsigned long long p3, unsigned long long p4,
                           unsigned long long p5, unsigned long long p6,
                           unsigned long long p7);
static DemolishFn g_origDemolish = nullptr;

static volatile long g_depth = 0;

void DemolishHook_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool DemolishHook_InDemolish()
{
    return g_depth > 0;
}

static bool IsInBuildingsVector(__int64 building)
{
    __int64* it = *(__int64**)(g_exeBase + VEC_BUILDINGS_BEGIN_RVA);
    __int64* end = *(__int64**)(g_exeBase + VEC_BUILDINGS_END_RVA);
    for (; it && it < end; ++it)
        if (*it == building) return true;
    return false;
}

static int BuildingCategory(__int64 building)
{
    __int64 type = *(__int64*)(building + OBJ_TYPE_OFFSET);
    return type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
}

// Lockstep (lockstep/commands): a local demolition becomes a command and is
// not made here; every PC demolishes the building at the next tick boundary
// (Buildings_ApplyDemolishCommand finds it by position and category). True
// when it was taken.
static bool DemolishAsCommand(__int64 thisPtr, __int64 building, const unsigned long long args[5])
{
    BuildingMsg msg = {};
    __try {
        if (thisPtr != (__int64)(g_exeBase + WORLD_THIS_RVA) || !building || !IsInBuildingsVector(building))
            return false;
        const float* p = (const float*)(building + OBJ_POS_OFFSET);
        msg.kind = BUILDING_DEMOLISH;
        msg.x = p[0];
        msg.y = p[1];
        msg.z = p[2];
        msg.demolishCategory = BuildingCategory(building);
        for (int k = 0; k < 5; k++) msg.demolishArgs[k] = args[k];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    Commands_Submit(LS_CMD_DEMOLISH, &msg, sizeof(msg));
    return true;
}

static void HookedDemolish(__int64 thisPtr, __int64 building,
                           unsigned long long p3, unsigned long long p4,
                           unsigned long long p5, unsigned long long p6,
                           unsigned long long p7)
{
    long depth = InterlockedIncrement(&g_depth);

    // Only outermost calls by local play; everything else is either a
    // side effect of one, or a command being made.
    if (depth == 1 && !LocalBuilds_InBuildingPlacement() && !Buildings_IsApplyingCommand() &&
        !Networks_IsApplyingCommand() && g_terrainApplyingCommand == 0) {
        unsigned long long args[5] = { p3, p4, p5, p6, p7 };
        if (Commands_TakeLocal() && DemolishAsCommand(thisPtr, building, args)) {
            InterlockedDecrement(&g_depth);
            return;
        }
    }

    g_origDemolish(thisPtr, building, p3, p4, p5, p6, p7);
    InterlockedDecrement(&g_depth);
}

bool InstallDemolishHook()
{
    return InstallInlineHook(g_exeBase + HOOK_DEMOLISH_RVA, (void*)HookedDemolish,
                             (void**)&g_origDemolish, kDemolishExpect, sizeof(kDemolishExpect),
                             "republicmp demolish");
}

bool DemolishHook_CallOriginal(__int64 thisPtr, __int64 building, const unsigned long long args[5])
{
    DemolishFn fn = g_origDemolish ? g_origDemolish : (DemolishFn)(g_exeBase + HOOK_DEMOLISH_RVA);
    __try {
        *(unsigned char*)(building + OBJ_DEMOLISH_FLAG_OFFSET) = 0;
        InterlockedIncrement(&g_depth);
        __try {
            fn(thisPtr, building, args[0], args[1], args[2], args[3], args[4]);
        } __finally {
            InterlockedDecrement(&g_depth);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  DEMOLISH: <exception replaying demolition>");
        return false;
    }
}
