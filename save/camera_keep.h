// SPDX-License-Identifier: GPL-3.0-only
// Each player's view, kept across a re-sync and in the save.
//
// A load puts the strategy view where the save's loader puts it (431268
// writes the orbit point): a re-sync or a join threw every player's view back
// to the map's start. Two sources bring it back, after the load
// (game/camera: Camera_GetView / Camera_SetView):
//   - a session's load while a world was loaded (a re-sync, or joining from a
//     loaded game): the view this PC had right before the load (read on every
//     frame of a session and every step outside one; up to 10 minutes old);
//   - otherwise this player's entry in the save's [camera] section
//     (save/mod_save): every save writes this PC's view under its key
//     ("steam_<SteamID64>", or "nick_<nick>" without Steam) and keeps the
//     entries of the other players it read, so a shared save copied between
//     the PCs keeps each player's own view.
// A save without an entry for this player loads with the game's own view.
#pragma once

#include "tesmio_plugin.h"

void CameraKeep_Bind(const TsmHost* host);

// Determinism's load hook (main thread): before the game's load, and after it
// once the save's file was read (ModSave_AfterLoad).
void CameraKeep_BeforeLoad(bool session);
void CameraKeep_AfterLoad();
