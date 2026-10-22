// SPDX-License-Identifier: GPL-3.0-only
// The game's world: the one global object everything hangs off, and the
// fields on it that most modules read - one place for these addresses
// (SOVIET64.exe 1.1.1.9; older modules still keep their own copies).
//
//   exe+0x9D4F10   the world object ("god object"; the tools' thisPtr)
//   world+0x11B08  the buildings: std::vector<Building*> (exe+0x9E6A18 begin, +0x9E6A20 end)
//   world+0xF7A0   the road network manager (exe+0x9E46B0); 0 while no world is loaded
//   world+0x590    day of the year (int), +0x594 year (int), +0x59C day progress (float, 0..60)
//   world+0x580    dollars (double), +0x588 rubles (double)
//   world+0xF78    the terrain point under the mouse (3 floats; the game's own
//                  pick every frame, before a tool snaps it - 2f0f10)
//
// A building: +0x318 its type (type+0x360 the category), +0x350 its position
// (3 floats), +0x604 construction progress (float, 1 = finished).
//
// Reads are SEH-guarded; the pointers are valid for the current frame only
// (the lists are rebuilt on every load). Main thread for anything that walks
// a list while the simulation may change it.
#pragma once

#include "tesmio_plugin.h"

#define GAME_WORLD_RVA 0x9D4F10
#define GAME_BUILDINGS_BEGIN_RVA 0x9E6A18
#define GAME_BUILDINGS_END_RVA 0x9E6A20
#define GAME_ROAD_NETWORK_RVA 0x9E46B0
#define GAME_BUILDING_TYPE_OFFSET 0x318
#define GAME_TYPE_CATEGORY_OFFSET 0x360
#define GAME_BUILDING_POS_OFFSET 0x350
#define GAME_BUILDING_PROGRESS_OFFSET 0x604

void World_Bind(const TsmHost* host);

// The world object (never null: it is a global of the exe).
unsigned char* World_Object();

// A world is loaded (not the main menu, not in the middle of a load).
bool World_IsLoaded();

// The buildings list; false when there is none.
bool World_Buildings(unsigned char*** begin, unsigned char*** end);

// A building's type category (type+0x360), -1 for none (a construction site
// of a path, a scenery object), and its position.
int World_BuildingCategory(const unsigned char* building);
bool World_BuildingPos(const unsigned char* building, float out[3]);

// The terrain point under the mouse; false before a world or when unreadable.
bool World_MousePoint(float out[3]);

// The date and the two balances.
void World_Date(int* day, int* year, float* progress);
void World_Money(double* rubles, double* dollars);
