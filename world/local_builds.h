// SPDX-License-Identifier: GPL-3.0-only
// Local build capture: the two engine hooks that see what the local player
// builds and turn it into lockstep commands (lockstep/commands).
//
// Building placement - FUN_1402bade0 (HOOK_UNIVERSAL_RVA)
//   The game's single placement function. It runs every frame while a
//   building is selected (preview ghost) and creates the building on the
//   confirming click. In a session the click that would place becomes a
//   placement command with the call's INPUTS (cursor, yaw, mirror, param_5),
//   made on every PC by world/buildings.
//
// Infrastructure commit - FUN_140518a80 (HOOK_ROAD_COMMIT_RVA)
//   The commit shared by every drag tool. On entry, for a known tool manager
//   (world/networks Networks_CategoryForManager), the hook
//   copies the finished route - interior records, both node positions and
//   building ports - or, for an upgrade commit, the segments being re-typed,
//   or the bulldozer's piece, into a command instead of committing. Commits
//   made inside a building placement (a building laying its own access road)
//   are not taken.
#pragma once

#include "tesmio_plugin.h"

#define HOOK_UNIVERSAL_RVA 0x2BADE0
#define HOOK_ROAD_COMMIT_RVA 0x518A80

// Expected function prologues for the inline hooks (SOVIET64.exe, 2026-09-07).
extern const unsigned char kUniversalExpect[21];
extern const unsigned char kRoadCommitExpect[19];

typedef void (*UniversalFn)(__int64 param_1, char* param_2, char param_3, char param_4,
                             unsigned __int64 param_5, unsigned int param_6,
                             char param_7, char param_8);
extern UniversalFn g_origUniversal;

typedef void(__fastcall* RoadCommitFn)(__int64 manager,
                                        unsigned __int64 p2, unsigned __int64 p3,
                                        unsigned __int64 p4, unsigned int p5,
                                        unsigned __int64 p6, unsigned __int64 p7);
extern RoadCommitFn g_origRoadCommit;

void HookedUniversal(__int64 param_1, char* param_2, char param_3, char param_4,
                      unsigned __int64 param_5, unsigned int param_6,
                      char param_7, char param_8);

void HookedRoadCommit(__int64 manager,
                      unsigned __int64 p2, unsigned __int64 p3,
                      unsigned __int64 p4, unsigned int p5,
                      unsigned __int64 p6, unsigned __int64 p7);

// Bracket every building-placement call (local or a command's). Commits
// made inside it are the building's own auto-connections and are not taken.
// Lockstep: the building tool's placing press (sim_patches, 2cc37c). True if
// it became a placement command - the game then reads it as not pressed.
bool LocalBuilds_PlacementPress();

void LocalBuilds_BeginBuildingPlacement();
void LocalBuilds_EndBuildingPlacement();
bool LocalBuilds_InBuildingPlacement();


// Per-frame (game thread, Present hook): tracks the drag-active flag and logs
// a commit that was not taken.
void LocalBuilds_Tick();

// True while the LOCAL player has a drag in progress with any path tool
// (thisPtr+0xd2ac).
bool LocalBuilds_IsToolDragActive();

void LocalBuilds_Bind(const TsmHost* host);

// Lockstep: a network demolition command was made (every PC); the clicking PC
// may send that piece again if the tool still selects it.
struct NetDemolishMsg;
struct NetDemolishHover;
void LocalBuilds_DemolishMade(const NetDemolishMsg& m, const NetDemolishHover* hover = nullptr);
