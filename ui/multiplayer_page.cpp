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

void MultiplayerPage_Open()
{
    s_open = true;
    s_menuOn = true; // its builder just ran: the click came from it
    MainMenuHook_SetHidden(true);
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
    s_sawUnloaded = !SaveSync_IsWorldLoaded();
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
    if (s_mode < 0) s_mode = Profile_GetInt("profile", "game_mode", 0) == 1 ? 1 : 0;

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

    // Left: the game mode, then hosting in the menu's own rows.
    float left = cx - half.x * 0.5f;
    Label(GAME_FONT_ARIAL, 24 * s, ImVec2(left - 225 * s, 478 * s), kInk, "Game mode:");
    int mode = s_mode;
    if (Choice("##coop", "CO-OP", left - 110 * s, 505 * s, s, mode == 0)) mode = 0;
    if (Choice("##republics", "REPUBLICS", left + 110 * s, 505 * s, s, mode == 1)) mode = 1;
    if (mode != s_mode) {
        s_mode = mode;
        Profile_SetInt("profile", "game_mode", mode);
    }
    // Only co-op plays for now; Republics shows what it will be (modes/republics).
    const RmpMode* republics = Modes_Republics();
    bool playable = s_mode == 0;
    bool idle = SaveSync_GetState() == SESSION_IDLE;
    if (playable) {
        if (Row("##hostNew", "HOST NEW GAME", left, 575 * s, s, idle)) HostThrough(MAIN_MENU_CUSTOM_GAME);
        if (Row("##hostSave", "HOST SAVED GAME", left, 615 * s, s, idle)) HostThrough(MAIN_MENU_LOAD_GAME);
        Label(GAME_FONT_ARIAL, 22 * s, ImVec2(left - 225 * s, 670 * s), kInk, "Starts hosting once the city is loaded.");
    } else {
        float y = 575 * s;
        if (republics && republics->line1) {
            Label(GAME_FONT_ARIAL, 24 * s, ImVec2(left - 225 * s, y), kInk, republics->line1);
            y += 32 * s;
        }
        if (republics && republics->line2) {
            Label(GAME_FONT_ARIAL, 24 * s, ImVec2(left - 225 * s, y), kInk, republics->line2);
            y += 32 * s;
        }
        Label(GAME_FONT_ARIAL, 24 * s, ImVec2(left - 225 * s, y + 10 * s), kRowRed, "In development - not playable yet.");
    }

    // Right: joining, and our own SteamID64 to give out.
    float right = cx + 20 * s;
    float width = half.x - 60 * s;
    Label(GAME_FONT_ARIAL, 26 * s, ImVec2(right, 495 * s), kInk, "Host's SteamID64 or IP:");
    ImGui::SetCursorScreenPos(ImVec2(right, 530 * s));
    ImGui::PushFont(nullptr, 26 * s);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0xE2, 0xD3, 0xBA, 0xFF));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0xEA, 0xDD, 0xC6, 0xFF));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0xF2, 0xE8, 0xD6, 0xFF));
    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    ImGui::PushStyleColor(ImGuiCol_Border, kRowRed);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::SetNextItemWidth(width);
    bool enter = ImGui::InputTextWithHint("##target", "76561198... or 192.168...", &s_target,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    ImGui::PopFont();
    bool canJoin = playable && idle && !s_target.empty();
    if ((Row("##connect", "CONNECT", right + width * 0.5f, 580 * s, s, canJoin) || enter) && canJoin)
        SaveSync_StartClient(s_target.c_str());

    if (unsigned long long id = NetworkSteamId()) {
        char idText[32];
        snprintf(idText, sizeof(idText), "%llu", id);
        Label(GAME_FONT_ARIAL, 26 * s, ImVec2(right, 660 * s), kInk, "Your SteamID64:");
        Label(GAME_FONT_ARIAL, 30 * s, ImVec2(right, 692 * s), kInk, idText);
        if (Row("##copy", "COPY", right + width * 0.5f, 730 * s, s)) ImGui::SetClipboardText(idText);
    } else {
        Label(GAME_FONT_ARIAL, 22 * s, ImVec2(right, 660 * s), kInk, "Steam is not running: join by IP only.");
    }

    const char* status = !idle ? "Connecting..." : SaveSync_GetLastError();
    if (status && status[0]) Label(GAME_FONT_ARIAL, 22 * s, ImVec2(right, 800 * s), kRowRed, status);

    // Escape in the text field only reverts the text (WantTextInput is last
    // frame's: the field lets go of Escape within this one).
    if (Row("##back", "BACK", left, pMax.y - 70 * s, s) ||
        (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::GetIO().WantTextInput))
        Close();
    ImGui::End();
}
