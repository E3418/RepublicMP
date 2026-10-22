// SPDX-License-Identifier: GPL-3.0-only
// Debug window (opened from the main menu): the lockstep state, the
// determinism probe, the diagnostic logs and the verbose debug log.
#pragma once

class DebugWindow
{
public:
    void Open();
    void Close();
    void Draw();

private:
    bool isOpen = false;
};

extern DebugWindow g_debug_window;
