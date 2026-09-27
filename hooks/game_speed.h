// Game pause/speed control: a plain int at a fixed address.
#pragma once

#include "tesmio_plugin.h"

#define GAME_SPEED_DAT_RVA 0x9D4F10
#define GAME_SPEED_OFFSET       0x5a4
#define GAME_SPEED_SAVED_OFFSET 0x5a8

void GameSpeed_Bind(const TsmHost* host);

int ReadGameSpeed();       // 0 = paused, 1/2
int ReadGameSpeedSaved();  // speed saved before pause (-1 = none)
void WriteGameSpeed(int speed);

void ForceTogglePause();   // mirrors the game's own TOGGLE_PAUSE behavior
void EnsureGamePaused();   // idempotent
void EnsureGameUnpaused(); // idempotent

DWORD WINAPI GameSpeedHotkeyThread(LPVOID); // F10 test hotkey
