// SPDX-License-Identifier: GPL-3.0-only
// See republics.h.
#include "republics.h"
#include "../../api/republicmp_api.h"
#include "imgui.h"
#include <math.h>
#include <string.h>
#include <algorithm>

// The live C3D_TERRAIN's size in metres, x and z: the map is [-x/2, x/2] by
// [-z/2, z/2] around the origin, y up (C3DDLL64's GetTerrainSize, DLL 0xF8DF0,
// returns these two floats; 20000 x 20000 on the stock maps, from $WORLDSIZE
// in the map's script.ini).
#define TERRAIN_SIZE_X_OFFSET 0x87C
#define TERRAIN_SIZE_Z_OFFSET 0x880
// bool 410d50(world, const float* xyz): the point is outside the playable
// border - the map's $BORDER_POLYGON, else its BOUNDARY rectangle (placement
// refuses with it at 2caa4c; findings.md "Terrain").
#define OUT_OF_PLAYABLE_RVA 0x410D50
typedef bool (*OutOfPlayableFn)(void* world, const float* xyz);

// Points of the border drawn along the cut, and how often their heights
// are read again (terrain edits along it show within that time).
#define BORDER_POINTS 512
#define BORDER_REFRESH_MS 5000
#define REFUSED_NOTICE_MS 3000
// Metres on each side of the cut open to both players: a road or rail can
// only join the two halves through it.
#define BORDER_BAND_M 50.0f
// The playable area is measured on a grid of CUT_GRID x CUT_GRID cells
// (100 m on a stock map).
#define CUT_GRID 200

// The cut: the points with p.x*nx + p.z*nz < offset are half 0, above it
// half 1; the host plays the one it chose (SaveSync_GetHostSide). n is one of the map's two diagonals'
// normals, offset moves the line so both halves get the same playable area.
struct Cut {
    float nx, nz, offset;
};

static Cut g_cut;
static bool g_cutReady = false; // main thread
static unsigned g_cutLoads = 0; // Determinism_WorldLoads() when it was measured
static float g_border[BORDER_POINTS][3];
static volatile int g_borderCount = 0; // 0 until drawn points are ready
// The same cut inside the playable border, in pieces of at most STRIP_STEP_M
// as the game cuts its own border line (40f3f0), for the game's strip.
#define STRIP_STEP_M 500.0f
#define STRIP_POINTS 128
static float g_strip[STRIP_POINTS][3];
static volatile int g_stripCount = 0;
// The game's border signs on each half, as it sets its own (410080): one a
// piece, SIGN_INSET_M inside, SIGN_LIFT_M above the ground - without its
// random +-20 m, so every PC shows them alike. East vs West: half 0 (the
// East) has the red star, half 1 the blue one; on equal terms both
// alternate. A sign shrinks away as the camera comes near (from
// SIGN_HIDE_FAR_M to SIGN_HIDE_NEAR_M of view depth): it is 120 m wide and
// the shader has no partial transparency (BumpBorderPixel clips at alpha 0.5).
#define SIGN_INSET_M 350.0f
#define SIGN_LIFT_M 5.0f
#define SIGN_HIDE_FAR_M 1200.0f
#define SIGN_HIDE_NEAR_M 700.0f
static float g_signs[2][STRIP_POINTS][3];
static volatile int g_signCount[2] = { 0, 0 };
static BorderSign g_drawnSigns[2 * STRIP_POINTS]; // render thread
static bool g_showSigns = true; // profile "overlay" "border_signs"
static ULONGLONG g_borderAt = 0;
static unsigned g_borderLoads = 0; // the world the drawn points were read on
static volatile ULONGLONG g_refusedAt = 0; // this PC's player was refused (main thread), shown by the overlay
static float g_inside[CUT_GRID * CUT_GRID];

static bool MapSize(float* w, float* h)
{
    unsigned char* t = (unsigned char*)TerrainHook_GetTerrain();
    if (!t) return false;
    __try {
        *w = *(float*)(t + TERRAIN_SIZE_X_OFFSET);
        *h = *(float*)(t + TERRAIN_SIZE_Z_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return *w > 0 && *h > 0;
}

static bool g_measureFault = false;

static bool OutOfPlayable(const float p[3])
{
    __try {
        return ((OutOfPlayableFn)(g_exeBase + OUT_OF_PLAYABLE_RVA))(World_Object(), p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_measureFault = true;
        return false;
    }
}

// The offset that halves the playable cells along normal n (their median),
// in *cut; false when no cell is playable.
static bool HalveAlong(float nx, float nz, float w, float h, Cut* cut)
{
    int n = 0;
    for (int i = 0; i < CUT_GRID; i++)
        for (int j = 0; j < CUT_GRID; j++) {
            float p[3] = { ((i + 0.5f) / CUT_GRID - 0.5f) * w, 0, ((j + 0.5f) / CUT_GRID - 0.5f) * h };
            if (!OutOfPlayable(p)) g_inside[n++] = p[0] * nx + p[2] * nz;
        }
    if (!n) return false;
    std::nth_element(g_inside, g_inside + n / 2, g_inside + n);
    *cut = { nx, nz, g_inside[n / 2] };
    return true;
}

// Main thread. The two diagonals are tried, and the one whose halving line
// lies nearest the map's centre wins: the cut stays close to corner to corner
// on any border shape. The same map gives the same cut on every PC.
static void MakeCut(float w, float h)
{
    float len = sqrtf(w * w + h * h);
    Cut a = { h / len, -w / len, 0 }, b = { h / len, w / len, 0 };
    g_measureFault = false;
    bool okA = HalveAlong(a.nx, a.nz, w, h, &a), okB = HalveAlong(b.nx, b.nz, w, h, &b);
    if (g_measureFault) return; // measured again next time: a half-measured cut would differ between the PCs
    g_cut = okB && (!okA || fabsf(b.offset) < fabsf(a.offset)) ? b : a;
    g_cutReady = true;
    Logf("republicmp  REPUBLICS: the cut - normal (%.3f, %.3f), %.0f m from the centre (world +0x1090 %d, 0xA54BC0 %d)",
         g_cut.nx, g_cut.nz, g_cut.offset, *(int*)(World_Object() + 0x1090), *(unsigned char*)(g_exeBase + 0xA54BC0));
}

// A Republics session is running here (the host's choice, on both PCs).
static bool Active()
{
    return SaveSync_IsBusy() && SaveSync_GetGameMode() == SESSION_MODE_REPUBLICS;
}

// The cut of the world loaded now, if it was measured; any thread.
static const Cut* ReadyCut()
{
    return Active() && g_cutReady && g_cutLoads == Determinism_WorldLoads() ? &g_cut : nullptr;
}

// Main thread, outside the game's own calls (the observers, a command being
// made): the cut, measured on first use after each load - a player joining
// from a city of its own measures the host's world once it is loaded.
static const Cut* CurrentCut()
{
    if (!Active()) {
        g_cutReady = false;
        return nullptr;
    }
    unsigned loads = Determinism_WorldLoads();
    if (g_cutLoads != loads) g_cutReady = false;
    float w, h;
    if (!g_cutReady && World_IsReady() && MapSize(&w, &h)) {
        g_cutLoads = loads;
        MakeCut(w, h);
    }
    return g_cutReady ? &g_cut : nullptr;
}

// shortcut: 2 players, so the player is always 1; with more, the host has to
// tell each player its number.
static int LocalPlayer()
{
    return SaveSync_IsHostRole() ? 0 : 1;
}

// 0 the first half, 1 the second; -1 in the band along the cut, which
// belongs to both.
static int HalfOf(const float p[3], const Cut& c)
{
    float d = p[0] * c.nx + p[2] * c.nz - c.offset; // metres from the cut
    return d < -BORDER_BAND_M ? 0 : d > BORDER_BAND_M ? 1 : -1;
}

// Commands_SetAreaRule: a player builds and demolishes on their own half only,
// the host also while alone (COMMANDS_LOCAL_PLAYER). The game's own border
// still refuses what is out of the map.
static bool AreaRule(int player, const float pos[3])
{
    // A local check runs inside the game's own call (and the bulldozer's on
    // any thread): it only uses a cut already measured.
    bool local = player == COMMANDS_LOCAL_PLAYER;
    const Cut* c = local ? ReadyCut() : CurrentCut();
    if (!pos) return c != nullptr;
    if (!c) return true;
    if (local) player = LocalPlayer();
    if (!isfinite(pos[0]) || !isfinite(pos[2])) return false;
    int half = HalfOf(pos, *c);
    int hostHalf = SaveSync_GetHostSide(); // the host's choice: the player has the other half
    bool ok = half < 0 || half == (player == 0 ? hostHalf : 1 - hostHalf);
    if (!ok && player == LocalPlayer()) g_refusedAt = GetTickCount64();
    return ok;
}

// Main thread: the cut's points on the terrain, for the overlay. Every frame
// of a lockstep session (Determinism_AddFrameObserver), and every step while
// the host waits alone, before the lockstep starts (a step observer). Only
// reads the world.
static void SampleBorder()
{
    const Cut* c = CurrentCut();
    if (!c) {
        g_borderCount = 0;
        g_stripCount = g_signCount[0] = g_signCount[1] = 0;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (g_borderCount && now - g_borderAt < BORDER_REFRESH_MS && g_borderLoads == g_cutLoads) return;
    g_borderLoads = g_cutLoads;
    float w, h;
    if (!MapSize(&w, &h)) return;
    // Along the line from its point nearest the centre, both ways, inside the map.
    float len = sqrtf(w * w + h * h);
    float foot[2] = { c->nx * c->offset, c->nz * c->offset }, dir[2] = { -c->nz, c->nx };
    int n = 0;
    for (int i = 0; i < BORDER_POINTS; i++) {
        float t = ((float)i / (BORDER_POINTS - 1) - 0.5f) * len;
        float* p = g_border[n];
        p[0] = foot[0] + dir[0] * t;
        p[1] = 0;
        p[2] = foot[1] + dir[1] * t;
        if (fabsf(p[0]) > w * 0.5f || fabsf(p[2]) > h * 0.5f) continue;
        float y;
        if (TerrainHook_HeightAt(p, &y)) p[1] = y + 2.0f; // just above the ground
        n++;
    }
    g_borderAt = now;
    g_borderCount = n;
    // The strip: from the first point inside the playable border to the last.
    int first = -1, last = -1;
    g_measureFault = false;
    for (int i = 0; i < n; i++)
        if (!OutOfPlayable(g_border[i])) {
            if (first < 0) first = i;
            last = i;
        }
    if (first < 0 || last <= first || g_measureFault) {
        g_stripCount = g_signCount[0] = g_signCount[1] = 0;
        return;
    }
    const float* a = g_border[first];
    const float* b = g_border[last];
    float dx = b[0] - a[0], dz = b[2] - a[2];
    int pieces = (int)ceilf(sqrtf(dx * dx + dz * dz) / STRIP_STEP_M);
    if (pieces < 1) pieces = 1;
    if (pieces > STRIP_POINTS - 1) pieces = STRIP_POINTS - 1;
    g_stripCount = 0; // the render thread draws none while they change
    for (int i = 0; i <= pieces; i++) {
        float t = (float)i / pieces;
        float* p = g_strip[i];
        p[0] = a[0] + dx * t;
        p[1] = 0;
        p[2] = a[2] + dz * t;
        float y;
        if (TerrainHook_HeightAt(p, &y)) p[1] = y; // as the game's: on the ground, drawn over it
    }
    g_stripCount = pieces + 1;
    // The signs: across from each piece's middle, on both sides, inside the map.
    for (int half = 0; half < 2; half++) {
        float k = half == 0 ? -SIGN_INSET_M : SIGN_INSET_M; // half 0 lies below the cut's normal
        int m = 0;
        g_signCount[half] = 0;
        for (int i = 0; i < pieces; i++) {
            float* p = g_signs[half][m];
            p[0] = a[0] + dx * (i + 0.5f) / pieces + c->nx * k;
            p[1] = 0;
            p[2] = a[2] + dz * (i + 0.5f) / pieces + c->nz * k;
            if (OutOfPlayable(p)) continue;
            float y;
            p[1] = (TerrainHook_HeightAt(p, &y) ? fmaxf(y, 0.0f) : 0.0f) + SIGN_LIFT_M;
            m++;
        }
        g_signCount[half] = g_measureFault ? 0 : m;
    }
}

// On the terrain the line is dimmer, and fades out with its distance from the
// camera (metres of view depth): full up to NEAR, gone from FAR on. It is
// drawn in FADE_STEPS batches of one alpha each (the panel takes one colour
// per call).
static const float kTerrainRed[4] = { 0.72f, 0.16f, 0.14f, 0.55f };
#define BORDER_FADE_NEAR_M 300.0f
#define BORDER_FADE_FAR_M 1500.0f
#define FADE_STEPS 8
static float g_segments[FADE_STEPS][BORDER_POINTS * 4]; // render thread
static int g_segmentCount[FADE_STEPS];

// Render thread, right after the game's own border line (BorderDraw_SetAfter):
// the cut drawn the same way, the game's dash-and-dot strip on the ground.
static void DrawAfterBorder()
{
    int count = g_stripCount;
    if (!Active() || count < 2) return;
    BorderDraw_Strip(g_strip, count);
    if (!g_showSigns) return;
    bool equal = SaveSync_GetGameVariant() == SESSION_REPUBLICS_EQUAL;
    int n = 0;
    for (int half = 0; half < 2; half++)
        for (int i = 0, m = g_signCount[half]; i < m; i++) {
            BorderSign& s = g_drawnSigns[n++];
            memcpy(s.pos, g_signs[half][i], sizeof(s.pos));
            s.sign = (equal ? (i + half) % 2 : half) == 0 ? BORDER_SIGN_EAST : BORDER_SIGN_WEST;
            float clip[4];
            float depth = Camera_Clip(s.pos, clip) ? clip[3] : SIGN_HIDE_FAR_M;
            float t = (depth - SIGN_HIDE_NEAR_M) / (SIGN_HIDE_FAR_M - SIGN_HIDE_NEAR_M);
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            s.size = t * t * (3 - 2 * t); // smooth
        }
    BorderDraw_Signs(g_drawnSigns, n);
}

bool Republics_GetShowSigns() { return g_showSigns; }

void Republics_SetShowSigns(bool on)
{
    g_showSigns = on;
    Profile_SetInt("overlay", "border_signs", on ? 1 : 0);
}

// Render thread, right before the interface draws (SimPatches_SetUnderUi):
// where the game's strip cannot be drawn, a line with the game's 2D panel,
// under the HUD.
static void DrawUnderUi()
{
    int count = g_borderCount;
    float w, h;
    if (BorderDraw_Ready() || !Active() || count < 2 || !Panel2D_ScreenSize(&w, &h)) return;
    memset(g_segmentCount, 0, sizeof(g_segmentCount));
    float prev[2], prevDepth = 0;
    bool prevInFront = false;
    for (int i = 0; i < count; i++) {
        float clip[4];
        bool inFront = Camera_Clip(g_border[i], clip) && clip[3] > 1e-4f;
        float q[2] = { 0, 0 };
        if (inFront) {
            q[0] = (clip[0] / clip[3] + 1) * 0.5f * w;
            q[1] = (1 - clip[1] / clip[3]) * 0.5f * h;
        }
        if (inFront && prevInFront) {
            float depth = (clip[3] + prevDepth) * 0.5f;
            float fade = (BORDER_FADE_FAR_M - depth) / (BORDER_FADE_FAR_M - BORDER_FADE_NEAR_M);
            int step = (int)(fminf(fade, 1.0f) * FADE_STEPS + 0.5f); // 0: not drawn
            if (step > 0) {
                float* s = g_segments[step - 1] + g_segmentCount[step - 1]++ * 4;
                s[0] = prev[0];
                s[1] = prev[1];
                s[2] = q[0];
                s[3] = q[1];
            }
        }
        prev[0] = q[0];
        prev[1] = q[1];
        prevDepth = clip[3];
        prevInFront = inFront;
    }
    for (int k = 0; k < FADE_STEPS; k++) {
        float rgba[4] = { kTerrainRed[0], kTerrainRed[1], kTerrainRed[2], kTerrainRed[3] * (k + 1) / FADE_STEPS };
        Panel2D_ScreenSegments(g_segments[k], g_segmentCount[k], 2.5f, rgba);
    }
}

// A muted, dusty red on the minimap (the menu strip's #953C37): it stands out
// on the map without glaring, and is darker than the game's bright red border.
static const float kMinimapRed[4] = { 0.584f, 0.235f, 0.216f, 0.85f };

// Render thread, right after the minimap's border (Minimap_SetDraw): the cut
// across the playable area (the strip's ends; the map's when there is no
// strip), as wide as the game's border (S * 0.004).
static void DrawOnMinimap()
{
    int strip = g_stripCount, count = g_borderCount;
    float cx, cy, k;
    if (!Active() || (strip < 2 && count < 2) || !Minimap_Frame(&cx, &cy, &k)) return;
    const float* a = strip >= 2 ? g_strip[0] : g_border[0];
    const float* b = strip >= 2 ? g_strip[strip - 1] : g_border[count - 1];
    float seg[4] = { cx + a[0] * k, cy - a[2] * k, cx + b[0] * k, cy - b[2] * k };
    Minimap_DrawSegments(seg, 1, fmaxf(2.0f, k * 80.0f), kMinimapRed);
}

static bool WantsOverlay()
{
    if (!Active()) g_borderCount = g_stripCount = g_signCount[0] = g_signCount[1] = 0; // the next session's map is read again
    ULONGLONG refused = g_refusedAt;
    return refused && GetTickCount64() - refused < REFUSED_NOTICE_MS;
}

// Render thread, in the ImGui frame: the notice when this PC's player was
// refused for building on the other half.
static void DrawOverlay()
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ULONGLONG refused = g_refusedAt;
    if (refused && GetTickCount64() - refused < REFUSED_NOTICE_MS) {
        const char* text = "That is the other republic's side of the border.";
        ImVec2 ts = ImGui::CalcTextSize(text);
        ImVec2 at(size.x * 0.5f - ts.x * 0.5f, size.y * 0.12f);
        dl->AddRectFilled(ImVec2(at.x - 10, at.y - 6), ImVec2(at.x + ts.x + 10, at.y + ts.y + 6), IM_COL32(0, 0, 0, 0xB0));
        dl->AddText(at, IM_COL32(0xFF, 0xE0, 0x80, 0xFF), text);
    }
}

static void DrawPage()
{
    ImGui::TextWrapped("Each player builds their own republic on one map, split in two along a diagonal "
                       "moved so both halves get the same playable area. A red line on the map and the minimap shows it.");
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.25f, 1.0f),
                       "In development: buildings and networks stay on your half; money is still shared.");
}

static const RmpMode kRepublics = {
    "Republics",
    "Each player their own republic, 2 players",
    "One map split in two - in development",
    DrawPage,
    nullptr, // frame
    WantsOverlay,
    DrawOverlay,
};

void Republics_Bind(const TsmHost* host)
{
    TsmBind(host);
    g_showSigns = Profile_GetInt("overlay", "border_signs", 1) != 0;
    Commands_SetAreaRule(AreaRule);
    Determinism_AddFrameObserver(SampleBorder);
    Determinism_AddStepObserver(SampleBorder);
    SimPatches_SetUnderUi(DrawUnderUi);
    BorderDraw_SetAfter(DrawAfterBorder);
    Minimap_SetDraw(DrawOnMinimap);
    Modes_SetRepublics(&kRepublics);
}
