// SPDX-License-Identifier: GPL-3.0-only
// See main_menu_hook.h. Root-menu layout from WRMP's RE (by Sohn), confirmed
// by disassembly of SOVIET64.exe 1.1.1.9 on 2026-10-22.
#include "main_menu_hook.h"
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

// 48 89 5C 24 08 / 57 / 48 83 EC 60 / 48 8B D9 / BA 57 02 00 00 - 18 bytes up
// to the first RIP-relative instruction (lea rcx at +0x12).
static const unsigned char kBuildRootExpect[18] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC,
    0x60, 0x48, 0x8B, 0xD9, 0xBA, 0x57, 0x02, 0x00, 0x00
};

typedef void (*BuildRootFn)(unsigned char* god);
static BuildRootFn g_origBuildRoot = nullptr;
static volatile LONG g_clicked = 0;
static int g_ourRow = -1; // the row our item had last frame (game's main thread only)

// Inside the game's frame: no allocation, logging or I/O.
static void HookedBuildRoot(unsigned char* god)
{
    // The click was made on last frame's rows: take ours before the game's
    // items look at it, since the count can change between frames ("Continue").
    __try {
        int* click = (int*)(god + MENU_CLICK_OFFSET);
        if (g_ourRow >= 0 && *click == 1 && *(int*)(god + MENU_CLICK_INDEX_OFFSET) == g_ourRow) {
            *click = 0;
            InterlockedExchange(&g_clicked, 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_origBuildRoot(god);
    g_ourRow = -1;
    __try {
        int* count = (int*)(god + MENU_ITEM_COUNT_OFFSET);
        int n = *count;
        if (n < 0 || n >= MENU_ITEM_SLOTS) return;
        unsigned char* slot = god + MENU_ITEM_TEXT_OFFSET + n * MENU_ITEM_STRIDE;
        wcscpy_s((wchar_t*)slot, 256, L"MULTIPLAYER");
        *(int*)(slot + 0x200) = 0; // the slot's icon/lock int, unused on the root page
        *count = n + 1;
        g_ourRow = n;
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
