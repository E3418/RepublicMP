// SPDX-License-Identifier: GPL-3.0-only
// See main_menu_hook.h. Root-menu layout from WRMP's RE (by Sohn), confirmed
// by disassembly of SOVIET64.exe 1.1.1.9 on 2026-10-22.
#include "main_menu_hook.h"
#include <cstring>
#include <cwchar>

// The root page's builder, called every frame while god+0x5044 == 1 (page
// dispatcher call at 0x261897; rcx = the world object). It zeroes and refills
// the item list, and acts on a click whose index is one of its own items.
#define MENU_BUILD_ROOT_RVA 0x28AF60
#define MENU_ITEM_COUNT_OFFSET 0x11B0    // int32, set at 0x28AFA2
#define MENU_ITEM_TEXT_OFFSET 0x11B4     // wchar_t[256] per slot, then an int32 (icons, other pages)
#define MENU_ITEM_STRIDE 0x204
#define MENU_ITEM_SLOTS 30               // up to the title at +0x4E2C
#define MENU_CLICK_OFFSET 0x5038         // int32 1 = a click; set by the drawer 0x274E70 at 0x27533E, cleared at its start
#define MENU_CLICK_INDEX_OFFSET 0x503C   // int32 the clicked row (0x275348)
#define MENU_TITLE_OFFSET 0x4E2C         // wchar_t text of the panel's title, set by the builder
#define MENU_CONTINUE_OFFSET 0x502C      // byte: a save to continue exists, so row 0 is "Continue" (0x28AFA8)
// Rows after the optional "Continue": Campaign, Custom game, Load game, ...
// (the builder's order; Custom game sets page 0x63, Load game page 2).
#define MENU_ROW_CUSTOM_GAME 1
#define MENU_ROW_LOAD_GAME 2
#define INPUT_LEFT_PRESSED_RVA 0xA54E91  // byte, the left button pressed this frame (cleared by a row's click)
#define UI_SCALE_RVA 0x995214            // float, the menu's scale s (positions are multiples of s)

// 48 89 5C 24 08 / 57 / 48 83 EC 60 / 48 8B D9 / BA 57 02 00 00 - 18 bytes up
// to the first RIP-relative instruction (lea rcx at +0x12).
static const unsigned char kBuildRootExpect[18] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC,
    0x60, 0x48, 0x8B, 0xD9, 0xBA, 0x57, 0x02, 0x00, 0x00
};

typedef void (*BuildRootFn)(unsigned char* god);
static BuildRootFn g_origBuildRoot = nullptr;
static volatile LONG g_clicked = 0;
static volatile LONG g_openRequest = -1; // a MainMenuItem to open, or -1
static volatile bool g_hidden = false;
static volatile LONG g_buildCount = 0;
static int g_ourRow = -1; // the row our item had last frame (game's main thread only)
// MULTIPLAYER goes right after "Load game", away from "Quit" at the end. The
// game knows its items by the order it built them in, so the rows below ours
// show one lower than the game counts them: a click there is mapped back.

// Inside the game's frame: no allocation, logging or I/O.
static void HookedBuildRoot(unsigned char* god)
{
    // The click was made on last frame's rows: take ours before the game's
    // items look at it, since the count can change between frames ("Continue").
    bool continueRow = false; // read when the builder reads it (0x28AFA8), before it acts
    __try {
        continueRow = *(god + MENU_CONTINUE_OFFSET) != 0;
        int* click = (int*)(god + MENU_CLICK_OFFSET);
        int* index = (int*)(god + MENU_CLICK_INDEX_OFFSET);
        if (g_ourRow >= 0 && *click == 1) {
            if (*index == g_ourRow) {
                *click = 0;
                InterlockedExchange(&g_clicked, 1);
            } else if (*index > g_ourRow) {
                --*index; // a game item shown below ours
            }
        }
        LONG open = InterlockedExchange(&g_openRequest, -1);
        if (open >= 0) { // a click the builder below acts on, like the drawer's
            int first = continueRow ? 1 : 0;
            *click = 1;
            *(int*)(god + MENU_CLICK_INDEX_OFFSET) =
                first + (open == MAIN_MENU_CUSTOM_GAME ? MENU_ROW_CUSTOM_GAME : MENU_ROW_LOAD_GAME);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_origBuildRoot(god);
    InterlockedIncrement(&g_buildCount);
    g_ourRow = -1;
    __try {
        int* count = (int*)(god + MENU_ITEM_COUNT_OFFSET);
        if (g_hidden) {
            *count = 0;                                // no rows, and the drawer skips the panel
            *(wchar_t*)(god + MENU_TITLE_OFFSET) = 0;   // nor the title
            *(g_exeBase + INPUT_LEFT_PRESSED_RVA) = 0; // nothing of the game's takes the click
            return;
        }
        int n = *count;
        if (n < 0 || n >= MENU_ITEM_SLOTS) return;
        int row = (continueRow ? 1 : 0) + MENU_ROW_LOAD_GAME + 1;
        if (row > n) row = n;
        unsigned char* slot = god + MENU_ITEM_TEXT_OFFSET + row * MENU_ITEM_STRIDE;
        memmove(slot + MENU_ITEM_STRIDE, slot, (size_t)(n - row) * MENU_ITEM_STRIDE); // the rows below, one down
        wcscpy_s((wchar_t*)slot, 256, L"MULTIPLAYER");
        *(int*)(slot + 0x200) = 0; // the slot's icon/lock int, unused on the root page
        *count = n + 1;
        g_ourRow = row;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void MainMenuHook_Bind(const TsmHost* host)
{
    TsmBind(host);
    if (InstallInlineHook(g_exeBase + MENU_BUILD_ROOT_RVA, (void*)HookedBuildRoot, (void**)&g_origBuildRoot,
                          kBuildRootExpect, sizeof(kBuildRootExpect), "republicmp main menu"))
        Logf("republicmp  MAIN MENU: MULTIPLAYER item installed");
    else
        Logf("republicmp  MAIN MENU: hook refused - no MULTIPLAYER item (other game build?)");
}

bool MainMenuHook_TakeClick()
{
    return InterlockedExchange(&g_clicked, 0) != 0;
}

unsigned MainMenuHook_BuildCount()
{
    return (unsigned)g_buildCount;
}

void MainMenuHook_SetHidden(bool hidden)
{
    g_hidden = hidden;
    if (hidden) InterlockedExchange(&g_openRequest, -1); // a request the builder never saw is dropped
}

void MainMenuHook_Open(MainMenuItem item)
{
    InterlockedExchange(&g_openRequest, item);
}

float MainMenuHook_UiScale()
{
    float s = 0;
    __try {
        s = *(float*)(g_exeBase + UI_SCALE_RVA);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return s > 0.1f && s < 10.0f ? s : 1.0f;
}
