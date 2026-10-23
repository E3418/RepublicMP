// SPDX-License-Identifier: GPL-3.0-only
// See main_menu.h.
#include "main_menu.h"
#include "debug_window.h"
#include "ui_log.h"
#include "sync_block.h"
#include "../game/imgui_hook.h" // g_imguiVisible - "Hide menu" button
#include "../session/save_sync.h"
#include "../lockstep/lockstep_net.h"
#include "../lockstep/presence.h"
#include "../modes/mode.h"
#include "../determinism/determinism.h"
#include "../core/profile.h"
#include "../transport/network.h" // NetworkSteamId

#include "imgui.h"
#include <misc/cpp/imgui_stdlib.h> // ImGui::InputText(const char*, std::string*)

#include <vector>
#include <string>
#include <cstdio>
#include <cstdarg>

MainMenu g_mainMenu;

static const ImVec4 kGrey   = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
static const ImVec4 kBlue   = ImVec4(0.4f, 0.75f, 0.95f, 1.0f);
static const ImVec4 kYellow = ImVec4(0.95f, 0.8f, 0.25f, 1.0f);
static const ImVec4 kGreen  = ImVec4(0.35f, 0.9f, 0.35f, 1.0f);
static const ImVec4 kRed    = ImVec4(0.95f, 0.4f, 0.35f, 1.0f);

// Text in a colour that wraps at the window's edge.
static void Wrapped(const ImVec4& color, const char* fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

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

// The current join step (from the sync screen), e.g. "Step 4/7 - Sending...  62%".
static void JoinStepLine()
{
    int step, total, percent;
    char text[160];
    if (!SyncBlock_GetStep(&step, &total, text, sizeof(text), &percent)) {
        ImGui::TextColored(kGrey, "Preparing...");
        return;
    }
    if (total > 0) Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), "Step %d/%d - %s", step, total, text);
    else Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", text);
    if (percent >= 0) {
        char overlay[16];
        _snprintf_s(overlay, sizeof(overlay), _TRUNCATE, "%d%%", percent);
        ImGui::ProgressBar(percent / 100.0f, ImVec2(-1, 0), overlay);
    }
}

static void MinutesSeconds(char* out, size_t n, unsigned long long ms)
{
    _snprintf_s(out, n, _TRUNCATE, "%llu:%02llu", ms / 60000, (ms / 1000) % 60);
}

// The session's players, the host first: ping and how far behind (the host also sees addresses).
static void PlayersList(bool host)
{
    LockstepPlayer players[PROFILE_MAX_PLAYERS];
    int n = LockstepNet_Players(players, PROFILE_MAX_PLAYERS);
    if (!n) {
        Wrapped(kGrey, "Players: waiting for the list...");
        return;
    }
    ImGui::TextColored(kGrey, "Players:");
    for (int i = 0; i < n; i++) {
        const LockstepPlayer& pl = players[i];
        const char* you = pl.self ? " - you" : "";
        if (pl.host) {
            StatusDot(kBlue);
            Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s (host)%s", pl.nick, you);
            continue;
        }
        ImVec4 col = pl.rttMs < 0 ? kGrey : pl.rttMs < 80 ? kGreen : pl.rttMs < 250 ? kYellow : kRed;
        StatusDot(col);
        char ping[24];
        if (pl.rttMs < 0) strcpy_s(ping, "?");
        else _snprintf_s(ping, sizeof(ping), _TRUNCATE, "%d ms", pl.rttMs);
        if (host && pl.ip[0])
            Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s%s - ping %s, %llu tick(s) behind  (%s)", pl.nick, you, ping, pl.behind, pl.ip);
        else
            Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s%s - ping %s, %llu tick(s) behind", pl.nick, you, ping, pl.behind);
    }
}

// The ping key: the button takes the next key or mouse button pressed (Esc keeps the old one).
static void PingKey()
{
    static bool s_waiting = false;
    static bool s_down[256];
    char name[32], label[64];
    Presence_KeyName(Presence_GetPingKey(), name, sizeof(name));
    if (s_waiting) strcpy_s(label, "Press a key...##pingKey");
    else _snprintf_s(label, sizeof(label), _TRUNCATE, "%s##pingKey", name);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Ping key:");
    ImGui::SameLine();
    if (ImGui::Button(label, ImVec2(140, 0)) && !s_waiting) {
        s_waiting = true;
        for (int vk = 0; vk < 256; vk++) s_down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        return;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Point at a place on the map and press it: every player sees a ping there.\n"
                          "Click to choose another key or mouse button (Esc keeps this one).");
    if (!s_waiting) return;
    for (int vk = 1; vk < 256; vk++) {
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !s_down[vk] && (vk == VK_ESCAPE || Presence_SetPingKey(vk))) {
            s_waiting = false;
            break;
        }
        s_down[vk] = down;
    }
}

// Lockstep in the game: the simulation, the players, how the ticks flow and the comparison.
static void LockstepPanel(bool host)
{
    bool paused = true;
    int speed = 1;
    Determinism_SessionSpeed(&paused, &speed);
    unsigned long long tick = Determinism_StepCount();
    if (paused) Wrapped(kYellow, "Simulation: paused at tick %llu", tick);
    else {
        static const char* const kSpeeds[] = { "", "normal", "fast", "Hyper", "Plaid" };
        Wrapped(kGreen, "Simulation: running, tick %llu (%s)", tick, kSpeeds[speed < 1 ? 1 : speed > 4 ? 4 : speed]);
    }

    PlayersList(host);
    bool cursors = Presence_GetShowCursors();
    if (ImGui::Checkbox("Show the players' cursors", &cursors)) Presence_SetShowCursors(cursors);
    PingKey();

    LockstepFlow f = LockstepNet_Flow();
    if (host) {
        if (f.msSinceStall < 5000)
            Wrapped(kYellow, "Flow: waiting for a player (%llu ticks behind) - the slowest PC sets the pace", f.behind);
        else
            Wrapped(kGreen, "Flow: steady (at most %llu tick(s) behind)", f.behind);
    } else {
        if (f.behind > LOCKSTEP_WINDOW / 2)
            Wrapped(kYellow, "Flow: catching up, %llu tick(s) waiting", f.behind);
        else
            Wrapped(kGreen, "Flow: steady");
    }

    const char* desync = LockstepNet_Desync();
    unsigned long long matched = LockstepNet_LastMatchedTick();
    if (desync[0]) {
        Wrapped(kRed, "Sync: %s", desync);
        ULONGLONG left = host ? SaveSync_GetMsUntilDesyncResync() : 0;
        if (left) {
            char t[16];
            MinutesSeconds(t, sizeof(t), left);
            Wrapped(kYellow, "The game goes on; re-sync in %s.", t);
        } else {
            Wrapped(kGrey, host ? "The game goes on; re-sync when you choose." : "The game goes on until the host re-syncs.");
        }
    } else if (host && matched) {
        Wrapped(kGreen, "Sync: the games match (checked after tick %llu)", matched);
    } else {
        Wrapped(kGrey, "Sync: compared every %d ticks%s", LOCKSTEP_HASH_TICKS, host ? "" : " by the host");
    }

    if (!host) return;
    ImGui::Spacing();
    if (ImGui::Button("Re-sync now")) SaveSync_RequestResync();
    ImGui::SameLine();
    static const int kSeconds[] = { -1, 0, 60, 120, 300 };
    static const char* const kLabels[] = { "Off", "immediately", "after 1 min", "after 2 min", "after 5 min" };
    int current = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kSeconds); i++)
        if (kSeconds[i] == SaveSync_GetDesyncResyncSeconds()) current = i;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("After a desync:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    if (ImGui::Combo("##desyncResync", &current, kLabels, IM_ARRAYSIZE(kLabels))) SaveSync_SetDesyncResyncSeconds(kSeconds[current]);
    Wrapped(kGrey, "Re-sync sends your world to every player again; all games then go on from it.");
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
        if (lastError[0]) Wrapped(kRed, "Last session: %s", lastError);
        break;

    case SESSION_HOST_LISTENING:
        if (SaveSync_IsResyncing()) {
            StatusDot(kYellow);
            Wrapped(kYellow, "Re-syncing - waiting for %s to reconnect", peer[0] ? peer : "the player");
            break;
        }
        StatusDot(kBlue);
        Wrapped(kBlue, "Hosting - waiting for players (up to %d)", SaveSync_MaxPlayers() - 1);
        Wrapped(kGrey, "Your game keeps running. Players connect to your IP (port %d).", SAVE_SYNC_PORT);
        if (lastError[0]) Wrapped(kYellow, "%s", lastError);
        break;

    case SESSION_HOST_JOINING:
        StatusDot(kYellow);
        Wrapped(kYellow, SaveSync_IsResyncing() ? "Re-syncing - sending the world to %s" : "Hosting - joining: %s",
                peer[0] ? peer : "a player");
        JoinStepLine();
        break;

    case SESSION_HOST_IN_GAME:
        StatusDot(kGreen);
        Wrapped(kGreen, "Hosting - in the game with %s", peer[0] ? peer : "a player");
        LockstepPanel(true);
        break;

    case SESSION_CLIENT_JOINING:
        StatusDot(kYellow);
        Wrapped(kYellow, SaveSync_IsResyncing() ? "Re-syncing with %s" : "Joining %s", peer[0] ? peer : SaveSync_GetTargetIp());
        JoinStepLine();
        break;

    case SESSION_CLIENT_IN_GAME:
        StatusDot(kGreen);
        Wrapped(kGreen, "In the game of %s", peer[0] ? peer : SaveSync_GetTargetIp());
        LockstepPanel(false);
        break;
    }
}

// The player's name (remembered in the profile).
static void NameInput()
{
    static char s_nick[PROFILE_NICK_BYTES];
    static bool s_nickRead = false;
    if (!s_nickRead) {
        strncpy_s(s_nick, Profile_GetNick(), _TRUNCATE);
        if (!strcmp(s_nick, "Player")) s_nick[0] = 0;
        s_nickRead = true;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Your name:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##nick", "Player", s_nick, sizeof(s_nick));
    if (ImGui::IsItemDeactivatedAfterEdit()) Profile_SetNick(s_nick);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the other players see. Two players may use the same name.");
}

// Host/Connect while idle; Stop hosting / Disconnect during a session.
static void SessionButtons(std::string& hostIp)
{
    SessionState st = SaveSync_GetState();
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;

    if (st == SESSION_IDLE) {
        static const char* const kPlayers[] = { "2 players", "3 players", "4 players" };
        int players = Profile_GetMaxPlayers() - PROFILE_MIN_PLAYERS;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Session:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        if (ImGui::Combo("##maxPlayers", &players, kPlayers, IM_ARRAYSIZE(kPlayers)))
            Profile_SetMaxPlayers(players + PROFILE_MIN_PLAYERS);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The most players in your session, you included (the host's choice).\n"
                              "A full session stops listening for more.\n"
                              "A player who joins during the game brings a re-sync for everyone.");
        bool dlcCheck = SaveSync_GetDlcCheck();
        if (ImGui::Checkbox("Check DLCs", &dlcCheck)) SaveSync_SetDlcCheck(dlcCheck);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Refuse a join when the two games have different DLCs installed:\n"
                              "without a DLC its maps, buildings and vehicles are drawn wrong or not at all.\n"
                              "Untick it on either PC to play anyway, on a map with no DLC content.");
        ImGui::Spacing();

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

        NameInput();
        PingKey();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Host:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##hostIp", "IP or SteamID64", &hostIp);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("An IP connects over TCP (LAN or VPN, port 5556 and 5560 open on the host).\n"
                              "A SteamID64 connects through Steam: no IP and no open port.\n"
                              "Steam friends can also use Join Game or an invite while you host.");
        if (unsigned long long id = NetworkSteamId()) {
            char idText[32];
            snprintf(idText, sizeof(idText), "%llu", id);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Your SteamID64: %s", idText);
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(idText);
        }
        if (!world) Wrapped(kGrey, "Host is available once a city is loaded.");
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
}

// ---------------------------------------------------------------------------
// Pages: the start page picks the kind of game; a session always shows co-op.
// ---------------------------------------------------------------------------

enum MenuPage { PAGE_START, PAGE_COOP, PAGE_REPUBLICS };
static MenuPage s_page = PAGE_START;

static ImU32 Col(const ImVec4& c) { return ImGui::GetColorU32(c); }

// A wide button with a coloured edge, a title and two lines under it.
static bool ModeCard(const char* id, const char* title, const char* line1, const char* line2, const ImVec4& accent)
{
    float lh = ImGui::GetTextLineHeightWithSpacing();
    ImVec2 size(ImGui::GetContentRegionAvail().x, lh * 3 + 14);
    bool clicked = ImGui::Button(id, size);
    ImVec2 min = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, ImVec2(min.x + 4, min.y + size.y), Col(accent));
    ImVec2 at(min.x + 14, min.y + 7);
    dl->AddText(at, Col(accent), title);
    dl->AddText(ImVec2(at.x, at.y + lh), Col(ImGui::GetStyleColorVec4(ImGuiCol_Text)), line1);
    dl->AddText(ImVec2(at.x, at.y + lh * 2), Col(kGrey), line2);
    return clicked;
}

static void StartPage()
{
    ImGui::TextUnformatted("Choose a game");
    ImGui::Spacing();
    if (ModeCard("##coop", "Co-op", "One republic, 2-4 players", "Every PC runs the same city in lockstep", kGreen))
        s_page = PAGE_COOP;
    ImGui::Spacing();
    const RmpMode* republics = Modes_Republics();
    if (republics && ModeCard("##republics", republics->title, republics->line1, republics->line2, kBlue))
        s_page = PAGE_REPUBLICS;
    ImGui::Spacing();
    NameInput();
}

static bool BackButton()
{
    bool back = ImGui::SmallButton("< Back");
    ImGui::SameLine();
    return back;
}

// The page of the mode plugged in under modes/ (modes/mode.h).
static void RepublicsPage()
{
    const RmpMode* mode = Modes_Republics();
    if (BackButton() || !mode) {
        s_page = PAGE_START;
        return;
    }
    ImGui::TextColored(kBlue, "%s", mode->title);
    ImGui::Spacing();
    if (mode->drawPage) mode->drawPage();
}

static void CoopPage(std::string& hostIp)
{
    // A session stays on this page; back only while idle.
    if (SaveSync_GetState() == SESSION_IDLE) {
        if (BackButton()) s_page = PAGE_START;
        ImGui::TextColored(kGreen, "Co-op");
        ImGui::Spacing();
    }
    StatusPanel();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    SessionButtons(hostIp);
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
        if (SaveSync_GetState() != SESSION_IDLE) s_page = PAGE_COOP;
        switch (s_page) {
        case PAGE_START: StartPage(); break;
        case PAGE_COOP: CoopPage(hostIp); break;
        case PAGE_REPUBLICS: RepublicsPage(); break;
        }

        ImGui::Spacing();
        ImGui::Separator();
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
                ImGui::TextWrapped("%s", line.c_str());

            if (autoScrollLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
