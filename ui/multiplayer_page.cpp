// SPDX-License-Identifier: GPL-3.0-only
// See multiplayer_page.h. Geometry and colours are the game's root menu's
// (0x261210 / 0x274E70, SOVIET64.exe 1.1.1.9): positions are multiples of the
// menu's scale s, the panel is menu\strip*.png at 87%, the rows are the
// "Square" font 32s high, red with a gold outline, white under the mouse.
#include "multiplayer_page.h"
#include "game_assets.h"
#include "../game/imgui_hook.h" // g_imguiVisible
#include "../game/main_menu_hook.h"
#include "../game/world.h"
#include "../core/profile.h"
#include "../modes/mode.h"
#include "../session/save_sync.h"
#include "../transport/network.h" // NetworkSteamId
#include "../transport/steam_lobby.h"
#include "../lockstep/lockstep_net.h" // LockstepNet_PeerCount
#include "imgui.h"
#include <misc/cpp/imgui_stdlib.h>
#include <cstdio>
#include <string>

static const ImU32 kRowRed = IM_COL32(0xAB, 0x03, 0x1D, 0xFF);
static const ImU32 kRowGold = IM_COL32(0xE3, 0xC5, 0x68, 0xFF);
static const ImU32 kWhite = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
static const ImU32 kInk = IM_COL32(0x4A, 0x32, 0x24, 0xFF); // dark brown on the parchment
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
static int s_public = -1; // a hosted game is listed for everyone (1) or for friends (0)
static int s_tab = 0;     // 0 host, 1 join
static int s_selected = -1;

void MultiplayerPage_Open()
{
    s_open = true;
    s_menuOn = true; // its builder just ran: the click came from it
    MainMenuHook_SetHidden(true);
    if (s_tab == 1) SteamLobby_Refresh();
}

bool MultiplayerPage_IsOpen() { return s_open; }

static void Close()
{
    s_open = false;
    MainMenuHook_SetHidden(false);
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

    // A hosted session is listed in the game list (Steam lobby) while it runs,
    // whether it was started here or from the RepublicMP window.
    if (s_mode < 0) s_mode = Profile_GetInt("profile", "game_mode", 0) == 1 ? 1 : 0;
    if (s_public < 0) s_public = Profile_GetInt("profile", "game_public", 0) == 1 ? 1 : 0;
    SessionState st = SaveSync_GetState();
    if (SaveSync_IsHostRole() && (st == SESSION_HOST_LISTENING || st == SESSION_HOST_JOINING || st == SESSION_HOST_IN_GAME))
        SteamLobby_Advertise(s_public == 1, Profile_GetNick(), "Co-op", 1 + LockstepNet_PeerCount(),
                             Profile_GetMaxPlayers());
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
        if (SaveSync_GetState() == SESSION_IDLE) {
            SaveSync_StartHost();
            g_imguiVisible = true; // the session panel: the SteamID64 to give out, the players
        }
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
    float h = kTextHeight * s;
    ImVec2 size = GameText_Size(GAME_FONT_SQUARE, h, label);
    ImGui::SetCursorScreenPos(ImVec2(cx - 237.5f * s, y));
    bool clicked = ImGui::InvisibleButton(id, ImVec2(475 * s, h)) && enabled;
    bool hot = enabled && ImGui::IsItemHovered();
    RowText(ImGui::GetWindowDrawList(), h, ImVec2(cx - size.x * 0.5f, y), label, hot, enabled ? 1.0f : 0.4f);
    return clicked;
}

static void Label(GameFont font, float height, ImVec2 pos, ImU32 color, const char* text)
{
    GameText_Draw(ImGui::GetWindowDrawList(), font, height, pos, color, text);
}

// One choice of a row of choices: the chosen one in the rows' red and gold,
// the others faded, white under the mouse.
static bool Choice(const char* id, const char* label, float cx, float y, float s, bool chosen)
{
    float h = kTextHeight * s;
    ImVec2 size = GameText_Size(GAME_FONT_SQUARE, h, label);
    ImGui::SetCursorScreenPos(ImVec2(cx - size.x * 0.5f - 10 * s, y));
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size.x + 20 * s, h));
    bool hot = ImGui::IsItemHovered();
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

// HOST: the game mode and who sees the game on the left, the ways to start
// hosting and our SteamID64 on the right.
static void HostTab(float cx, float left, float right, float s)
{
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(left - 225 * s, 540 * s), kInk, "Game mode:");
    if (Choice("##coop", "CO-OP", left - 110 * s, 570 * s, s, s_mode == 0)) SetChoice(&s_mode, 0, "game_mode");
    if (Choice("##republics", "REPUBLICS", left + 110 * s, 570 * s, s, s_mode == 1)) SetChoice(&s_mode, 1, "game_mode");
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(left - 225 * s, 640 * s), kInk, "Listed for:");
    if (Choice("##friends", "FRIENDS", left - 110 * s, 670 * s, s, s_public == 0)) SetChoice(&s_public, 0, "game_public");
    if (Choice("##public", "EVERYONE", left + 110 * s, 670 * s, s, s_public == 1)) SetChoice(&s_public, 1, "game_public");
    Label(GAME_FONT_ARIAL, 20 * s, ImVec2(left - 225 * s, 720 * s), kInk,
          s_public == 1 ? "Every player with this version sees your game." : "Only your Steam friends see your game.");

    bool idle = SaveSync_GetState() == SESSION_IDLE;
    if (s_mode == 0) {
        float rowX = right + 225 * s;
        if (Row("##hostNew", "HOST NEW GAME", rowX, 560 * s, s, idle)) HostThrough(MAIN_MENU_CUSTOM_GAME);
        if (Row("##hostSave", "HOST SAVED GAME", rowX, 600 * s, s, idle)) HostThrough(MAIN_MENU_LOAD_GAME);
        Label(GAME_FONT_ARIAL, 20 * s, ImVec2(right, 655 * s), kInk, "Hosting starts once the city is loaded.");
    } else {
        // Only co-op plays for now; Republics shows what it will be (modes/republics).
        const RmpMode* republics = Modes_Republics();
        float y = 545 * s;
        if (republics && republics->line1) {
            Label(GAME_FONT_ARIAL, 22 * s, ImVec2(right, y), kInk, republics->line1);
            y += 30 * s;
        }
        if (republics && republics->line2) {
            Label(GAME_FONT_ARIAL, 22 * s, ImVec2(right, y), kInk, republics->line2);
            y += 30 * s;
        }
        Label(GAME_FONT_ARIAL, 22 * s, ImVec2(right, y + 10 * s), kRowRed, "In development - not playable yet.");
    }
    if (unsigned long long id = NetworkSteamId()) {
        char idText[32];
        snprintf(idText, sizeof(idText), "%llu", id);
        Label(GAME_FONT_ARIAL, 22 * s, ImVec2(right, 740 * s), kInk, "Your SteamID64 (for a join by hand):");
        Label(GAME_FONT_ARIAL, 28 * s, ImVec2(right, 770 * s), kInk, idText);
        if (Row("##copy", "COPY", right + 360 * s, 768 * s, s)) ImGui::SetClipboardText(idText);
    }
}

// JOIN: the game list (friends' games and public ones of this version), and a
// join by SteamID64 or IP.
static void JoinTab(float cx, float listMin, float listMax, float s)
{
    bool idle = SaveSync_GetState() == SESSION_IDLE;
    bool playable = s_mode == 0;
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
        Label(GAME_FONT_ARIAL, 24 * s, ImVec2(listMin + 10 * s, y + 4 * s), kInk, text);
        Label(GAME_FONT_ARIAL, 24 * s, ImVec2(colMode, y + 4 * s), kInk, g.mode);
        snprintf(text, sizeof(text), "%d/%d", g.players, g.maxPlayers);
        Label(GAME_FONT_ARIAL, 24 * s, ImVec2(colPlayers, y + 4 * s), kInk, text);
        if (g.pingMs >= 0) snprintf(text, sizeof(text), "%d ms", g.pingMs);
        else snprintf(text, sizeof(text), "-");
        Label(GAME_FONT_ARIAL, 24 * s, ImVec2(colPing, y + 4 * s), kInk, text);
        if (join && idle && playable) {
            snprintf(text, sizeof(text), "steam:%llu", g.host);
            SaveSync_StartClient(text);
        }
    }
    if (n == 0)
        Label(GAME_FONT_ARIAL, 22 * s, ImVec2(listMin + 10 * s, y + 6 * s), kInk,
              SteamLobby_Refreshing() ? "Looking for games..."
                                      : "No games found: your friends' games and public ones of this version show here.");

    float mid = (listMin + listMax) * 0.5f;
    if (Row("##refresh", SteamLobby_Refreshing() ? "LOOKING..." : "REFRESH", mid - 250 * s, 820 * s, s,
            !SteamLobby_Refreshing()))
        SteamLobby_Refresh();
    bool canJoinListed = idle && playable && s_selected >= 0;
    if (Row("##joinListed", "JOIN", mid + 250 * s, 820 * s, s, canJoinListed) && canJoinListed) {
        char host[32];
        snprintf(host, sizeof(host), "steam:%llu", games[s_selected].host);
        SaveSync_StartClient(host);
    }

    Label(GAME_FONT_ARIAL, 22 * s, ImVec2(listMin + 10 * s, 880 * s), kInk, "Or by SteamID64 or IP:");
    ImGui::SetCursorScreenPos(ImVec2(listMin + 260 * s, 876 * s));
    bool enter = false;
    InkInput("##target", "76561198... or 192.168...", &s_target, 400 * s, s, &enter);
    bool canJoin = idle && playable && !s_target.empty();
    if ((Row("##connect", "CONNECT", listMax - 140 * s, 872 * s, s, canJoin) || enter) && canJoin)
        SaveSync_StartClient(s_target.c_str());
    if (!playable)
        Label(GAME_FONT_ARIAL, 20 * s, ImVec2(listMin + 10 * s, 915 * s), kRowRed,
              "Republics is in development: pick Co-op on the HOST tab to join.");
}

void MultiplayerPage_Draw()
{
    if (!s_open) return;
    // The game left its menu (a city is loading, a join began): the page goes too.
    if (!s_menuOn) {
        Close();
        return;
    }
    if (!s_targetInit) {
        s_target = SaveSync_GetTargetIp();
        s_targetInit = true;
    }

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
    const char* title = ":: MULTIPLAYER ::";
    float titleH = kTextHeight * s;
    ImVec2 titleSize = GameText_Size(GAME_FONT_SQUARE, titleH, title);
    Label(GAME_FONT_SQUARE, titleH, ImVec2(cx - titleSize.x * 0.5f, pMin.y + 42 * 0.87f * s - titleH * 0.5f), kWhite,
          title);

    // The tabs under the bar; JOIN asks for the list the first time it shows.
    int tab = s_tab;
    if (Choice("##tabHost", "HOST", cx - 150 * s, 478 * s, s, tab == 0)) tab = 0;
    if (Choice("##tabJoin", "JOIN", cx + 150 * s, 478 * s, s, tab == 1)) tab = 1;
    if (tab == 1 && s_tab != 1) SteamLobby_Refresh();
    s_tab = tab;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(pMin.x + 40 * s, 520 * s), ImVec2(pMax.x - 40 * s, 520 * s), kRowRed);

    float left = cx - half.x * 0.5f;
    float right = cx + 20 * s;
    if (s_tab == 0) HostTab(cx, left, right, s);
    else JoinTab(cx, pMin.x + 50 * s, pMax.x - 50 * s, s);

    bool idle = SaveSync_GetState() == SESSION_IDLE;
    const char* status = !idle ? "Connecting..." : SaveSync_GetLastError();
    if (status && status[0]) Label(GAME_FONT_ARIAL, 22 * s, ImVec2(right, pMax.y - 60 * s), kRowRed, status);

    // Escape in the text field only reverts the text (WantTextInput is last
    // frame's: the field lets go of Escape within this one).
    if (Row("##back", "BACK", left, pMax.y - 70 * s, s) ||
        (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::GetIO().WantTextInput))
        Close();
    ImGui::End();
}
