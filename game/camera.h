// SPDX-License-Identifier: GPL-3.0-only
// The game's camera, for drawing at world points in the overlay.
//
// [exe+0x9EAD10] points at the camera's matrices: the view matrix at +0 and
// the projection at +0xC8 (row vectors, D3D style) - as the game's own
// projection at 403f70 copies them for C3DVec3Project. Read on any thread; a
// matrix being written meanwhile costs one frame's wrong position at worst.
#pragma once

#include "tesmio_plugin.h"

void Camera_Bind(const TsmHost* host);

// A world point in clip space (x, y, z, w); false when there is no camera.
bool Camera_Clip(const float p[3], float clip[4]);

// Where the camera puts a world point on a screen of width x height pixels:
// 1 in front of the camera, -1 behind it (x, y then only give the direction
// to it), 0 unknown.
int Camera_Project(const float p[3], float width, float height, float* x, float* y);
