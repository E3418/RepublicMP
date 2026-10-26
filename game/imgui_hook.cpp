// SPDX-License-Identifier: GPL-3.0-only
#include "imgui_hook.h"
#include "window_focus.h"
#include "main_menu_hook.h"
#include "input_block.h"
#include "../ui/main_menu.h"
#include "../ui/debug_window.h"
#include "../ui/sync_block.h"
#include "../ui/multiplayer_page.h"
#include "../lockstep/presence.h"
#include "../modes/mode.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>

// Declared in imgui_impl_win32.h only under IMGUI_IMPL_API; re-declared here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dContext = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static HWND g_hwnd = nullptr;
static WNDPROC g_originalWndProc = nullptr;
static bool g_initialized = false;
static bool g_insertWasDown = false;

bool g_imguiVisible = false; // a debug tool now: INSERT shows it; the MULTIPLAYER page is the players' UI

void ImGuiHook_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static void CreateRenderTarget(IDXGISwapChain* swapChain)
{
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (backBuffer)
    {
        g_pd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRenderTargetView);
        backBuffer->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView)
    {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // The sync-block screen must keep receiving input even while the overlay is
    // hidden with INSERT.
    bool syncBlocking = SyncBlock_IsActive();
    bool page = MultiplayerPage_IsOpen(); // the MULTIPLAYER page, over the main menu or a city
    // ...and its button beside the game's Escape menu takes clicks too.
    bool menuButton = MultiplayerPage_MenuButtonShown();
    bool effectiveVisible = g_imguiVisible || syncBlocking || page || menuButton;

    ImGuiIO& io = ImGui::GetIO();

    // Draw ImGui's own cursor only while the mouse is over an ImGui window, or
    // while the sync-block screen is up (it uses NoInputs and never captures the
    // mouse). Elsewhere the game's own cursor handling runs unmodified.
    bool showOurCursor = syncBlocking || ((g_imguiVisible || page || menuButton) && io.WantCaptureMouse);
    io.MouseDrawCursor = showOurCursor;

    // Releases always reach ImGui: what it shows can vanish between a press and
    // its release (the game closes its menu), and a button it thinks still held
    // would eat the next press.
    bool release = msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_KEYUP ||
                   msg == WM_SYSKEYUP;
    if (effectiveVisible || release)
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

    bool imguiWantsInput = effectiveVisible && (io.WantCaptureMouse || io.WantCaptureKeyboard);

    // During a sync all input is swallowed regardless of ImGui's capture flags;
    // while our page covers the menu, every key is ours (Escape included).
    bool pageKey = page && (msg == WM_KEYDOWN || msg == WM_CHAR); // releases go through: no key left held
    if (imguiWantsInput || syncBlocking || pageKey)
    {
        // Consumed by ImGui, or dropped during a sync - do not forward to the game.
        switch (msg)
        {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_MOUSEMOVE:
        case WM_KEYDOWN: case WM_CHAR: case WM_SYSKEYDOWN:
            return TRUE; // key releases go through: the game never keeps a key held

        }
    }

    // Swallow WM_SETCURSOR only while we draw the cursor, so the game cannot
    // override our sprite; otherwise let the game handle it.
    if (showOurCursor && msg == WM_SETCURSOR)
        return TRUE;

    return CallWindowProc(g_originalWndProc, hwnd, msg, wParam, lParam);
}

ID3D11Device* ImGuiHook_Device()
{
    return g_pd3dDevice;
}

void ImGuiHook_EnsureInit(IDXGISwapChain* swapChain)
{
    if (g_initialized || !swapChain)
        return;

    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_pd3dDevice)) || !g_pd3dDevice)
        return;
    g_pd3dDevice->GetImmediateContext(&g_pd3dContext);

    DXGI_SWAP_CHAIN_DESC desc = {};
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;
    if (!g_hwnd)
        return;

    CreateRenderTarget(swapChain);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);

    // Subclass the game window now that its HWND is known.
    g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);

    g_initialized = true;
    Logf("republicmp  IMGUI: initialized, hwnd=%p", (void*)g_hwnd);
}

void ImGuiHook_NewFrameAndRender()
{
    if (!g_initialized)
        return;

    // INSERT toggles the overlay (only while the game window has focus).
    bool insertDown = IsGameWindowFocused() && (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (insertDown && !g_insertWasDown)
        g_imguiVisible = !g_imguiVisible;
    g_insertWasDown = insertDown;
    // The MULTIPLAYER item of the game's main menu opens our page over it.
    if (MainMenuHook_TakeClick())
        MultiplayerPage_Open();
    MultiplayerPage_Tick();
    bool page = MultiplayerPage_IsOpen();

    // The sync-block screen renders even while the overlay is hidden.
    bool syncBlocking = SyncBlock_IsActive();

    // Refresh the cursor flag for frames where hover changed without a message.
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = syncBlocking || ((g_imguiVisible || page || MultiplayerPage_MenuButtonShown()) && io.WantCaptureMouse);
    // What goes to the mod's windows does not reach the game too (game/input_block).
    // The page is modal, like the game's own menus: while it is up the game gets
    // no click and no key. The Escape that closed it stays ours until released,
    // so it does not open the game's pause menu as well; so does the left
    // button that clicked BACK - the game took that held button as its own and,
    // at a high game speed, answered it on every tick of the frame.
    static bool s_escapeHeld = false, s_mouseHeld = false;
    s_escapeHeld = (page || s_escapeHeld) && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    s_mouseHeld = (page || s_mouseHeld) && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool menuButton = MultiplayerPage_MenuButtonShown(); // beside the game's Escape menu
    InputBlock_SetUiCapture(page || s_mouseHeld || ((g_imguiVisible || menuButton) && io.WantCaptureMouse),
                            page || s_escapeHeld || (g_imguiVisible && io.WantTextInput));

    // The other players' cursors and pings show while the overlay is hidden too.
    bool presence = Presence_Active();
    bool modeOverlay = Modes_WantsOverlay();
    if (!g_imguiVisible && !syncBlocking && !presence && !modeOverlay && !page && !menuButton)
        return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (page) MultiplayerPage_Draw(); // under the overlay's windows
    if (menuButton) MultiplayerPage_DrawMenuButton();
    if (g_imguiVisible) {
        g_mainMenu.Draw();
        g_debug_window.Draw();
    }
    if (presence) Presence_Frame(); // under the windows (background draw list)
    if (modeOverlay) Modes_DrawOverlay();
    SyncBlock_Draw(); // drawn last (on top), independent of g_imguiVisible

    ImGui::Render();
    g_pd3dContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // ResizeBuffers is not hooked: a runtime resolution change would leave
    // g_mainRenderTargetView stale (fix: recreate the render target on resize).
}
