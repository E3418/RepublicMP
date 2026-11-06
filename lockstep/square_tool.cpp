// SPDX-License-Identifier: GPL-3.0-only
// See square_tool.h. Follows SoylentGreed's route (RepublicLAN, read against the
// disassembly of 1.1.1.9).
#include "square_tool.h"
#include "commands.h"
#include "../world/view_list_scope.h"
#include <math.h>
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define BUILD_RVA 0x3CE6C0
#define CONSTRUCT_RVA 0x5247F0
#define CLEAR_RVA 0x3AE120
#define YAW_RVA 0x9E9D20
#define NETWORK_OFFSET 0xF7A8
#define LIST_OFFSET 0xF608
#define FIRST_OFFSET 0xF620
#define SECOND_OFFSET 0xF62C
#define PIECE_POINTS 8
#define POINT_BYTES 0x18
#define MAX_PIECES (1u << 16)
#define SCOPE_MARGIN 20.0f
#define MAGIC 0x31525153

struct Routine {
    unsigned rva;
    unsigned char entry[16];
};
static const Routine kRoutines[] = {
    { BUILD_RVA, { 0x48, 0x8b, 0xc4, 0x44, 0x88, 0x40, 0x18, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54 } },
    { CONSTRUCT_RVA, { 0x4c, 0x8b, 0xdc, 0x45, 0x88, 0x4b, 0x20, 0x45, 0x89, 0x43, 0x18, 0x49, 0x89, 0x4b, 0x08, 0x55 } },
    { CLEAR_RVA, { 0x48, 0x8b, 0xc4, 0x44, 0x88, 0x48, 0x20, 0x48, 0x89, 0x48, 0x08, 0x55, 0x48, 0x8d, 0x68, 0xe8 } },
};

typedef U64 (*BuildFn)(U64 world, U64 type, U64 preview);
typedef void (*ConstructFn)(U64 network, U64 piece, int kind, char a4, char a5, float a6, float a7);
typedef void (*ClearFn)(U64 world, const float* point, float radius, char a4);

static int g_ready = -1;

void SquareTool_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

bool SquareTool_Ready()
{
    if (g_ready < 0) {
        g_ready = 1;
        for (const Routine& r : kRoutines)
            if (memcmp(g_exeBase + r.rva, r.entry, sizeof(r.entry)) != 0) {
                Logf("republicmp  SQUARE: game routine at 0x%X differs - the square tools are put away in lockstep", r.rva);
                g_ready = 0;
                break;
            }
    }
    return g_ready == 1;
}

static U64 Q(U64 at) { return *(U64*)at; }

U64 SquareTool_Preview(U64 world, U64 type, U64 preview)
{
    if (Commands_Capturing() && SquareTool_Ready()) return 0;
    return ((BuildFn)(g_exeBase + BUILD_RVA))(world, type, preview);
}

U64 SquareTool_Final(U64 world, U64 type, U64 preview)
{
    if (!Commands_Capturing() || !SquareTool_Ready()) return ((BuildFn)(g_exeBase + BUILD_RVA))(world, type, preview);
    if (!Commands_TakeLocal()) {
        Logf("republicmp  SQUARE: a square while a command or a tick was being made - nothing is done");
        return 0;
    }
    SquareMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    m.type = (int)(unsigned)type;
    memcpy(m.first, (const void*)(world + FIRST_OFFSET), 12);
    memcpy(m.second, (const void*)(world + SECOND_OFFSET), 12);
    m.yaw = *(float*)(g_exeBase + YAW_RVA);
    Commands_Submit(LS_CMD_SQUARE, &m, sizeof(m));
    return 0;
}

void SquareTool_Corners(const SquareMsg& m, float corners[4][3])
{
    float dx = m.second[0] - m.first[0], dz = m.second[2] - m.first[2];
    float c = cosf(m.yaw), s = sinf(m.yaw);
    float u = dx * c + dz * s, v = -dx * s + dz * c; // into the square's frame
    memcpy(corners[0], m.first, 12);
    memcpy(corners[1], m.second, 12);
    corners[2][0] = m.first[0] + u * c;
    corners[2][1] = m.first[1];
    corners[2][2] = m.first[2] + u * s;
    corners[3][0] = m.first[0] - v * s;
    corners[3][1] = m.first[1];
    corners[3][2] = m.first[2] + v * c;
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

static bool Build(U64 world, int type)
{
    __try {
        ((BuildFn)(g_exeBase + BUILD_RVA))(world, (U64)(unsigned)type, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The handler's loop on the pieces (3cff3d..3d00cf), last to first.
static bool Construct(U64 world, int type, unsigned* pieces, unsigned* points)
{
    __try {
        U64 network = Q(world + NETWORK_OFFSET);
        U64 begin = Q(world + LIST_OFFSET), end = Q(world + LIST_OFFSET + 8);
        if (!network || end < begin || (end - begin) % 8 || (end - begin) / 8 > MAX_PIECES) return false;
        unsigned count = (unsigned)((end - begin) / 8);
        int kind = type == 0 ? 0xD : (type == 1 || type == -2) ? 0 : 4;
        float radius = type == 0 ? 7.0f : 3.0f;
        for (unsigned k = count; k-- > 0;) {
            U64 piece = Q(Q(world + LIST_OFFSET) + (U64)k * 8);
            ((ConstructFn)(g_exeBase + CONSTRUCT_RVA))(network, piece, kind, 0, 1, 1.0f, 1.0f);
            U64 first = Q(piece + PIECE_POINTS);
            unsigned n = (unsigned)((Q(piece + PIECE_POINTS + 8) - first) / POINT_BYTES);
            for (unsigned i = 0; i < n; i++) {
                float point[3];
                memcpy(point, (const void*)(Q(piece + PIECE_POINTS) + (U64)i * POINT_BYTES), 12);
                ((ClearFn)(g_exeBase + CLEAR_RVA))(world, point, radius, 1);
                ++*points;
            }
        }
        *pieces = count;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SquareTool_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(SquareMsg)) return false;
    SquareMsg m;
    memcpy(&m, data, sizeof(m));
    bool typeOk = m.type == 0 || m.type == 1 || m.type == 2 || m.type == -2 || m.type == -3 || m.type == 9;
    if (m.magic != MAGIC || !typeOk || !SquareTool_Ready() || !isfinite(m.yaw) || fabsf(m.yaw) > 1.0e4f) return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(m.first[i]) || !isfinite(m.second[i]) || fabsf(m.first[i]) > 1.0e6f || fabsf(m.second[i]) > 1.0e6f) return false;
    U64 world = (U64)(g_exeBase + WORLD_RVA);
    BYTE* w = (BYTE*)world;
    float own[6], yaw = *(float*)(g_exeBase + YAW_RVA);
    memcpy(own, w + FIRST_OFFSET, sizeof(own));
    *(U64*)(world + LIST_OFFSET + 8) = Q(world + LIST_OFFSET);
    memcpy(w + FIRST_OFFSET, m.first, 12);
    memcpy(w + SECOND_OFFSET, m.second, 12);
    *(float*)(g_exeBase + YAW_RVA) = m.yaw;
    // Only the edges around the square in the networks' visible lists, alike on every PC.
    float mx = (m.first[0] + m.second[0]) * 0.5f, mz = (m.first[2] + m.second[2]) * 0.5f;
    float dx = m.second[0] - m.first[0], dz = m.second[2] - m.first[2];
    float reach = sqrtf(dx * dx + dz * dz) * 0.5f + SCOPE_MARGIN;
    bool scoped = ViewListScope_Begin(mx - reach, mz - reach, mx + reach, mz + reach);
    unsigned pieces = 0, points = 0;
    bool ok = Build(world, m.type) && Construct(world, m.type, &pieces, &points);
    if (scoped) ViewListScope_End();
    *(U64*)(world + LIST_OFFSET + 8) = Q(world + LIST_OFFSET);
    memcpy(w + FIRST_OFFSET, own, sizeof(own));
    *(float*)(g_exeBase + YAW_RVA) = yaw;
    Logf("republicmp  SQUARE: type %d from (%.1f,%.1f) to (%.1f,%.1f): %s, %u piece(s), %u point(s)", m.type, m.first[0], m.first[2],
         m.second[0], m.second[2], ok ? "made" : "a fault", pieces, points);
    return ok && pieces > 0;
}
