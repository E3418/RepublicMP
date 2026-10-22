// SPDX-License-Identifier: GPL-3.0-only
// Game modes: the kinds of multiplayer the RepublicMP window offers on its
// start page. Co-op (one republic, every PC in lockstep) is built in
// (session/, lockstep/). Another mode lives in its own folder under modes/
// and plugs in here, so it needs no window, frame or overlay hook of its own
// (see ARCHITECTURE.md for the hooks on the game, which have one owner each).
#pragma once

#include "tesmio_plugin.h"

struct RmpMode {
    const char* title;  // the start page's card: its title and two lines
    const char* line1;
    const char* line2;
    // Render thread, inside the overlay's ImGui frame while the RepublicMP
    // window is open: the mode's page, under the window's Back button.
    void (*drawPage)();
    // Render thread, every frame (the Present hook), before the overlay.
    void (*frame)();
    // Render thread, inside the ImGui frame - also while the RepublicMP window
    // is hidden - as long as wantsOverlay() is true. Draw into
    // ImGui::GetBackgroundDrawList() to stay under the windows.
    bool (*wantsOverlay)();
    void (*drawOverlay)();
};

// Called once, from the mode's Bind (TsmPluginInit). Null members are skipped.
void Modes_SetRepublics(const RmpMode* mode);
// Null until a Republics mode is plugged in.
const RmpMode* Modes_Republics();

// For game/present_hook and game/imgui_hook.
void Modes_Frame();
bool Modes_WantsOverlay();
void Modes_DrawOverlay();
