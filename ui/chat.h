// SPDX-License-Identifier: GPL-3.0-only
// The session's chat: a small window in the MULTIPLAYER page's look, over the
// game and every other window, that does not stop the game.
//
// Presentation only, like the cursors (lockstep/presence): a line goes through
// the lockstep connection beside the commands (the host passes a player's on
// to the others) and the simulation never sees it. The window can be shrunk
// to an icon (which counts the lines not read yet) or hidden; the chat key
// (Enter by default, chosen on the MULTIPLAYER page) brings it back and starts
// typing. The game gets no key while a line is typed and no click on the
// window; everything else stays the game's.
#pragma once

#include "tesmio_plugin.h"

#define CHAT_TEXT_BYTES 200 // a line's text, UTF-8, the terminating zero included

void Chat_Bind(const TsmHost* host);

// Render thread, inside the overlay's frame: the chat key, the window or its icon.
// Shown: anything of the chat is on screen this frame (the overlay draws it).
bool Chat_Shown();
void Chat_Frame();
// Last frame: a line is being typed (the game gets no key).
bool Chat_Typing();

// Any thread (a receive thread): another player's line. player: the host's
// number for it (0 the host).
void Chat_Receive(int player, const char* nick, const char* text);

// The MULTIPLAYER page's setting (remembered in the profile).
int Chat_GetKey(); // a virtual-key code
// false: not a key for the chat (the left and right mouse buttons, Esc,
// Insert, the ping key).
bool Chat_SetKey(int vk);
