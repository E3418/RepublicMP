// See sync_block.h.
#include "sync_block.h"
#include "../hooks/game_speed.h" // EnsureGamePaused() re-assert, see SyncBlock_Draw
#include "imgui.h"
#include <windows.h>
#include <mutex>
#include <cstdio>

static std::mutex g_mutex;
static volatile bool g_active = false;
static char g_reason[128] = "Syncing...";

void SyncBlock_SetActive(bool active, const char* reason)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active = active;
    if (active && reason && reason[0]) {
        _snprintf_s(g_reason, sizeof(g_reason), _TRUNCATE, "%s", reason);
    }
}

bool SyncBlock_IsActive()
{
    return g_active;
}

void SyncBlock_Draw()
{
    if (!g_active)
        return;

    // Clicking/unpausing still works while this screen is up, despite
    // HookedWndProc unconditionally swallowing mouse/keyboard window messages
    // during SyncBlock_IsActive() (see hooks/imgui_hook.cpp) - the game reads
    // mouse/keyboard state some other way for at least some of its own input
    // (raw/polled, not through the window message queue; see sync_block.h).
    // Re-asserting the pause every single frame is a blunt but reliable fix
    // for the "can unpause" half of that: no matter how the player flips
    // game speed back on (clicking the UI, an unguarded hotkey, whatever raw
    // path the engine itself uses), this stomps it back to paused before the
    // next frame renders. See hooks/detect_spawn.cpp's HookedUniversal for
    // the matching fix on the "can still place buildings" half.
    EnsureGamePaused();

    char reasonCopy[128];
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        _snprintf_s(reasonCopy, sizeof(reasonCopy), _TRUNCATE, "%s", g_reason);
    }

    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.05f, 0.75f)); // dark gray, translucent
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##syncblock", nullptr, flags))
    {
        ImVec2 textSize = ImGui::CalcTextSize(reasonCopy);
        ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        ImGui::SetCursorPos(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f));
        ImGui::TextUnformatted(reasonCopy);
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // NOTE: ImGuiWindowFlags_NoInputs above means this window itself doesn't
    // set ImGui's WantCaptureMouse/WantCaptureKeyboard - that's intentional,
    // the actual input block is a separate, unconditional check in
    // hooks/imgui_hook.cpp's HookedWndProc keyed off SyncBlock_IsActive(),
    // not off ImGui's own capture flags (which this window deliberately
    // doesn't try to claim). This window is purely the visible dimming.
}
