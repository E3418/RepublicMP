// SPDX-License-Identifier: GPL-3.0-only
// Republics: each player runs their own republic on one map, split in two;
// the neighbour's buildings, vehicles and citizens are drawn as render-only
// copies, and the two trade at the border.
//
// Not built yet. This folder is its place in RepublicMP.dll: the start
// page's card leads to the page drawn here, and the frame and overlay hooks
// are wired (modes/mode.h). What the mode may use of the rest of the DLL is
// listed in api/republicmp_api.h.
#pragma once

#include "tesmio_plugin.h"

// Called from TsmPluginInit; plugs the mode in (Modes_SetRepublics).
void Republics_Bind(const TsmHost* host);
