// SPDX-License-Identifier: GPL-3.0-only
// See multiplayer_page.h. Geometry and colours are the game's root menu's
// (0x261210 / 0x274E70, SOVIET64.exe 1.1.1.9): positions are multiples of the
// menu's scale s, the panel is menu\strip*.png at 87%, the rows are the
// "Square" font 32s high, red with a gold outline, white under the mouse.
#include "multiplayer_page.h"
#include "game_assets.h"
#include "ui_text.h"
#include "../game/main_menu_hook.h"
#include "../game/world.h"
#include "../game/window_focus.h"
#include "../game/system_menu.h"
#include "../core/profile.h"
#include "../modes/mode.h"
#include "../modes/republics/republics.h"
#include "../session/save_sync.h"
#include "../transport/network.h" // NetworkSteamId
#include "../transport/steam_lobby.h"
#include "../lockstep/lockstep_net.h" // LockstepNet_PeerCount, LockstepNet_Players
#include "../lockstep/presence.h"
#include "chat.h"
#include "sync_block.h"
#include "imgui.h"
#include <misc/cpp/imgui_stdlib.h>
#include <cstdio>
#include <string>

static const ImU32 kRowRed = IM_COL32(0xAB, 0x03, 0x1D, 0xFF);
static const ImU32 kRowGold = IM_COL32(0xE3, 0xC5, 0x68, 0xFF);
static const ImU32 kWhite = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
static const ImU32 kInk = IM_COL32(0x4A, 0x32, 0x24, 0xFF); // dark brown on the parchment
static const ImU32 kHot = IM_COL32(0xAB, 0x03, 0x1D, 0xC0);  // under a row in the mouse, as the game's menu
static const ImU32 kSection = IM_COL32(0x8C, 0x80, 0x76, 0xFF); // the grey of the game's panel headings
// The rows' text height in units of s: the game asks for "Square" at 40f with
// f = 0.8s (as measured against its own rows on screen).
static const float kTextHeight = 32.0f;

static bool s_open = false;
static bool s_menuOn = false;        // the root menu is on screen (its builder keeps running)
static bool s_hostAfterLoad = false;
static bool s_leftMenu = false;      // since the request: the root menu went away
static unsigned s_armedFrames = 0;   // frames since the request
static bool s_sawUnloaded = false;   // since the request: no world
static ULONGLONG s_loadedSince = 0;  // the world has been loaded since (0: not loaded)
static std::string s_target;
static bool s_targetInit = false;
static int s_mode = -1; // 0 co-op, 1 republics (remembered in the profile); -1 not read yet
static int s_variant = -1; // Republics: SESSION_REPUBLICS_* (profile "republics_variant"); -1 not read yet
static int s_side = -1;    // Republics: the host's half, 0 or 1 (profile "republics_side"); -1 not read yet
static int s_public = -1; // a hosted game is listed for everyone (1), for friends (0) or not at all (2: LAN / IP)
static int s_tab = 0;     // 0 host, 1 join, 2 settings
static int s_sessionTab = 0; // while a session runs: 0 the session, 1 settings
static int s_selected = -1;
static int s_keyWaiting = 0; // a key being chosen: 1 the ping key, 2 the chat key
static bool s_keyDown[256] = {};

void MultiplayerPage_Open()
{
    s_open = true;
    MainMenuHook_SetHidden(true); // only matters on the main menu
    if (s_tab == 1 && SaveSync_GetState() == SESSION_IDLE) SteamLobby_Refresh();
}

// Drawn over the main menu or over a loaded city; between the two (a load,
// the game's own new-game page) it waits, still open. Under the sync screen it
// waits too: that screen does not take the mouse, the page would.
bool MultiplayerPage_IsOpen() { return s_open && (s_menuOn || World_IsReady()) && !SyncBlock_IsActive(); }

static void Close()
{
    s_open = false;
    s_keyWaiting = 0;
    MainMenuHook_SetHidden(false);
    ImGui::SetWindowFocus(nullptr); // no focus left over to take keys while nothing is shown
}

// Hosting from the menu: the game's own new-game or load page, then Host once
// a city is loaded. Back on the root menu without one cancels it.
static void HostThrough(MainMenuItem item)
{
    Close();
    MainMenuHook_Open(item);
    s_hostAfterLoad = true;
    s_leftMenu = false;
    s_armedFrames = 0;
    s_sawUnloaded = !World_IsReady();
    s_loadedSince = 0;
}

void MultiplayerPage_Tick()
{
    // The menu counts as on screen while its builder ran within the last 30
    // frames - frames, not time, so a pause of the game does not count.
    static unsigned lastBuild = 0, framesWithout = 0;
    unsigned build = MainMenuHook_BuildCount();
    framesWithout = build != lastBuild ? 0 : framesWithout + 1;
    lastBuild = build;
    s_menuOn = framesWithout < 30;

    // The game's menu item in the game's language, once.
    static bool itemText = false;
    if (!itemText) {
        itemText = true;
        wchar_t text[64];
        if (MultiByteToWideChar(CP_UTF8, 0, T("MULTIPLAYER"), -1, text, 64) > 0) MainMenuHook_SetItemText(text);
    }

    // A hosted session is listed in the game list (Steam lobby) while it runs,
    // whether it was started here or from the RepublicMP window.
    if (s_mode < 0) s_mode = Profile_GetInt("profile", "game_mode", 0) == 1 ? 1 : 0;
    if (s_variant < 0)
        s_variant = Profile_GetInt("profile", "republics_variant", 0) == SESSION_REPUBLICS_EQUAL ? SESSION_REPUBLICS_EQUAL
                                                                                                 : SESSION_REPUBLICS_EAST_WEST;
    if (s_side < 0) s_side = Profile_GetInt("profile", "republics_side", 0) == 1 ? 1 : 0;
    if (s_public < 0) {
        int v = Profile_GetInt("profile", "game_public", 0);
        s_public = v == 1 || v == 2 ? v : 0;
    }
    SessionState st = SaveSync_GetState();
    // A session that starts, however it was started (here, Steam's Join Game,
    // the RepublicMP window), brings the page up on its SESSION view.
    static SessionState lastState = SESSION_IDLE;
    if (lastState == SESSION_IDLE && st != SESSION_IDLE) MultiplayerPage_Open();
    lastState = st;
    // LAN / IP: no Steam lobby; the players connect by this PC's address.
    if (s_public != 2 && SaveSync_IsHostRole() &&
        (st == SESSION_HOST_LISTENING || st == SESSION_HOST_JOINING || st == SESSION_HOST_IN_GAME))
        SteamLobby_Advertise(s_public == 1, Profile_GetNick(),
                             SaveSync_GetGameMode() == SESSION_MODE_REPUBLICS ? "Republics" : "Co-op",
                             1 + LockstepNet_PeerCount(), SaveSync_MaxPlayers());
    else
        SteamLobby_StopAdvertising();
    SteamLobby_Tick();

    if (!s_hostAfterLoad) return;
    // Only a city loaded after the request, the menu gone and the world there
    // for 2 s: an old world's pointer, or one half way through its load, never
    // starts the session. Missing that, the player hosts by hand.
    bool loaded = World_IsReady(); // the load finished, not only begun
    if (!s_menuOn) s_leftMenu = true;
    if (!loaded) s_sawUnloaded = true;
    s_loadedSince = loaded ? (s_loadedSince ? s_loadedSince : GetTickCount64()) : 0;
    // Back on the root menu without a city, or the game never took the click: cancelled.
    if ((s_leftMenu && s_menuOn) || (!s_leftMenu && ++s_armedFrames > 120)) {
        s_hostAfterLoad = false;
        return;
    }
    if (s_leftMenu && s_sawUnloaded && loaded && !s_menuOn && GetTickCount64() - s_loadedSince > 2000) {
        s_hostAfterLoad = false;
        if (SaveSync_GetState() == SESSION_IDLE) SaveSync_StartHost(s_mode); // the page comes up on its SESSION view
    }
}

// Text with the menu rows' look: four gold passes 1 px around, then red; or
// white when hot.
static void RowText(ImDrawList* dl, float height, ImVec2 pos, const char* text, bool hot, float alpha)
{
    ImU32 a = (ImU32)(alpha * 255) << IM_COL32_A_SHIFT;
    ImU32 rgb = ~IM_COL32_A_MASK;
    if (hot) {
        GameText_Draw(dl, GAME_FONT_SQUARE, height, pos, (kWhite & rgb) | a, text);
        return;
    }
    static const ImVec2 kOutline[4] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    for (const ImVec2& o : kOutline)
        GameText_Draw(dl, GAME_FONT_SQUARE, height, ImVec2(pos.x + o.x, pos.y + o.y), (kRowGold & rgb) | a, text);
    GameText_Draw(dl, GAME_FONT_SQUARE, height, pos, (kRowRed & rgb) | a, text);
}

// A clickable row centred on cx, its text top at y (game units times s).
static bool Row(const char* id, const char* label, float cx, float y, float s, bool enabled = true)
{
    label = T(label);
    float h = kTextHeight * s;
    ImVec2 size = GameText_Size(GAME_FONT_SQUARE, h, label);
    // The click area is the text's, like the choices' (the game's own row box
    // is 475s wide, far wider than a short label such as BACK).
    ImGui::SetCursorScreenPos(ImVec2(cx - size.x * 0.5f - 10 * s, y));
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size.x + 20 * s, h)) && enabled;
    bool hot = enabled && ImGui::IsItemHovered();
    if (hot) ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), kHot);
    RowText(ImGui::GetWindowDrawList(), h, ImVec2(cx - size.x * 0.5f, y), label, hot, enabled ? 1.0f : 0.4f);
    return clicked;
}

// Every text on the page goes through T (ui_text.h): the game's language.
static void Label(GameFont font, float height, ImVec2 pos, ImU32 color, const char* text)
{
    GameText_Draw(ImGui::GetWindowDrawList(), font, height, pos, color, T(text));
}

// Text broken into lines at spaces to fit width, at most maxLines (the last
// one cut short with "...").
// Returns the number of lines drawn.
static int WrappedLabel(GameFont font, float height, ImVec2 pos, float width, int maxLines, ImU32 color,
                        const char* text)
{
    text = T(text);
    std::string line, word;
    int lines = 0;
    auto flush = [&](bool last) {
        if (last && lines == maxLines - 1 && *text) line += "...";
        GameText_Draw(ImGui::GetWindowDrawList(), font, height, ImVec2(pos.x, pos.y + lines * height * 1.15f), color,
                      line.c_str());
        ++lines;
        line.clear();
    };
    while (*text && lines < maxLines) {
        const char* end = text;
        while (*end && *end != ' ') ++end;
        word.assign(text, end);
        std::string tryLine = line.empty() ? word : line + " " + word;
        if (!line.empty() && GameText_Size(font, height, tryLine.c_str()).x > width) {
            flush(lines == maxLines - 1);
            continue; // the word starts the next line
        }
        line = tryLine;
        text = *end ? end + 1 : end;
    }
    if (!line.empty() && lines < maxLines) flush(false);
    return lines;
}

// Text drawn as it is: names, ids and numbers are not translated.
static void Raw(GameFont font, float height, ImVec2 pos, ImU32 color, const char* text)
{
    GameText_Draw(ImGui::GetWindowDrawList(), font, height, pos, color, text);
}

// One choice of a row of choices: the chosen one in the rows' red and gold,
// the others faded, white under the mouse.
static bool Choice(const char* id, const char* label, float cx, float y, float s, bool chosen)
{
    label = T(label);
    float h = kTextHeight * s;
    ImVec2 size = GameText_Size(GAME_FONT_SQUARE, h, label);
    ImGui::SetCursorScreenPos(ImVec2(cx - size.x * 0.5f - 10 * s, y));
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size.x + 20 * s, h));
    bool hot = ImGui::IsItemHovered();
    if (hot) ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), kHot);
    RowText(ImGui::GetWindowDrawList(), h, ImVec2(cx - size.x * 0.5f, y), label, hot, chosen || hot ? 1.0f : 0.45f);
    return clicked;
}

static void InkInput(const char* id, const char* hint, std::string* text, float width, float s, bool* enter)
{
    ImGui::PushFont(nullptr, 24 * s);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0xE2, 0xD3, 0xBA, 0xFF));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0xEA, 0xDD, 0xC6, 0xFF));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0xF2, 0xE8, 0xD6, 0xFF));
    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    ImGui::PushStyleColor(ImGuiCol_Border, kRowRed);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::SetNextItemWidth(width);
    *enter = ImGui::InputTextWithHint(id, hint, text, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    ImGui::PopFont();
}

static void SetChoice(int* value, int chosen, const char* key)
{
    if (*value == chosen) return;
    *value = chosen;
    Profile_SetInt("profile", key, chosen);
}

// LAN / IP: this PC's LAN address with a Copy button (one: a list of every
// adapter ran past the panel). Read again every few seconds.
static void LocalAddresses(float x, float y, float s, const char* copyId)
{
    static char addresses[4][16];
    static int count = 0;
    static ULONGLONG readAt = 0;
    ULONGLONG now = GetTickCount64();
    if (!readAt || now - readAt > 5000) {
        count = NetworkLocalAddresses(addresses, 4);
        readAt = now;
    }
    Label(GAME_FONT_ARIAL, 22 * s, ImVec2(x, y), kInk, "Your IP:");
    if (!count) {
        Raw(GAME_FONT_ARIAL, 22 * s, ImVec2(x, y + 30 * s), kInk, T("No network address found."));
        return;
    }
    Raw(GAME_FONT_ARIAL, 28 * s, ImVec2(x, y + 30 * s), kInk, addresses[0]);
    if (Row(copyId, "COPY", x + 380 * s, y + 28 * s, s)) ImGui::SetClipboardText(addresses[0]);
}

// HOST: the game mode and who sees the game on the left, the ways to start
// hosting and our SteamID64 (or IP) on the right.
static void HostTab(float cx, float left, float right, float s)
{
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(left - 225 * s, 540 * s), kInk, "Game mode:");
    if (Choice("##coop", "CO-OP", left - 110 * s, 570 * s, s, s_mode == 0)) SetChoice(&s_mode, 0, "game_mode");
    if (Choice("##republics", "REPUBLICS", left + 110 * s, 570 * s, s, s_mode == 1)) SetChoice(&s_mode, 1, "game_mode");
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(left - 225 * s, 640 * s), kInk, "Listed for:");
    if (Choice("##friends", "FRIENDS", left - 150 * s, 670 * s, s, s_public == 0)) SetChoice(&s_public, 0, "game_public");
    if (Choice("##public", "EVERYONE", left + 25 * s, 670 * s, s, s_public == 1)) SetChoice(&s_public, 1, "game_public");
    if (Choice("##lan", "LAN / IP", left + 190 * s, 670 * s, s, s_public == 2)) SetChoice(&s_public, 2, "game_public");
    WrappedLabel(GAME_FONT_ARIAL, 20 * s, ImVec2(left - 225 * s, 720 * s), 470 * s, 2, kInk,
          s_public == 1   ? "Every player with this version sees your game."
          : s_public == 2 ? "Not listed on Steam: players join by your IP (LAN or VPN, ports 5556 and 5560)."
                          : "Only your Steam friends see your game.");
    // The session's size, the same setting as "Session" in the RepublicMP window
    // (Republics is always 2).
    if (s_mode == 0) {
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(left - 225 * s, 765 * s), kInk, "Players, you included:");
    int players = Profile_GetMaxPlayers();
    for (int n = PROFILE_MIN_PLAYERS; n <= PROFILE_MAX_PLAYERS; ++n) {
        char id[16], label[4];
        snprintf(id, sizeof(id), "##players%d", n);
        snprintf(label, sizeof(label), "%d", n);
        if (Choice(id, label, left + (n - 3) * 110 * s, 795 * s, s, players == n)) Profile_SetMaxPlayers(n);
    }
    } else {
    // Republics (always 2 players): one republic of each bloc, or two equal ones.
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(left - 225 * s, 765 * s), kInk, "Republics:");
    if (Choice("##eastWest", "EAST VS WEST", left - 110 * s, 795 * s, s, s_variant == SESSION_REPUBLICS_EAST_WEST))
        SetChoice(&s_variant, SESSION_REPUBLICS_EAST_WEST, "republics_variant");
    if (Choice("##equal", "EQUAL", left + 110 * s, 795 * s, s, s_variant == SESSION_REPUBLICS_EQUAL))
        SetChoice(&s_variant, SESSION_REPUBLICS_EQUAL, "republics_variant");
    // Which half the host plays; the player gets the other.
    bool blocs = s_variant == SESSION_REPUBLICS_EAST_WEST;
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(left - 225 * s, 845 * s), kInk, "Your side:");
    if (Choice("##side0", blocs ? "EAST" : "SIDE 1", left - 110 * s, 875 * s, s, s_side == 0)) SetChoice(&s_side, 0, "republics_side");
    if (Choice("##side1", blocs ? "WEST" : "SIDE 2", left + 110 * s, 875 * s, s, s_side == 1)) SetChoice(&s_side, 1, "republics_side");
    }

    bool idle = SaveSync_GetState() == SESSION_IDLE;
    if (!s_menuOn) {
        // Over a loaded city: that city is the game.
        if (Row("##hostCity", "HOST THIS CITY", right + 225 * s, 560 * s, s, idle)) SaveSync_StartHost(s_mode);
        WrappedLabel(GAME_FONT_ARIAL, 20 * s, ImVec2(right, 615 * s), 460 * s, 2, kInk,
                     "The players who join get this city.");
    } else {
        float rowX = right + 225 * s;
        if (Row("##hostNew", "HOST NEW GAME", rowX, 560 * s, s, idle)) HostThrough(MAIN_MENU_CUSTOM_GAME);
        if (Row("##hostSave", "HOST SAVED GAME", rowX, 600 * s, s, idle)) HostThrough(MAIN_MENU_LOAD_GAME);
        WrappedLabel(GAME_FONT_ARIAL, 20 * s, ImVec2(right, 655 * s), 460 * s, 2, kInk,
                     "Hosting starts once the city is loaded.");
    }
    // Republics (modes/republics): what it is, under the ways to host.
    const RmpMode* republics = Modes_Republics();
    if (s_mode == 1 && republics && republics->line1)
        WrappedLabel(GAME_FONT_ARIAL, 20 * s, ImVec2(right, 700 * s), 460 * s, 1, kRowRed, republics->line1);
    if (s_public == 2) {
        LocalAddresses(right, 740 * s, s, "##copyIp");
    } else if (unsigned long long id = NetworkSteamId()) {
        char idText[32];
        snprintf(idText, sizeof(idText), "%llu", id);
        Label(GAME_FONT_ARIAL, 22 * s, ImVec2(right, 740 * s), kInk, "Your SteamID64:");
        Raw(GAME_FONT_ARIAL, 28 * s, ImVec2(right, 770 * s), kInk, idText);
        if (Row("##copy", "COPY", right + 380 * s, 768 * s, s)) ImGui::SetClipboardText(idText);
    }
}

// JOIN: the game list (friends' games and public ones of this version), and a
// join by SteamID64 or IP.
static void JoinTab(float cx, float listMin, float listMax, float s)
{
    bool idle = SaveSync_GetState() == SESSION_IDLE;
    LobbyGame games[16];
    int n = SteamLobby_Games(games, 16);
    if (s_selected >= n) s_selected = -1;

    float colMode = listMin + 470 * s, colPlayers = listMin + 650 * s, colPing = listMin + 800 * s;
    float y = 540 * s, rowH = 34 * s;
    Label(GAME_FONT_ARIAL, 22 * s, ImVec2(listMin + 10 * s, y), kInk, "GAME");
    Label(GAME_FONT_ARIAL, 22 * s, ImVec2(colMode, y), kInk, "MODE");
    Label(GAME_FONT_ARIAL, 22 * s, ImVec2(colPlayers, y), kInk, "PLAYERS");
    Label(GAME_FONT_ARIAL, 22 * s, ImVec2(colPing, y), kInk, "PING");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(listMin, y + 28 * s), ImVec2(listMax, y + 28 * s), kRowRed);
    y += 36 * s;
    for (int i = 0; i < n && i < 7; ++i, y += rowH) {
        const LobbyGame& g = games[i];
        ImGui::SetCursorScreenPos(ImVec2(listMin, y));
        char id[24];
        snprintf(id, sizeof(id), "##game%d", i);
        if (ImGui::InvisibleButton(id, ImVec2(listMax - listMin, rowH))) s_selected = i;
        bool hot = ImGui::IsItemHovered();
        bool join = hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (i == s_selected || hot)
            dl->AddRectFilled(ImVec2(listMin, y), ImVec2(listMax, y + rowH),
                              i == s_selected ? IM_COL32(0xAB, 0x03, 0x1D, 0x40) : IM_COL32(0xAB, 0x03, 0x1D, 0x18));
        char text[96];
        snprintf(text, sizeof(text), "%s%s", g.friendGame ? "* " : "", g.name);
        Raw(GAME_FONT_ARIAL, 24 * s, ImVec2(listMin + 10 * s, y + 4 * s), kInk, text);
        Label(GAME_FONT_ARIAL, 24 * s, ImVec2(colMode, y + 4 * s), kInk, g.mode);
        snprintf(text, sizeof(text), "%d/%d", g.players, g.maxPlayers);
        Label(GAME_FONT_ARIAL, 24 * s, ImVec2(colPlayers, y + 4 * s), kInk, text);
        if (g.pingMs >= 0) snprintf(text, sizeof(text), "%d ms", g.pingMs);
        else snprintf(text, sizeof(text), "-");
        Label(GAME_FONT_ARIAL, 24 * s, ImVec2(colPing, y + 4 * s), kInk, text);
        if (join && idle) {
            snprintf(text, sizeof(text), "steam:%llu", g.host);
            SaveSync_StartClient(text);
        }
    }
    if (n == 0)
        WrappedLabel(GAME_FONT_ARIAL, 22 * s, ImVec2(listMin + 10 * s, y + 6 * s), listMax - listMin - 20 * s, 2, kInk,
              SteamLobby_Refreshing() ? "Looking for games..."
                                      : "No games found: your friends' games and public ones of this version show here.");

    float mid = (listMin + listMax) * 0.5f;
    if (Row("##refresh", SteamLobby_Refreshing() ? "LOOKING..." : "REFRESH", mid - 250 * s, 820 * s, s,
            !SteamLobby_Refreshing()))
        SteamLobby_Refresh();
    bool canJoinListed = idle && s_selected >= 0;
    if (Row("##joinListed", "JOIN", mid + 250 * s, 820 * s, s, canJoinListed) && canJoinListed) {
        char host[32];
        snprintf(host, sizeof(host), "steam:%llu", games[s_selected].host);
        SaveSync_StartClient(host);
    }

    Label(GAME_FONT_ARIAL, 22 * s, ImVec2(listMin + 10 * s, 880 * s), kInk, "Or by SteamID64 or IP:");
    ImGui::SetCursorScreenPos(ImVec2(listMin + 260 * s, 876 * s));
    bool enter = false;
    InkInput("##target", "76561198... or 192.168...", &s_target, 400 * s, s, &enter);
    bool canJoin = idle && !s_target.empty();
    if ((Row("##connect", "CONNECT", listMax - 140 * s, 872 * s, s, canJoin) || enter) && canJoin)
        SaveSync_StartClient(s_target.c_str());
}

// The map ping key (the same binding as the ImGui panel) and the chat key
// (ui/chat), each on its row. Ignore keys already held when capture starts,
// and let Escape cancel without leaving this page. The two keys differ.
static void SessionKeys(float x, float y, float s)
{
    char name[32];
    bool start = false;
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(x, y), kInk, "Map ping key:");
    Presence_KeyName(Presence_GetPingKey(), name, sizeof(name));
    if (Row("##sessionPingKey", s_keyWaiting == 1 ? "PRESS A KEY..." : name, x + 380 * s, y - 4 * s, s) && !s_keyWaiting) {
        s_keyWaiting = 1;
        start = true;
    }
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(x, y + 38 * s), kInk, "Chat key:");
    Presence_KeyName(Chat_GetKey(), name, sizeof(name));
    if (Row("##sessionChatKey", s_keyWaiting == 2 ? "PRESS A KEY..." : name, x + 380 * s, y + 34 * s, s) && !s_keyWaiting) {
        s_keyWaiting = 2;
        start = true;
    }
    if (start) {
        for (int vk = 0; vk < 256; ++vk) s_keyDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        return;
    }
    WrappedLabel(GAME_FONT_ARIAL, 20 * s, ImVec2(x, y + 78 * s), 450 * s, 2, kInk,
                 s_keyWaiting ? "Press a key or mouse button; Esc cancels."
                              : "Point at the map and press the ping key; the chat key opens the chat.");
    if (!s_keyWaiting || !IsGameWindowFocused()) return;
    for (int vk = 1; vk < 256; ++vk) {
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !s_keyDown[vk] &&
            (vk == VK_ESCAPE ||
             (s_keyWaiting == 1 ? vk != Chat_GetKey() && Presence_SetPingKey(vk) : Chat_SetKey(vk)))) {
            s_keyWaiting = 0;
            break;
        }
        s_keyDown[vk] = down;
    }
}

// Reuse the session service's policy and countdown; these controls change the
// same values as the ImGui panel, with no separate resync clock or requests.
static void SessionResync(float x, float y, float s, bool host, bool ready)
{
    if (Row("##sessionResync", "RE-SYNC NOW", x + 235 * s, y, s, host && ready)) SaveSync_RequestResync();
    if (!host) {
        WrappedLabel(GAME_FONT_ARIAL, 22 * s, ImVec2(x, y + 48 * s), 470 * s, 2, kInk,
                     "The host controls re-sync.");
        return;
    }
    ULONGLONG left = SaveSync_GetMsUntilDesyncResync();
    if (left) {
        char countdown[128];
        ULONGLONG secondsLeft = (left + 999) / 1000;
        snprintf(countdown, sizeof(countdown), "%s %llu:%02llu", T("Re-sync in:"), secondsLeft / 60, secondsLeft % 60);
        Raw(GAME_FONT_ARIAL, 22 * s, ImVec2(x, y + 48 * s), kRowRed, countdown);
    } else {
        WrappedLabel(GAME_FONT_ARIAL, 22 * s, ImVec2(x, y + 48 * s), 470 * s, 2, kInk,
                     LockstepNet_Desync()[0] ? "No automatic re-sync pending." : "No desync detected.");
    }
}

// SETTINGS: what is set once and kept (the profile), out of the way of HOST,
// JOIN and SESSION. Left: the host's join and re-sync policies; right: what
// this PC shows of the others and its keys.
static void SettingsTab(float left, float right, float s)
{
    float x = left - 225 * s, y = 540 * s;
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(x, y), kInk, "Check DLCs:");
    bool dlcCheck = SaveSync_GetDlcCheck();
    if (Choice("##dlcOn", "ON", x + 115 * s, y + 30 * s, s, dlcCheck)) SaveSync_SetDlcCheck(true);
    if (Choice("##dlcOff", "OFF", x + 335 * s, y + 30 * s, s, !dlcCheck)) SaveSync_SetDlcCheck(false);
    WrappedLabel(GAME_FONT_ARIAL, 20 * s, ImVec2(x, y + 70 * s), 470 * s, 1, kInk,
                 dlcCheck ? "Refuse players with different DLCs." : "Allow players with different DLCs.");
    // The host's: a player's session follows the host.
    y += 130 * s;
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(x, y), kInk, "After a desync:");
    static const int seconds[] = { -1, 0, 60, 120, 300 };
    static const char* const labels[] = { "OFF", "NOW", "1 MIN", "2 MIN", "5 MIN" };
    for (int i = 0; i < IM_ARRAYSIZE(seconds); ++i) {
        ImGui::PushID(i);
        if (Choice("##resyncDelay", labels[i], x + (47 + 94 * i) * s, y + 30 * s, s * 0.8f,
                   SaveSync_GetDesyncResyncSeconds() == seconds[i])) SaveSync_SetDesyncResyncSeconds(seconds[i]);
        ImGui::PopID();
    }
    WrappedLabel(GAME_FONT_ARIAL, 20 * s, ImVec2(x, y + 70 * s), 470 * s, 2, kInk,
                 "When you host: the world is sent again this long after the games differ.");

    float c1 = right + 120 * s, c2 = right + 340 * s;
    float ry = 540 * s;
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(right, ry), kInk, "Players' cursors:");
    bool cursors = Presence_GetShowCursors();
    if (Choice("##sShow", "SHOW", c1, ry + 30 * s, s, cursors)) Presence_SetShowCursors(true);
    if (Choice("##sHide", "HIDE", c2, ry + 30 * s, s, !cursors)) Presence_SetShowCursors(false);
    ry += 80 * s;
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(right, ry), kInk, "Border stars:");
    bool signs = Republics_GetShowSigns();
    if (Choice("##sSigns", "SHOW", c1, ry + 30 * s, s, signs)) Republics_SetShowSigns(true);
    if (Choice("##sNoSigns", "HIDE", c2, ry + 30 * s, s, !signs)) Republics_SetShowSigns(false);
    SessionKeys(right, ry + 100 * s, s);
}

// SESSION: what the session is doing and who is in it on the left; who sees
// the game, our SteamID64 and Stop / Disconnect on the right.
static void SessionTab(float left, float right, float bottom, float s)
{
    SessionState st = SaveSync_GetState();
    bool host = SaveSync_IsHostRole();
    bool resync = SaveSync_IsResyncing();
    const char* state = "";
    ImU32 stateColor = kInk;
    switch (st) {
    case SESSION_HOST_LISTENING:
        state = resync ? "Re-syncing - waiting for the player to come back" : "Hosting - waiting for players";
        break;
    case SESSION_HOST_JOINING:
        state = resync ? "Re-syncing - sending the world" : "A player is joining...";
        break;
    case SESSION_HOST_IN_GAME: state = "Hosting - in the game"; break;
    case SESSION_CLIENT_JOINING: state = resync ? "Re-syncing with the host" : "Joining..."; break;
    case SESSION_CLIENT_IN_GAME: state = "In the host's game"; break;
    default: break;
    }
    float x = left - 225 * s, y = 540 * s;
    y += WrappedLabel(GAME_FONT_ARIAL, 24 * s, ImVec2(x, y), 470 * s, 2, stateColor, state) * 28 * s + 6 * s;

    // The players, the host first, with their ping.
    y += 10 * s;
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(x, y), kInk, "Players:");
    y += 32 * s;
    LockstepPlayer players[PROFILE_MAX_PLAYERS];
    int n = LockstepNet_Players(players, PROFILE_MAX_PLAYERS);
    if (n == 0 && host) { // waiting for the first player: no lockstep list yet, only us
        memset(&players[0], 0, sizeof(players[0]));
        strncpy_s(players[0].nick, Profile_GetNick(), _TRUNCATE);
        players[0].host = players[0].self = true;
        n = 1;
    }
    for (int i = 0; i < n; ++i, y += 30 * s) {
        const LockstepPlayer& p = players[i];
        char name[96];
        snprintf(name, sizeof(name), "%s%s%s", p.nick, p.host ? T(" (host)") : "", p.self ? T(" - you") : "");
        Raw(GAME_FONT_ARIAL, 24 * s, ImVec2(x + 10 * s, y), kInk, name);
        if (!p.host) { // each player's ping to the host (the host has none)
            char ping[16];
            if (p.rttMs >= 0) snprintf(ping, sizeof(ping), "%d ms", p.rttMs);
            else snprintf(ping, sizeof(ping), "-");
            Raw(GAME_FONT_ARIAL, 24 * s, ImVec2(x + 380 * s, y), kInk, ping);
        }
    }
    if (n == 0 && st != SESSION_HOST_LISTENING) Label(GAME_FONT_ARIAL, 22 * s, ImVec2(x + 10 * s, y), kInk, "...");

    // Four player rows end above this block; keep the actions clear of Back.
    float resyncY = y + 20 * s;
    if (resyncY < 760 * s) resyncY = 760 * s;
    SessionResync(x, resyncY, s, host, st == SESSION_HOST_IN_GAME && !resync);

    // Right: the host chooses who sees the game in the list, and gives out its SteamID64.
    float ry = 540 * s;
    if (host) {
        Label(GAME_FONT_ARIAL, 24 * s, ImVec2(right, ry), kInk, "Listed for:");
        // Three in the column's width: centred at 60, 205 and 350 from its left edge.
        if (Choice("##sFriends", "FRIENDS", right + 60 * s, ry + 30 * s, s, s_public == 0)) SetChoice(&s_public, 0, "game_public");
        if (Choice("##sPublic", "EVERYONE", right + 205 * s, ry + 30 * s, s, s_public == 1)) SetChoice(&s_public, 1, "game_public");
        if (Choice("##sLan", "LAN / IP", right + 350 * s, ry + 30 * s, s, s_public == 2)) SetChoice(&s_public, 2, "game_public");
        ry += 100 * s;
        if (s_public == 2) {
            LocalAddresses(right, ry, s, "##sCopyIp");
            ry += 90 * s;
        } else if (unsigned long long id = NetworkSteamId()) {
            char idText[32];
            snprintf(idText, sizeof(idText), "%llu", id);
            Label(GAME_FONT_ARIAL, 22 * s, ImVec2(right, ry), kInk, "Your SteamID64:");
            Raw(GAME_FONT_ARIAL, 28 * s, ImVec2(right, ry + 30 * s), kInk, idText);
            if (Row("##sCopy", "COPY", right + 380 * s, ry + 28 * s, s)) ImGui::SetClipboardText(idText);
            ry += 90 * s;
        }
    }
    // The game is left (Stop hosting / Disconnect) only after the session's first moments.
    bool canStop = SaveSync_GetSessionAgeMs() >= SAVE_SYNC_STOP_BUTTON_DELAY_MS;
    if (Row("##stop", host ? "STOP HOSTING" : "DISCONNECT", right + 230 * s, bottom, s, canStop))
        SaveSync_RequestDisconnect();
}

bool MultiplayerPage_MenuButtonShown()
{
    return !s_open && World_IsReady() && SystemMenu_IsOpen() && !SyncBlock_IsActive();
}

// A section of our own in the game's panel, below its last one ("Other"),
// headed like the game's sections: their headings sit at x = 108s, and the
// panel's contents end near 460s below its top (measured on screen).
void MultiplayerPage_DrawMenuButton()
{
    float top, s;
    if (!MultiplayerPage_MenuButtonShown() || !SystemMenu_Panel(&top, &s)) return;
    float x = 108 * s;
    GameText_Draw(ImGui::GetBackgroundDrawList(), GAME_FONT_ARIAL, 26 * s, ImVec2(x, top + 505 * s), kSection,
                  T("Multiplayer"));
    const char* label = T("MULTIPLAYER");
    float h = kTextHeight * s;
    ImVec2 text = GameText_Size(GAME_FONT_SQUARE, h, label);
    ImVec2 size(text.x + 20 * s, h);
    ImVec2 pos(x - 10 * s, top + 545 * s);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##multiplayerMenuButton", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav); // the game's keys stay the game's
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton("##openPage", size);
    bool hot = ImGui::IsItemHovered();
    if (hot) ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), kHot);
    RowText(ImGui::GetWindowDrawList(), h, ImVec2(x, pos.y), label, hot, 1.0f);
    ImGui::End();
    if (clicked) {
        SystemMenu_Close();
        MultiplayerPage_Open();
    }
}

void MultiplayerPage_Draw()
{
    if (!MultiplayerPage_IsOpen()) return; // closed, or waiting for the menu or a city
    if (!s_targetInit) {
        s_target = SaveSync_GetTargetIp();
        s_targetInit = true;
    }

    bool bindingAtStart = s_keyWaiting != 0;
    float s = MainMenuHook_UiScale();
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float cx = display.x * 0.5f;
    ImVec2 texSize;
    ImTextureID panel = GameAssets_Texture("menu\\strip_wide.png", &texSize);
    ImVec2 half(texSize.x * 0.87f * 0.5f * s, texSize.y * 0.87f * 0.5f * s);
    if (!panel) half = ImVec2(522 * s, 304.5f * s);
    ImVec2 pMin(cx - half.x, 720 * s - half.y), pMax(cx + half.x, 720 * s + half.y);

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (panel) bg->AddImage(panel, pMin, pMax);
    else bg->AddRectFilled(pMin, pMax, IM_COL32(0xEF, 0xE4, 0xD3, 0xFF));

    ImGui::SetNextWindowPos(pMin);
    ImGui::SetNextWindowSize(ImVec2(pMax.x - pMin.x, pMax.y - pMin.y));
    ImGui::Begin("##multiplayerPage", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus); // under the overlay's windows

    // The title, centred on the panel's red bar (42 of the texture's 700 lines).
    const char* title = T(":: MULTIPLAYER ::");
    float titleH = kTextHeight * s;
    ImVec2 titleSize = GameText_Size(GAME_FONT_SQUARE, titleH, title);
    Label(GAME_FONT_SQUARE, titleH, ImVec2(cx - titleSize.x * 0.5f, pMin.y + 42 * 0.87f * s - titleH * 0.5f), kWhite,
          title);

    float left = cx - half.x * 0.5f;
    float right = cx + 20 * s;
    bool session = SaveSync_GetState() != SESSION_IDLE;
    if (session) {
        // While a session runs the page is the session (and the settings); HOST and JOIN come back after it.
        if (Choice("##tabSession", "SESSION", cx - 150 * s, 478 * s, s, s_sessionTab == 0)) s_sessionTab = 0;
        if (Choice("##tabSettings", "SETTINGS", cx + 150 * s, 478 * s, s, s_sessionTab == 1)) s_sessionTab = 1;
    } else {
        // The tabs under the bar; JOIN asks for the list the first time it shows.
        int tab = s_tab;
        if (Choice("##tabHost", "HOST", cx - 250 * s, 478 * s, s, tab == 0)) tab = 0;
        if (Choice("##tabJoin", "JOIN", cx, 478 * s, s, tab == 1)) tab = 1;
        if (Choice("##tabSettings", "SETTINGS", cx + 250 * s, 478 * s, s, tab == 2)) tab = 2;
        if (tab == 1 && s_tab != 1) SteamLobby_Refresh();
        s_tab = tab;
    }
    ImGui::GetWindowDrawList()->AddLine(ImVec2(pMin.x + 40 * s, 520 * s), ImVec2(pMax.x - 40 * s, 520 * s), kRowRed);

    bool settings = session ? s_sessionTab == 1 : s_tab == 2;
    if (!settings) s_keyWaiting = 0; // a key is bound on the settings only
    if (settings) SettingsTab(left, right, s);
    else if (session) SessionTab(left, right, pMax.y - 70 * s, s);
    else if (s_tab == 0) HostTab(cx, left, right, s);
    else JoinTab(cx, pMin.x + 50 * s, pMax.x - 50 * s, s);

    const char* status = SaveSync_GetLastError(); // why the last session ended or failed
    if (!session && status && status[0])
        WrappedLabel(GAME_FONT_ARIAL, 20 * s, ImVec2(right, pMax.y - 80 * s), pMax.x - 40 * s - right, 3, kRowRed, status);

    // Escape in the text field only reverts the text (WantTextInput is last
    // frame's: the field lets go of Escape within this one).
    if (Row("##back", "BACK", left, pMax.y - 70 * s, s) ||
        (ImGui::IsKeyPressed(ImGuiKey_Escape) && !bindingAtStart && !ImGui::GetIO().WantTextInput))
        Close();
    // The build belongs to this card, bottom left under BACK, in the menu's
    // plain player-list font and ink (bottom right it ran into the errors the
    // page shows there). The window clips it with the parchment.
    char build[96];
    snprintf(build, sizeof(build), "RepublicMP %s  build %08X", SaveSync_GetLocalVersion(), SaveSync_GetLocalDllCrc());
    float buildHeight = 18 * s;
    ImVec2 buildSize = GameText_Size(GAME_FONT_ARIAL, buildHeight, build);
    ImVec2 buildAt(left - 225 * s, pMax.y - buildSize.y - 14 * s);
    Raw(GAME_FONT_ARIAL, buildHeight, buildAt, kInk, build);
    ImGui::End();
}
