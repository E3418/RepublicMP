// SPDX-License-Identifier: GPL-3.0-only
// See system_menu.h.
#include "system_menu.h"
#include "world.h"
#include <cstring>

// The toolbar's tabs are entries of 0x68 bytes (name at +1); the open one is
// a qword at god+0xC650 (0: none), set by Escape at 0x30E0DD..0x30E1A1 and
// cleared by the close at 0x32826E..0x3282F9. 0x337450 draws the "system" tab.
#define TOOLBAR_OPEN_TAB_OFFSET 0xC650   // qword: the open tab's entry, 0 = closed
#define TOOLBAR_TAB_NAME_OFFSET 0x1      // char[]: the entry's name ("system")
#define TOOLBAR_SUBPAGE_OFFSET 0xC65C    // int32: -1 the main page, 0..4 settings sub-pages
#define TOOLBAR_PANEL_Y_OFFSET 0xAE50    // float: the panel's top (icons at y = it + 87.5s)
#define SELECTED_TOOL_OFFSET 0xD428      // qword: the selected tool, 0 = none (Escape opens the menu only then)
#define TOOLBAR_SCALE_RVA 0x992088       // float: the toolbar's scale s

void SystemMenu_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool SystemMenu_IsOpen()
{
    __try {
        unsigned char* god = World_Object();
        const unsigned char* tab = *(unsigned char**)(god + TOOLBAR_OPEN_TAB_OFFSET);
        // No tool selected: our close, unlike the game's, leaves the tool alone.
        return tab && strncmp((const char*)tab + TOOLBAR_TAB_NAME_OFFSET, "system", 7) == 0 &&
               *(int*)(god + TOOLBAR_SUBPAGE_OFFSET) == -1 && *(unsigned long long*)(god + SELECTED_TOOL_OFFSET) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SystemMenu_Panel(float* top, float* scale)
{
    float s = 0, y = -1;
    __try {
        s = *(float*)(g_exeBase + TOOLBAR_SCALE_RVA);
        y = *(float*)(World_Object() + TOOLBAR_PANEL_Y_OFFSET); // a float: 184.0 in a test at 1080 lines
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!(s > 0.1f && s < 10.0f) || !(y >= 0.0f && y < 20000.0f)) return false;
    *top = y;
    *scale = s;
    return true;
}

void SystemMenu_Close()
{
    __try {
        *(unsigned long long*)(World_Object() + TOOLBAR_OPEN_TAB_OFFSET) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
