// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP by E3418 and Sohn (juniorsohn); all authors in README.md.
// Republics: each republic's share of the economy statistics.
//
// The game's current statistics record (world +11328, 0x400 bytes) holds 16
// resource lists (imports and exports, spending of constructions, factories,
// shops and vehicles, production, waste), each entry {resource, quantity,
// money}, written by Stats_Add 2fdb00 and Stats_AddList 2fdf40; the day tick
// pushes it to the history and resets it (2fc290). The record and its history
// stay the whole map's: the day's price update (2fb1d0) and the scenario
// objectives read them.
//
// Beside them this keeps, per list and resource, East minus West: what a call
// adds while the wallets know whose it is (Wallets_Half: a command's player,
// the building, vehicle or loan a tick is updating) counts for that republic,
// the rest for neither. A republic's share is then (total + d) / 2 for the East
// and (total - d) / 2 for the West, which splits what had no owner in two. Each
// history record's (by its place in the history, as citizen_stats keeps the
// citizens') goes into the save's republicmp.ini, the resources by name as the
// game's own save writes them. The current record's does not: a load empties
// the game's current record, and the reset hook these shares with it.
//
// Not counted yet: the record's scalars (vehicle imports and exports,
// immigrants, tourism, loans) and the imports 159070 writes without a call. An
// add from the interface's thread takes whatever the simulation's scope is at
// that moment. Nothing shows the shares yet.
#pragma once

#include "host.h"

void EconomyStats_Bind(const ModHost* host);
bool EconomyStats_Install();

// citizen_stats' hook on the history push: the game pushed the current record
// as history record `index` (on the simulation thread of a Republics session).
void EconomyStats_HistoryPushed(int index, int day, int year);
// The game's history started over (a new game): the shares' history goes too.
void EconomyStats_HistoryCleared();
