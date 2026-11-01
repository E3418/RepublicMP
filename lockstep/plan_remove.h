// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the factory window's "Remove from plan" (by SoylentGreed).
//
// The factory window 794fb0 lists the production queue +11d8..+11e0 (16-byte
// entries {type, variant, count}). An entry's "Remove from plan" acts on the
// press at 798cc2: it clears the press (798cca) and, with the row hovered
// ([rdi+20]), erases the entry inline (798cd5..798cf3; r15 the factory, r14
// the entry's byte offset): on the clicking PC only.
//
// The press clear jumps to a relay that clears it as the game does and tests
// the hover; in a session's interface frame the press becomes a command (the
// factory, the entry's place, its type and variant) and the erase is skipped;
// every PC erases the same entry at the boundary.
#pragma once

#include "tesmio_plugin.h"

void PlanRemove_Bind(const TsmHost* host);
bool PlanRemove_Install();

// The command (lockstep/commands).
bool PlanRemove_ApplyCommand(const void* data, size_t size);
