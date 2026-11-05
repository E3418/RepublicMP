// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the crime list's "Cancel assignment" (by SoylentGreed).
//
// The police station and court windows draw their cases through 7677c0
// (world r13, the building's case list +1388 at [rbp+170], a row's case in
// r12). A row's button acts when the mouse was pressed (exe+a54e91, the je at
// 768584), the button is hovered ([rdi+20]) and no row acted yet this frame
// ([rbp+150]); the effect (76859f..76866e) was made on the clicking PC only:
// the case counted in its building (+330/+334/+338 of [[case+8]+108]+1b8 by
// the case's kind +70) and in world +11658/+1165c/+11660, then 2571e0(world,
// case, 0.0) and 2580d0(world, case).
//
// The je jumps to a relay: in a session's interface frame the press becomes a
// command (the building, the case's place in +1388 and a check of its +4 and
// +70) and the row counts as acted; every PC makes the effect on its own case
// at the boundary. Elsewhere the window goes on as before.
#pragma once

#include "host.h"

void CrimeCancel_Bind(const ModHost* host);
bool CrimeCancel_Install();

// The command (lockstep/commands).
bool CrimeCancel_ApplyCommand(const void* data, size_t size);
