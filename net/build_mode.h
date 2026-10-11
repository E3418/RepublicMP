// Construction mode of the small build window: pay with rubles, pay with
// dollars, or realistic construction - plus two more settings the placement
// copies into each new building. All four are ints on the world object
// (thisPtr = exe+0x9D4F10) and are saved in the save header next to the
// game speed.
//
//   +0xaf8  != 0: build with rubles - FUN_1402bade0 completes the new
//                 building at once and pays in rubles (FUN_140160550(.., 1, 0)),
//                 path tools pay their pieces the same way (FUN_1405258e0)
//   +0xafc  != 0: the same with dollars (FUN_140160550(.., 0, 1))
//   both 0:       realistic - a construction site is placed
//   +0xb00        copied into the new building (building+0x11b8)
//   +0xb04        read when a building is created (fill level)
//
// A remote placement/path is replayed with the SENDER's mode: written here
// just before the replay and the local player's own restored right after,
// so the window keeps showing what the local player chose. Money is not
// synced yet - each side pays its own pool.
#pragma once

#include "tesmio_plugin.h"

#define BUILD_MODE_THIS_RVA 0x9D4F10
#define BUILD_MODE_RUB_OFFSET 0xaf8
#define BUILD_MODE_USD_OFFSET 0xafc
#define BUILD_MODE_B00_OFFSET 0xb00
#define BUILD_MODE_B04_OFFSET 0xb04

// Wire form (part of the build and road sync messages).
struct BuildMode {
    int rub; // +0xaf8
    int usd; // +0xafc
    int b00; // +0xb00
    int b04; // +0xb04
};

static BuildMode BuildMode_Read()
{
    BuildMode m = {};
    __try {
        BYTE* w = g_exeBase + BUILD_MODE_THIS_RVA;
        m.rub = *(int*)(w + BUILD_MODE_RUB_OFFSET);
        m.usd = *(int*)(w + BUILD_MODE_USD_OFFSET);
        m.b00 = *(int*)(w + BUILD_MODE_B00_OFFSET);
        m.b04 = *(int*)(w + BUILD_MODE_B04_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return m;
}

static void BuildMode_Write(const BuildMode& m)
{
    __try {
        BYTE* w = g_exeBase + BUILD_MODE_THIS_RVA;
        *(int*)(w + BUILD_MODE_RUB_OFFSET) = m.rub;
        *(int*)(w + BUILD_MODE_USD_OFFSET) = m.usd;
        *(int*)(w + BUILD_MODE_B00_OFFSET) = m.b00;
        *(int*)(w + BUILD_MODE_B04_OFFSET) = m.b04;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool BuildMode_Equal(const BuildMode& a, const BuildMode& b)
{
    return a.rub == b.rub && a.usd == b.usd && a.b00 == b.b00 && a.b04 == b.b04;
}

// "rubles", "dollars", "realistic" (+ the two extra settings), for the log.
static const char* BuildMode_Describe(const BuildMode& m, char* out, size_t outSize)
{
    const char* pay = (m.rub && m.usd) ? "rubles+dollars?" : m.rub ? "rubles" : m.usd ? "dollars" : "realistic";
    _snprintf_s(out, outSize, _TRUNCATE, "%s (b00=%d b04=%d)", pay, m.b00, m.b04);
    return out;
}
