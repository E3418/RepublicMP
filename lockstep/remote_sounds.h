// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: another player's command makes no sound here.
//
// Every PC makes every player's command, and the game plays the sounds the
// command's routine asks for (a placed building, a road, a purchase) on each
// of them: the host heard the player's roads being built across the map. The
// routines ask the sound manager by name, through two imports:
//   C3D_SOUNDMANAGER::PlaySound2D(name, bool, float, char*)  (interface sounds)
//   C3D_SOUNDMANAGER::PlaySound3D(name, C3DVECTOR3, bool, float)  (at a point)
// and looped sounds the game keeps start with C3D_SOUND2D / C3D_SOUND3D::Play.
// The tools' loops (terrain, construction: world +d488, 0x110 bytes each) are
// not started by the command itself: it sets a loop's switch (+10c) and the
// Late body fades it in on the next frames. Around another player's command
// the switches are put back (RemoteSounds_CommandBegin / End).
// While a command of another player is being made (Commands_ApplyingRemote,
// on the thread making it) both answer "not played" without playing; the
// player who gave the command hears it as before. The world is not changed.
// profile [debug] sound_log=1 logs every sound and the command it came in.
#pragma once

#include "host.h"

void RemoteSounds_Bind(const ModHost* host);
bool RemoteSounds_Install();

// lockstep/commands, main thread, around another player's command.
void RemoteSounds_CommandBegin();
void RemoteSounds_CommandEnd();
// Main thread, around what no player did: no sound for anyone, whatever the setting.
void RemoteSounds_Silence(bool on);

// Sounds of other players' commands left out (profile "overlay" "remote_sounds" 0);
// on by default.
bool RemoteSounds_GetMuted();
void RemoteSounds_SetMuted(bool on);
