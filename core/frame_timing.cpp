// SPDX-License-Identifier: GPL-3.0-only
// See frame_timing.h.
#include "frame_timing.h"
#include <stdio.h>

#define LOG_MS 2000

static volatile bool g_on = false;
static LARGE_INTEGER g_frequency = {};

struct SlotTotals {
    volatile LONG64 ticks;
    volatile LONG64 calls;
};
static SlotTotals g_slots[FRAME_SLOT_COUNT] = {};

// The main thread's frames: the time between two Late calls.
static LONG64 g_lastMain = 0;
static volatile LONG64 g_mainFrames = 0;
static volatile LONG64 g_mainTicks = 0;
static volatile LONG64 g_mainLongest = 0;
static volatile LONG64 g_presents = 0;
static ULONGLONG g_lastLogMs = 0;

static const char* const kSlotNames[FRAME_SLOT_COUNT] = {
    "early", "held", "prepare", "commands", "late", "after late", "ui", "visible",
    "site draw", "present ours", "overlay", "swap",
};

void FrameTiming_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool FrameTiming_GetOn()
{
    return g_on;
}

void FrameTiming_SetOn(bool on)
{
    if (!g_frequency.QuadPart) QueryPerformanceFrequency(&g_frequency);
    for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
        InterlockedExchange64(&g_slots[i].ticks, 0);
        InterlockedExchange64(&g_slots[i].calls, 0);
    }
    InterlockedExchange64(&g_mainFrames, 0);
    InterlockedExchange64(&g_mainTicks, 0);
    InterlockedExchange64(&g_mainLongest, 0);
    InterlockedExchange64(&g_presents, 0);
    g_lastMain = 0;
    g_lastLogMs = GetTickCount64();
    g_on = on;
    Logf("republicmp  FRAME_TIMING: %s", on ? "on - a line every 2 s" : "off");
}

long long FrameTiming_Begin()
{
    if (!g_on) return 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

void FrameTiming_End(FrameSlot slot, long long begin)
{
    if (!begin || !g_on) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    InterlockedAdd64(&g_slots[slot].ticks, now.QuadPart - begin);
    InterlockedIncrement64(&g_slots[slot].calls);
}

void FrameTiming_MainFrame()
{
    if (!g_on) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_lastMain) {
        LONG64 dt = now.QuadPart - g_lastMain;
        InterlockedIncrement64(&g_mainFrames);
        InterlockedAdd64(&g_mainTicks, dt);
        if (dt > g_mainLongest) InterlockedExchange64(&g_mainLongest, dt);
    }
    g_lastMain = now.QuadPart;
}

static double Ms(LONG64 ticks)
{
    return g_frequency.QuadPart ? ticks * 1000.0 / g_frequency.QuadPart : 0.0;
}

void FrameTiming_Present()
{
    if (!g_on) return;
    InterlockedIncrement64(&g_presents);
    ULONGLONG now = GetTickCount64();
    if (now - g_lastLogMs < LOG_MS) return;
    double seconds = (now - g_lastLogMs) / 1000.0;
    g_lastLogMs = now;
    LONG64 frames = InterlockedExchange64(&g_mainFrames, 0);
    LONG64 mainTicks = InterlockedExchange64(&g_mainTicks, 0);
    LONG64 longest = InterlockedExchange64(&g_mainLongest, 0);
    LONG64 presents = InterlockedExchange64(&g_presents, 0);
    // Each slot as ms per main-thread frame, and its calls per frame.
    char parts[1024];
    int at = 0;
    for (int i = 0; i < FRAME_SLOT_COUNT && at < (int)sizeof(parts); i++) {
        LONG64 ticks = InterlockedExchange64(&g_slots[i].ticks, 0);
        LONG64 calls = InterlockedExchange64(&g_slots[i].calls, 0);
        double per = frames ? Ms(ticks) / frames : 0.0;
        double callsPer = frames ? (double)calls / frames : 0.0;
        at += snprintf(parts + at, sizeof(parts) - at, "%s%s %.2f (x%.1f)", i ? ", " : "", kSlotNames[i], per, callsPer);
    }
    Logf("republicmp  FRAME_TIMING: %.1f s - main frames %lld (avg %.1f ms, longest %.1f), presents %lld; "
         "ms per frame: %s",
         seconds, frames, frames ? Ms(mainTicks) / frames : 0.0, Ms(longest), presents, parts);
}
