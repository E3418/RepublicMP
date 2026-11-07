// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP by E3418 and Sohn (juniorsohn); all authors in README.md.
// RepublicMP - the entry point the proxy calls: RepublicMP_Start.
#include "host.h"
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
#include "../determinism/site_watch.h"
#include "../determinism/probe_trace.h"
#include "../determinism/citizen_trace.h"
#include "../determinism/probe_watch.h"
#include "../lockstep/wallets.h"
#include "../lockstep/citizen_stats.h"
#include "../lockstep/economy_stats.h"
#include "../save/save_zip.h"
#include "../transport/network.h"
#include "../session/save_sync.h"
#include "../session/workshop.h"
#include "../world/buildings.h"
#include "../world/networks.h"
#include "../lockstep/building_settings.h"
#include "../lockstep/cancel_tool.h"
#include "../lockstep/world_workers.h"
#include "../lockstep/cancel_plans.h"
#include "../lockstep/plan_remove.h"
#include "../lockstep/car_owners.h"
#include "../lockstep/line_spacing.h"
#include "../lockstep/crime_cancel.h"
#include "../lockstep/call_home.h"
#include "../lockstep/popups.h"
#include "../lockstep/remote_sounds.h"
#include "../save/mod_save.h"
#include "../save/camera_keep.h"
#include "../lockstep/rename.h"

#include "../game/world.h"
#include "../game/camera.h"
#include "../game/panel2d.h"
#include "../game/minimap.h"
#include "../game/border_draw.h"
#include "../game/weather.h"
#include "../modes/republics/republics.h"
#include "version.h"
#include "profile.h"
#include "frame_timing.h"

// Binds every module to the host.
static void BindModules(const ModHost* host)
{
    ModHost_Bind(host);
    Profile_Bind(host);
    FrameTiming_Bind(host);
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
    ModSave_Bind(host);
    CameraKeep_Bind(host);
    RemoteSounds_Bind(host);
    Network_Bind(host);
    SaveSync_Bind(host);
    Workshop_Bind(host);
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
    SiteWatch_Bind(host);
    ProbeTrace_Bind(host);
    CitizenTrace_Bind(host);
    ProbeWatch_Bind(host);
    BuildingSettings_Bind(host);
    CitizenStats_Bind(host);
    EconomyStats_Bind(host);
    Republics_Bind(host);
}

// Installs the engine hooks and starts the background threads.
static int InstallHooks()
{
    Logf("republicmp  version=%s", REPUBLICMP_VERSION);
    Logf("republicmp  by %s - GPL v3, keep this line in any copy", REPUBLICMP_AUTHORS);

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

    bool okSiteWatch = SiteWatch_Install();
    Logf("republicmp  site watch hooks: %d%s", okSiteWatch,
         okSiteWatch ? "" : " - the construction site watch does not see sites made or destroyed");

    bool okLabels = Rename_InstallLabelRefresh();
    Logf("republicmp  city label refresh: %d%s", okLabels, okLabels ? "" : " - a shared city name shows when its card is opened");

    bool okCancel = CancelTool_Install();
    Logf("republicmp  network cancel tool: %d%s", okCancel, okCancel ? "" : " - the cancel tools are made on one PC only");

    bool okWorkers = WorldWorkers_Install();
    Logf("republicmp  workers without an office: %d%s", okWorkers,
         okWorkers ? "" : " - the construction menu's field stays each PC's own");

    bool okCancelPlans = CancelPlans_Install();
    Logf("republicmp  cancel all plans buttons: %d%s", okCancelPlans, okCancelPlans ? "" : " - cancel all plans acts on the clicking PC only");

    bool okPlanRemove = PlanRemove_Install();
    Logf("republicmp  factory plan removal: %d%s", okPlanRemove, okPlanRemove ? "" : " - the factory's remove from plan acts on the clicking PC only");

    bool okCarOwners = CarOwners_Install();
    Logf("republicmp  car owners reset: %d%s", okCarOwners, okCarOwners ? "" : " - the car window's reset acts on the clicking PC only");

    bool okLineSpacing = LineSpacing_Install();
    Logf("republicmp  line spacing button: %d%s", okLineSpacing, okLineSpacing ? "" : " - line spacing acts on the clicking PC only");

    bool okCrimeCancel = CrimeCancel_Install();
    Logf("republicmp  crime cancel button: %d%s", okCrimeCancel, okCrimeCancel ? "" : " - the crime list's cancel acts on the clicking PC only");

    bool okCallHome = CallHome_Install();
    Logf("republicmp  partner call home: %d%s", okCallHome, okCallHome ? "" : " - a partner building's call home acts on one PC");

    bool okPopups = Popups_Install();
    Logf("republicmp  popup callbacks: %d%s", okPopups, okPopups ? "" : " - popups act on the clicking PC only");

    bool okModSave = ModSave_Install();
    Logf("republicmp  save hook: %d%s", okModSave, okModSave ? "" : " - saves get no " MOD_SAVE_FILE " (views are not kept in them)");

    bool okWallets = Wallets_Install();
    Logf("republicmp  republics' accounts: %d%s", okWallets, okWallets ? "" : " - some of a tick's money stays split in two");

    bool okCitizenStats = CitizenStats_Install();
    Logf("republicmp  republics' citizen averages: %d%s", okCitizenStats, okCitizenStats ? "" : " - each player sees the whole map's");

    bool okEconomyStats = EconomyStats_Install();
    Logf("republicmp  republics' economy statistics: %d%s", okEconomyStats, okEconomyStats ? "" : " - no republic's share is kept");

    bool okSounds = RemoteSounds_Install();
    Logf("republicmp  remote command sounds: %d%s", okSounds, okSounds ? "" : " - other players' commands are heard here");

    bool okLinkSearch = Buildings_InstallLinkSearch();
    Logf("republicmp  building link search: %d%s", okLinkSearch,
         okLinkSearch ? "" : " - placed buildings get no conveyor, pipe or factory connections in a session");

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

// Called once by the proxy (xinput9_1_0.dll) on the game's main thread, before
// the game's first instruction. Non-zero: the main hook could not be installed.
extern "C" __declspec(dllexport) int RepublicMP_Start(void)
{
    BindModules(Host_Init());
    return InstallHooks();
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
