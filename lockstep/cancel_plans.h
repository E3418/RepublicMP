// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: "Cancel all plans" (by SoylentGreed).
//
// Two buttons remove every planned (not yet started) edge of networks:
//   the network tool panel (press at 39f2ab, the panel's network r13): with
//   no scenario (world +13178 == -1) it walks the network's edges +2b0 from
//   last to first; each edge with +140 set widens a box by its nodes' +4/+c
//   and goes with 5434c0(network, edge, 0, -1, 0); then for a network whose
//   mode +258 is 0 or 4, 12d990(world, min, max) and 12f010(world, min, max),
//   for mode 0xd 592990([world+fa38], 0);
//   the construction menu (press at 0883c8): the same for each network of the
//   menu's list [rbp-10] (count [rbp+18]) from last to first, one box.
// 5434c0 frees heap edges: a press changed the clicking PC only.
//
// Both presses (call [GetMouseLeftPress]) go through relays: in a session's
// interface frame a press is answered "no press" and the networks become a
// command (each by its world field); every PC makes the loop at the boundary.
#pragma once

#include "host.h"

void CancelPlans_Bind(const ModHost* host);
bool CancelPlans_Install();

// The command (lockstep/commands).
bool CancelPlans_ApplyCommand(const void* data, size_t size);
