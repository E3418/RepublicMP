// SPDX-License-Identifier: GPL-3.0-only
// "Menus closed" for the length of a command.
//
// world+0xfbc / +0xfbe / +0xfc3 are the game's menu-open flags: +0xfc3 is set
// every frame while the cursor is over a window (the window dispatcher
// FUN_1406e9b80), the other two while a menu is open. The building and
// network tools return at once while any is set - meant for the local
// player's clicks, but it also held every building and road from the other
// player until this PC's cursor moved off a window (#21). A command now
// writes them 0 for its call and puts this PC's values back after.
//
// (by TucKnock; the same as SoylentGreed's replays do)
#pragma once

#include "tesmio_plugin.h"

#define MENUS_WORLD_RVA 0x9D4F10
#define MENU_FLAG_COUNT 3

static const unsigned int kMenuFlagOffsets[MENU_FLAG_COUNT] = { 0xfbc, 0xfbe, 0xfc3 };

class MenusClosedScope {
public:
    MenusClosedScope()
    {
        BYTE* world = g_exeBase + MENUS_WORLD_RVA;
        for (int i = 0; i < MENU_FLAG_COUNT; i++) {
            saved_[i] = world[kMenuFlagOffsets[i]];
            world[kMenuFlagOffsets[i]] = 0;
        }
    }
    ~MenusClosedScope()
    {
        BYTE* world = g_exeBase + MENUS_WORLD_RVA;
        for (int i = 0; i < MENU_FLAG_COUNT; i++) world[kMenuFlagOffsets[i]] = saved_[i];
    }
    // True if a window or menu was in the way (for the log).
    bool WasBlocked() const { return saved_[0] || saved_[1] || saved_[2]; }

private:
    unsigned char saved_[MENU_FLAG_COUNT];
    MenusClosedScope(const MenusClosedScope&);
    MenusClosedScope& operator=(const MenusClosedScope&);
};
