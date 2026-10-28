// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP - TesmioLoader plugin entry points.
#include "tesmio_plugin.h"
#include "../world/local_builds.h"
#include "../game/present_hook.h"
#include "../session/game_speed.h"
#include "../game/save_load.h"
#include "../game/imgui_hook.h"
#include "../game/main_menu_hook.h"
#include "../game/system_menu.h"
#include "../world/terrain_hook.h"
#include "../world/demolish_hook.h"
#include "../world/placement_collision.h"
#include "../game/session_patches.h"
#include "../world/view_list_scope.h"
#include "../game/input_block.h"
#include "../ui/chat.h"
#include "../determinism/state_hash.h"
#include "../determinism/determinism.h"
#include "../determinism/sim_patches.h"
#include "../determinism/construction_view.h"
#include "../determinism/probe_trace.h"
#include "../determinism/citizen_trace.h"
#include "../determinism/probe_watch.h"
#include "../save/save_zip.h"
#include "../transport/network.h"
#include "../session/save_sync.h"
#include "../world/buildings.h"
#include "../world/networks.h"
#include "../lockstep/building_settings.h"

#include "../game/world.h"
#include "../game/camera.h"
#include "../game/panel2d.h"
#include "../game/minimap.h"
#include "../game/border_draw.h"
#include "../game/weather.h"
#include "../modes/republics/republics.h"
#include "version.h"
#include "profile.h"

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

// Binds every module to the loader's host API.
extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    Profile_Bind(host);
    World_Bind(host);
    Camera_Bind(host);
    Panel2D_Bind(host);
    Minimap_Bind(host);
    BorderDraw_Bind(host);
    Weather_Bind(host);
    LocalBuilds_Bind(host);
    PresentHook_Bind(host);
    GameSpeed_Bind(host);
    SaveLoad_Bind(host);
    SaveZip_Bind(host);
    Network_Bind(host);
    SaveSync_Bind(host);
    Buildings_Bind(host);
    Networks_Bind(host);
    ImGuiHook_Bind(host);
    Chat_Bind(host);
    MainMenuHook_Bind(host);
    SystemMenu_Bind(host);
    TerrainHook_Bind(host);
    DemolishHook_Bind(host);
    PlacementCollision_Bind(host);
    SessionPatches_Bind(host);
    ViewListScope_Bind(host);
    InputBlock_Bind(host);
    StateHash_Bind(host);
    Determinism_Bind(host);
    SimPatches_Bind(host);
    ConstructionView_Bind(host);
    ProbeTrace_Bind(host);
    CitizenTrace_Bind(host);
    ProbeWatch_Bind(host);
    BuildingSettings_Bind(host);
    Republics_Bind(host);
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

    bool okPlanStart = Networks_InstallPlanStartHooks();
    Logf("republicmp  plan start hooks: %d%s", okPlanStart,
         okPlanStart ? "" : " - starting planned roads/rails will NOT be synced");

    bool okDragStart = Networks_InstallDragStartHook();
    Logf("republicmp  drag start hook: %d%s", okDragStart,
         okDragStart ? "" : " - another player's command ends the local drag instead of pausing it");

    bool okGroupTool = Networks_InstallGroupToolHook();
    Logf("republicmp  group tool hook: %d%s", okGroupTool,
         okGroupTool ? "" : " - the group construction tools will NOT be synced");

    bool okCollision = InstallPlacementCollisionHook();
    Logf("republicmp  placement collision hook: %d%s", okCollision,
         okCollision ? "" : " - access roads may take a different route on each PC");

    bool okMinimap = Minimap_InstallHook();
    Logf("republicmp  minimap draw hook: %d%s", okMinimap, okMinimap ? "" : " - modes draw nothing on the minimap");

    bool okCitizenTrace = CitizenTrace_Install();
    Logf("republicmp  citizen trace hooks: %d%s", okCitizenTrace, okCitizenTrace ? "" : " - desync reports list no citizen events");

    bool okTerrain = InstallTerrainHooks();
    Logf("republicmp  terrain hooks: %d%s", okTerrain, okTerrain ? "" : " - terrain will NOT be fully synced");

    // Fixed timer, call-site random numbers, pinned math - inactive until used.
    bool okDeterminism = Determinism_Install();
    Logf("republicmp  determinism layer: %d%s", okDeterminism, okDeterminism ? "" : " - lockstep and the determinism probe are unavailable");

    HANDLE presentHook = CreateThread(nullptr, 0, PresentHookThread, nullptr, 0, nullptr);
    Logf("republicmp  present hook thread started: %d", presentHook != nullptr);

    if (!okUniversal) return 1;
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
