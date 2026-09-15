// Shared hotkey guard: true only while the game's own window has focus, so
// the remaining dev-only hotkeys (F9/F10 - see hooks/detect_spawn.cpp and
// hooks/game_speed.cpp) don't fire while alt-tabbed into Discord, a browser,
// etc. Every other action (host/connect, disconnect, build sync, hide menu)
// is UI-only and goes through ui/main_menu.cpp / ui/debugui.cpp instead.
#pragma once

#include "tesmio_plugin.h"

inline bool IsGameWindowFocused()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD fgProcessId = 0;
    GetWindowThreadProcessId(fg, &fgProcessId);
    return fgProcessId == GetCurrentProcessId();
}
