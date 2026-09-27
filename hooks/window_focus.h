// True only while the game window has focus, so the dev hotkeys (F9, F10,
// INSERT) do not fire while alt-tabbed.
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
