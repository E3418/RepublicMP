// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the residential window's immigrant invitations as a command (by
// SoylentGreed).
//
// A residential building's window (6f8cc0, the interface frame) draws three
// invite buttons and acts inline on a press: up to ten immigrants (five
// experts) are created with 823470 and paid for, each with a chance of a
// partner drawn from rand(). That ran on the clicking PC only - the money came
// back as a command, the citizens stayed there.
//
// The buttons ask C3D_INPUT::GetMouseLeftPressAOff (import exe+86c8b0) at
// 6fedba (third world, dollars), 6ff2a9 (Soviet bloc, rubles) and 6ff9ea
// (experts, rubles). In a session a press there is answered "no press" and
// becomes a command: the button and the building. Every PC replays the
// button's loop at the boundary (sounds and refusal messages are not replayed).
#pragma once

#include "tesmio_plugin.h"

void ImmigrantInvite_Bind(const TsmHost* host);

// Startup: points the three press reads at a relay. False leaves the buttons
// as they were (they then act on the clicking PC only).
bool ImmigrantInvite_Install();

// The command (lockstep/commands).
bool ImmigrantInvite_ApplyCommand(const void* data, size_t size);
