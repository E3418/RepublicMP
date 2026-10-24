// SPDX-License-Identifier: GPL-3.0-only
// Republics: each player runs their own republic on one map, split in two,
// and the two trade at the border.
//
// Built on co-op's lockstep: both PCs simulate the whole map. So far the map
// is cut along a diagonal moved to halve the playable area (the same cut on
// every PC, measured from the map), each player's building and network commands are
// refused on the other half (Commands_SetAreaRule) and the border is drawn on
// the terrain and the minimap with the game's own 2D panel. Money is still the game's one pair of balances. What the mode
// may use of the rest of the DLL is listed in api/republicmp_api.h.
#pragma once

#include "tesmio_plugin.h"

// Called from TsmPluginInit; plugs the mode in (Modes_SetRepublics).
void Republics_Bind(const TsmHost* host);
