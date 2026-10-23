// SPDX-License-Identifier: GPL-3.0-only
// The MULTIPLAYER page, the players' multiplayer UI: drawn in the game's own
// panel, fonts and colours over the main menu (whose rows are hidden
// meanwhile) or over the game. Host a new game or a saved one through the
// game's own pages - hosting starts by itself once the city is loaded - host
// the city in play, or join a host from the game list or by SteamID64 / IP.
// While a session runs it shows the session: players, who sees the game,
// Stop hosting / Disconnect. It opens by itself when a session starts.
#pragma once

void MultiplayerPage_Open();
// Open and on screen (not while the game loads a city or shows another page).
bool MultiplayerPage_IsOpen();
// Render thread, every frame, also with nothing drawn: the hosting that waits
// for a city to load.
void MultiplayerPage_Tick();
// Render thread, inside the ImGui frame, while the page is open.
void MultiplayerPage_Draw();

// The Multiplayer section in the game's in-game menu (Escape in a city),
// shown while that menu is open and the page is not: its button closes the
// game's menu and opens the page.
bool MultiplayerPage_MenuButtonShown();
void MultiplayerPage_DrawMenuButton();
