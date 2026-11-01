// SPDX-License-Identifier: GPL-3.0-only
// Frame timing: a diagnostic (off by default, the Debug window sets it,
// remembered) that
// logs every 2 s where a frame's time went - the game's phases with the hooks
// around them, and this plugin's own work inside them - to find what makes a
// PC slow in a session.
#pragma once

#include "tesmio_plugin.h"

enum FrameSlot {
    FRAME_EARLY,          // the tick's Early (the simulation), on step frames
    FRAME_HELD,           // a held frame's presentation
    FRAME_PREPARE,        // the view preparation, with the commands made before it
    FRAME_COMMANDS,       // the commands made at the boundary (inside Prepare)
    FRAME_LATE,           // the Late body
    FRAME_AFTER_LATE,     // after a tick: the state hash, the money trace, the commands' cleanup
    FRAME_UI,             // the interface routine
    FRAME_VISIBLE,        // this PC's visible list in and out (ours, inside the phases)
    FRAME_SITE_DRAW,      // the construction site drawer, every pass
    FRAME_PRESENT_OURS,   // the plugin's per-frame ticks before the overlay
    FRAME_OVERLAY,        // the ImGui overlay
    FRAME_SWAP,           // the swap chain's own Present
    FRAME_SLOT_COUNT
};

void FrameTiming_Bind(const TsmHost* host);
bool FrameTiming_GetOn();
void FrameTiming_SetOn(bool on);

// Once per main-thread frame (the Late hook).
void FrameTiming_MainFrame();
// Once per Present; writes the log line every 2 s.
void FrameTiming_Present();

long long FrameTiming_Begin(); // 0 while off
void FrameTiming_End(FrameSlot slot, long long begin);

struct FrameTimingScope {
    FrameSlot slot;
    long long begin;
    explicit FrameTimingScope(FrameSlot s) : slot(s), begin(FrameTiming_Begin()) {}
    ~FrameTimingScope() { FrameTiming_End(slot, begin); }
};
