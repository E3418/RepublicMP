// See game_speed.h.
#include "game_speed.h"
#include "window_focus.h"
#include "../ui/sync_block.h"

#pragma comment(lib, "User32.lib")

#define GAME_SPEED_TOGGLE_VK VK_F10
#define GAME_SPEED_POLL_MS 50

void GameSpeed_Bind(const TsmHost* host)
{
    TsmBind(host);
}

int ReadGameSpeed()
{
    return *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_OFFSET);
}

int ReadGameSpeedSaved()
{
    return *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_SAVED_OFFSET);
}

void WriteGameSpeed(int speed)
{
    *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_OFFSET) = speed;
}

static void WriteGameSpeedSaved(int speed)
{
    *(int*)(g_exeBase + GAME_SPEED_DAT_RVA + GAME_SPEED_SAVED_OFFSET) = speed;
}

void ForceTogglePause()
{
    int cur = ReadGameSpeed();
    if (cur == 0) {
        int restore = ReadGameSpeedSaved();
        if (restore <= 0) restore = 1; // 0/-1/uninitialized all mean "no real saved speed"
        WriteGameSpeed(restore);
        Logf("republicmp  GAME_SPEED: ForceTogglePause -> unpaused, speed=%d", restore);
    } else {
        WriteGameSpeedSaved(cur);
        WriteGameSpeed(0);
        Logf("republicmp  GAME_SPEED: ForceTogglePause -> paused (was speed %d)", cur);
    }
}

void EnsureGamePaused()
{
    if (ReadGameSpeed() != 0) {
        int cur = ReadGameSpeed();
        WriteGameSpeedSaved(cur);
        WriteGameSpeed(0);
        Logf("republicmp  GAME_SPEED: EnsureGamePaused -> paused (was speed %d)", cur);
    } else {
        Logf("republicmp  GAME_SPEED: EnsureGamePaused -> already paused");
    }
}

void EnsureGameUnpaused()
{
    if (ReadGameSpeed() == 0) {
        int restore = ReadGameSpeedSaved();
        if (restore <= 0) restore = 1; // 0/-1/uninitialized all mean "no real saved speed"
        WriteGameSpeed(restore);
        Logf("republicmp  GAME_SPEED: EnsureGameUnpaused -> unpaused, speed=%d", restore);
    } else {
        Logf("republicmp  GAME_SPEED: EnsureGameUnpaused -> already not paused");
    }
}

DWORD WINAPI GameSpeedHotkeyThread(LPVOID)
{
    bool keyWasDown = false;
    for (;;) {
        Sleep(GAME_SPEED_POLL_MS);
        bool keyIsDown = IsGameWindowFocused() && (GetAsyncKeyState(GAME_SPEED_TOGGLE_VK) & 0x8000) != 0;
        bool keyJustPressed = keyIsDown && !keyWasDown;
        keyWasDown = keyIsDown;
        if (!keyJustPressed) {
            continue;
        }
        if (SyncBlock_IsActive()) {
            // F10 polls GetAsyncKeyState directly, not routed through
            // WndProc, so it isn't covered by the input block in
            // hooks/imgui_hook.cpp - guard it here explicitly so it can't
            // unpause the game out from under an active save-sync.
            Logf("republicmp  GAME_SPEED: F10 pressed but ignored - a save-sync is in progress");
            continue;
        }
        Logf("republicmp  GAME_SPEED: F10 pressed");
        ForceTogglePause();
    }
    return 0;
}
