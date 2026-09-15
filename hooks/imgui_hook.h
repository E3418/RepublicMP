// Embeds a Dear ImGui overlay into the game via the existing
// IDXGISwapChain::Present hook (see present_hook.cpp) - own module by project
// convention (own *_Bind(), own copy of any needed globals).
#pragma once

#include "tesmio_plugin.h"
#include <dxgi.h>

void ImGuiHook_Bind(const TsmHost* host);

// Call once per frame from HookedPresent, before its own logic - creates the
// ImGui context and DX11/Win32 backends the first time it sees a real swap
// chain (device + window handle come from here), no-op on every later call.
void ImGuiHook_EnsureInit(IDXGISwapChain* swapChain);

// Call once per frame from HookedPresent, right before the original Present
// call - draws the overlay on top of the frame the game already rendered.
// Also polls INSERT (focus-guarded, same convention as the other hotkeys) to
// toggle overlay visibility.
void ImGuiHook_NewFrameAndRender();

// true while the overlay is visible and capturing mouse/keyboard input
extern bool g_imguiVisible;
