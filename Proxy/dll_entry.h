// SPDX-License-Identifier: GPL-3.0-only
// Proxy DLL for integrating a multiplayer mod into a game's executable file, experimental version by E3418 (XINPUT9_1_0.DLL)

#pragma once

// Makes the exe's main thread call `callback` right before the game's first instruction.
// Call it from DllMain: by the time the callback runs, every DLL is initialised and the
// loader lock is free, while the game itself has not done anything yet.
bool Entry_Hook(void (*callback)());
