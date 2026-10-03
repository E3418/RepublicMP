// RepublicMP - TesmioLoader plugin entry points.
#include "tesmio_plugin.h"
#include "hooks/detect_spawn.h"
#include "hooks/present_hook.h"
#include "hooks/game_speed.h"
#include "hooks/save_load.h"
#include "hooks/imgui_hook.h"
#include "hooks/terrain_hook.h"
#include "hooks/demolish_hook.h"
#include "hooks/input_block.h"
#include "save/save_zip.h"
#include "net/network.h"
#include "net/save_sync.h"
#include "net/build_sync.h"
#include "net/road_sync.h"
#include "net/terrain_sync.h"

#include "version.h"

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

// Binds every module to the loader's host API.
extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    DetectSpawn_Bind(host);
    PresentHook_Bind(host);
    GameSpeed_Bind(host);
    SaveLoad_Bind(host);
    SaveZip_Bind(host);
    Network_Bind(host);
    SaveSync_Bind(host);
    BuildSync_Bind(host);
    RoadSync_Bind(host);
    ImGuiHook_Bind(host);
    TerrainHook_Bind(host);
    TerrainSync_Bind(host);
    DemolishHook_Bind(host);
    InputBlock_Bind(host);
    info->name    = "republicmp";
    info->version = REPUBLICMP_VERSION;
    return 0;
}

// Installs the engine hooks and starts the background threads.
extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    Logf("republicmp  version=%s", REPUBLICMP_VERSION);

    bool okUniversal = InstallInlineHook(g_exeBase + HOOK_UNIVERSAL_RVA, (void*)HookedUniversal,
                                          (void**)&g_origUniversal, kUniversalExpect,
                                          sizeof(kUniversalExpect), "republicmp universal");
    Logf("republicmp  universal hook: %d", okUniversal);

    bool okRoadCommit = InstallInlineHook(g_exeBase + HOOK_ROAD_COMMIT_RVA, (void*)HookedRoadCommit,
                                           (void**)&g_origRoadCommit, kRoadCommitExpect,
                                           sizeof(kRoadCommitExpect), "republicmp road commit");
    Logf("republicmp  road commit hook: %d%s", okRoadCommit,
         okRoadCommit ? "" : " - infrastructure will NOT be synced");

    // The sync screen relies on these to actually stop the game's input.
    bool okInput = InstallInputBlockHooks();
    Logf("republicmp  input block hooks: %d%s", okInput, okInput ? "" : " - the join screen will NOT fully lock input");

    bool okDemolish = InstallDemolishHook();
    Logf("republicmp  demolish hook: %d%s", okDemolish, okDemolish ? "" : " - demolitions will NOT be synced");

    bool okTerrain = InstallTerrainHooks();
    Logf("republicmp  terrain hooks: %d%s", okTerrain, okTerrain ? "" : " - terrain will NOT be fully synced");

    HANDLE presentHook = CreateThread(nullptr, 0, PresentHookThread, nullptr, 0, nullptr);
    Logf("republicmp  present hook thread started: %d", presentHook != nullptr);

    HANDLE hb = CreateThread(nullptr, 0, HeartbeatThread, nullptr, 0, nullptr);
    Logf("republicmp  heartbeat thread started: %d", hb != nullptr);

    // Dev hotkeys: F9 debug spawn, F10 pause toggle.
    HANDLE debugSpawn = CreateThread(nullptr, 0, DebugSpawnThread, nullptr, 0, nullptr);
    Logf("republicmp  debug spawn thread started: %d (hotkey F9)", debugSpawn != nullptr);

    HANDLE gameSpeedHotkey = CreateThread(nullptr, 0, GameSpeedHotkeyThread, nullptr, 0, nullptr);
    Logf("republicmp  game speed hotkey thread started: %d (hotkey F10)", gameSpeedHotkey != nullptr);

    if (!okUniversal) return 1;
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
