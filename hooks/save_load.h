// Triggers the game's own "load game" path (same as clicking Load in the menu).
// We write the save path + a couple of flags exactly like the Load-menu click
// handler does (FUN_140285fc0, confirmed by x64dbg + decompile), then the game's
// main loop (WinMain -> FUN_140105700 -> FUN_14028c770 -> FUN_140430fc0) picks it
// up on the next frame and loads it itself. No hooks, no manual file parsing.
#pragma once

#include "tesmio_plugin.h"

#define SAVE_NAME_DAT_RVA     0x9962b0  // DAT_1409962b0 - save/map name buffer; menu writes "save\\<name>" here
#define LOAD_TRIGGER_DAT_RVA  0x993c66  // DAT_140993c66 - "start/load game" flag byte
#define TERRAIN_FLAG_DAT_RVA  0x995298  // DAT_140995298 - must be 0 or the name above gets replaced with a hardcoded terrain
#define RETURN_MENU_DAT_RVA   0x995289  // DAT_140995289 - cleared by the menu right alongside the trigger
#define SAVE_NAME_MAX_LEN     120       // safe cap, real save names are much shorter
#define SAVE_FOLDER_PREFIX    "save\\"  // saves picked from the normal save list are stored as "save\\<name>"

// --- Save trigger: same "god-object" build_sync.cpp already uses for
// placement (UNIVERSAL_THIS_RVA there), not a plain DAT_ global like the Load
// side above - the save-name field and trigger flag both live INSIDE this
// object's memory. Identified from a Ghidra decompile of FUN_14042cc70 (the
// master save routine - writes every subsystem's .bin file, gated by
// "Cannot open %s for save X" strings) and FUN_14030d1a0 (the per-frame
// dispatcher that calls it), and confirmed in x64dbg: a hardware
// write-breakpoint on WORLD_THIS_RVA+SAVE_TRIGGER_FLAG_OFFSET fires exactly
// when clicking Save in the pause menu, and the name field at
// WORLD_THIS_RVA+SAVE_NAME_FIELD_OFFSET holds the real current save name
// ("save/<name>") at that point.
#define WORLD_THIS_RVA              0x9D4F10  // == build_sync.cpp's UNIVERSAL_THIS_RVA; duplicated here on purpose (project convention: each module owns its copy)
#define SAVE_NAME_FIELD_OFFSET      0x9F8     // char[] inside the object - full save path, e.g. "save/<name>" (forward slash, unlike SAVE_NAME_DAT_RVA's backslash)
#define SAVE_TRIGGER_FLAG_OFFSET    0x14D35   // byte - set 1 to request a save; the per-frame dispatcher (FUN_14030d1a0) runs FUN_14042cc70 and clears this back to 0 once done
#define SAVE_ZIP_AFTER_FLAG_OFFSET  0x14D36   // byte - "also zip to 7z after saving" (the AUTOSAVE_TO_ZIP path); we force this to 0, save_zip.h already does our own zipping
#define SAVE_DELAY_COUNTER_OFFSET   0x14D38   // float - frames-to-wait before the dispatcher actually fires; forced to 0 so it fires on the very next frame instead of whatever it last held
#define SAVE_NAME_FIELD_MAX_LEN     120       // matches the buffer the engine itself copies into (strcpy_s ...,0x80,...) with margin
#define SAVE_NAME_FIELD_PREFIX      "save/"   // observed live in memory ("save/11000 - Testingsaves") - forward slash, no leading path

void SaveLoad_Bind(const TsmHost* host);

void TriggerLoadSave(const char* saveName); // writes name + flag; game loads it on its next frame

// Symmetric counterpart to TriggerLoadSave() above - triggers the game's own
// "save game" action under the given name (writes it into the world object's
// name field, then flips its trigger flag - see WORLD_THIS_RVA and friends
// above). The game's own per-frame dispatcher does the actual file I/O on
// the main thread on its next tick; this function only ever pokes memory, it
// never calls engine code directly (safe to call from any thread). Always
// returns true - the write itself can't meaningfully fail; use
// WaitForSaveToFinish() to know when the save has actually completed.
bool TriggerSaveGame(const char* saveName);

// Mirrors WaitForLoadToFinish() - polls SAVE_TRIGGER_FLAG_OFFSET until the
// game's own dispatcher clears it back to 0 (meaning FUN_14042cc70 finished
// writing every subsystem file) or timeoutMs runs out. timeoutMs == 0 means
// wait indefinitely, same reasoning as the Load side: the flag always gets
// cleared eventually once the game's per-frame update processes it.
//
// cancelFlag: optional, checked once per 100ms poll step alongside the
// trigger byte - used by the Disconnect button. If *cancelFlag becomes true,
// returns false early instead of waiting out the full save. This can only
// ever observe the flag BETWEEN polls, same as every other cancel point in
// save_sync.cpp - it can't interrupt a single already-blocking OS call, but
// there isn't one here (this function only ever Sleep()s and re-checks), so
// in practice it reacts within ~100ms. nullptr keeps an unconditional wait
// for a caller that has no cancel path.
bool WaitForSaveToFinish(DWORD timeoutMs, volatile bool* cancelFlag = nullptr);

// Polls the trigger flag until the game's own per-frame dispatcher clears it
// (meaning FUN_140430fc0 already ran and read the save off disk) or timeoutMs
// runs out. Call after TriggerLoadSave() to know when it's safe to tell the
// other side "I'm loaded". timeoutMs == 0 means wait indefinitely - the flag
// always gets cleared eventually once the game processes the load, so there's
// no need to give up early and risk telling the host "ready" before we are.
//
// cancelFlag: see WaitForSaveToFinish() above - same deal, same ~100ms
// reaction time, same nullptr-default backward compatibility.
bool WaitForLoadToFinish(DWORD timeoutMs, volatile bool* cancelFlag = nullptr);
