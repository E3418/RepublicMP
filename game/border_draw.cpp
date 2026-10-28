// SPDX-License-Identifier: GPL-3.0-only
// See border_draw.h.
#include "border_draw.h"
#include "world.h"
#include <math.h>
#include <string.h>
#include <limits.h>

#define BORDER_DRAW_RVA 0x411820
#define BORDER_DRAW_INSET_RVA 0x411ED0
#define BORDER_CALL_INSET 0x318D06 // call 411ed0
#define BORDER_CALL 0x318D0D       // call 411820
#define BORDER_MESH_OFFSET 0xFC20  // world: border.nmf's C3D_MESH
#define EAST_MESH_OFFSET 0xFC28    // border_east.nmf: the Soviet star
#define WEST_MESH_OFFSET 0xFC30    // border_west.nmf: the western star
#define MIDDLEPOINT_RVA 0x9EACD0
#define SHADERS_RVA 0x996820       // pointer: the current C3DAPI_SHADERS
#define TECHNIQUE_RVA 0x8CBF50     // "BumpBorder"
#define SET_SHADER_PAIR_IAT 0x86C4A8 // C3D_MIDDLEPOINT::RenderingSetShaderPair(shaders, technique)
#define NODE_CTOR_IAT 0x86C368       // C3D_NODE::C3D_NODE()
#define NODE_CREATE_IAT 0x86C360     // C3D_NODE::CreateFromPositionRotationScale(pos, rot, scale)
#define SET_NODE_IAT 0x86C438        // C3D_MIDDLEPOINT::RenderingSetNode(node, bool, bool, bool, float)
#define MESH_RENDER_IAT 0x86CAA0     // C3D_MESH::Render(...), 14 arguments
#define VEC_CROSS_IAT 0x86CAC0       // C3DVec3Cross(out, a, b)
#define VEC_ROTATION_IAT 0x86D268    // C3DVec3GetRotationFull(out, dir, side, up)
#define SHADERS_BEGIN_SLOT 6         // vtable +0x30: the passes
#define SHADERS_END_SLOT 7           // +0x38
#define SHADERS_BEGIN_PASS_SLOT 8    // +0x40 (pass, -1)
#define SHADERS_END_PASS_SLOT 9      // +0x48
#define STRIP_WIDTH_SCALE 5.0f
#define STRIP_LENGTH_SCALE 0.01f     // the mesh is 100 long
#define NODE_BYTES 0x800             // the game keeps one in under 0x240 bytes of its frame

typedef void (*SetShaderPairFn)(void* mp, void* shaders, const char* technique);
typedef void (*NodeCtorFn)(void* node);
typedef void (*NodeCreateFn)(void* node, const float* pos, const float* rot, const float* scale);
typedef void (*SetNodeFn)(void* mp, void* node, bool a, bool b, bool c, float d);
typedef void (*MeshRenderFn)(void* mesh, int a, bool b, bool c, int d, int e, void* f, bool g, void* h, unsigned i,
                             unsigned j, void* k, unsigned l, unsigned m, float n);
typedef float* (*CrossFn)(float* out, const float* a, const float* b);
typedef void (*RotationFn)(float* out, const float* dir, const float* side, const float* up);
typedef unsigned (*ShadersBeginFn)(void* shaders);
typedef void (*ShadersFn)(void* shaders);
typedef void (*ShadersPassFn)(void* shaders, unsigned pass, int flags);
typedef void (*BorderFn)(void* world);

static const unsigned char kBorderEntry[16] = { 0x4c, 0x8b, 0xdc, 0x55, 0x53, 0x41, 0x55, 0x49,
                                                0x8d, 0xab, 0xd8, 0xfc, 0xff, 0xff, 0x48, 0x81 };
static bool g_ready = false;
static BorderDrawFn g_after = nullptr;
alignas(16) static unsigned char g_node[NODE_BYTES]; // render thread

template <typename T> static T Import(unsigned iat)
{
    return *(T*)(g_exeBase + iat);
}

static void After()
{
    BorderDrawFn fn = g_after;
    if (fn) fn();
}

static void RelayBorder(void* world)
{
    ((BorderFn)(g_exeBase + BORDER_DRAW_RVA))(world);
    After();
}

static void RelayBorderInset(void* world)
{
    ((BorderFn)(g_exeBase + BORDER_DRAW_INSET_RVA))(world);
    After();
}

static bool CallsTo(unsigned site, unsigned target)
{
    const BYTE* at = g_exeBase + site;
    int rel;
    memcpy(&rel, at + 1, 4);
    return at[0] == 0xE8 && (long long)site + 5 + rel == target;
}

// Each site's call goes to a jump in a relay near the exe.
static bool Redirect(BYTE* slot, unsigned site, void* target)
{
    DWORD old;
    VirtualProtect(slot, 14, PAGE_EXECUTE_READWRITE, &old);
    slot[0] = 0xFF;
    slot[1] = 0x25;
    memset(slot + 2, 0, 4);
    memcpy(slot + 6, &target, 8);
    VirtualProtect(slot, 14, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, 14);
    BYTE* at = g_exeBase + site;
    long long to = (long long)slot - (long long)(at + 5);
    if (to < INT_MIN || to > INT_MAX) return false;
    int rel = (int)to;
    VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at + 1, &rel, 4);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    return true;
}

void BorderDraw_Bind(const TsmHost* host)
{
    TsmBind(host);
    if (!CallsTo(BORDER_CALL, BORDER_DRAW_RVA) || !CallsTo(BORDER_CALL_INSET, BORDER_DRAW_INSET_RVA) ||
        memcmp(g_exeBase + BORDER_DRAW_RVA, kBorderEntry, sizeof(kBorderEntry)) ||
        strcmp((const char*)(g_exeBase + TECHNIQUE_RVA), "BumpBorder")) {
        Logf("republicmp  BORDER_DRAW: the border's calls differ - the game's border line is not offered");
        return;
    }
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) return;
    g_ready = Redirect(relay, BORDER_CALL, (void*)&RelayBorder) &&
              Redirect(relay + 16, BORDER_CALL_INSET, (void*)&RelayBorderInset);
    Logf("republicmp  BORDER_DRAW: %s", g_ready ? "ready" : "a relay out of reach - not offered");
}

bool BorderDraw_Ready() { return g_ready; }

void BorderDraw_SetAfter(BorderDrawFn fn) { g_after = fn; }

// As 411820 draws one edge: the node at a, along b - a.
static void Piece(void* mp, void* mesh, const float* a, const float* b)
{
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 0.01f) return;
    static const float kUp[3] = { 0, 1, 0 };
    float side[3], rot[3];
    Import<CrossFn>(VEC_CROSS_IAT)(side, kUp, d);
    Import<RotationFn>(VEC_ROTATION_IAT)(rot, d, side, kUp);
    float pos[3] = { a[0], a[1], a[2] };
    float scale[3] = { STRIP_WIDTH_SCALE, STRIP_WIDTH_SCALE, len * STRIP_LENGTH_SCALE };
    Import<NodeCreateFn>(NODE_CREATE_IAT)(g_node, pos, rot, scale);
    Import<SetNodeFn>(SET_NODE_IAT)(mp, g_node, true, false, false, 0.0f);
    Import<MeshRenderFn>(MESH_RENDER_IAT)(mesh, 0, true, false, -1, -1, nullptr, true, nullptr, 0, 0, nullptr, 0, 0,
                                         -1.0f);
}

// As 411820 draws a sign: the node at p, not turned, scale 5 (times `size`).
static void Sign(void* mp, void* mesh, const float* p, float size)
{
    static const float kRot[3] = { 0, 0, 0 };
    float pos[3] = { p[0], p[1], p[2] };
    float k = STRIP_WIDTH_SCALE * size;
    float scale[3] = { k, k, k };
    Import<NodeCreateFn>(NODE_CREATE_IAT)(g_node, pos, kRot, scale);
    Import<SetNodeFn>(SET_NODE_IAT)(mp, g_node, true, false, false, 0.0f);
    Import<MeshRenderFn>(MESH_RENDER_IAT)(mesh, 0, true, false, -1, -1, nullptr, true, nullptr, 0, 0, nullptr, 0, 0,
                                         -1.0f);
}

// The mesh at world `meshOffset`, through every pass of "BumpBorder": as a
// strip along the points, or the signs of that kind.
static void Draw(unsigned meshOffset, const float (*points)[3], int count, const BorderSign* signs, int kind)
{
    bool strip = signs == nullptr;
    if (!g_ready || count < (strip ? 2 : 1)) return;
    __try {
        void* mesh = *(void**)(g_exeBase + GAME_WORLD_RVA + meshOffset);
        void* shaders = *(void**)(g_exeBase + SHADERS_RVA);
        if (!mesh || !shaders) return;
        void* mp = g_exeBase + MIDDLEPOINT_RVA;
        Import<SetShaderPairFn>(SET_SHADER_PAIR_IAT)(mp, shaders, (const char*)(g_exeBase + TECHNIQUE_RVA));
        Import<NodeCtorFn>(NODE_CTOR_IAT)(g_node);
        void** vt = *(void***)shaders;
        unsigned passes = ((ShadersBeginFn)vt[SHADERS_BEGIN_SLOT])(shaders);
        for (unsigned p = 0; p < passes; p++) {
            ((ShadersPassFn)vt[SHADERS_BEGIN_PASS_SLOT])(shaders, p, -1);
            if (strip)
                for (int i = 0; i + 1 < count; i++) Piece(mp, mesh, points[i], points[i + 1]);
            else
                for (int i = 0; i < count; i++)
                    if (signs[i].sign == kind && signs[i].size > 0.01f) Sign(mp, mesh, signs[i].pos, signs[i].size);
            ((ShadersFn)vt[SHADERS_END_PASS_SLOT])(shaders);
        }
        ((ShadersFn)vt[SHADERS_END_SLOT])(shaders);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void BorderDraw_Strip(const float (*points)[3], int count)
{
    Draw(BORDER_MESH_OFFSET, points, count, nullptr, 0);
}

void BorderDraw_Signs(const BorderSign* signs, int count)
{
    Draw(EAST_MESH_OFFSET, nullptr, count, signs, BORDER_SIGN_EAST);
    Draw(WEST_MESH_OFFSET, nullptr, count, signs, BORDER_SIGN_WEST);
}
