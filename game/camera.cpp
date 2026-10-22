// SPDX-License-Identifier: GPL-3.0-only
// See camera.h.
#include "camera.h"
#include <math.h>

#define CAMERA_RVA 0x9EAD10 // pointer: the camera's view matrix, then its projection
#define PROJECTION_OFFSET 0xC8

void Camera_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool Camera_Clip(const float p[3], float c[4])
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

int Camera_Project(const float p[3], float width, float height, float* x, float* y)
{
    float c[4];
    if (!Camera_Clip(p, c) || fabsf(c[3]) < 1e-4f) return 0;
    *x = (c[0] / fabsf(c[3]) + 1) * 0.5f * width;
    *y = (1 - c[1] / fabsf(c[3])) * 0.5f * height;
    return c[3] > 0 ? 1 : -1;
}
