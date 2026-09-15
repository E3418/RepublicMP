// Diagnostic hook on C3D_TIMER (Start/Stop/MultiplyFPS) via IAT patching.
// Logs which timer instance fires, to help identify the real simulation
// clock. Superseded by game_speed.h for actual pause/speed control; kept
// as diagnostic infrastructure.
#pragma once

#include "tesmio_plugin.h"

void PauseHook_Bind(const TsmHost* host);
bool InstallPauseHooks();
