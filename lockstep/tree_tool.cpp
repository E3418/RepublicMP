// SPDX-License-Identifier: GPL-3.0-only
// See tree_tool.h. Follows SoylentGreed's terrain tool route, kind 3
// (RepublicLAN, read against the disassembly of 1.1.1.9).
#include "tree_tool.h"
#include "commands.h"
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define TREE_TOOL_RVA 0x2F94C0
#define TIMER_RVA 0x9D4EE0      // the game timer the tool's PowerTime reads
#define DOWN_RVA 0xA54E90
#define PRESS_RVA 0xA54E91
#define SELECTED_OFFSET 0xD428  // the selected tool record
#define MENU_OFFSET 0xD280      // the build menu's records: begin, end
#define MENU_STRIDE 0x2D0       // name at +0
#define CURSOR_OFFSET 0xF6C
#define BRUSH_OFFSET 0xF98
#define NAME_BYTES 32

static const unsigned char kToolEntry[16] = { 0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41,
                                              0x56, 0x48, 0x8d, 0xa8, 0xd8, 0xfe, 0xff, 0xff };

#pragma pack(push, 1)
struct TreeMsg {
    int record;             // place in the build menu's records
    char name[NAME_BYTES];  // its name, a check
    float cursor[3];
    float brush;
    unsigned char timer[16];
    unsigned char press;
    unsigned char pad[3];
};
#pragma pack(pop)

static int g_ready = -1;

void TreeTool_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool TreeTool_Ready()
{
    return g_ready != 0 && memcmp(g_exeBase + TREE_TOOL_RVA, kToolEntry, sizeof(kToolEntry)) == 0;
}

static bool Ready()
{
    if (g_ready < 0) {
        g_ready = memcmp(g_exeBase + TREE_TOOL_RVA, kToolEntry, sizeof(kToolEntry)) == 0 ? 1 : 0;
        if (!g_ready) Logf("republicmp  TREES: the tree tool's entry differs - planting stays local");
    }
    return g_ready == 1;
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }

// The selected record's place in the build menu, or -1.
static int RecordPlace(U64 record, char name[NAME_BYTES])
{
    BYTE* world = World();
    U64 begin = *(U64*)(world + MENU_OFFSET), end = *(U64*)(world + MENU_OFFSET + 8);
    if (!record || !begin || end < begin || record < begin || record >= end || (record - begin) % MENU_STRIDE) return -1;
    memcpy(name, (const void*)record, NAME_BYTES);
    name[NAME_BYTES - 1] = 0;
    return (int)((record - begin) / MENU_STRIDE);
}

static U64 RecordAt(int place, const char* name)
{
    BYTE* world = World();
    U64 begin = *(U64*)(world + MENU_OFFSET), end = *(U64*)(world + MENU_OFFSET + 8);
    if (place < 0 || !begin || end < begin || (U64)place >= (end - begin) / MENU_STRIDE) return 0;
    U64 record = begin + (U64)place * MENU_STRIDE;
    return strncmp((const char*)record, name, NAME_BYTES - 1) == 0 ? record : 0;
}

typedef U64 (*ToolFn)(U64 world);

U64 TreeTool_Run(U64 world)
{
    ToolFn tool = (ToolFn)(g_exeBase + TREE_TOOL_RVA);
    BYTE* down = g_exeBase + DOWN_RVA;
    BYTE* press = g_exeBase + PRESS_RVA;
    if ((!*down && !*press) || !Commands_TakeLocal() || !Ready()) return tool(world);
    TreeMsg m;
    memset(&m, 0, sizeof(m));
    bool ok = false;
    __try {
        m.record = RecordPlace(*(U64*)(world + SELECTED_OFFSET), m.name);
        memcpy(m.cursor, (const void*)(world + CURSOR_OFFSET), 12);
        m.brush = *(float*)(world + BRUSH_OFFSET);
        memcpy(m.timer, g_exeBase + TIMER_RVA, 16);
        m.press = *press ? 1 : 0;
        ok = m.record >= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    // The tool with the button hidden: the brush preview only, nothing planted here.
    BYTE keptDown = *down, keptPress = *press;
    *down = 0;
    *press = 0;
    U64 r = tool(world);
    *down = keptDown;
    *press = keptPress;
    if (ok) Commands_Submit(LS_CMD_TREE, &m, sizeof(m));
    else Logf("republicmp  TREES: the selected tool is not a build menu record - nothing planted");
    return r;
}

bool TreeTool_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(TreeMsg) || !Ready()) return false;
    TreeMsg m;
    memcpy(&m, data, sizeof(m));
    m.name[NAME_BYTES - 1] = 0;
    U64 world = (U64)World();
    const char* why = nullptr;
    __try {
        U64 record = RecordAt(m.record, m.name);
        if (!record) {
            why = "the tool is not in this PC's build menu";
        } else {
            // This PC's own tool, cursor, brush, timer and buttons, put back after.
            U64 ownTool = *(U64*)(world + SELECTED_OFFSET);
            float ownCursor[3], ownBrush = *(float*)(world + BRUSH_OFFSET);
            unsigned char ownTimer[16];
            memcpy(ownCursor, (const void*)(world + CURSOR_OFFSET), 12);
            memcpy(ownTimer, g_exeBase + TIMER_RVA, 16);
            BYTE ownDown = g_exeBase[DOWN_RVA], ownPress = g_exeBase[PRESS_RVA];
            *(U64*)(world + SELECTED_OFFSET) = record;
            memcpy((void*)(world + CURSOR_OFFSET), m.cursor, 12);
            *(float*)(world + BRUSH_OFFSET) = m.brush;
            memcpy(g_exeBase + TIMER_RVA, m.timer, 16);
            g_exeBase[DOWN_RVA] = 1;
            g_exeBase[PRESS_RVA] = m.press;
            ((ToolFn)(g_exeBase + TREE_TOOL_RVA))(world);
            g_exeBase[DOWN_RVA] = ownDown;
            g_exeBase[PRESS_RVA] = ownPress;
            memcpy(g_exeBase + TIMER_RVA, ownTimer, 16);
            *(float*)(world + BRUSH_OFFSET) = ownBrush;
            memcpy((void*)(world + CURSOR_OFFSET), ownCursor, 12);
            *(U64*)(world + SELECTED_OFFSET) = ownTool;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "the tool faulted";
    }
    if (why) Logf("republicmp  TREES: %s (%s) - not planted", why, m.name);
    return why == nullptr;
}
