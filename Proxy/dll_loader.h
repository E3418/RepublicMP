// SPDX-License-Identifier: GPL-3.0-only
// Proxy DLL for integrating a multiplayer mod into a game's executable file, experimental version by E3418 (XINPUT9_1_0.DLL)

#pragma once
#include <windows.h>

// True only in the game itself: ModelViewer.exe sits in the same folder and uses the same engine.
bool Loader_IsGame();

// Creates RepublicMP and RepublicMP\logs next to the proxy and opens the log.
bool Loader_Init(HMODULE self);

// Loads RepublicMP.dll from the proxy's own folder and calls its RepublicMP_Start export.
// Must not run inside DllMain: it loads a DLL.
void Loader_StartMod();
