// SPDX-License-Identifier: GPL-3.0-only
// See camera.h.
#include "camera.h"
#include <math.h>
#include <string.h>

#define CAMERA_RVA 0x9EAD10 // pointer: the camera's view matrix, then its projection
#define PROJECTION_OFFSET 0xC8
#define WORLD_RVA 0x9D4F10
#define VIEW_CAMERA_RVA 0x9E9C90 // the C3D_CAMERA the strategy view sets
#define VIEW_POSITION 0x80
#define VIEW_LOOK_AT 0x98
#define VIEW_TARGET 0xFA0       // world: the point the view orbits
#define VIEW_ZOOM_GOAL 0xFB0
#define VIEW_ZOOM 0xFB4
#define DLL_C3D "C3DDLL64.dll"

// A C3DVECTOR3 by value is passed as a pointer to the caller's copy.
typedef void (*CamVectorFn)(void* camera, const float* v);
typedef void (*CamViewFn)(void* camera, void* a, void* b);
typedef void (*CamFinalFn)(void* camera);
static CamVectorFn g_setPosition = nullptr, g_setLookAt = nullptr;
static CamViewFn g_calculateView = nullptr;
static CamFinalFn g_calculateFinal = nullptr;

void Camera_Bind(const ModHost* host)
{
    ModHost_Bind(host);
    HMODULE c3d = GetModuleHandleA(DLL_C3D);
    if (!c3d) return;
    g_setPosition = (CamVectorFn)GetProcAddress(c3d, "?ViewSetPosition@C3D_CAMERA@@QEAAXVC3DVECTOR3@@@Z");
    g_setLookAt = (CamVectorFn)GetProcAddress(c3d, "?ViewSetLookAt@C3D_CAMERA@@QEAAXVC3DVECTOR3@@@Z");
    g_calculateView = (CamViewFn)GetProcAddress(c3d, "?CalculateView@C3D_CAMERA@@QEAAXPEAVC3DMATRIX@@0@Z");
    g_calculateFinal = (CamFinalFn)GetProcAddress(c3d, "?CalculateFinal@C3D_CAMERA@@QEAAXXZ");
}

static bool Finite(const float* v, int n)
{
    for (int i = 0; i < n; i++)
        if (!isfinite(v[i])) return false;
    return true;
}

bool Camera_GetView(CameraView* out)
{
    __try {
        const unsigned char* world = g_exeBase + WORLD_RVA;
        const unsigned char* camera = g_exeBase + VIEW_CAMERA_RVA;
        memcpy(out->target, world + VIEW_TARGET, sizeof(out->target));
        out->zoomGoal = *(const float*)(world + VIEW_ZOOM_GOAL);
        out->zoom = *(const float*)(world + VIEW_ZOOM);
        memcpy(out->position, camera + VIEW_POSITION, sizeof(out->position));
        memcpy(out->lookAt, camera + VIEW_LOOK_AT, sizeof(out->lookAt));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // The world's orbit fields are 0 in the usual view: its state is the
    // camera's own position and look-at.
    return Finite(out->position, 3) && Finite(out->lookAt, 3) &&
           (out->position[0] != out->lookAt[0] || out->position[1] != out->lookAt[1] || out->position[2] != out->lookAt[2]);
}

bool Camera_SetView(const CameraView& v)
{
    if (!g_setPosition || !g_setLookAt || !g_calculateView || !g_calculateFinal) return false;
    if (!Finite(v.position, 3) || !Finite(v.lookAt, 3)) return false;
    bool orbit = Finite(v.target, 3) && Finite(&v.zoomGoal, 2) && v.zoom > 0;
    __try {
        unsigned char* world = g_exeBase + WORLD_RVA;
        void* camera = g_exeBase + VIEW_CAMERA_RVA;
        if (orbit) {
            memcpy(world + VIEW_TARGET, v.target, sizeof(v.target));
            *(float*)(world + VIEW_ZOOM_GOAL) = v.zoomGoal;
            *(float*)(world + VIEW_ZOOM) = v.zoom;
        }
        g_setPosition(camera, v.position);
        g_setLookAt(camera, v.lookAt);
        g_calculateView(camera, nullptr, nullptr); // as the camera control does after setting it
        g_calculateFinal(camera);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
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
