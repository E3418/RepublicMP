// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the other players' cursors and pings on the map.
//
// Presentation only: the positions go through the lockstep connection beside
// the commands (the host passes a player's on to the others) and are drawn in
// the overlay; the simulation never sees them. The cursor is the terrain point
// under the mouse (World_MousePoint); a ping marks that point for a few
// seconds when the player presses the ping key (any key or mouse button,
// chosen in the RepublicMP window; ~ by default). A world point is drawn
// where the game's camera puts it (game/camera).
#pragma once

#include "tesmio_plugin.h"

#define PRESENCE_CURSOR 0
#define PRESENCE_PING 1
#define PRESENCE_HIDDEN 2 // the player's mouse left the map (alt-tab)

void Presence_Bind(const TsmHost* host);

// A session runs: the overlay draws the marks even while it is hidden.
bool Presence_Active();
// Render thread, inside the overlay's frame: sends this PC's cursor and ping,
// draws the other players'.
void Presence_Frame();
// Any thread (a receive thread): another player's mark. player: the host's
// number for it (0 the host).
void Presence_Receive(int player, int kind, const float pos[3], const char* nick);

// The RepublicMP window's settings (remembered in the profile).
bool Presence_GetShowCursors();
void Presence_SetShowCursors(bool on);
int Presence_GetPingKey(); // a virtual-key code
// false: not a key for pinging (the left and right mouse buttons, Esc, Insert).
bool Presence_SetPingKey(int vk);
// A key's name for the window ("~", "F5", "Mouse 4"...), ASCII.
void Presence_KeyName(int vk, char* out, size_t outSize);
