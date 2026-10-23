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
