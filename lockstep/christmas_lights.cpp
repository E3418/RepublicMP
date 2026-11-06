// SPDX-License-Identifier: GPL-3.0-only
// See christmas_lights.h. Follows SoylentGreed's route (RepublicLAN, read against
// the disassembly of 1.1.1.9: a line's points are 12 bytes here).
#include "christmas_lights.h"
#include "commands.h"
#include "../determinism/determinism.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define MANAGER_OFFSET 0xFAF8
#define SELECTED_OFFSET 0xD428
#define HANDLER_RVA 0xEA950
#define DRAW_RVA 0xEBF00
#define BUILD_RVA 0xE6FB0
#define POWER_RVA 0xEA4B0
#define INIT_RVA 0xE6CE0
#define RELEASE_RVA 0xE6E00
#define ALLOC_RVA 0x84DECC   // operator new
#define FREE_RVA 0x84DFE0    // sized delete
#define GROW_RVA 0x41A0      // room for one more in a vector of pointers
#define SEGMENT_IMPORT 0x86CC40 // C3DVec3PointSegmentDistance
#define DOWN_RVA 0xA54E90
#define CAMERA_RVA 0x9E9D10
#define CURSOR_RVA 0x9D5E7C
#define DRAG_OFFSET 0x20
#define VALID_OFFSET 0x21
#define SCENE_OFFSET 0x18
#define LINE_BYTES 0xF8
#define LINE_HEADER 8
#define LINE_ANCHOR_START 0x38
#define LINE_ANCHOR_END 0x44
#define LINE_START 0x68
#define LINE_END 0x74
#define LINE_POINTS 0x98
#define POINT_BYTES 12
#define MAX_LINES (1u << 16)
#define CANCEL_RADIUS 1.0f
#define MAGIC 0x314C4843

struct Routine {
    unsigned rva;
    unsigned char entry[16];
};
static const Routine kRoutines[] = {
    { HANDLER_RVA, { 0x48, 0x8b, 0xc4, 0xf3, 0x0f, 0x11, 0x48, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57 } },
    { DRAW_RVA, { 0x40, 0x55, 0x53, 0x41, 0x56, 0x48, 0x8d, 0xac, 0x24, 0x80, 0xfe, 0xff, 0xff, 0x48, 0x81, 0xec } },
    { BUILD_RVA, { 0x48, 0x8b, 0xc4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d } },
    { POWER_RVA, { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x57, 0x48, 0x83, 0xec, 0x78, 0x83, 0x3d, 0xff, 0xaf, 0x8e, 0x00 } },
    { INIT_RVA, { 0x48, 0x83, 0xec, 0x18, 0xf3, 0x0f, 0x10, 0x0d, 0x44, 0x45, 0x82, 0x00, 0x45, 0x33, 0xc0, 0x4c } },
    { RELEASE_RVA, { 0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48 } },
    { ALLOC_RVA, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xeb, 0x21, 0x48, 0x8b, 0xcb, 0xe8, 0xd5 } },
    { FREE_RVA, { 0xe9, 0xf3, 0xff, 0xff, 0xff, 0xcc, 0xcc, 0xcc, 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d } },
    { GROW_RVA, { 0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0x49, 0x10, 0x4c, 0x8b, 0xc1, 0x48, 0x8b, 0x51, 0x08, 0x49 } },
};

typedef void (*ToolFn)(U64 manager, float t);
typedef unsigned char (*BuildFn)(U64 line, const float* start, const float* end, U64 scene);
typedef void (*PowerFn)(U64 manager, U64 line);
typedef U64 (*InitFn)(U64 line, U64 manager);
typedef void (*ReleaseFn)(U64 line);
typedef U64 (*AllocFn)(size_t bytes);
typedef void (*FreeFn)(U64 at, size_t bytes);
typedef void (*GrowFn)(U64 vector);
typedef float (*SegmentFn)(const float* point, const float* a, const float* b);

static int g_ready = -1;
// The drag's line while it is out of the vector.
static U64 g_held = 0;
static BYTE g_heldValid = 0;
static unsigned g_heldLoad = 0;
// The lines the held cancel button last sent: not sent again until they change.
static LightsMsg g_lastCancel = {};

void ChristmasLights_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

bool ChristmasLights_Ready()
{
    if (g_ready < 0) {
        g_ready = 1;
        for (const Routine& r : kRoutines)
            if (memcmp(g_exeBase + r.rva, r.entry, sizeof(r.entry)) != 0) {
                Logf("republicmp  LIGHTS: game routine at 0x%X differs - the light tools are put away in lockstep", r.rva);
                g_ready = 0;
                break;
            }
    }
    return g_ready == 1;
}

static U64 Q(U64 at) { return *(U64*)at; }

static bool Lines(U64 manager, U64& begin, unsigned& count)
{
    U64 b = Q(manager), e = Q(manager + 8);
    begin = 0;
    count = 0;
    if (e < b || (e - b) % 8 || (e - b) / 8 > MAX_LINES) return false;
    begin = b;
    count = (unsigned)((e - b) / 8);
    return true;
}

static void PushLine(U64 manager, U64 line)
{
    if (Q(manager + 8) == Q(manager + 0x10)) ((GrowFn)(g_exeBase + GROW_RVA))(manager);
    *(U64*)Q(manager + 8) = line;
    *(U64*)(manager + 8) += 8;
}

static void FreeLine(U64 line)
{
    ((ReleaseFn)(g_exeBase + RELEASE_RVA))(line);
    ((FreeFn)(g_exeBase + FREE_RVA))(line, LINE_BYTES);
}

// The drag's line leaves the vector (+20 cleared, +21 kept).
static void Detach(U64 manager)
{
    __try {
        if (g_held || !*(BYTE*)(manager + DRAG_OFFSET)) return;
        U64 begin = 0;
        unsigned count = 0;
        if (!Lines(manager, begin, count) || !count) return;
        g_held = Q(begin + (U64)(count - 1) * 8);
        g_heldValid = *(BYTE*)(manager + VALID_OFFSET);
        g_heldLoad = Determinism_WorldLoads();
        *(U64*)(manager + 8) -= 8;
        *(BYTE*)(manager + DRAG_OFFSET) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_held = 0;
    }
}

// It comes back as the last line. A load since then took its world away.
static void Attach(U64 manager)
{
    if (!g_held) return;
    U64 line = g_held;
    g_held = 0;
    if (g_heldLoad != Determinism_WorldLoads()) return;
    __try {
        PushLine(manager, line);
        *(BYTE*)(manager + DRAG_OFFSET) = 1;
        *(BYTE*)(manager + VALID_OFFSET) = g_heldValid;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void Drop()
{
    if (!g_held) return;
    U64 line = g_held;
    g_held = 0;
    if (g_heldLoad != Determinism_WorldLoads()) return;
    __try {
        FreeLine(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A release: the drag's line as a command. It is invalid here after this, so the
// game's own release frees it on this PC.
static void SendRelease(U64 manager)
{
    __try {
        if (!*(BYTE*)(manager + DRAG_OFFSET)) return;
        BYTE valid = *(BYTE*)(manager + VALID_OFFSET);
        *(BYTE*)(manager + VALID_OFFSET) = 0;
        U64 begin = 0;
        unsigned count = 0;
        if (!valid || !Lines(manager, begin, count) || !count) return;
        U64 line = Q(begin + (U64)(count - 1) * 8);
        if ((Q(line + LINE_POINTS + 8) - Q(line + LINE_POINTS)) / POINT_BYTES < 2) return;
        static LightsMsg m;
        memset(&m, 0, sizeof(m));
        m.magic = MAGIC;
        m.mode = 0;
        memcpy(m.header, (const void*)(line + LINE_HEADER), sizeof(m.header));
        memcpy(m.anchorStart, (const void*)(line + LINE_ANCHOR_START), 12);
        memcpy(m.anchorEnd, (const void*)(line + LINE_ANCHOR_END), 12);
        memcpy(m.start, (const void*)(line + LINE_START), 12);
        memcpy(m.end, (const void*)(line + LINE_END), 12);
        if (Commands_TakeLocal()) Commands_Submit(LS_CMD_LIGHTS, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The cancel tool's lines for this frame's ray, last to first (ebd80..ebe66).
static void SendCancel(U64 manager)
{
    __try {
        SegmentFn segment = *(SegmentFn*)(g_exeBase + SEGMENT_IMPORT);
        const float* camera = (const float*)(g_exeBase + CAMERA_RVA);
        const float* cursor = (const float*)(g_exeBase + CURSOR_RVA);
        static LightsMsg m;
        memset(&m, 0, sizeof(m));
        m.magic = MAGIC;
        m.mode = 1;
        U64 begin = 0;
        unsigned count = 0;
        if (!Lines(manager, begin, count)) return;
        for (unsigned i = count; i-- > 0 && m.count < LIGHTS_MAX_REMOVED;) {
            U64 line = Q(begin + (U64)i * 8);
            const float* start = (const float*)(line + LINE_START);
            const float* end = (const float*)(line + LINE_END);
            if (segment(start, camera, cursor) < CANCEL_RADIUS || segment(end, camera, cursor) < CANCEL_RADIUS) {
                memcpy(m.ends[m.count], start, 12);
                memcpy(&m.ends[m.count][3], end, 12);
                ++m.count;
            }
        }
        if (!m.count || memcmp(&m, &g_lastCancel, sizeof(m)) == 0) return;
        g_lastCancel = m;
        if (Commands_TakeLocal()) Commands_Submit(LS_CMD_LIGHTS, &m, sizeof(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ChristmasLights_Tool(U64 manager, float t)
{
    ToolFn handler = (ToolFn)(g_exeBase + HANDLER_RVA);
    if (!Commands_Capturing() || !ChristmasLights_Ready()) {
        Attach(manager);
        handler(manager, t);
        return;
    }
    const char* tool = *(const char**)(g_exeBase + WORLD_RVA + SELECTED_OFFSET);
    bool line = false, cancel = false;
    __try {
        line = tool && strstr(tool, "christmas_line_") != nullptr;
        cancel = tool && !line && strcmp(tool, "christmas_cancel") == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    BYTE* down = g_exeBase + DOWN_RVA;
    if (!*down) memset(&g_lastCancel, 0, sizeof(g_lastCancel));
    if (line) {
        Attach(manager);
        if (!*down) SendRelease(manager);
        handler(manager, t);
        Detach(manager);
        return;
    }
    Drop(); // the game drops a drag when the tool changes
    if (cancel && *down) {
        SendCancel(manager);
        BYTE kept = *down;
        *down = 0; // nothing is freed on this PC now
        handler(manager, t);
        *down = kept;
        return;
    }
    handler(manager, t);
}

void ChristmasLights_Draw(U64 manager, float t)
{
    ToolFn draw = (ToolFn)(g_exeBase + DRAW_RVA);
    if (!Commands_Capturing()) {
        draw(manager, t);
        return;
    }
    // With time passing the draw must not see this PC's tool: out of season it
    // would draw power for the lines' buildings here only.
    const char** selected = (const char**)(g_exeBase + WORLD_RVA + SELECTED_OFFSET);
    const char* tool = *selected;
    bool hide = false;
    __try {
        hide = t != 0.0f && tool && strstr(tool, "christmas_line_") != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    bool held = g_held != 0;
    if (held) Attach(manager);
    if (hide) *selected = nullptr;
    draw(manager, t);
    if (hide) *selected = tool;
    if (held) Detach(manager);
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

// The press, the last held frame and the release, with the game's routines.
static const char* Make(U64 manager, const LightsMsg& m, unsigned* points)
{
    __try {
        U64 line = ((InitFn)(g_exeBase + INIT_RVA))(((AllocFn)(g_exeBase + ALLOC_RVA))(LINE_BYTES), manager);
        if (!line) return "no line";
        memcpy((void*)(line + LINE_HEADER), m.header, sizeof(m.header));
        memcpy((void*)(line + LINE_ANCHOR_START), m.anchorStart, 12);
        PushLine(manager, line);
        ((BuildFn)(g_exeBase + BUILD_RVA))(line, m.start, m.end, Q(manager + SCENE_OFFSET));
        memcpy((void*)(line + LINE_ANCHOR_END), m.anchorEnd, 12);
        *points = (unsigned)((Q(line + LINE_POINTS + 8) - Q(line + LINE_POINTS)) / POINT_BYTES);
        if (*points >= 2) {
            ((PowerFn)(g_exeBase + POWER_RVA))(manager, line);
            return nullptr;
        }
        *(U64*)(manager + 8) -= 8;
        FreeLine(line);
        return "too short";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "a fault while making it";
    }
}

// The named lines, each by its two ends.
static unsigned Free(U64 manager, const LightsMsg& m)
{
    unsigned freed = 0;
    __try {
        for (unsigned k = 0; k < m.count; k++) {
            U64 begin = 0;
            unsigned count = 0;
            if (!Lines(manager, begin, count)) break;
            for (unsigned i = count; i-- > 0;) {
                U64 at = begin + (U64)i * 8, line = Q(at);
                if (memcmp((const void*)(line + LINE_START), m.ends[k], 12) != 0 ||
                    memcmp((const void*)(line + LINE_END), &m.ends[k][3], 12) != 0)
                    continue;
                FreeLine(line);
                memmove((void*)at, (const void*)(at + 8), (size_t)(Q(manager + 8) - (at + 8)));
                *(U64*)(manager + 8) -= 8;
                ++freed;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return freed;
}

bool ChristmasLights_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(LightsMsg)) return false;
    static LightsMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.mode > 1 || !ChristmasLights_Ready()) return false;
    if (m.mode == 1 && (!m.count || m.count > LIGHTS_MAX_REMOVED)) return false;
    const float* floats = m.anchorStart;
    for (size_t i = 0; i < (sizeof(LightsMsg) - offsetof(LightsMsg, anchorStart)) / sizeof(float); i++)
        if (!isfinite(floats[i]) || fabsf(floats[i]) > 1.0e7f) return false;
    U64 manager = Q((U64)(g_exeBase + WORLD_RVA + MANAGER_OFFSET));
    if (!manager) return false;
    if (m.mode == 0) {
        unsigned points = 0;
        const char* why = Make(manager, m, &points);
        Logf("republicmp  LIGHTS: a line (%.1f,%.1f) to (%.1f,%.1f): %s (%u points)", m.start[0], m.start[2], m.end[0], m.end[2],
             why ? why : "made", points);
        return why == nullptr;
    }
    unsigned freed = Free(manager, m);
    Logf("republicmp  LIGHTS: %u of %u line(s) freed", freed, m.count);
    return freed == m.count;
}
