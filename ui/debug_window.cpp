// SPDX-License-Identifier: GPL-3.0-only
// See debug_window.h.
#include "debug_window.h"
#include "imgui.h"
#include <misc/cpp/imgui_stdlib.h> // ImGui::InputText(const char*, std::string*)
#include "../game/present_hook.h"
#include "../world/terrain_hook.h"
#include "../session/save_sync.h"
#include "../determinism/determinism.h"
#include "../determinism/construction_view.h"
#include "../determinism/state_hash.h"
#include "../lockstep/building_settings.h"
#include "debug_log.h"
#include "../lockstep/lockstep_net.h"
#include "../core/frame_timing.h"
#include <vector>
#include <string>

DebugWindow g_debug_window;

void DebugWindow::Open()  { isOpen = true; }
void DebugWindow::Close() { isOpen = false; }

// Draws the Debug window: the lockstep state, the determinism probe, the
// diagnostic logs and the verbose debug log.
void DebugWindow::Draw()
{
    if (!isOpen)
        return;

    static bool s_autoScrollDebugLog = true;

    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug", &isOpen))
    {
        ImGui::Text("Present calls: %lld", g_presentCallCount);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Lockstep (lockstep/lockstep_net) ---
        if (SaveSync_IsBusy() && LockstepNet_IsConnected()) {
            bool paused = true;
            int speed = 1;
            Determinism_SessionSpeed(&paused, &speed);
            unsigned long long rtt = 0;
            bool rttKnown = LockstepNet_RoundTrip(&rtt);
            LockstepFlow f = LockstepNet_Flow();
            ImGui::TextUnformatted("Lockstep:");
            ImGui::TextWrapped("%s, tick %llu, %s (speed %d)", LockstepNet_IsHost() ? "host" : "player", Determinism_StepCount(),
                               paused ? "paused" : "running", speed);
            ImGui::TextWrapped("Ping %s ms, silent %llu ms", rttKnown ? std::to_string(rtt).c_str() : "?", LockstepNet_MsSinceReceived());
            ImGui::TextWrapped("%s %llu tick(s); waited for the player %d s in all, last %s ago",
                               LockstepNet_IsHost() ? "Player behind by" : "Grants waiting:", f.behind, f.stalls,
                               f.msSinceStall == ~0ull ? "never" : (std::to_string(f.msSinceStall / 1000) + " s").c_str());
            ImGui::TextWrapped("Last match after tick %llu; %s", LockstepNet_LastMatchedTick(),
                               LockstepNet_Desync()[0] ? LockstepNet_Desync() : "no difference");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        // --- Determinism probe (determinism/determinism) ---
        ImGui::TextUnformatted("Determinism probe:");
        static int s_probeLength = 1;
        static bool s_probeFuzz = false;
        static bool s_probeRecord = false;
    static bool s_probeLongHolds = false;
        static const int kProbeSteps[] = { 600, 1800, 3600, 10800 };
        static const char* const kProbeLabels[] = { "600 ticks", "1800 ticks", "3600 ticks", "10800 ticks" };
        ImGui::BeginDisabled(Probe_IsRunning() || SaveSync_IsBusy());
        ImGui::SetNextItemWidth(170);
        ImGui::Combo("##probeLength", &s_probeLength, kProbeLabels, IM_ARRAYSIZE(kProbeLabels));
        ImGui::SameLine();
        if (ImGui::Button("Run probe"))
            Probe_Start(kProbeSteps[s_probeLength], 30, s_probeFuzz, s_probeRecord, s_probeLongHolds);
        ImGui::Checkbox("Fuzz held frames and speed", &s_probeFuzz);
        ImGui::BeginDisabled(!s_probeFuzz);
        ImGui::Checkbox("Long holds (as with a high ping)", &s_probeLongHolds);
        ImGui::EndDisabled();
        ImGui::Checkbox("Build during run 1 (lockstep commands)", &s_probeRecord);
        ImGui::EndDisabled();
        ImGui::TextWrapped("Saves the game as 'mp_probe', then loads and simulates it twice and compares the "
                           "state after complete ticks. Fuzz holds 0-3 frames in run 2 (long holds: 4-35 frames "
                           "before one tick in four) and changes speed every 300 ticks. "
                           "Space may pause; avoid other actions. Not during a multiplayer session.");
        ImGui::TextWrapped("Result: %s", Probe_Status());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Latency emulation (lockstep/lockstep_net) ---
        static const int kDelays[] = { 0, 50, 100, 150, 250, 400 };
        static const char* const kDelayLabels[] = { "Off", "50 ms", "100 ms", "150 ms", "250 ms", "400 ms" };
        int delay = 0;
        for (int i = 0; i < IM_ARRAYSIZE(kDelays); i++)
            if (kDelays[i] == LockstepNet_GetAddedDelay()) delay = i;
        ImGui::SetNextItemWidth(130);
        if (ImGui::Combo("Added latency", &delay, kDelayLabels, IM_ARRAYSIZE(kDelayLabels)))
            LockstepNet_SetAddedDelay(kDelays[delay]);
        ImGui::TextWrapped("Off by default, for LAN tests: what this PC receives in a session is held this long. "
                           "Set it on both PCs - the round trip grows by twice the value.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Frame timing (core/frame_timing) ---
        bool frameTiming = FrameTiming_GetOn();
        if (ImGui::Checkbox("Frame timing (every 2 s)", &frameTiming)) FrameTiming_SetOn(frameTiming);
        ImGui::TextWrapped("Off by default: logs the main thread's frames and the ms per frame of the game's "
                           "phases and this plugin's work in them, to find what makes a PC slow.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Terrain call log (world/terrain_hook) ---
        static const char* const kTerrainLog[] = { "Off", "Per frame", "Per call" };
        int terrainLog = TerrainHook_GetLogMode();
        ImGui::SetNextItemWidth(130);
        if (ImGui::Combo("Terrain call log", &terrainLog, kTerrainLog, IM_ARRAYSIZE(kTerrainLog))) TerrainHook_SetLogMode(terrainLog);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Money trace (determinism) ---
        bool moneyTrace = Determinism_GetMoneyTrace();
        if (ImGui::Checkbox("Money trace (lockstep desyncs)", &moneyTrace)) Determinism_SetMoneyTrace(moneyTrace);
        ImGui::TextWrapped("On by default: on a desync each log gets the balances and construction sites "
                           "of the ticks before it, and every command that changed the money.");
        ImGui::Spacing();

        bool storageTrace = StateHash_GetStorageTrace();
        if (ImGui::Checkbox("Storage resource trace (lockstep desyncs)", &storageTrace)) StateHash_SetStorageTrace(storageTrace);
        ImGui::TextWrapped("Off by default: enable on both PCs before the test. Desync records include resource names "
                           "and exact amounts for every slot; up to 32 stock changes left by the UI are logged as STORAGE_UI. "
                           "Command-created or changed material requirements are logged as CONSTRUCTION_RECIPE (up to 512 lines). "
                           "This adds per-frame reads while enabled.");
        ImGui::Spacing();

        // --- Construction sites (determinism/construction_view) ---
        bool siteLog = ConstructionView_GetLog();
        if (ImGui::Checkbox("Log construction sites (every 2 s)", &siteLog)) ConstructionView_SetLog(siteLog);
        ImGui::TextWrapped("The stages are always drawn with this PC's view; the log shows the sites the drawer sees, "
                           "with the simulation's and this PC's camera fields.");
        ImGui::Spacing();

        // --- Field diff (lockstep/building_settings) ---
        bool fieldDiff = BuildingSettings_GetFieldDiff();
        if (ImGui::Checkbox("Log building fields changed by clicks (FIELD_DIFF)", &fieldDiff))
            BuildingSettings_SetFieldDiff(fieldDiff);
        ImGui::TextWrapped("For finding window settings that are not synced yet: open the building's window, "
                           "leave it a few seconds, then click the setting. The changed bytes go to the log.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Verbose sync log (kept out of the main window's log) ---
        ImGui::TextUnformatted("Debug log:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##dbg"))
            DebugLog_Clear();
        ImGui::SameLine();
        ImGui::Checkbox("Autoscroll##dbg", &s_autoScrollDebugLog);

        if (ImGui::BeginChild("dbglog", { -1, -1 }, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoSavedSettings))
        {
            std::vector<std::string> lines = DebugLog_Snapshot();
            for (const std::string& line : lines)
                ImGui::TextWrapped("%s", line.c_str());

            if (s_autoScrollDebugLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
