// SPDX-License-Identifier: GPL-3.0-only
// See chat.h. The colours and fonts are the MULTIPLAYER page's
// (ui/multiplayer_page): the parchment, the red bar with the "Square" title,
// the game's Arial in brown ink. Every text is drawn in the game's fonts, so
// any language's letters show; ImGui's own font (ASCII) only measures the
// input field, whose text is drawn over it.
#include "chat.h"
#include "game_assets.h"
#include "ui_text.h"
#include "sync_block.h"
#include "multiplayer_page.h"
#include "../game/main_menu_hook.h"
#include "../game/window_focus.h"
#include "../core/profile.h"
#include "../lockstep/lockstep_net.h"
#include "../lockstep/presence.h"
#include "imgui.h"
#include "imgui_internal.h" // ActivateItemByID
#include <misc/cpp/imgui_stdlib.h>
#include <string>
#include <vector>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define CHAT_LINES 100           // lines kept
#define DEFAULT_CHAT_KEY VK_RETURN
#define FOCUS_DELAY_FRAMES 2     // the chat key's own press and letter are gone by then

enum ChatMode { CHAT_OPEN = 0, CHAT_ICON = 1, CHAT_HIDDEN = 2 };

struct ChatLine {
    int player; // -1: this PC's own
    char nick[PROFILE_NICK_BYTES];
    char text[CHAT_TEXT_BYTES];
};

static const ImU32 kRed = IM_COL32(0xAB, 0x03, 0x1D, 0xFF);
static const ImU32 kWhite = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
static const ImU32 kInk = IM_COL32(0x4A, 0x32, 0x24, 0xFF);
static const ImU32 kFaded = IM_COL32(0x4A, 0x32, 0x24, 0x90);
static const ImU32 kParchment = IM_COL32(0xEF, 0xE4, 0xD3, 0xF0);
static const ImU32 kField = IM_COL32(0xE2, 0xD3, 0xBA, 0xFF);
static const ImU32 kHot = IM_COL32(0x70, 0x00, 0x12, 0xFF);
// The card's bar as the texture has it (menu\strip_wide.png, averaged): a
// dusty matt red, for the icon.
static const ImU32 kDusty = IM_COL32(0x95, 0x3C, 0x37, 0xFF);
static const ImU32 kDustyHot = IM_COL32(0xA8, 0x4A, 0x43, 0xFF);
static const ImU32 kDustyEdge = IM_COL32(0x6A, 0x29, 0x25, 0xFF);
static const ImU32 kPaper = IM_COL32(0xEF, 0xE4, 0xD3, 0xFF);
// The cursors' colours (lockstep/presence), darker for the parchment; this
// player's own lines are in the page's red.
static const ImU32 kNickColors[PROFILE_MAX_PLAYERS] = {
    IM_COL32(0x1F, 0x4E, 0x9A, 0xFF), // the host
    IM_COL32(0xB0, 0x5A, 0x00, 0xFF),
    IM_COL32(0x2E, 0x7D, 0x32, 0xFF),
    IM_COL32(0x8E, 0x24, 0xAA, 0xFF),
};

static CRITICAL_SECTION g_chatLock;
static bool g_ready = false;
static ChatLine g_lines[CHAT_LINES]; // a ring: g_next is the oldest once full
static int g_count = 0, g_next = 0;
static volatile LONG g_unread = 0;

static int g_mode = CHAT_OPEN;
static volatile int g_key = DEFAULT_CHAT_KEY;
// The icon is the chat's place (remembered): the window opens from it - its
// corner toward the nearer screen edges on the icon's - and grows toward the
// screen's middle, so shrinking it leaves the icon where it was. Moving
// either moves both.
static ImVec2 g_iconPos;
static bool g_iconPlaced = false;
static ImVec2 g_pos;                 // the window's top left, from the icon
static bool g_windowMoving = false;
static bool g_iconMoving = false;
static int g_style = 1; // 1: the MULTIPLAYER page's card, 0: plain (a right click on the bar switches; for the testers' choice)
static std::string g_input;
static int g_caret = 0;       // the input's cursor, in bytes
static int g_focusIn = 0;     // frames until the input takes the keys (0: not asked)
static bool g_typing = false; // last frame
static int g_scroll = 0;      // lines scrolled up from the newest

static bool KeyAllowed(int vk)
{
    return vk > 0 && vk < 0xFF && vk != VK_LBUTTON && vk != VK_RBUTTON && vk != VK_ESCAPE && vk != VK_INSERT &&
           vk != Presence_GetPingKey();
}

void Chat_Bind(const TsmHost* host)
{
    TsmBind(host);
    if (!g_ready) {
        InitializeCriticalSection(&g_chatLock);
        g_ready = true;
    }
    int mode = Profile_GetInt("overlay", "chat_mode", CHAT_OPEN);
    g_mode = mode == CHAT_ICON || mode == CHAT_HIDDEN ? mode : CHAT_OPEN;
    int key = Profile_GetInt("overlay", "chat_key", DEFAULT_CHAT_KEY);
    g_key = KeyAllowed(key) ? key : DEFAULT_CHAT_KEY;
    g_style = Profile_GetInt("overlay", "chat_style", 1) == 0 ? 0 : 1;
    int x = Profile_GetInt("overlay", "chat_icon_x", -1), y = Profile_GetInt("overlay", "chat_icon_y", -1);
    if (x >= 0 && y >= 0) {
        g_iconPos = ImVec2((float)x, (float)y);
        g_iconPlaced = true;
    }
}

int Chat_GetKey() { return g_key; }

bool Chat_SetKey(int vk)
{
    if (!KeyAllowed(vk)) return false;
    g_key = vk;
    Profile_SetInt("overlay", "chat_key", vk);
    return true;
}

static void SetMode(int mode)
{
    g_mode = mode;
    Profile_SetInt("overlay", "chat_mode", mode);
    if (mode == CHAT_OPEN) {
        InterlockedExchange(&g_unread, 0);
        g_scroll = 0;
    }
}

// UTF-8 text into out, cut at a whole letter, control characters as spaces.
static void CopyText(char* out, size_t outSize, const char* text)
{
    size_t n = 0;
    for (const char* p = text; *p;) {
        unsigned c;
        int used = ImTextCharFromUtf8(&c, p, nullptr);
        if (used <= 0) break;
        if (n + used >= outSize) break;
        if (c < 0x20) {
            out[n++] = ' ';
        } else {
            memcpy(out + n, p, used);
            n += used;
        }
        p += used;
    }
    out[n] = 0;
}

static void AddLine(int player, const char* nick, const char* text)
{
    EnterCriticalSection(&g_chatLock);
    ChatLine& l = g_lines[g_next];
    l.player = player;
    strncpy_s(l.nick, nick, _TRUNCATE);
    CopyText(l.text, sizeof(l.text), text);
    g_next = (g_next + 1) % CHAT_LINES;
    if (g_count < CHAT_LINES) g_count++;
    LeaveCriticalSection(&g_chatLock);
}

void Chat_Receive(int player, const char* nick, const char* text)
{
    if (!g_ready || player < 0 || player >= PROFILE_MAX_PLAYERS) return;
    AddLine(player, nick, text);
    Logf("republicmp  CHAT: %s: %s", nick, text);
    if (g_mode != CHAT_OPEN) InterlockedIncrement(&g_unread);
}

static void SendInput()
{
    size_t a = g_input.find_first_not_of(' '), b = g_input.find_last_not_of(' ');
    if (a == std::string::npos) return;
    char text[CHAT_TEXT_BYTES];
    CopyText(text, sizeof(text), g_input.substr(a, b - a + 1).c_str());
    LockstepNet_SendChat(text);
    AddLine(-1, Profile_GetNick(), text);
    Logf("republicmp  CHAT: %s: %s", Profile_GetNick(), text);
    g_scroll = 0;
}

bool Chat_Shown()
{
    return g_ready && Presence_Active() && !SyncBlock_IsActive() && g_mode != CHAT_HIDDEN;
}

bool Chat_Typing() { return g_typing; }

// The chat key opens the window and starts typing, a couple of frames later:
// its press and its letter have gone to the game (or nowhere) by then.
static void HandleKey()
{
    static bool wasDown = false;
    bool down = IsGameWindowFocused() && (GetAsyncKeyState(g_key) & 0x8000) != 0;
    bool pressed = down && !wasDown;
    wasDown = down;
    if (!pressed || g_typing || MultiplayerPage_IsOpen() || ImGui::GetIO().WantTextInput) return;
    if (g_mode != CHAT_OPEN) SetMode(CHAT_OPEN);
    g_focusIn = FOCUS_DELAY_FRAMES;
}

static void Clamp(ImVec2* p, ImVec2 size, ImVec2 display)
{
    if (p->x > display.x - size.x) p->x = display.x - size.x;
    if (p->y > display.y - size.y) p->y = display.y - size.y;
    if (p->x < 0) p->x = 0;
    if (p->y < 0) p->y = 0;
}

// Dragging the last item moves *pos (drawn there from the next frame: this
// one's window is already placed). True on the release that ends a move.
static bool Drag(ImVec2* pos, bool* moving)
{
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        pos->x += d.x;
        pos->y += d.y;
        *moving = true;
    }
    if (*moving && ImGui::IsItemDeactivated()) {
        *moving = false;
        return true;
    }
    return false;
}

static ImVec2 WindowSize(float s) { return ImVec2(520 * s, 300 * s); }
static float IconSize(float s) { return 46 * s; }

static void PlaceIcon(float s, ImVec2 display)
{
    float d = IconSize(s);
    if (!g_iconPlaced) {
        g_iconPos = ImVec2(20 * s, display.y * 0.45f);
        g_iconPlaced = true;
    }
    Clamp(&g_iconPos, ImVec2(d, d), display);
}

static void SaveIcon()
{
    Profile_SetInt("overlay", "chat_icon_x", (int)g_iconPos.x);
    Profile_SetInt("overlay", "chat_icon_y", (int)g_iconPos.y);
}

// The window's corner the icon sits at: toward the screen's nearer edges.
static void WindowFromIcon(float s, ImVec2 display)
{
    PlaceIcon(s, display);
    float d = IconSize(s);
    ImVec2 size = WindowSize(s);
    bool right = g_iconPos.x + d * 0.5f > display.x * 0.5f;
    bool low = g_iconPos.y + d * 0.5f > display.y * 0.5f;
    g_pos = ImVec2(right ? g_iconPos.x + d - size.x : g_iconPos.x, low ? g_iconPos.y + d - size.y : g_iconPos.y);
    Clamp(&g_pos, size, display);
}

// A moved window takes the icon along: to its corner toward the nearer edges.
static void IconFromWindow(float s, ImVec2 display)
{
    float d = IconSize(s);
    ImVec2 size = WindowSize(s);
    bool right = g_pos.x + size.x * 0.5f > display.x * 0.5f;
    bool low = g_pos.y + size.y * 0.5f > display.y * 0.5f;
    g_iconPos = ImVec2(right ? g_pos.x + size.x - d : g_pos.x, low ? g_pos.y + size.y - d : g_pos.y);
    g_iconPlaced = true;
    SaveIcon();
}

static const ImGuiWindowFlags kWindowFlags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
    ImGuiWindowFlags_NoNav; // the game's keys stay the game's until a line is typed

// A line of the window: the text, and how many of its first bytes are the
// name (drawn in its player's colour).
struct WrappedLine {
    std::string text;
    size_t nameBytes;
    ImU32 nameColor;
};

// Breaks text into lines of at most width at spaces (a word wider than a
// line is broken between letters).
static void Wrap(const std::string& text, float height, float width, std::vector<std::string>* out)
{
    std::string line;
    size_t p = 0;
    while (p < text.size()) {
        size_t end = text.find(' ', p);
        if (end == std::string::npos) end = text.size();
        std::string word = text.substr(p, end - p);
        std::string tryLine = line.empty() ? word : line + " " + word;
        if (GameText_Size(GAME_FONT_ARIAL, height, tryLine.c_str()).x <= width) {
            line = tryLine;
            p = end + 1;
            continue;
        }
        if (!line.empty()) { // the word starts the next line
            out->push_back(line);
            line.clear();
            continue;
        }
        size_t fit = 0;
        for (size_t q = 0; q < word.size();) {
            unsigned c;
            int used = ImTextCharFromUtf8(&c, word.c_str() + q, word.c_str() + word.size());
            if (used <= 0) used = 1;
            if (fit && GameText_Size(GAME_FONT_ARIAL, height, word.substr(0, q + used).c_str()).x > width) break;
            q += used;
            fit = q;
        }
        out->push_back(word.substr(0, fit));
        p += fit;
    }
    if (!line.empty()) out->push_back(line);
}

// The newest lines that fit, from the oldest kept.
static void Lines(float height, float width, std::vector<WrappedLine>* out)
{
    ChatLine lines[CHAT_LINES];
    int count, first;
    EnterCriticalSection(&g_chatLock);
    memcpy(lines, g_lines, sizeof(lines));
    count = g_count;
    first = g_count < CHAT_LINES ? 0 : g_next;
    LeaveCriticalSection(&g_chatLock);
    std::vector<std::string> wrapped;
    for (int i = 0; i < count; i++) {
        const ChatLine& l = lines[(first + i) % CHAT_LINES];
        std::string name = std::string(l.nick) + ":";
        wrapped.clear();
        Wrap(name + " " + l.text, height, width, &wrapped);
        ImU32 color = l.player < 0 ? kRed : kNickColors[l.player];
        for (size_t k = 0; k < wrapped.size(); k++)
            out->push_back({ wrapped[k], k == 0 ? (name.size() < wrapped[k].size() ? name.size() : wrapped[k].size()) : 0,
                             color });
    }
}

static int InputCallback(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
        return data->EventChar < 0x20 ? 1 : 0;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit && data->BufTextLen > CHAT_TEXT_BYTES - 1) {
        // Too long: the end goes, cut at a whole letter.
        int keep = 0;
        for (int i = 0; i < data->BufTextLen;) {
            unsigned c;
            int used = ImTextCharFromUtf8(&c, data->Buf + i, data->Buf + data->BufTextLen);
            if (used <= 0 || i + used > CHAT_TEXT_BYTES - 1) break;
            i += used;
            keep = i;
        }
        data->DeleteChars(keep, data->BufTextLen - keep);
    }
    g_caret = data->CursorPos;
    return 0;
}

// A button of the bar: its glyph in white, darker under the mouse.
static bool BarButton(const char* id, const char* glyph, ImVec2 pos, float size, float s)
{
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered()) dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), kHot);
    float h = 26 * s;
    ImVec2 ts = GameText_Size(GAME_FONT_SQUARE, h, glyph);
    GameText_Draw(dl, GAME_FONT_SQUARE, h, ImVec2(pos.x + (size - ts.x) * 0.5f, pos.y + (size - h) * 0.5f), kWhite, glyph);
    return clicked;
}

// The MULTIPLAYER page's card (menu\strip_wide.png, 1200x700, its red bar on
// lines 21..62 from 28 to 1160 across) cut in three down: the top drawn so
// the bar is high enough for a title, the parchment stretched, the bottom
// edge. Gives the bar's place; false when the texture is missing.
struct CardLayout {
    float barTop, barBottom, barLeft, barRight, bottomEdge;
};

static bool DrawCard(ImDrawList* dl, ImVec2 p, ImVec2 size, float s, CardLayout* out)
{
    ImVec2 tex;
    ImTextureID id = GameAssets_Texture("menu\\strip_wide.png", &tex);
    if (!id || tex.x <= 0 || tex.y <= 0) return false;
    const float topRows = 70, bottomRows = 40, barFrom = 21, barTo = 62, barX0 = 28, barX1 = 1160;
    float ky = 34 * s / (barTo - barFrom); // the bar 34s high
    float kx = size.x / tex.x;
    float top = topRows * ky, bottom = bottomRows * kx;
    float vTop = topRows / tex.y, vBottom = (tex.y - bottomRows) / tex.y;
    ImVec2 end(p.x + size.x, p.y + size.y);
    dl->AddImage(id, p, ImVec2(end.x, p.y + top), ImVec2(0, 0), ImVec2(1, vTop));
    dl->AddImage(id, ImVec2(p.x, p.y + top), ImVec2(end.x, end.y - bottom), ImVec2(0, vTop), ImVec2(1, vBottom));
    dl->AddImage(id, ImVec2(p.x, end.y - bottom), end, ImVec2(0, vBottom), ImVec2(1, 1));
    out->barTop = p.y + barFrom * ky;
    out->barBottom = p.y + barTo * ky;
    out->barLeft = p.x + barX0 * kx;
    out->barRight = p.x + barX1 * kx;
    out->bottomEdge = 10 * s; // the torn edge
    return true;
}

// The window: the bar (moves it; shrink to the icon, hide), the lines, the input.
// Returns whether a line is being typed.
static bool DrawWindow(float s, ImVec2 display)
{
    ImVec2 size = WindowSize(s);
    if (g_windowMoving) Clamp(&g_pos, size, display);
    else WindowFromIcon(s, display);
    ImGui::SetNextWindowPos(g_pos);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##chat", nullptr, kWindowFlags);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = g_pos, end(p.x + size.x, p.y + size.y);
    // The card, or the plain parchment with its red bar along the top.
    CardLayout card;
    bool cardStyle = g_style == 1 && DrawCard(dl, p, size, s, &card);
    if (!cardStyle) {
        dl->AddRectFilled(p, end, kParchment);
        dl->AddRect(p, end, kRed, 0.0f, 0, 1.5f);
        card = { p.y, p.y + 34 * s, p.x, end.x, 0.0f };
        dl->AddRectFilled(p, ImVec2(end.x, card.barBottom), kRed);
    }
    float bar = card.barBottom - card.barTop;

    // The title on the bar (centred on the card, as the page's); dragging the
    // bar moves the window.
    float titleH = 26 * s;
    if (cardStyle) {
        char title[64];
        snprintf(title, sizeof(title), ":: %s ::", T("CHAT"));
        float w = GameText_Size(GAME_FONT_SQUARE, titleH, title).x;
        GameText_Draw(dl, GAME_FONT_SQUARE, titleH,
                      ImVec2((card.barLeft + card.barRight - w) * 0.5f, card.barTop + (bar - titleH) * 0.5f), kWhite, title);
    } else {
        GameText_Draw(dl, GAME_FONT_SQUARE, titleH, ImVec2(p.x + 12 * s, card.barTop + (bar - titleH) * 0.5f), kWhite,
                      T("CHAT"));
    }
    ImGui::SetCursorScreenPos(ImVec2(card.barLeft, card.barTop));
    ImGui::InvisibleButton("##chatBar", ImVec2(card.barRight - card.barLeft - 2 * bar, bar));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g_style = 1 - g_style;
        Profile_SetInt("overlay", "chat_style", g_style);
    }
    if (Drag(&g_pos, &g_windowMoving)) IconFromWindow(s, display);
    if (BarButton("##chatShrink", "_", ImVec2(card.barRight - 2 * bar, card.barTop), bar, s)) SetMode(CHAT_ICON);
    if (BarButton("##chatHide", "X", ImVec2(card.barRight - bar, card.barTop), bar, s)) SetMode(CHAT_HIDDEN);

    // The lines, the newest at the bottom; the wheel scrolls back.
    float textH = 20 * s, lineH = 24 * s, pad = 10 * s;
    float left = card.barLeft + pad, right = card.barRight - pad, floor = end.y - card.bottomEdge;
    float top = card.barBottom + 6 * s, bottom = floor - 46 * s;
    std::vector<WrappedLine> lines;
    Lines(textH, right - left, &lines);
    int fit = (int)((bottom - top) / lineH);
    int total = (int)lines.size();
    if (ImGui::IsWindowHovered()) g_scroll += (int)(ImGui::GetIO().MouseWheel * 3);
    int maxScroll = total > fit ? total - fit : 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;
    if (g_scroll < 0) g_scroll = 0;
    int from = total - fit - g_scroll;
    if (from < 0) from = 0;
    dl->PushClipRect(ImVec2(p.x, top), ImVec2(end.x, bottom), true);
    float y = bottom - (total - from < fit ? total - from : fit) * lineH;
    for (int i = from; i < total && i < from + fit; i++, y += lineH) {
        const WrappedLine& l = lines[i];
        ImVec2 at(left, y);
        if (l.nameBytes) {
            std::string name = l.text.substr(0, l.nameBytes);
            GameText_Draw(dl, GAME_FONT_ARIAL, textH, at, l.nameColor, name.c_str());
            at.x += GameText_Size(GAME_FONT_ARIAL, textH, name.c_str()).x;
        }
        GameText_Draw(dl, GAME_FONT_ARIAL, textH, at, kInk, l.text.c_str() + l.nameBytes);
    }
    if (!total) GameText_Draw(dl, GAME_FONT_ARIAL, textH, ImVec2(left, top + 4 * s), kFaded, T("No messages yet."));
    dl->PopClipRect();

    // The input: ImGui's field with its text hidden, the text drawn in the game's font over it.
    ImGui::SetCursorScreenPos(ImVec2(left - 2 * s, floor - 40 * s));
    ImGui::PushFont(nullptr, textH);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kField);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kField);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0xF2, 0xE8, 0xD6, 0xFF));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_InputTextCursor, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, kRed);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * s, 6 * s));
    ImGui::SetNextItemWidth(right - left + 4 * s);
    ImGuiID inputId = ImGui::GetID("##chatInput");
    if (g_focusIn > 0 && --g_focusIn == 0) ImGui::ActivateItemByID(inputId);
    bool enter = ImGui::InputText("##chatInput", &g_input,
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter |
                                      ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackAlways,
                                  InputCallback);
    bool active = ImGui::IsItemActive();
    ImVec2 fMin = ImGui::GetItemRectMin(), fMax = ImGui::GetItemRectMax();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(7);
    ImGui::PopFont();
    if (enter) {
        SendInput();
        g_input.clear();
        g_caret = 0;
    }

    float fieldPad = 6 * s, avail = fMax.x - fMin.x - 2 * fieldPad;
    ImVec2 at(fMin.x + fieldPad, fMin.y + (fMax.y - fMin.y - textH) * 0.5f);
    dl->PushClipRect(ImVec2(fMin.x + 2, fMin.y), ImVec2(fMax.x - 2, fMax.y), true);
    if (g_input.empty() && !active) {
        char key[32], hint[96];
        Presence_KeyName(g_key, key, sizeof(key));
        snprintf(hint, sizeof(hint), "[%s] %s", key, T("to write a message"));
        GameText_Draw(dl, GAME_FONT_ARIAL, textH, at, kFaded, hint);
    } else {
        // Scrolled so the cursor stays in the field.
        int caret = g_caret < 0 ? 0 : g_caret > (int)g_input.size() ? (int)g_input.size() : g_caret;
        float caretX = GameText_Size(GAME_FONT_ARIAL, textH, g_input.substr(0, caret).c_str()).x;
        float shift = caretX > avail ? caretX - avail : 0.0f;
        GameText_Draw(dl, GAME_FONT_ARIAL, textH, ImVec2(at.x - shift, at.y), kInk, g_input.c_str());
        if (active && fmodf((float)ImGui::GetTime(), 1.0f) < 0.6f)
            dl->AddLine(ImVec2(at.x - shift + caretX, at.y), ImVec2(at.x - shift + caretX, at.y + textH), kInk, 1.5f);
    }
    dl->PopClipRect();
    ImGui::End();
    return active;
}

// The icon: a speech bubble on the bar's red, the count of the lines not read
// yet. A click opens the window; a drag moves it.
static void DrawIcon(float s, ImVec2 display)
{
    float d = IconSize(s);
    PlaceIcon(s, display);
    ImVec2 p = g_iconPos; // this frame's place: a drag moves it from the next
    ImGui::SetNextWindowPos(p);
    ImGui::SetNextWindowSize(ImVec2(d, d));
    ImGui::Begin("##chatIcon", nullptr, kWindowFlags);
    ImGui::SetCursorScreenPos(p);
    bool clicked = ImGui::InvisibleButton("##chatIconButton", ImVec2(d, d));
    bool released = Drag(&g_iconPos, &g_iconMoving);
    if (released) SaveIcon();
    bool dragged = released || g_iconMoving;
    bool hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + d, p.y + d), hot ? kDustyHot : kDusty, 3 * s);
    dl->AddRect(p, ImVec2(p.x + d, p.y + d), kDustyEdge, 3 * s, 0, 1.5f * s);
    ImVec2 bMin(p.x + 9 * s, p.y + 10 * s), bMax(p.x + d - 9 * s, p.y + d - 17 * s);
    dl->AddRectFilled(bMin, bMax, kPaper, 4 * s);
    dl->AddTriangleFilled(ImVec2(p.x + 15 * s, bMax.y - 1), ImVec2(p.x + 14 * s, p.y + d - 9 * s),
                          ImVec2(p.x + 25 * s, bMax.y - 1), kPaper);
    float cy = (bMin.y + bMax.y) * 0.5f, cx = (bMin.x + bMax.x) * 0.5f;
    for (int k = -1; k <= 1; k++) dl->AddCircleFilled(ImVec2(cx + k * 7 * s, cy), 2.2f * s, kDusty);
    LONG unread = InterlockedCompareExchange(&g_unread, 0, 0);
    if (unread > 0) {
        char n[8];
        snprintf(n, sizeof(n), unread > 99 ? "99+" : "%ld", unread);
        float h = 18 * s;
        ImVec2 ts = GameText_Size(GAME_FONT_ARIAL, h, n);
        float r = (ts.x * 0.5f + 6 * s) > 11 * s ? ts.x * 0.5f + 6 * s : 11 * s;
        ImVec2 c(p.x + d - 4 * s, p.y + 4 * s);
        // Over the icon's corner, past its window (which would clip it).
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddCircleFilled(c, r, kPaper);
        fg->AddCircle(c, r, kDustyEdge, 0, 1.5f * s);
        GameText_Draw(fg, GAME_FONT_ARIAL, h, ImVec2(c.x - ts.x * 0.5f, c.y - h * 0.5f), kInk, n);
    }
    ImGui::End();
    if (clicked && !dragged) SetMode(CHAT_OPEN);
}

void Chat_Frame()
{
    if (!g_ready || !Presence_Active() || SyncBlock_IsActive()) {
        g_typing = false;
        g_focusIn = 0;
        return;
    }
    HandleKey();
    float s = MainMenuHook_UiScale();
    ImVec2 display = ImGui::GetIO().DisplaySize;
    bool typing = false;
    if (g_mode == CHAT_OPEN) typing = DrawWindow(s, display);
    else if (g_mode == CHAT_ICON) DrawIcon(s, display);
    g_typing = typing;
}
