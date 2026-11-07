// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP's own data in a save: republicmp.ini in the save's folder, next
// to the game's files, so it travels with the save (a join, a re-sync and a
// copied folder carry it).
//
// Written after every save the game makes (the save routine 42cc70 writes
// into "media_soviet/" + world +9f8, "save/<name>": the save window, an
// autosave, a shared save, the host's re-sync save). Read right after a world
// load (determinism/determinism's load hook), from the folder the load
// globals name (exe+9962b0, "save\<name>"). A save without the file loads as
// before: every section reads as missing.
//
// The file is plain INI text, one [section] per module that registered one
// (ModSave_Add): a module writes its keys in its write callback and reads
// them back in its read callback. A key and its value are one line; the value
// is everything after the first '='.
//
//   [republicmp]  version, protocol - the build that wrote it;
//                 world_seed - the world's own seed (ModSave_WorldSeed)
//   [camera]      each player's view (save/camera_keep)
//   [wallets]     each republic's money (lockstep/wallets)
//   [republics]   where the cut runs (modes/republics)
//   [citizen_history]  each republic's citizens per history record (lockstep/citizen_stats)
//   [economy]     each republic's share of the economy lists (lockstep/economy_stats)
//   a mode adds its own sections (e.g. Republics' parameters).
#pragma once

#include "host.h"

#define MOD_SAVE_FILE "republicmp.ini"

// write: called while the file is written, inside its section; it calls
// ModSave_Put*. read: called after every load with found = whether the file
// had the section; it calls ModSave_Get* / ModSave_ForEach. Either may be
// null. Main thread. At most 16 sections, added once from a Bind.
typedef void (*ModSaveWriteFn)();
typedef void (*ModSaveReadFn)(bool found);
void ModSave_Add(const char* section, ModSaveWriteFn write, ModSaveReadFn read);

void ModSave_Bind(const ModHost* host);
bool ModSave_Install();

// Inside a write callback.
void ModSave_Put(const char* key, const char* value);
void ModSave_PutInt(const char* key, long long value);
void ModSave_PutFloats(const char* key, const float* values, int count);

// Inside a read callback: the section's keys.
bool ModSave_Get(const char* key, char* out, size_t outSize);
bool ModSave_GetInt(const char* key, long long* out);
bool ModSave_GetFloats(const char* key, float* out, int count); // all `count` present and finite
void ModSave_ForEach(void (*fn)(const char* key, const char* value, void* ctx), void* ctx);

// Determinism's load hook: before the game's load, the save it is going to
// read (none for a built-in terrain); right after it, that save's file is
// read and every read callback called.
void ModSave_BeforeLoad();
void ModSave_AfterLoad();
// Between the two: the world seed in the save being loaded, 0 when it has
// none (determinism: the load's own draws already start from it).
unsigned long long ModSave_LoadSeed();
// The folder of the save loaded last, for its other files; false for none.
bool ModSave_LoadedFolder(char* out, size_t outSize);

// The world's own seed, made once and kept in its saves: the same number on
// every PC of a session (a join and a re-sync carry the host's save, file
// included), and again whenever the world is loaded. A world loaded without
// one (a new game, a save from before 0.7.6) gets a new one at its load, and
// its saves keep it from then on. For what a mode makes at random once per
// world (e.g. where the republics' border runs); never 0 after the first load.
// Not the simulation's seed (determinism/): nothing in the game draws from it.
unsigned long long ModSave_WorldSeed();
