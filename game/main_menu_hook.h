// SPDX-License-Identifier: GPL-3.0-only
// The game's own main menu: one more item, "MULTIPLAYER", at the end of the
// root list, drawn and clicked like the game's (same font, same column). The
// game rebuilds the list every frame in 0x28AF60; a hook appends our item
// after it and reports its click. (Root-menu RE by Sohn, WRMP.)
#pragma once

#include "tesmio_plugin.h"

void MainMenuHook_Bind(const TsmHost* host);

// True once per click on our item.
bool MainMenuHook_TakeClick();

// How many times the root menu's builder has run (the game's main thread):
// while it keeps rising, the root main menu is on screen.
unsigned MainMenuHook_BuildCount();
// Hides the game's rows, title and panel (the game draws no panel without
// rows) while a page of ours covers the menu; its clicks are consumed.
void MainMenuHook_SetHidden(bool hidden);

// The game's own new-game and load-game pages, opened as if their row were
// clicked (next frame), so the game's own setup runs.
enum MainMenuItem { MAIN_MENU_CUSTOM_GAME, MAIN_MENU_LOAD_GAME };
void MainMenuHook_Open(MainMenuItem item);

// The scale of the game's 2D interface (1 at 1080 lines), as its menu uses it.
float MainMenuHook_UiScale();
