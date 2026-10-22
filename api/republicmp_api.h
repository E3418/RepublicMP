// SPDX-License-Identifier: GPL-3.0-only
// What a game mode inside RepublicMP.dll (modes/<name>/) may use of the rest
// of the DLL - one include, so nothing below gets written a second time.
// Everything here is plain functions of the modules listed; each module's
// header says the details. See ARCHITECTURE.md for the layers and rules.
//
// Rules for code in a mode's folder
//   - Bind: every .cpp that reads the game calls TsmBind(host) in its own
//     Bind function (g_exeBase and Logf are per translation unit); the mode's
//     Bind is called from core/main.cpp.
//   - Hooks on the game have one owner each (ARCHITECTURE.md lists them):
//     never a second hook on the same function - add a callback to the owner.
//   - SEH (__try) around every read of the game's memory; no allocation,
//     logging or I/O inside a hook that runs within the game's own call.
//   - Offsets and RVAs as named #defines with what they are; the exe is
//     WRSR 1.1.1.9 (stamp 0x6A3EB6AD).
#pragma once

// --- Loader: Logf, g_exeBase / g_exe, TsmBind, InstallInlineHook, PatchIat
#include "tesmio_plugin.h"

// --- The game's world: the world object, buildings, date, money, the
// terrain point under the mouse.
//   World_Object, World_IsLoaded, World_Buildings, World_BuildingCategory,
//   World_BuildingPos, World_MousePoint, World_Date, World_Money
#include "../game/world.h"

// --- The camera: world point -> screen (for drawing in the overlay).
//   Camera_Clip, Camera_Project
#include "../game/camera.h"

// --- Pause and speed: ReadGameSpeed, WriteGameSpeed, EnsureGamePaused,
// EnsureGameUnpaused (in a co-op session the host decides them).
#include "../session/game_speed.h"

// --- Terrain: the live C3D_TERRAIN, its height at a point, and the engine's
// own edit functions (unhooked).
//   TerrainHook_GetTerrain, TerrainHook_HeightAt, TerrainHook_ApplyEdit,
//   TerrainHook_HeightmapOpen/Close, TerrainHook_MaskOpen/Close
#include "../world/terrain_hook.h"

// --- Networks (roads, rails, pipes, power lines...): the tool managers on
// the world object and their categories.
//   Networks_GetCategoryInfo, Networks_CategoryForManager, Networks_SegmentCount
#include "../world/networks.h"

// --- Saving and loading through the game's own actions, and packing a save.
//   TriggerSaveGame, WaitForSaveToFinish, TriggerLoadSave, WaitForLoadToFinish,
//   ZipSaveDirectory, UnzipToDirectory
#include "../game/save_load.h"
#include "../save/save_zip.h"

// --- TCP (blocking, one thread per connection): NetworkInit, NetworkListen,
// NetworkAcceptReady, NetworkHostAcceptOne, NetworkConnect, NetworkSendExact,
// NetworkRecvExact, NetworkSendFile, NetworkReceiveFile, NetworkWaitReadable,
// NetworkPeerGone, NetworkCloseSocket. (Steam goes behind the same calls
// later.)
#include "../transport/network.h"

// --- The player: Profile_GetNick, and settings remembered in
// RepublicMP.profile.ini (Profile_GetInt / Profile_SetInt - a [section] of
// the mode's own).
#include "../core/profile.h"
#include "../core/version.h"

// --- The RepublicMP window's log (UiLog_Add) and the window/overlay hooks
// of a mode (Modes_SetRepublics: page, frame, overlay).
#include "../ui/ui_log.h"
#include "../modes/mode.h"
