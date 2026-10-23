// SPDX-License-Identifier: GPL-3.0-only
// The MULTIPLAYER page of the game's main menu: drawn in the game's own panel,
// fonts and colours over the menu (whose rows are hidden meanwhile). Host a
// new game or a saved one through the game's own pages - hosting starts by
// itself once the city is loaded - or join a host by SteamID64 or IP.
#pragma once

void MultiplayerPage_Open();
bool MultiplayerPage_IsOpen();
// Render thread, every frame, also with nothing drawn: the hosting that waits
// for a city to load.
void MultiplayerPage_Tick();
// Render thread, inside the ImGui frame, while the page is open.
void MultiplayerPage_Draw();
