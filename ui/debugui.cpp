#include "debugui.h"
#include "imgui.h"
#include <misc/cpp/imgui_stdlib.h> // ImGui::InputText(const char*, std::string*)
#include "../hooks/present_hook.h" // g_presentCallCount
#include "../net/build_sync.h"
#include "../net/road_sync.h"
#include "../net/save_sync.h" // SaveSync_IsHostRole/GetTargetIp - "mirror save sync" button below
#include "../hooks/detect_spawn.h" // DetectSpawn_GetLast{Road,Pipe}Snapshot - self-test button below
#include "debug_log.h"
#include <vector>
#include <string>

Debugui g_debugui;

void Debugui::Open()  { isOpen = true; }
void Debugui::Close() { isOpen = false; }

void Debugui::Draw()
{
    if (!isOpen)
        return;

    // Function-local statics instead of class members - this window is kept
    // deliberately simple/hand-written (see debugui.h), no need to touch the
    // header for a couple of widget-persistence fields.
    static std::string s_buildSyncIp;
    static bool s_buildSyncIpInit = false;
    static std::string s_roadSyncIp;
    static bool s_roadSyncIpInit = false;
    static bool s_autoScrollDebugLog = true;
    if (!s_buildSyncIpInit) {
        s_buildSyncIp = BuildSync_GetTargetIp();
        s_buildSyncIpInit = true;
    }
    if (!s_roadSyncIpInit) {
        s_roadSyncIp = RoadSync_GetTargetIp();
        s_roadSyncIpInit = true;
    }

    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug", &isOpen))
    {
        ImGui::Text("Present calls: %lld", g_presentCallCount);
        // TODO: F9 spawn count / thisPtr diagnostics from hooks/detect_spawn.cpp,
        // if/when it exposes a counter to read from here.

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Building sync (moved here from the main menu - it's still a
        // rough/dev-only feature, see net/build_sync.cpp and project docs; a
        // proper automatic connect comes later once the UI/sync story as a
        // whole is settled) ---
        ImGui::TextUnformatted("Building sync (dev):");

        bool bsBusy = BuildSync_IsConnected() || BuildSync_IsConnecting();
        bool bsIsHost = BuildSync_IsHostRole();

        // One-click shortcut: reuses whichever role/IP the save-sync run
        // (ui/main_menu.cpp's Host/Connect) already used, instead of typing
        // the IP a second time into the field below. Only meaningful after
        // a save-sync run has actually happened this session.
        ImGui::BeginDisabled(bsBusy);
        if (ImGui::Button("Enable building sync (same role/IP as save sync)"))
        {
            if (SaveSync_IsHostRole())
                BuildSync_StartHost();
            else
                BuildSync_StartClient(SaveSync_GetTargetIp());
        }
        ImGui::EndDisabled();

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

        // --- Road/pipe sync (dev, see net/road_sync.cpp) - own connection,
        // own port, same pattern as building sync above. Also auto-started
        // alongside building sync at the end of the main Host/Connect
        // (save_sync) flow - see net/save_sync.cpp's
        // RunHostSendSaveFlow/RunClientReceiveSaveFlow. These buttons stay
        // for manual/standalone testing (idempotent - a click here after
        // the auto-start already connected is a harmless no-op, same as
        // building sync's own buttons). ---
        ImGui::TextUnformatted("Road/pipe sync (dev, UNVERIFIED - self-test first):");

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

        // Self-test: replays your OWN last captured segment against your OWN
        // local manager, offset sideways so it lands next to the original
        // instead of on top of it - no second machine, no network, needed
        // before ever trusting RoadSync_ApplySegment with real remote data.
        // Build a road/pipe segment first (see detect_spawn.cpp TOOL_COMMIT
        // logs), then press the matching button below and check in-game/log
        // whether a second copy appeared where expected.
        if (ImGui::Button("Self-test: replay last ROAD (offset +15m X)"))
        {
            int typeId, variant, count;
            float curveValue;
            static float points[ROAD_SYNC_MAX_POINTS][3];
            if (DetectSpawn_GetLastRoadSnapshot(&typeId, &variant, points, &count, &curveValue)) {
                for (int i = 0; i < count; i++) points[i][0] += 15.0f;
                RoadSync_ApplySegment(ROAD_SYNC_CATEGORY_ROAD, typeId, variant, points, count, curveValue);
            } else {
                DebugLog_Add("[ROAD_SYNC] self-test: no valid ROAD segment captured yet this session");
            }
        }
        if (ImGui::Button("Self-test: replay last PIPE (offset +15m X)"))
        {
            int typeId, variant, count;
            float curveValue;
            static float points[ROAD_SYNC_MAX_POINTS][3];
            if (DetectSpawn_GetLastPipeSnapshot(&typeId, &variant, points, &count, &curveValue)) {
                for (int i = 0; i < count; i++) points[i][0] += 15.0f;
                RoadSync_ApplySegment(ROAD_SYNC_CATEGORY_PIPE, typeId, variant, points, count, curveValue);
            } else {
                DebugLog_Add("[ROAD_SYNC] self-test: no valid PIPE segment captured yet this session");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Verbose debug log - building-sync connection/placement events
        // (net/build_sync.cpp's BuildSyncStatus()), kept out of the main
        // window's log so that one stays readable for everyday use. ---
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
