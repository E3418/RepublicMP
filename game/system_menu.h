// SPDX-License-Identifier: GPL-3.0-only
// The game's in-game menu: Escape while a city is loaded opens the "system"
// tab of the left toolbar (save, save as, load, quit to menu, quit to
// desktop as icon buttons; settings below). It is no list of rows, so the mod
// draws its own section in it (ui/multiplayer_page). Read only,
// no hook. RE of SOVIET64.exe 1.1.1.9 on 2026-10-23.
#pragma once

#include "tesmio_plugin.h"

void SystemMenu_Bind(const TsmHost* host);

// The menu is open on its main page (no settings sub-page).
bool SystemMenu_IsOpen();
// The panel's top in pixels and the toolbar's scale s (the panel's contents
// sit at fixed multiples of s below it). False when unreadable.
bool SystemMenu_Panel(float* top, float* scale);
// Closes it, as the game's own close does.
void SystemMenu_Close();
