// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the line window's line spacing button (by SoylentGreed).
//
// The line window 7cff50 reads the button's press at 7d3c71 and, hovered,
// plays a click, asks 491500(line) whether spacing is on (it also caches the
// line's length +25c) and on the window's real line (window +240) sets +0 to
// the inverse and each of the line's vehicles (+240..+248) +658 alike
// (7d3c9e..7d3d03): on the clicking PC only.
//
// The click sound's lea at 7d3c8c jumps to a relay: in a session's interface
// frame the press becomes a command (the line) and the effect is skipped;
// every PC makes the toggle at the boundary.
#pragma once

#include "host.h"

void LineSpacing_Bind(const ModHost* host);
bool LineSpacing_Install();

// The command (lockstep/commands).
bool LineSpacing_ApplyCommand(const void* data, size_t size);
