// See debugui.h.
#include "debugui.h"
#include "imgui.h"
#include <misc/cpp/imgui_stdlib.h> // ImGui::InputText(const char*, std::string*)
#include "../hooks/present_hook.h"
#include "../net/build_sync.h"
#include "../net/road_sync.h"
#include "../net/terrain_sync.h"
#include "../net/save_sync.h"
#include "debug_log.h"
#include <vector>
#include <string>

Debugui g_debugui;

void Debugui::Open()  { isOpen = true; }
void Debugui::Close() { isOpen = false; }

// Draws the Debug window: manual building/road sync connection controls and
// the verbose debug log.
void Debugui::Draw()
{
    if (!isOpen)
        return;

    static std::string s_buildSyncIp;
    static bool s_buildSyncIpInit = false;
    static std::string s_roadSyncIp;
    static bool s_roadSyncIpInit = false;
    static std::string s_terrainSyncIp;
    static bool s_terrainSyncIpInit = false;
    static bool s_autoScrollDebugLog = true;
    if (!s_buildSyncIpInit) {
        s_buildSyncIp = BuildSync_GetTargetIp();
        s_buildSyncIpInit = true;
    }
    if (!s_roadSyncIpInit) {
        s_roadSyncIp = RoadSync_GetTargetIp();
        s_roadSyncIpInit = true;
    }
    if (!s_terrainSyncIpInit) {
        s_terrainSyncIp = TerrainSync_GetTargetIp();
        s_terrainSyncIpInit = true;
    }

    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug", &isOpen))
    {
        ImGui::Text("Present calls: %lld", g_presentCallCount);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Building sync (started automatically by the session; these
        // buttons are for manual reconnects) ---
        ImGui::TextUnformatted("Building sync:");

        bool bsBusy = BuildSync_IsConnected() || BuildSync_IsConnecting();
        bool bsIsHost = BuildSync_IsHostRole();

        ImGui::BeginDisabled(bsBusy);
        if (ImGui::Button("Host##bs"))
            BuildSync_StartHost();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##bsIp", &s_buildSyncIp);
        ImGui::SameLine();
        if (ImGui::Button("Connect##bs"))
            BuildSync_StartClient(s_buildSyncIp.c_str());
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!bsBusy);
        if (ImGui::Button("Disconnect##bs"))
            BuildSync_Reset();
        ImGui::EndDisabled();

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (BuildSync_IsConnected())
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), bsIsHost ? "Hosting" : "Connected");
        else if (BuildSync_IsConnecting())
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), bsIsHost ? "Waiting for client..." : "Connecting...");
        else
            ImGui::TextDisabled("Idle");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Infrastructure sync (started automatically by the save-sync
        // flow; these buttons are for manual reconnects) ---
        ImGui::TextUnformatted("Infrastructure sync:");

        bool rsBusy = RoadSync_IsConnected() || RoadSync_IsConnecting();
        bool rsIsHost = RoadSync_IsHostRole();

        ImGui::BeginDisabled(rsBusy);
        if (ImGui::Button("Host##rs"))
            RoadSync_StartHost();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##rsIp", &s_roadSyncIp);
        ImGui::SameLine();
        if (ImGui::Button("Connect##rs"))
            RoadSync_StartClient(s_roadSyncIp.c_str());
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!rsBusy);
        if (ImGui::Button("Disconnect##rs"))
            RoadSync_Reset();
        ImGui::EndDisabled();

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (RoadSync_IsConnected())
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), rsIsHost ? "Hosting" : "Connected");
        else if (RoadSync_IsConnecting())
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), rsIsHost ? "Waiting for client..." : "Connecting...");
        else
            ImGui::TextDisabled("Idle");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Terrain sync (started automatically by the save-sync flow;
        // these buttons are for manual reconnects) ---
        ImGui::TextUnformatted("Terrain sync:");

        bool tsBusy = TerrainSync_IsConnected() || TerrainSync_IsConnecting();
        bool tsIsHost = TerrainSync_IsHostRole();

        ImGui::BeginDisabled(tsBusy);
        if (ImGui::Button("Host##ts"))
            TerrainSync_StartHost();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##tsIp", &s_terrainSyncIp);
        ImGui::SameLine();
        if (ImGui::Button("Connect##ts"))
            TerrainSync_StartClient(s_terrainSyncIp.c_str());
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!tsBusy);
        if (ImGui::Button("Disconnect##ts"))
            TerrainSync_Reset();
        ImGui::EndDisabled();

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (TerrainSync_IsConnected())
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), tsIsHost ? "Hosting" : "Connected");
        else if (TerrainSync_IsConnecting())
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), tsIsHost ? "Waiting for client..." : "Connecting...");
        else
            ImGui::TextDisabled("Idle");

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
                ImGui::TextUnformatted(line.c_str());

            if (s_autoScrollDebugLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
