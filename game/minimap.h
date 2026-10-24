// SPDX-License-Identifier: GPL-3.0-only
// The HUD's minimap (render thread). 4bde80 draws its map, town names and
// the playable border (called only from 4bec9a in 4beb10, under 4c1370 in the
// interface 317db0); it is hooked so a mode draws on the map right after the
// border, in the game's 2D pass (game/panel2d): the windows drawn later cover it.
//
//   [world+0xD400]  the minimap: byte +0 open (else collapsed), +0x48/+0x4C the
//                   panel's right and bottom edges (rewritten each frame),
//                   +0x50 the map's side S, +0x54/+0x58 the panel's width
//                   and height. The map square is centred at (right - width +
//                   height/2, bottom - height/2), +x right, +z up, S/20000 px
//                   a metre: the scale the border is drawn with.
#pragma once

#include "tesmio_plugin.h"

void Minimap_Bind(const TsmHost* host);
bool Minimap_InstallHook();

// Called right after the border is drawn (render thread); one, null for none.
typedef void (*MinimapDrawFn)(void);
void Minimap_SetDraw(MinimapDrawFn fn);

// The open map's centre and pixels per metre; false while it is collapsed.
bool Minimap_Frame(float* cx, float* cy, float* pxPerMetre);

// Inside a MinimapDrawFn: solid segments (x1, y1, x2, y2 each, the minimap's
// coordinates from Minimap_Frame), drawn as the border is.
void Minimap_DrawSegments(const float* seg, int count, float width, const float rgba[4]);
