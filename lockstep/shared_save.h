// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: saving the shared game, on every PC at the same tick.
//
// A save writes every vehicle and ages it one step (42cc70 -> 6c6770 ->
// 6c1bd0), so a save on one PC alone split the games and was refused in a
// session. The save window writes the name (world +9f8, "save/<name>"), the
// zip byte +14d36, a frame count +14d38 and the request byte +14d35; the Late
// body counts down and calls 42cc70 at 3151fb. In a session the request
// becomes a command instead, and every PC saves at the boundary - the same
// state on every PC, paused or not. Each PC keeps its own copy of the save. A
// save that leaves the game (+5030) is left alone: that PC leaves the session.
//
// The autosave goes the same way. The game's own (the Late body, 314dd2..
// 3151a5) runs on each PC's real time, so in a session its timer (world +0)
// is held at 0 (determinism/sim_patches). Instead the host counts the
// interval it has set in the options (AUTOSAVE_INTERVAL) and sends an
// autosave command; at the boundary every PC writes its own next slot
// ("save/autosave<n>", its counter AUTOSAVE, AUTOSAVE_NUM slots, a zip when
// AUTOSAVE_TO_ZIP), as the game does - every player keeps backups of the
// shared game.
#pragma once

#include "tesmio_plugin.h"

void SharedSave_Bind(const TsmHost* host);

// Main thread, before the Late body: this PC's save request becomes a command.
void SharedSave_BeforeLate(unsigned char* world);

// The command (lockstep/commands).
bool SharedSave_ApplyCommand(const void* data, size_t size);
