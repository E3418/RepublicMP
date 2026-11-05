// SPDX-License-Identifier: GPL-3.0-only
// What every module of the mod works with: the log, the patching primitives
// and where the game is in memory. Implemented in host.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

#define DLL_ENGINE "C3DDLL64.dll"
#define DLL_STDIO  "api-ms-win-crt-stdio-l1-1-0.dll"

struct ModHost
{
    HMODULE exe;      // SOVIET64.exe
    BYTE*   exeBase;  // add an RVA to this; ASLR is on
    SIZE_T  exeSize;
    HMODULE engine;   // C3DDLL64.dll
};

// Every .cpp that includes this has its own copy, filled by its module's Bind.
static HMODULE          g_exe;
static BYTE*            g_exeBase;
static SIZE_T           g_exeSize;
static HMODULE          g_engine;
static CRITICAL_SECTION g_lock;

static void ModHost_Bind(const ModHost* host)
{
    g_exe     = host->exe;
    g_exeBase = host->exeBase;
    g_exeSize = host->exeSize;
    g_engine  = host->engine;
    InitializeCriticalSection(&g_lock);
}

// Opens the log and the crash report and finds the game. The first call of the
// mod: nothing below works before it. Runs once, from RepublicMP_Start.
const ModHost* Host_Init();

// The folder RepublicMP.dll lives in, the game's folder; "" when unknown. The
// mod's own files go to its RepublicMP subfolder, which Host_Init makes.
const wchar_t* Host_GameDir();

// Writes to RepublicMP\logs\republicmp.log with a timestamp. Prefix the message
// with the module's short name. The game holds the file open, so reading it
// while the game runs needs FileShare.ReadWrite.
void Logf(const char* fmt, ...);

// Walks a module's import table. Returns the slot, or null.
void** FindIatSlot(HMODULE mod, const char* dll, const char* fn);

// Swaps one import and hands back what was there.
bool PatchIat(HMODULE mod, const char* dll, const char* fn,
              void* detour, void** original, const char* label);

// Moves `stolen` bytes of prologue into a trampoline and writes a 14 byte
// absolute jump over them. `expect` must match the site exactly or nothing is
// written and this returns false: a game update has to make a hook refuse, not
// corrupt the process. `stolen` must be at least 14, must end on an instruction
// boundary, and the stolen code must not depend on where it runs.
bool InstallInlineHook(void* target, void* detour, void** trampoline,
                       const BYTE* expect, size_t stolen, const char* label);

// Executable memory within +-2GB of `anchor`, for a code cave a `call rel32`
// can reach. Never freed.
BYTE* AllocNear(BYTE* anchor, SIZE_T size);

// True when the whole range is committed and readable.
bool ReadablePtr(const void* p, size_t n);

// Reads a NUL-terminated plain-ASCII name out of a pointer that came from a game
// structure. Refuses anything with a byte outside 32..126, which is what tells a
// real name from a stale pointer.
bool SafeReadStr(const void* p, char* out, size_t n);

// Strips spaces, tabs and line ends from both ends, in place.
void Trim(char* s);
