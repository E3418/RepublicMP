// SPDX-License-Identifier: GPL-3.0-only
// Dear ImGui overlay rendered from the IDXGISwapChain::Present hook.
#pragma once

#include "tesmio_plugin.h"
#include <dxgi.h>

struct ID3D11Device;

void ImGuiHook_Bind(const TsmHost* host);

// Called every frame from HookedPresent; creates the ImGui context and the
// DX11/Win32 backends on first call.
void ImGuiHook_EnsureInit(IDXGISwapChain* swapChain);

// Called every frame right before the original Present: draws the overlay
// and polls INSERT to toggle it.
void ImGuiHook_NewFrameAndRender();

// The game's D3D11 device, null before the first frame (for the textures of
// the mod's pages).
ID3D11Device* ImGuiHook_Device();

// True while the overlay is visible and capturing input.
extern bool g_imguiVisible;
