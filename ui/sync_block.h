// Full-screen block shown while save sync has the game force-paused (host:
// from client connect until the client is ready; client: from Connect until
// loaded). It dims the screen with a short reason and prevents state changes:
//   - mouse/keyboard window messages are swallowed (hooks/imgui_hook.cpp);
//   - SyncBlock_Draw re-asserts the pause every frame, since the game also
//     reads input outside the message queue;
//   - building placement (hooks/detect_spawn.cpp) and the F10 pause hotkey
//     (hooks/game_speed.cpp) refuse while active.
#pragma once

// Raises/lowers the block. reason is shown on the overlay. Any thread.
void SyncBlock_SetActive(bool active, const char* reason);

bool SyncBlock_IsActive();

// Called every frame from ImGuiHook_NewFrameAndRender, even while the
// overlay is hidden; no-op when inactive.
void SyncBlock_Draw();
