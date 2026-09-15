#include "tesmio_plugin.h"
#include "hooks/detect_spawn.h"
#include "hooks/present_hook.h"
#include "hooks/game_speed.h"
#include "hooks/save_load.h"
#include "hooks/imgui_hook.h"
#include "save/save_zip.h"
#include "net/network.h"
#include "net/save_sync.h"
#include "net/build_sync.h"
#include "net/road_sync.h"

// MAJOR.FEATURE.PATCH (see README.md's Versioning section)
#define REPUBLICMP_VERSION "0.1.0"

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

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
    info->name    = "republicmp";
    info->version = "0.1";
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    Logf("republicmp  version=%s", REPUBLICMP_VERSION);

    bool okUniversal = InstallInlineHook(g_exeBase + HOOK_UNIVERSAL_RVA, (void*)HookedUniversal,
                                          (void**)&g_origUniversal, kUniversalExpect,
                                          sizeof(kUniversalExpect), "republicmp universal");
    Logf("republicmp  universal hook: %d", okUniversal);

    HANDLE presentHook = CreateThread(nullptr, 0, PresentHookThread, nullptr, 0, nullptr);
    Logf("republicmp  present hook thread started: %d", presentHook != nullptr);

    HANDLE hb = CreateThread(nullptr, 0, HeartbeatThread, nullptr, 0, nullptr);
    Logf("republicmp  heartbeat thread started: %d", hb != nullptr);

    // F9/F10 are the only hotkeys left - both are dev-only diagnostics with
    // no UI equivalent yet (synthetic spawn / instant pause toggle). O/I/P/L
    // duplicated buttons already in ui/main_menu.cpp and ui/debugui.cpp and
    // are gone.
    HANDLE debugSpawn = CreateThread(nullptr, 0, DebugSpawnThread, nullptr, 0, nullptr);
    Logf("republicmp  debug spawn thread started: %d (hotkey F9)", debugSpawn != nullptr);

    HANDLE gameSpeedHotkey = CreateThread(nullptr, 0, GameSpeedHotkeyThread, nullptr, 0, nullptr);
    Logf("republicmp  game speed hotkey thread started: %d (hotkey F10)", gameSpeedHotkey != nullptr);

    if (!okUniversal) return 1;
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
