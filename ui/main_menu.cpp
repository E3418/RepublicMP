// See main_menu.h - layout ported from the ImRAD export (imrad\mp_panel.cpp),
// GLFW/MainWindow chrome stripped out. Host/Connect now drive net\save_sync
// (the everyday "get the save from the host" flow) - net\build_sync (the
// building-placement relay) moved to the Debug window (ui\debugui.cpp),
// since its network side is still rough/dev-only for now (see project docs).
#include "main_menu.h"
#include "debugui.h"
#include "ui_log.h"
#include "../hooks/imgui_hook.h" // g_imguiVisible - "Hide menu" button
#include "../net/save_sync.h"

#include "imgui.h"

// imrad.h ships both the interface and the implementation in one file - this
// is the one .cpp in the whole project that defines IMRAD_H_IMPLEMENTATION,
// so the ImRad::HBox/Spacing/SeparatorEx bodies get compiled exactly once.
#define IMRAD_H_IMPLEMENTATION
#include <imrad.h>

#include <vector>
#include <string>

MainMenu g_mainMenu;

void MainMenu::ResetLayout()
{
    hb2.Reset();
    hb4.Reset();
    hb5.Reset();
}

void MainMenu::Draw()
{
    if (!hostIpInitialized) {
        hostIp = SaveSync_GetTargetIp(); // prefill with mp_config.h's default so the field isn't blank
        hostIpInitialized = true;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("RepublicMP"))
    {
        // --- top row: Disconnect / Hide menu / Debug ---
        bool busy = SaveSync_IsBusy();

        hb2.BeginLayout();
        // While a host is just listening with nobody connected yet, there's
        // nothing to "disconnect" from - it reads as "End Hosting"/"Close"
        // instead, even though it calls the exact same
        // SaveSync_RequestDisconnect() (see save_sync.h). Always enabled,
        // not just while busy - it also tears down the build_sync AND
        // road_sync connections that outlive the one-shot save_sync flow
        // that started them.
        bool hostAwaitingClient = busy && SaveSync_IsHostRole() && SaveSync_IsHostAwaitingClient();
        const char* disconnectLabel = hostAwaitingClient ? "End Hosting" : "Disconnect";
        if (ImGui::Button(disconnectLabel, { hb2.GetSize(), 0 }))
            SaveSync_RequestDisconnect();
        hb2.AddSize(0 * ImGui::GetStyle().ItemSpacing.x, ImRad::HBox::Stretch(1.0f));

        ImGui::SameLine(0, 1 * ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Hide menu", { hb2.GetSize(), 0 }))
            g_imguiVisible = false; // INSERT brings it back (hooks/imgui_hook.cpp)
        hb2.AddSize(1 * ImGui::GetStyle().ItemSpacing.x, ImRad::HBox::Stretch(1.0f));

        ImGui::SameLine(0, 1 * ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Debug", { hb2.GetSize(), 0 }))
            g_debugui.Open();
        hb2.AddSize(1 * ImGui::GetStyle().ItemSpacing.x, ImRad::HBox::Stretch(1.0f));

        ImRad::Spacing(2);
        ImRad::SeparatorEx(ImRad::SeparatorFlags_Horizontal);

        // --- Host / Connect row - now drives save_sync (get/send the save),
        // not build_sync anymore (see file header comment). Disabled while a
        // run is in flight; the active role is tinted green. ---
        bool isHost = SaveSync_IsHostRole();
        ImGui::BeginDisabled(busy);

        hb4.BeginLayout();
        ImRad::Spacing(2);
        bool tintHost = busy && isHost;
        if (tintHost) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.2f, 1.0f));
        if (ImGui::Button("Host", { hb4.GetSize(), 0 }))
            SaveSync_StartHost();
        if (tintHost) ImGui::PopStyleColor();
        hb4.AddSize(0 * ImGui::GetStyle().ItemSpacing.x, ImRad::HBox::Stretch(1.0f));

        ImGui::SameLine(0, 1 * ImGui::GetStyle().ItemSpacing.x);
        bool tintClient = busy && !isHost;
        if (tintClient) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.2f, 1.0f));
        if (ImGui::Button("Connect", { hb4.GetSize(), 0 }))
            SaveSync_StartClient(hostIp.c_str());
        if (tintClient) ImGui::PopStyleColor();
        hb4.AddSize(1 * ImGui::GetStyle().ItemSpacing.x, ImRad::HBox::Stretch(1.0f));

        ImGui::EndDisabled();

        // --- Host IP row ---
        hb5.BeginLayout();
        ImRad::Spacing(1);
        ImGui::TextUnformatted("Host IP:");
        hb5.AddSize(0 * ImGui::GetStyle().ItemSpacing.x, ImRad::HBox::ItemSize);

        ImGui::SameLine(0, 1 * ImGui::GetStyle().ItemSpacing.x);
        ImGui::SetNextItemWidth(hb5.GetSize());
        ImGui::InputText("##hostIp", &hostIp);
        hb5.AddSize(1 * ImGui::GetStyle().ItemSpacing.x, ImRad::HBox::Stretch(1.0f));

        // --- Status ---
        ImRad::Spacing(1);
        ImGui::TextUnformatted("Status:");
        ImGui::SameLine(0, 2 * ImGui::GetStyle().ItemSpacing.x);
        // Kept deliberately generic for everything past the initial wait -
        // the flow has several distinct stages once a player is connected
        // (pausing / saving / zipping / sending / waiting for ready), each
        // already narrated in the log below via SaveStatus(), so this line
        // doesn't try to track which one is current. The one distinction
        // surfaced here: a host just listening for a player is NOT
        // paused/locked - a visibly different line so nobody mistakes
        // "waiting" for "frozen".
        if (busy && hostAwaitingClient)
            ImGui::TextColored(ImVec4(0.4f, 0.75f, 0.9f, 1.0f), "Hosting - waiting for a player (game not paused)");
        else if (busy)
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), isHost ? "Hosting - see log" : "Connecting - see log");
        else
            ImGui::TextDisabled("Idle");

        ImRad::Spacing(2);
        ImRad::SeparatorEx(ImRad::SeparatorFlags_Horizontal);

        // --- Log - global/everyday info only (connect, save loading,
        // progress %, errors), fed via SaveStatus() -> UiLog_Add(), see
        // net/save_sync.cpp. Building-sync's own verbose log lives in the
        // Debug window instead (ui/debugui.cpp, net/build_sync.cpp). ---
        ImRad::Spacing(2);
        ImGui::TextUnformatted("Log:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear"))
            UiLog_Clear();
        ImGui::SameLine();
        ImGui::Checkbox("Autoscroll", &autoScrollLog);

        if (ImGui::BeginChild("log", { -1, -1 }, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoSavedSettings))
        {
            std::vector<std::string> lines = UiLog_Snapshot();
            for (const std::string& line : lines)
                ImGui::TextUnformatted(line.c_str());

            if (autoScrollLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
