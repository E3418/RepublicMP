// SPDX-License-Identifier: GPL-3.0-only
// The game's own map border line, for lines of the mod's (render thread).
//
// The interface routine 317db0 draws the border every frame, right after the
// 3D frame: 411820 (411ed0 with world +0x1090 set, the map inset by 100). For
// each pair of neighbouring points of the border (world +0xFC38, 12-byte
// points) it places a C3D_NODE at the first one - turned along the pair
// (C3DVec3GetRotationFull with the side from up x d), scaled (5, 5, length *
// 0.01) - and renders the flat strip border.nmf (world +0xFC20: 5.8 m wide,
// 100 long, the dash-and-dot border.png) - then, unturned at scale 5, the
// flat signs (24 m square in the mesh) at the points +0xFC08 (border_east.nmf,
// +0xFC28: the red star) and +0xFBF0 (border_west.nmf, +0xFC30: the blue
// one): every 500 m, 350 m (+-20 at random) inside each side, 5 m above
// the ground (410080) - all with the shader technique
// "BumpBorder" (C3D_MIDDLEPOINT exe+0x9EACD0, shaders [exe+0x996820]: its
// vtable +0x30 begins and gives the passes, +0x40 / +0x48 begin and end a
// pass, +0x38 ends). Found from Sohn's notes on the border meshes (by Sohn).
//
// The calls to both at 318d06 / 318d0d go through here: the game's border,
// then the callback, which can draw strips the same way.
#pragma once

#include "tesmio_plugin.h"

void BorderDraw_Bind(const TsmHost* host);

// False when the call sites or the routine differ (nothing is drawn then).
bool BorderDraw_Ready();

// Render thread, right after the game's border, every frame a world is drawn.
typedef void (*BorderDrawFn)();
void BorderDraw_SetAfter(BorderDrawFn fn);

// From that callback: the game's border strip along `count` points (x, y, z;
// y the ground's height), each point joined to the next.
void BorderDraw_Strip(const float (*points)[3], int count);

// From that callback too: the game's border signs.
#define BORDER_SIGN_EAST 0 // the red star of the Soviet border
#define BORDER_SIGN_WEST 1 // the blue star of the western border
struct BorderSign {
    float pos[3];
    float size; // 1 the game's; 0 not drawn
    int sign;   // BORDER_SIGN_*
};
void BorderDraw_Signs(const BorderSign* signs, int count);
