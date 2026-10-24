// SPDX-License-Identifier: GPL-3.0-only
// The game's own 2D drawing (render thread): the engine's C3D_PANEL2D at
// exe+0x9BE060, the one the HUD and the minimap draw with. What is drawn
// through it lands in the game's frame in call order, so the HUD and the
// windows drawn after it cover it (an ImGui overlay is above everything).
//
//   +0x288/+0x28C size w, h (px; h runs along the rotated quad), +0x290..+0x298
//   centre x, y, z, +0x2A8 rotation (radians; the game keeps it 0), +0x2AC
//   colour (float RGBA), +0x6730/+0x6734 the UI zoom's centre, +0x673C the
//   zoom (Draw scales the quad around the centre when it is above 1).
// BeginDraw / Draw / EndDraw are C3DDLL64's (IAT exe+0x86C918, +0x86C860,
// +0x86C8F8); a texture is bound with its vtable +0x70 (slot, the current
// shaders exe+0x9EAD08). Solid shapes use the game's editor/white.png
// (exe+0x9E0B38). Found as the minimap draws its border (4bde80).
#pragma once

#include "tesmio_plugin.h"

void Panel2D_Bind(const TsmHost* host);

// The back buffer's size in pixels (exe+0x99528C width, +0x995274 height).
bool Panel2D_ScreenSize(float* width, float* height);

// Solid segments, `count` of them as x1, y1, x2, y2 in back-buffer pixels
// (y down), `width` px wide, colour rgba 0..1. Outside the game's 2D pass:
// the panel is begun and ended here (e.g. before the interface draws).
void Panel2D_ScreenSegments(const float* seg, int count, float width, const float rgba[4]);

// The same inside a 2D pass the game has begun, in the game's own UI
// coordinates; the texture at `restoreTextureRva` (a texture pointer global)
// is bound again after, for the game's next draws.
void Panel2D_PassSegments(const float* seg, int count, float width, const float rgba[4], unsigned restoreTextureRva);
