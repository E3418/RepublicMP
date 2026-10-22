// SPDX-License-Identifier: GPL-3.0-only
// Game pause/speed control: a plain int at a fixed address.
//
// thisPtr+0x5a4 (exe+0x9D54B4): 0 = paused, 1 = x1, 2.. = faster. The main
// loop (FUN_140105a90) rebuilds the game timer's time scale from it every
// frame, so writing the int is all it takes to change the speed.
// thisPtr+0x5a8: the speed to restore when unpausing.
// thisPtr+0x5ac: "turbo" - with speed 2, a nonzero value makes the main loop
//   use a larger multiplier. The speed panel (FUN_140317db0) clears it on
//   every speed click and sets it when the fast button is clicked with keys
//   0x47+0x48 held.
// thisPtr+0x5a2: fast-forward while held - cleared by the speed panel every
//   frame and set while the left button is held on its button.
//
// At speed 2 the main loop multiplies the game timer by (smaller = faster):
//   0.35  fast (>>)
//   0.05  "Hyper"  - turbo != 0, or left Ctrl + Numpad1 held
//   0.01  "Plaid"  - hold flag set, or left Ctrl + Numpad2 held
// The keys are read straight from the C3D_INPUT key bytes (exe+0xA54B90 +
// 0x1d / 0x4f / 0x50) every frame, so Hyper/Plaid only ever speed up the
// machine whose player holds them.
//
// In a lockstep session the host decides pause and speed for every PC
// (lockstep/lockstep_net, determinism/determinism).
#pragma once

#include "tesmio_plugin.h"

#define GAME_SPEED_DAT_RVA 0x9D4F10
#define GAME_SPEED_OFFSET       0x5a4
#define GAME_SPEED_SAVED_OFFSET 0x5a8
#define GAME_SPEED_TURBO_OFFSET 0x5ac // int: extra-fast on top of speed 2 (see below)
#define GAME_SPEED_HOLD_OFFSET  0x5a2 // byte: fast-forward while a button is held

// Game clock (same object). FUN_140334780 adds the timer's PowerTime to the
// day progress every tick; past GAME_DAY_LENGTH it resets it to 0, runs the
// daily logic and increments the day; at day 365 the year rolls.
#define GAME_DAY_OFFSET          0x590 // int: day of the year
#define GAME_YEAR_OFFSET         0x594 // int: year
#define GAME_DAY_PROGRESS_OFFSET 0x59c // float: 0..60 through the day (2.5 = one hour)
#define GAME_DAY_LENGTH          60.0f

void GameSpeed_Bind(const TsmHost* host);

int ReadGameSpeed();       // 0 = paused, 1/2
int ReadGameSpeedSaved();  // speed saved before pause (-1 = none)
void WriteGameSpeed(int speed);

void EnsureGamePaused();   // idempotent
void EnsureGameUnpaused(); // idempotent
