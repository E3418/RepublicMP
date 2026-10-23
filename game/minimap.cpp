// SPDX-License-Identifier: GPL-3.0-only
// See minimap.h.
#include "minimap.h"
#include "panel2d.h"
#include "world.h"

#define MINIMAP_DRAW_RVA 0x4BDE80
// mov rax,rsp; push rbp, rbx, rsi, rdi, r14; lea rbp,[rax-0xd8] - no RIP-relative byte.
static const unsigned char kMinimapDrawExpect[16] = { 0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41,
                                                      0x56, 0x48, 0x8d, 0xa8, 0x28, 0xff, 0xff, 0xff };
#define WORLD_MINIMAP_OFFSET 0xD400
#define MINIMAP_RIGHT_OFFSET 0x48
#define MINIMAP_BOTTOM_OFFSET 0x4C
#define MINIMAP_SIDE_OFFSET 0x50
#define MINIMAP_PANEL_W_OFFSET 0x54
#define MINIMAP_PANEL_H_OFFSET 0x58
// shortcut: the game's border uses 20000 whatever the map's size (all the
// stock maps are 20 km); on another size its camera marker uses the map's.
#define MINIMAP_WORLD_SPAN 20000.0f
// The border's dash texture, bound again after a mode's segments; it is bound
// for the border's edges, the vector at world+0xFC50 (a map without
// $BORDER_POLYGON has none, and then nothing is drawn after it).
#define MINIMAP_DASH_TEXTURE_RVA 0x9E0B40
#define WORLD_BORDER_EDGES_OFFSET 0xFC50

typedef __int64 (*MinimapDrawGameFn)(void* minimap);
static MinimapDrawGameFn g_origDraw = nullptr;
static MinimapDrawFn g_draw = nullptr;

void Minimap_Bind(const TsmHost* host)
{
    TsmBind(host);
}

void Minimap_SetDraw(MinimapDrawFn fn)
{
    g_draw = fn;
}

static bool BorderDrawn()
{
    __try {
        void** edges = (void**)(World_Object() + WORLD_BORDER_EDGES_OFFSET);
        return edges[0] && edges[1] != edges[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static __int64 HookedMinimapDraw(void* minimap)
{
    __int64 r = g_origDraw(minimap);
    if (g_draw && BorderDrawn()) g_draw(); // the panel is still begun ("Basic") for the game's next draws
    return r;
}

bool Minimap_InstallHook()
{
    return InstallInlineHook(g_exeBase + MINIMAP_DRAW_RVA, (void*)HookedMinimapDraw, (void**)&g_origDraw,
                             kMinimapDrawExpect, sizeof(kMinimapDrawExpect), "republicmp minimap draw");
}

bool Minimap_Frame(float* cx, float* cy, float* pxPerMetre)
{
    __try {
        unsigned char* m = *(unsigned char**)(World_Object() + WORLD_MINIMAP_OFFSET);
        if (!m || !m[0]) return false;
        float side = *(float*)(m + MINIMAP_SIDE_OFFSET), height = *(float*)(m + MINIMAP_PANEL_H_OFFSET);
        if (!(side > 0)) return false;
        *cx = *(float*)(m + MINIMAP_RIGHT_OFFSET) - *(float*)(m + MINIMAP_PANEL_W_OFFSET) + height * 0.5f;
        *cy = *(float*)(m + MINIMAP_BOTTOM_OFFSET) - height * 0.5f;
        *pxPerMetre = side / MINIMAP_WORLD_SPAN;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Minimap_DrawSegments(const float* seg, int count, float width, const float rgba[4])
{
    Panel2D_PassSegments(seg, count, width, rgba, MINIMAP_DASH_TEXTURE_RVA);
}
