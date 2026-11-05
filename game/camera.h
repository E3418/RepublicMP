// SPDX-License-Identifier: GPL-3.0-only
// The game's camera, for drawing at world points in the overlay.
//
// [exe+0x9EAD10] points at the camera's matrices: the view matrix at +0 and
// the projection at +0xC8 (row vectors, D3D style) - as the game's own
// projection at 403f70 copies them for C3DVec3Project. Read on any thread; a
// matrix being written meanwhile costs one frame's wrong position at worst.
#pragma once

#include "host.h"

void Camera_Bind(const ModHost* host);

// A world point in clip space (x, y, z, w); false when there is no camera.
bool Camera_Clip(const float p[3], float clip[4]);

// Where the camera puts a world point on a screen of width x height pixels:
// 1 in front of the camera, -1 behind it (x, y then only give the direction
// to it), 0 unknown.
int Camera_Project(const float p[3], float width, float height, float* x, float* y);

// The strategy view, as the game's camera control (3d7ab0, on the world
// object) keeps it: the point it orbits (world +fa0), the zoom it eases to
// (+fb0) and the zoom now (+fb4); the angles it takes from the camera's own
// look-at minus position (the C3D_CAMERA at exe+9e9c90: position +80,
// look-at +98), and it sets the position at target - direction * zoom.
struct CameraView {
    float target[3];
    float zoomGoal, zoom;
    float position[3];
    float lookAt[3];
};
// The orbit fields are 0 outside the orbit view (the usual one keeps its state
// in the camera only). Main thread. False when the position or look-at is
// unusable or a read faulted.
bool Camera_GetView(CameraView* out);
// Main thread: the view back, through the camera's own setters (C3DDLL64).
bool Camera_SetView(const CameraView& v);
