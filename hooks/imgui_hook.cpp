#include "imgui_hook.h"
#include "window_focus.h"
#include "../ui/main_menu.h"
#include "../ui/debugui.h"
#include "../ui/sync_block.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>

// Forward decl (declared in imgui_impl_win32.h, but only under
// IMGUI_IMPL_API when compiled as part of ImGui itself - re-declaring here
// keeps this file independent of that macro's expansion).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dContext = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static HWND g_hwnd = nullptr;
static WNDPROC g_originalWndProc = nullptr;
static bool g_initialized = false;
static bool g_insertWasDown = false;

bool g_imguiVisible = true;

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
    // While a save-sync is in progress (see ui/sync_block.h), the ImGui
    // overlay must keep receiving/rendering input even if the user has the
    // menu hidden (INSERT) - the gray blocking screen has to stay up and
    // stay in control regardless of that toggle.
    bool syncBlocking = SyncBlock_IsActive();
    bool effectiveVisible = g_imguiVisible || syncBlocking;

    ImGuiIO& io = ImGui::GetIO();

    // Draw ImGui's own software cursor sprite (in its own draw list, on top
    // of everything) ONLY while the mouse is actually hovering an ImGui
    // window/widget (io.WantCaptureMouse) - or unconditionally during the
    // sync-block gray screen, which uses ImGuiWindowFlags_NoInputs and so
    // never sets WantCaptureMouse on its own, but still needs a visible
    // pointer the whole time it's up.
    //
    // Gated on WantCaptureMouse rather than the whole overlay being open
    // (effectiveVisible): tying it to effectiveVisible would keep the
    // sprite on screen everywhere, even with the mouse sitting over bare
    // game area while a menu window is merely open somewhere else. Gating
    // on WantCaptureMouse instead means: off the menu, WM_SETCURSOR below
    // is left alone and the game's own WndProc/cursor logic runs exactly as
    // it would with no mod loaded (however it prefers to hide/lock the
    // cursor during normal gameplay); over the menu, our sprite takes over.
    bool showOurCursor = syncBlocking || (g_imguiVisible && io.WantCaptureMouse);
    io.MouseDrawCursor = showOurCursor;

    if (effectiveVisible)
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

    bool imguiWantsInput = effectiveVisible && (io.WantCaptureMouse || io.WantCaptureKeyboard);

    // syncBlocking swallows input UNCONDITIONALLY, independent of ImGui's own
    // want-capture flags - see ui/sync_block.h's header comment for why this
    // can't just rely on WantCaptureMouse/Keyboard the way the menu-visible
    // case above does.
    if (imguiWantsInput || syncBlocking)
    {
        // ImGui already consumed this event above (or we're intentionally
        // dropping it during a sync) - don't also forward it to the game
        switch (msg)
        {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_MOUSEMOVE:
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            return TRUE;
        }
    }

    // WM_SETCURSOR gets its own check, separate from imguiWantsInput above:
    // swallow it exactly while WE are drawing the cursor (showOurCursor),
    // so the game's original WndProc can't re-assert its own cursor state
    // over our sprite. The rest of the time (overlay open, mouse over bare
    // game area) this falls through on purpose, so the game's own cursor
    // behavior runs unmodified.
    if (showOurCursor && msg == WM_SETCURSOR)
        return TRUE;

    return CallWindowProc(g_originalWndProc, hwnd, msg, wParam, lParam);
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

    // Input hook: subclass the game window now that its HWND is known
    g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);

    g_initialized = true;
    Logf("republicmp  IMGUI: initialized, hwnd=%p", (void*)g_hwnd);
}

void ImGuiHook_NewFrameAndRender()
{
    if (!g_initialized)
        return;

    // INSERT toggles the whole overlay - same window-focus guard as O/I/P/L/F9/F10
    bool insertDown = IsGameWindowFocused() && (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (insertDown && !g_insertWasDown)
        g_imguiVisible = !g_imguiVisible;
    g_insertWasDown = insertDown;

    // The sync-block screen (see ui/sync_block.h) must keep rendering even if
    // the user hid the menu with INSERT - it's not optional UI, it's what's
    // actually holding the game still during a save-sync.
    bool syncBlocking = SyncBlock_IsActive();

    // Belt-and-suspenders re-sync of the cursor-drawing flag set in
    // HookedWndProc (see its comment there) - covers the rare frame where
    // hover state just changed but no window message has arrived yet to
    // refresh it.
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = syncBlocking || (g_imguiVisible && io.WantCaptureMouse);

    if (!g_imguiVisible && !syncBlocking)
        return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_imguiVisible) {
        g_mainMenu.Draw();
        g_debugui.Draw();
    }
    SyncBlock_Draw(); // drawn last (on top), independent of g_imguiVisible

    ImGui::Render();
    g_pd3dContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // NOTE: no ResizeBuffers hook yet - if the game resolution changes at
    // runtime, g_mainRenderTargetView will point at a stale back buffer.
    // Not needed for now (fullscreen res usually fixed for a session), but if
    // it starts flickering/crashing on alt-tab or resize, that's the fix:
    // CleanupRenderTarget() + CreateRenderTarget() around the resize call.
}
