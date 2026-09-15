// Full-screen "don't touch anything" overlay shown while net/save_sync.cpp
// has the game forced-paused for a sync (host: from the moment a client
// connects until it's ready; client: from the moment Connect is pressed
// until it's loaded and ready). Two independent jobs:
//   1. Visual - dims the screen and shows a short reason, so it's obvious
//      something is happening and not just a frozen game.
//   2. Actually blocks state changes - hooks/imgui_hook.cpp's HookedWndProc
//      unconditionally swallowing mouse/keyboard window messages while
//      SyncBlock_IsActive() is not sufficient on its own: the game reads at
//      least some input some other way than the window message queue
//      (raw/polled - exactly which path is unconfirmed). Two more targeted,
//      state-level backstops cover this instead of chasing every possible
//      input path:
//        - SyncBlock_Draw() (below) re-asserts EnsureGamePaused() every
//          single frame while active - whatever flips game speed back on,
//          it's stomped back to paused before the next frame renders.
//        - hooks/detect_spawn.cpp's HookedUniversal (the one choke point
//          every real building placement funnels through, regardless of
//          what triggered it) refuses outright while SyncBlock_IsActive().
//      This covers the two actions that actually matter for host/client
//      staying in sync (time advancing, buildings appearing/disappearing).
//      Other interactions (camera panning, demolish, road building, menu
//      clicks that don't place anything) have NOT been individually
//      audited - if one of those turns out to matter too, the fix is the
//      same pattern: find its choke point, gate it on SyncBlock_IsActive().
//
// hooks/game_speed.cpp's F10 hotkey also checks this flag directly (it polls
// GetAsyncKeyState, not WndProc messages, so it isn't covered by the
// WndProc-level block above) - without that extra check, F10 could unpause
// the game out from under an active sync.
#pragma once

// active: true to raise the block, false to lower it. reason: short text
// shown on the overlay (e.g. "Syncing with a new player..."); ignored when
// active is false. Safe to call from any thread (network worker threads).
void SyncBlock_SetActive(bool active, const char* reason);

bool SyncBlock_IsActive();

// Call every frame from hooks/imgui_hook.cpp's ImGuiHook_NewFrameAndRender(),
// unconditionally (even while the rest of the overlay is hidden via INSERT) -
// no-op when not active.
void SyncBlock_Draw();
