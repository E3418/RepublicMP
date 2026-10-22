// SPDX-License-Identifier: GPL-3.0-only
// See game_speed.h.
#include "game_speed.h"

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

// Called every frame by the sync screen - logs only when it changes something.
void EnsureGamePaused()
{
    if (ReadGameSpeed() != 0) {
        int cur = ReadGameSpeed();
        WriteGameSpeedSaved(cur);
        WriteGameSpeed(0);
        Logf("republicmp  GAME_SPEED: EnsureGamePaused -> paused (was speed %d)", cur);
    }
}

void EnsureGameUnpaused()
{
    if (ReadGameSpeed() == 0) {
        int restore = ReadGameSpeedSaved();
        if (restore <= 0) restore = 1; // 0/-1/uninitialized all mean "no real saved speed"
        WriteGameSpeed(restore);
        Logf("republicmp  GAME_SPEED: EnsureGameUnpaused -> unpaused, speed=%d", restore);
    }
}
