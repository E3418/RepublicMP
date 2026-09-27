// See main_menu.h.
#include "main_menu.h"
#include "debugui.h"
#include "ui_log.h"
#include "../hooks/imgui_hook.h" // g_imguiVisible - "Hide menu" button
#include "../net/save_sync.h"

#include "imgui.h"

// The single translation unit that compiles the imrad.h implementation.
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
        hostIp = SaveSync_GetTargetIp(); // prefill with the default host IP
        hostIpInitialized = true;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("RepublicMP"))
    {
        // --- top row: Disconnect / Hide menu / Debug ---
        bool busy = SaveSync_IsBusy();

        hb2.BeginLayout();
        // "End Hosting"/"Close" while a host is listening with nobody connected;
        // always enabled because it also closes the build/road sync connections.
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

        // --- Host / Connect row (save sync); the active role is tinted green ---
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
        // Once a player is connected the log narrates each stage; this line only
        // distinguishes "listening, not paused" from "syncing".
        if (busy && hostAwaitingClient)
            ImGui::TextColored(ImVec4(0.4f, 0.75f, 0.9f, 1.0f), "Hosting - waiting for a player (game not paused)");
        else if (busy)
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), isHost ? "Hosting - see log" : "Connecting - see log");
        else
            ImGui::TextDisabled("Idle");

        ImRad::Spacing(2);
        ImRad::SeparatorEx(ImRad::SeparatorFlags_Horizontal);

        // --- Everyday log (save sync); verbose sync logs are in the Debug window ---
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
