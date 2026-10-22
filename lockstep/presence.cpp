// SPDX-License-Identifier: GPL-3.0-only
// See presence.h.
#include "presence.h"
#include "lockstep_net.h"
#include "../core/profile.h"
#include "../game/window_focus.h"
#include "../ui/sync_block.h"
#include "imgui.h"
#include <math.h>
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define MOUSE_POINT_OFFSET 0xF78 // the terrain point under the mouse
#define CAMERA_RVA 0x9EAD10      // pointer: the camera's view matrix, then its projection
#define PROJECTION_OFFSET 0xC8
#define SEND_MS 100              // the cursor is sent at most this often
#define KEEPALIVE_MS 2000        // and a still one this often
#define CURSOR_TIMEOUT_MS 6000   // a cursor not heard of for this long is gone
#define PING_MS 4000             // a ping shows this long
#define PING_COOLDOWN_MS 500
#define MAX_PINGS 8
#define EDGE_MARGIN 28.0f
#define DEFAULT_PING_KEY VK_OEM_3 // ~

struct Cursor {
    char nick[PROFILE_NICK_BYTES];
    float target[3], shown[3];
    bool visible, placed; // placed: shown holds a position
    ULONGLONG heardMs;
};

struct Ping {
    int player; // -1: this PC's own
    char nick[PROFILE_NICK_BYTES];
    float pos[3];
    ULONGLONG atMs; // 0: free
};

static CRITICAL_SECTION g_marksLock;
static bool g_marksReady = false;
static Cursor g_cursors[PROFILE_MAX_PLAYERS];
static Ping g_pings[MAX_PINGS];

static bool g_showCursors = true;
static volatile int g_pingKey = DEFAULT_PING_KEY;

static const ImU32 kColors[PROFILE_MAX_PLAYERS] = {
    IM_COL32(90, 160, 255, 255), // the host
    IM_COL32(255, 160, 60, 255),
    IM_COL32(110, 220, 110, 255),
    IM_COL32(230, 110, 230, 255),
};
static const ImU32 kOwnColor = IM_COL32(255, 255, 255, 255);

static bool PingKeyAllowed(int vk)
{
    return vk > 0 && vk < 0xFF && vk != VK_LBUTTON && vk != VK_RBUTTON && vk != VK_ESCAPE && vk != VK_INSERT;
}

void Presence_Bind(const TsmHost* host)
{
    TsmBind(host);
    if (!g_marksReady) {
        InitializeCriticalSection(&g_marksLock);
        g_marksReady = true;
    }
    g_showCursors = Profile_GetInt("overlay", "show_cursors", 1) != 0;
    int key = Profile_GetInt("overlay", "ping_key", DEFAULT_PING_KEY);
    g_pingKey = PingKeyAllowed(key) ? key : DEFAULT_PING_KEY;
}

bool Presence_GetShowCursors() { return g_showCursors; }

void Presence_SetShowCursors(bool on)
{
    g_showCursors = on;
    Profile_SetInt("overlay", "show_cursors", on ? 1 : 0);
}

int Presence_GetPingKey() { return g_pingKey; }

bool Presence_SetPingKey(int vk)
{
    if (!PingKeyAllowed(vk)) return false;
    g_pingKey = vk;
    Profile_SetInt("overlay", "ping_key", vk);
    return true;
}

void Presence_KeyName(int vk, char* out, size_t outSize)
{
    static const struct { int vk; const char* name; } kNames[] = {
        { VK_OEM_3, "~" }, { VK_MBUTTON, "Middle mouse" }, { VK_XBUTTON1, "Mouse 4" }, { VK_XBUTTON2, "Mouse 5" },
        { VK_SPACE, "Space" }, { VK_TAB, "Tab" }, { VK_CAPITAL, "Caps Lock" }, { VK_BACK, "Backspace" },
        { VK_RETURN, "Enter" }, { VK_SHIFT, "Shift" }, { VK_CONTROL, "Ctrl" }, { VK_MENU, "Alt" },
        { VK_OEM_MINUS, "-" }, { VK_OEM_PLUS, "=" }, { VK_OEM_4, "[" }, { VK_OEM_6, "]" }, { VK_OEM_5, "\\" },
        { VK_OEM_1, ";" }, { VK_OEM_7, "'" }, { VK_OEM_COMMA, "," }, { VK_OEM_PERIOD, "." }, { VK_OEM_2, "/" },
    };
    for (const auto& k : kNames)
        if (k.vk == vk) {
            strncpy_s(out, outSize, k.name, _TRUNCATE);
            return;
        }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        _snprintf_s(out, outSize, _TRUNCATE, "%c", vk);
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        _snprintf_s(out, outSize, _TRUNCATE, "F%d", vk - VK_F1 + 1);
        return;
    }
    // The keyboard's own name (numpad, arrows...), when it is ASCII.
    wchar_t w[32] = {};
    LONG scan = (LONG)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC) << 16;
    if (vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
        (vk >= VK_LEFT && vk <= VK_DOWN) || vk == VK_DIVIDE)
        scan |= 1 << 24; // the extended key, not its numpad twin
    size_t n = 0;
    if (scan && GetKeyNameTextW(scan, w, 32) > 0)
        for (; w[n] && n + 1 < outSize; n++) {
            if (w[n] < 0x20 || w[n] > 0x7E) {
                n = 0;
                break;
            }
            out[n] = (char)w[n];
        }
    if (n) out[n] = 0;
    else _snprintf_s(out, outSize, _TRUNCATE, "Key %d", vk);
}

bool Presence_Active()
{
    return LockstepNet_IsConnected() && (LockstepNet_IsHost() || LockstepNet_SessionStarted());
}

static bool Finite(const float p[3])
{
    return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]);
}

static void AddPing(int player, const char* nick, const float pos[3], ULONGLONG now)
{
    Ping* slot = &g_pings[0];
    for (Ping& p : g_pings)
        if (p.atMs < slot->atMs) slot = &p; // a free one, or the oldest
    slot->player = player;
    strncpy_s(slot->nick, nick, _TRUNCATE);
    memcpy(slot->pos, pos, sizeof(slot->pos));
    slot->atMs = now;
}

void Presence_Receive(int player, int kind, const float pos[3], const char* nick)
{
    if (!g_marksReady || player < 0 || player >= PROFILE_MAX_PLAYERS || !Finite(pos)) return;
    ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&g_marksLock);
    Cursor& c = g_cursors[player];
    strncpy_s(c.nick, nick, _TRUNCATE);
    if (kind == PRESENCE_PING) {
        AddPing(player, nick, pos, now);
    } else if (kind == PRESENCE_CURSOR) {
        memcpy(c.target, pos, sizeof(c.target));
        if (!c.placed || !c.visible) memcpy(c.shown, pos, sizeof(c.shown));
        c.placed = c.visible = true;
        c.heardMs = now;
    } else if (kind == PRESENCE_HIDDEN) {
        c.visible = false;
    }
    LeaveCriticalSection(&g_marksLock);
}

// ---------------------------------------------------------------------------
// This PC's cursor and ping
// ---------------------------------------------------------------------------

static bool MousePoint(float p[3])
{
    __try {
        memcpy(p, g_exeBase + WORLD_RVA + MOUSE_POINT_OFFSET, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return Finite(p) && (p[0] != 0 || p[1] != 0 || p[2] != 0);
}

static void SendOwn(ULONGLONG now)
{
    static ULONGLONG lastSent = 0, lastPing = 0;
    static float sent[3];
    static bool hidden = true, keyWasDown = false;
    float p[3];
    if (!IsGameWindowFocused() || !MousePoint(p)) {
        if (!hidden) LockstepNet_SendMark(PRESENCE_HIDDEN, sent);
        hidden = true;
        keyWasDown = false;
        return;
    }
    bool keyDown = (GetAsyncKeyState(g_pingKey) & 0x8000) != 0 && !ImGui::GetIO().WantCaptureKeyboard;
    if (keyDown && !keyWasDown && now - lastPing >= PING_COOLDOWN_MS) {
        lastPing = now;
        LockstepNet_SendMark(PRESENCE_PING, p);
        EnterCriticalSection(&g_marksLock);
        AddPing(-1, Profile_GetNick(), p, now);
        LeaveCriticalSection(&g_marksLock);
    }
    keyWasDown = keyDown;
    if (now - lastSent < SEND_MS) return;
    float dx = p[0] - sent[0], dy = p[1] - sent[1], dz = p[2] - sent[2];
    if (hidden || dx * dx + dy * dy + dz * dz > 0.04f || now - lastSent >= KEEPALIVE_MS) {
        LockstepNet_SendMark(PRESENCE_CURSOR, p);
        memcpy(sent, p, sizeof(sent));
        lastSent = now;
        hidden = false;
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// A world point in clip space (row vector * view * projection, as the game's
// own projection at 403f70).
static bool Clip(const float p[3], float c[4])
{
    __try {
        unsigned __int64 camera = *(unsigned __int64*)(g_exeBase + CAMERA_RVA);
        if (!camera) return false;
        const float* v = (const float*)camera;
        const float* m = (const float*)(camera + PROJECTION_OFFSET);
        float e[3];
        for (int i = 0; i < 3; i++) e[i] = p[0] * v[i] + p[1] * v[4 + i] + p[2] * v[8 + i] + v[12 + i];
        for (int i = 0; i < 4; i++) c[i] = e[0] * m[i] + e[1] * m[4 + i] + e[2] * m[8 + i] + m[12 + i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return isfinite(c[0]) && isfinite(c[1]) && isfinite(c[3]);
}

// Where the camera puts a world point, in the overlay's pixels: 1 in front of
// the camera, -1 behind it (then the point only gives the direction), 0 unknown.
static int Project(const float p[3], const ImVec2& size, ImVec2* out)
{
    float c[4];
    if (!Clip(p, c) || fabsf(c[3]) < 1e-4f) return 0;
    out->x = (c[0] / fabsf(c[3]) + 1) * 0.5f * size.x;
    out->y = (1 - c[1] / fabsf(c[3])) * 0.5f * size.y;
    return c[3] > 0 ? 1 : -1;
}

static bool OnScreen(const ImVec2& p, const ImVec2& size)
{
    return p.x >= 0 && p.y >= 0 && p.x <= size.x && p.y <= size.y;
}

// A point off the screen moved to its edge, toward it from the centre.
static ImVec2 ToEdge(ImVec2 p, const ImVec2& size, bool behind, ImVec2* dir)
{
    ImVec2 centre(size.x * 0.5f, size.y * 0.5f);
    ImVec2 d(p.x - centre.x, p.y - centre.y);
    if (fabsf(d.x) < 1 && fabsf(d.y) < 1) d = ImVec2(0, 1);
    float len = sqrtf(d.x * d.x + d.y * d.y);
    *dir = ImVec2(d.x / len, d.y / len);
    float hx = centre.x - EDGE_MARGIN, hy = centre.y - EDGE_MARGIN;
    float t = fminf(fabsf(d.x) > 1e-3f ? hx / fabsf(d.x) : 1e9f, fabsf(d.y) > 1e-3f ? hy / fabsf(d.y) : 1e9f);
    if (!behind && t > 1) t = 1;
    return ImVec2(centre.x + d.x * t, centre.y + d.y * t);
}

static ImU32 WithAlpha(ImU32 col, float a)
{
    unsigned alpha = (unsigned)(((col >> IM_COL32_A_SHIFT) & 0xFF) * (a < 0 ? 0 : a > 1 ? 1 : a));
    return (col & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

static void Label(ImDrawList* dl, ImVec2 at, const char* text, ImU32 col, float alpha)
{
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1), ImVec2(at.x + ts.x + 3, at.y + ts.y + 1),
                      IM_COL32(0, 0, 0, (int)(150 * alpha)), 3.0f);
    dl->AddText(at, WithAlpha(col, alpha), text);
}

static void DrawCursor(ImDrawList* dl, ImVec2 p, ImU32 col, const char* nick)
{
    ImVec2 a(p.x, p.y), b(p.x, p.y + 17), c(p.x + 12, p.y + 12);
    dl->AddTriangleFilled(a, b, c, col);
    dl->AddTriangle(a, b, c, IM_COL32(0, 0, 0, 220), 1.5f);
    Label(dl, ImVec2(p.x + 14, p.y + 12), nick, col, 1.0f);
}

static void DrawPing(ImDrawList* dl, const Ping& ping, const ImVec2& size, ULONGLONG now)
{
    ImVec2 p;
    int side = Project(ping.pos, size, &p);
    if (!side) return;
    float age = (float)(now - ping.atMs);
    float fade = age < PING_MS - 1000 ? 1.0f : (PING_MS - age) / 1000.0f;
    ImU32 col = ping.player < 0 ? kOwnColor : kColors[ping.player];
    if (side > 0 && OnScreen(p, size)) {
        for (int k = 0; k < 2; k++) {
            float phase = fmodf(age / 900.0f + k * 0.5f, 1.0f);
            dl->AddCircle(p, 8 + phase * 36, WithAlpha(col, (1 - phase) * fade), 32, 3.0f);
        }
        dl->AddCircleFilled(p, 6, WithAlpha(col, fade));
        dl->AddCircle(p, 6, IM_COL32(0, 0, 0, (int)(220 * fade)), 16, 1.5f);
        dl->AddLine(ImVec2(p.x, p.y - 6), ImVec2(p.x, p.y - 34), WithAlpha(col, fade), 2.0f);
        ImVec2 ts = ImGui::CalcTextSize(ping.nick);
        Label(dl, ImVec2(p.x - ts.x * 0.5f, p.y - 34 - ts.y - 2), ping.nick, col, fade);
        return;
    }
    // Off the screen: an arrow at the edge, toward it.
    ImVec2 dir;
    ImVec2 e = ToEdge(p, size, side < 0, &dir);
    ImVec2 n(-dir.y, dir.x);
    ImVec2 tip(e.x + dir.x * 14, e.y + dir.y * 14);
    ImVec2 l(e.x + n.x * 10, e.y + n.y * 10), r(e.x - n.x * 10, e.y - n.y * 10);
    float pulse = 0.6f + 0.4f * sinf(age / 120.0f);
    dl->AddTriangleFilled(tip, l, r, WithAlpha(col, fade * pulse));
    dl->AddTriangle(tip, l, r, IM_COL32(0, 0, 0, (int)(220 * fade)), 1.5f);
    ImVec2 ts = ImGui::CalcTextSize(ping.nick);
    Label(dl, ImVec2(e.x - dir.x * 14 - ts.x * 0.5f, e.y - dir.y * 14 - ts.y * 0.5f), ping.nick, col, fade);
}

static void Draw(ULONGLONG now)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    float follow = ImGui::GetIO().DeltaTime * 15.0f;
    if (follow > 1) follow = 1;
    Cursor cursors[PROFILE_MAX_PLAYERS];
    Ping pings[MAX_PINGS];
    EnterCriticalSection(&g_marksLock);
    for (Cursor& c : g_cursors)
        for (int i = 0; i < 3; i++) c.shown[i] += (c.target[i] - c.shown[i]) * follow; // smooth between updates
    memcpy(cursors, g_cursors, sizeof(cursors));
    memcpy(pings, g_pings, sizeof(pings));
    LeaveCriticalSection(&g_marksLock);

    if (g_showCursors)
        for (int i = 0; i < PROFILE_MAX_PLAYERS; i++) {
            const Cursor& c = cursors[i];
            if (!c.placed || !c.visible || now - c.heardMs > CURSOR_TIMEOUT_MS) continue;
            ImVec2 p;
            if (Project(c.shown, size, &p) > 0 && OnScreen(p, size)) DrawCursor(dl, p, kColors[i], c.nick);
        }
    for (const Ping& ping : pings)
        if (ping.atMs && now - ping.atMs < PING_MS) DrawPing(dl, ping, size, now);
}

void Presence_Frame()
{
    if (!g_marksReady || !Presence_Active() || SyncBlock_IsActive()) return;
    ULONGLONG now = GetTickCount64();
    SendOwn(now);
    Draw(now);
}
