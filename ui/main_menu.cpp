// See main_menu.h.
#include "main_menu.h"
#include "debug_window.h"
#include "ui_log.h"
#include "sync_block.h"
#include "../game/imgui_hook.h" // g_imguiVisible - "Hide menu" button
#include "../session/save_sync.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/terrain/terrain_sync.h"
#include "../session/sync_check.h"

#include "imgui.h"
#include <misc/cpp/imgui_stdlib.h> // ImGui::InputText(const char*, std::string*)

#include <vector>
#include <string>
#include <cstdio>

MainMenu g_mainMenu;

static const ImVec4 kGrey   = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
static const ImVec4 kBlue   = ImVec4(0.4f, 0.75f, 0.95f, 1.0f);
static const ImVec4 kYellow = ImVec4(0.95f, 0.8f, 0.25f, 1.0f);
static const ImVec4 kGreen  = ImVec4(0.35f, 0.9f, 0.35f, 1.0f);
static const ImVec4 kRed    = ImVec4(0.95f, 0.4f, 0.35f, 1.0f);

// A coloured dot followed by text on the same line.
static void StatusDot(const ImVec4& color)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.4f, p.y + h * 0.5f), h * 0.3f,
                                                ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(h * 0.9f, h));
    ImGui::SameLine();
}

// "Buildings  Roads  Terrain" with a green/red dot each.
static void ChannelIndicators()
{
    struct { const char* name; bool up; } ch[] = {
        { "Buildings", BuildSync_IsConnected() },
        { "Roads", RoadSync_IsConnected() },
        { "Terrain", TerrainSync_IsConnected() },
    };
    ImGui::TextColored(kGrey, "Live sync:");
    for (auto& c : ch) {
        ImGui::SameLine(0, 12);
        StatusDot(c.up ? kGreen : kRed);
        ImGui::TextUnformatted(c.name);
    }
}

// The current join step (from the sync screen), e.g. "Step 4/7 - Sending...  62%".
static void JoinStepLine()
{
    int step, total, percent;
    char text[160];
    if (!SyncBlock_GetStep(&step, &total, text, sizeof(text), &percent)) {
        ImGui::TextColored(kGrey, "Preparing...");
        return;
    }
    if (total > 0) ImGui::Text("Step %d/%d - %s", step, total, text);
    else ImGui::TextUnformatted(text);
    if (percent >= 0) {
        char overlay[16];
        _snprintf_s(overlay, sizeof(overlay), _TRUNCATE, "%d%%", percent);
        ImGui::ProgressBar(percent / 100.0f, ImVec2(-1, 0), overlay);
    }
}

// Host with a player in the game: re-sync now, and the automatic interval.
static void ResyncControls()
{
    static const int kMinutes[] = { 0, 5, 10, 15, 30, 60 };
    static const char* const kLabels[] = { "Off", "every 5 min", "every 10 min", "every 15 min",
                                           "every 30 min", "every 60 min" };
    int current = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kMinutes); i++)
        if (kMinutes[i] == SaveSync_GetAutoResyncMinutes()) current = i;

    if (ImGui::Button("Re-sync now"))
        SaveSync_RequestResync();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Auto:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    if (ImGui::Combo("##autoResync", &current, kLabels, IM_ARRAYSIZE(kLabels)))
        SaveSync_SetAutoResyncMinutes(kMinutes[current]);
    ULONGLONG left = SaveSync_GetMsUntilAutoResync();
    if (left) {
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "next in %llu:%02llu", left / 60000, (left / 1000) % 60);
    }
    ImGui::TextColored(kGrey, "Re-sync sends your world to the player again (vehicles, citizens, production).");

    // Drift measured by the hourly sync check (session/sync_check).
    SyncDrift d = SyncCheck_GetDrift();
    if (d.valid) {
        ImVec4 col = d.total < 2.0f ? kGreen : d.total < 10.0f ? kYellow : kRed;
        ImGui::TextColored(col, "Drift %.1f%%", d.total);
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "(day %d %02d:00 - citizens %d/%d, vehicles %d/%d, storage %.1f%%)", d.day,
                           d.hour, d.hostCitizens, d.clientCitizens, d.hostVehicles, d.clientVehicles, d.storage);
    } else {
        ImGui::TextColored(kGrey, "Drift: measured every game hour");
    }
    static const int kLimits[] = { 0, 2, 5, 10, 20 };
    static const char* const kLimitLabels[] = { "Off", "above 2%", "above 5%", "above 10%", "above 20%" };
    int limit = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kLimits); i++)
        if (kLimits[i] == SyncCheck_GetAutoResyncPercent()) limit = i;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Re-sync on drift:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("##driftResync", &limit, kLimitLabels, IM_ARRAYSIZE(kLimitLabels)))
        SyncCheck_SetAutoResyncPercent(kLimits[limit]);
    // Why an automatic re-sync has not happened yet (see session/sync_check.h).
    if (kLimits[limit] > 0 && d.valid && d.overLimitChecks > 0) {
        ImGui::SameLine();
        unsigned long long wait = SyncCheck_GetResyncCooldownMs();
        if (d.overLimitChecks < SYNC_CHECK_CONFIRM_CHECKS)
            ImGui::TextColored(kYellow, "above for %d/%d hours", d.overLimitChecks, SYNC_CHECK_CONFIRM_CHECKS);
        else if (wait)
            ImGui::TextColored(kYellow, "waiting %llu:%02llu after the last re-sync", wait / 60000, (wait / 1000) % 60);
    }
}

static void StatusPanel()
{
    SessionState st = SaveSync_GetState();
    const char* peer = SaveSync_GetPeerIp();
    const char* lastError = SaveSync_GetLastError();

    switch (st) {
    case SESSION_IDLE:
        StatusDot(kGrey);
        ImGui::TextColored(kGrey, "Not in a multiplayer session");
        if (lastError[0]) ImGui::TextColored(kRed, "Last session: %s", lastError);
        break;

    case SESSION_HOST_LISTENING:
        if (SaveSync_IsResyncing()) {
            StatusDot(kYellow);
            ImGui::TextColored(kYellow, "Re-syncing - waiting for %s to reconnect", peer[0] ? peer : "the player");
            break;
        }
        StatusDot(kBlue);
        ImGui::TextColored(kBlue, "Hosting - waiting for a player");
        ImGui::TextColored(kGrey, "Your game keeps running. Players connect to your IP (port %d).", SAVE_SYNC_PORT);
        if (lastError[0]) ImGui::TextColored(kYellow, "%s", lastError);
        break;

    case SESSION_HOST_JOINING:
        StatusDot(kYellow);
        ImGui::TextColored(kYellow, SaveSync_IsResyncing() ? "Re-syncing - sending the world to %s"
                                                           : "Hosting - %s is joining", peer[0] ? peer : "a player");
        JoinStepLine();
        break;

    case SESSION_HOST_IN_GAME:
        StatusDot(kGreen);
        ImGui::TextColored(kGreen, "Hosting - %s is in the game", peer[0] ? peer : "a player");
        ChannelIndicators();
        ResyncControls();
        break;

    case SESSION_CLIENT_JOINING:
        StatusDot(kYellow);
        ImGui::TextColored(kYellow, SaveSync_IsResyncing() ? "Re-syncing with %s" : "Joining %s",
                           peer[0] ? peer : SaveSync_GetTargetIp());
        JoinStepLine();
        break;

    case SESSION_CLIENT_IN_GAME:
        StatusDot(kGreen);
        ImGui::TextColored(kGreen, "Connected to %s - in the game", peer[0] ? peer : SaveSync_GetTargetIp());
        ChannelIndicators();
        break;
    }
}

// Host/Connect while idle; Stop hosting / Disconnect during a session.
static void SessionButtons(std::string& hostIp)
{
    SessionState st = SaveSync_GetState();
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;

    if (st == SESSION_IDLE) {
        bool world = SaveSync_IsWorldLoaded();
        ImGui::BeginDisabled(!world);
        if (ImGui::Button("Host", ImVec2(half, 0)))
            SaveSync_StartHost();
        ImGui::EndDisabled();
        if (!world && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Load a city first - the joining player gets your world.");
        ImGui::SameLine();
        ImGui::BeginDisabled(hostIp.empty());
        if (ImGui::Button("Connect", ImVec2(half, 0)))
            SaveSync_StartClient(hostIp.c_str());
        ImGui::EndDisabled();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Host IP:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##hostIp", &hostIp);
        if (!world) ImGui::TextColored(kGrey, "Host is available once a city is loaded.");
        return;
    }

    bool host = SaveSync_IsHostRole();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.25f, 0.25f, 1.0f));
    ImGui::BeginDisabled(SaveSync_GetSessionAgeMs() < SAVE_SYNC_STOP_BUTTON_DELAY_MS);
    if (ImGui::Button(host ? "Stop hosting" : "Disconnect", ImVec2(-1, 0)))
        SaveSync_RequestDisconnect();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    ImGui::TextColored(kGrey, host ? "Hosting - Connect is unavailable until you stop."
                                   : "In a session - Host is unavailable until you disconnect.");
}

void MainMenu::Draw()
{
    if (!hostIpInitialized) {
        hostIp = SaveSync_GetTargetIp(); // prefill with the default host IP
        hostIpInitialized = true;
    }

    ImGui::SetNextWindowSize(ImVec2(440, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("RepublicMP"))
    {
        StatusPanel();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        SessionButtons(hostIp);

        ImGui::Spacing();
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        if (ImGui::Button("Hide menu (INSERT)", ImVec2(half, 0)))
            g_imguiVisible = false; // INSERT brings it back (game/imgui_hook.cpp)
        ImGui::SameLine();
        if (ImGui::Button("Debug", ImVec2(half, 0)))
            g_debug_window.Open();

        ImGui::TextColored(kGrey, "RepublicMP %s  build %08X", SaveSync_GetLocalVersion(), SaveSync_GetLocalDllCrc());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Everyday log; verbose sync logs are in the Debug window ---
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
