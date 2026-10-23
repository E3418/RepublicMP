// SPDX-License-Identifier: GPL-3.0-only
// See panel2d.h.
#include "panel2d.h"
#include <math.h>
#include <string.h>

#define PANEL_RVA 0x9BE060
#define PANEL_BEGIN_IAT 0x86C918 // ?BeginDraw@C3D_PANEL2D@@QEAAXPEBD_N@Z (this, technique or null = "Basic", batch)
#define PANEL_DRAW_IAT 0x86C860  // ?Draw@C3D_PANEL2D@@QEAAXMMMMM_N@Z (this, u0, v0, u1, v1, z, set colour)
#define PANEL_END_IAT 0x86C8F8   // ?EndDraw@C3D_PANEL2D@@QEAAXXZ
#define ROTATION_XY_IAT 0x86D000 // C3DVec3GetRotationXY(out, in): the minimap's border turns its quads with it
#define CURRENT_SHADERS_RVA 0x9EAD08
#define WHITE_TEXTURE_RVA 0x9E0B38
#define TEXTURE_SET_SLOT 14 // vtable +0x70: SetTexture(slot, shaders)
#define SCREEN_WIDTH_RVA 0x99528C
#define SCREEN_HEIGHT_RVA 0x995274
#define PANEL_SIZE 0x288
#define PANEL_POS 0x290
#define PANEL_ROTATION 0x2A8
#define PANEL_COLOR 0x2AC
#define PANEL_FIELDS_BYTES 0x34 // +0x288..+0x2BB, put back after drawing
#define PANEL_ZOOM_CENTRE 0x6730
#define PANEL_ZOOM 0x673C

typedef void (*BeginFn)(void* panel, const char* technique, bool batch);
typedef void (*EndFn)(void* panel);
typedef void (*DrawFn)(void* panel, float u0, float v0, float u1, float v1, float z, bool setColor);
typedef void (*RotationFn)(float* out, const float* in);
typedef void (*SetTextureFn)(void* texture, unsigned slot, void* shaders);

void Panel2D_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool Panel2D_ScreenSize(float* width, float* height)
{
    __try {
        *width = (float)*(int*)(g_exeBase + SCREEN_WIDTH_RVA);
        *height = (float)*(int*)(g_exeBase + SCREEN_HEIGHT_RVA);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return *width > 0 && *height > 0;
}

static bool BindTexture(unsigned rva)
{
    void* texture = *(void**)(g_exeBase + rva);
    if (!texture) return false;
    ((SetTextureFn)(*(void***)texture)[TEXTURE_SET_SLOT])(texture, 0, *(void**)(g_exeBase + CURRENT_SHADERS_RVA));
    return true;
}

// Each segment is one quad: centred between its ends, as long as it plus its
// width (the joins of a polyline close), turned as the minimap turns its border.
static void DrawSegments(const float* seg, int count, float width, const float rgba[4], bool screenPixels)
{
    unsigned char* p = g_exeBase + PANEL_RVA;
    if (!BindTexture(WHITE_TEXTURE_RVA)) return;
    unsigned char saved[PANEL_FIELDS_BYTES];
    memcpy(saved, p + PANEL_SIZE, sizeof(saved));
    __try {
    float zoom = screenPixels ? *(float*)(p + PANEL_ZOOM) : 1.0f;
    if (!(zoom > 1.0f)) zoom = 1.0f;
    float zx = *(float*)(p + PANEL_ZOOM_CENTRE), zy = *(float*)(p + PANEL_ZOOM_CENTRE + 4);
    memcpy(p + PANEL_COLOR, rgba, 16);
    DrawFn draw = *(DrawFn*)(g_exeBase + PANEL_DRAW_IAT);
    RotationFn rotation = *(RotationFn*)(g_exeBase + ROTATION_XY_IAT);
    for (int i = 0; i < count; i++) {
        float x1 = seg[i * 4], y1 = seg[i * 4 + 1], x2 = seg[i * 4 + 2], y2 = seg[i * 4 + 3];
        if (zoom > 1.0f) { // Draw scales around the zoom's centre: undone for true pixels
            x1 = zx + (x1 - zx) / zoom;
            y1 = zy + (y1 - zy) / zoom;
            x2 = zx + (x2 - zx) / zoom;
            y2 = zy + (y2 - zy) / zoom;
        }
        float dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy), w = width / zoom;
        if (len < 0.5f) continue;
        float* pos = (float*)(p + PANEL_POS);
        pos[0] = (x1 + x2) * 0.5f;
        pos[1] = (y1 + y2) * 0.5f;
        pos[2] = 0;
        float* size = (float*)(p + PANEL_SIZE);
        size[0] = w;
        size[1] = len + w;
        float v[3] = { dx / len, 0, -dy / len }; // screen y down, the game's z up
        rotation(v, v);
        *(float*)(p + PANEL_ROTATION) = v[1];
        draw(p, 0, 0, 1, 1, 0, true);
    }
    } __finally {
        memcpy(p + PANEL_SIZE, saved, sizeof(saved)); // the rotation back to 0 among them
    }
}

void Panel2D_ScreenSegments(const float* seg, int count, float width, const float rgba[4])
{
    if (count <= 0) return;
    unsigned char* p = g_exeBase + PANEL_RVA;
    __try {
        (*(BeginFn*)(g_exeBase + PANEL_BEGIN_IAT))(p, nullptr, false);
        __try {
            DrawSegments(seg, count, width, rgba, true);
        } __finally {
            (*(EndFn*)(g_exeBase + PANEL_END_IAT))(p);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Panel2D_PassSegments(const float* seg, int count, float width, const float rgba[4], unsigned restoreTextureRva)
{
    if (count <= 0) return;
    __try {
        DrawSegments(seg, count, width, rgba, false);
        if (restoreTextureRva) BindTexture(restoreTextureRva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
