// SPDX-License-Identifier: GPL-3.0-only
// See shared_save.h.
#include "shared_save.h"
#include "commands.h"
#include "../determinism/determinism.h"
#include "../session/save_sync.h" // SaveSync_IsHostRole
#include <stdio.h>
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define SAVE_RVA 0x42CC70      // the save routine (world)
#define NAME_OFFSET 0x9F8      // char[]: "save/<name>"
#define NAME_BYTES 120
#define REQUEST_OFFSET 0x14D35 // byte: save requested (cleared with +14d36 as a word when done)
#define ZIP_OFFSET 0x14D36     // byte: pack the save afterwards
#define COUNT_OFFSET 0x14D38   // int: frames the body waits before saving
#define LEAVE_OFFSET 0x5030    // int: != 0, the save leaves the game
#define PACKED_OFFSET 0x504C   // byte: the zip choice the body keeps after a save
#define CONFIG_RVA 0x9BDFC0    // C3D_CONFIGURATOR: the game's options
#define AUTOSAVE_OFF 6         // AUTOSAVE_INTERVAL's last choice: never

// AUTOSAVE_INTERVAL's choices (314dd2..314e98), seconds of real time.
static const unsigned kAutosaveSeconds[AUTOSAVE_OFF] = { 60, 160, 300, 600, 1200, 3600 };
// AUTOSAVE_NUM's choices (314fa6..314fef): slots before the counter wraps.
static const int kAutosaveSlots[3] = { 5, 10, 20 };

#pragma pack(push, 1)
struct SaveMsg {
    char name[NAME_BYTES]; // empty for an autosave
    unsigned char zip;
    unsigned char autosave; // 1: every PC writes its own next autosave slot
    unsigned char pad[2];
};
#pragma pack(pop)

typedef void (*SaveFn)(unsigned char* world);
typedef int (*ConfigIntFn)(void* config, const char* key);
typedef int* (*ConfigIntPtrFn)(void* config, const char* key);
typedef int (*ConfigSaveFn)(void* config);
static ConfigIntFn g_configInt = nullptr;
static ConfigIntPtrFn g_configIntPtr = nullptr;
static ConfigSaveFn g_configSave = nullptr;
static ULONGLONG g_autosaveFrom = 0; // the host: when the interval started (0: not in a session)

// The save window makes the save's folder before it asks for the save; the
// routine only writes files into it, so a PC that did not click wrote none.
static bool MakeSaveFolder(const char* name)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = n ? strrchr(path, '\\') : nullptr;
    if (!slash) return false;
    *slash = 0;
    if (strcat_s(path, "\\media_soviet\\") || strcat_s(path, name)) return false;
    for (char* c = path; *c; ++c)
        if (*c == '/') *c = '\\';
    // Each level: "save" and the save's own folder.
    for (char* c = path + 3; *c; ++c) {
        if (*c != '\\') continue;
        *c = 0;
        CreateDirectoryA(path, nullptr);
        *c = '\\';
    }
    return CreateDirectoryA(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

void SharedSave_Bind(const ModHost* host)
{
    ModHost_Bind(host);
    g_configInt = (ConfigIntFn)GetProcAddress(g_engine, "?GetValueInteger@C3D_CONFIGURATOR@@QEAAHPEAD@Z");
    g_configIntPtr = (ConfigIntPtrFn)GetProcAddress(g_engine, "?GetValueIntegerPointer@C3D_CONFIGURATOR@@QEAAPEAHPEAD@Z");
    g_configSave = (ConfigSaveFn)GetProcAddress(g_engine, "?SaveConfigParams@C3D_CONFIGURATOR@@QEAAHXZ");
}

static void* Config() { return g_exeBase + CONFIG_RVA; }

// The host's interval in milliseconds, 0 when it set autosave off.
static ULONGLONG AutosaveIntervalMs()
{
    if (!g_configInt) return 0;
    int choice = g_configInt(Config(), "AUTOSAVE_INTERVAL");
    if (choice < 0 || choice >= AUTOSAVE_OFF) return 0;
    return kAutosaveSeconds[choice] * 1000ull;
}

// The host, every frame of a session: its interval passed - every PC
// autosaves at the next boundary.
static void AutosaveTimer()
{
    if (!SaveSync_IsHostRole()) {
        g_autosaveFrom = 0;
        return;
    }
    ULONGLONG now = GetTickCount64();
    ULONGLONG interval = AutosaveIntervalMs();
    if (!g_autosaveFrom || !interval) {
        g_autosaveFrom = now;
        return;
    }
    if (now - g_autosaveFrom < interval) return;
    g_autosaveFrom = now;
    SaveMsg m;
    memset(&m, 0, sizeof(m));
    m.autosave = 1;
    Logf("republicmp  SAVE: autosave - every PC writes its next autosave at the next boundary");
    Commands_Submit(LS_CMD_SAVE, &m, sizeof(m));
}

void SharedSave_BeforeLate(unsigned char* world)
{
    if (!Determinism_SessionRunning()) {
        g_autosaveFrom = 0;
        return;
    }
    AutosaveTimer();
    if (!world[REQUEST_OFFSET] || *(int*)(world + LEAVE_OFFSET)) return;
    // This PC's save window: the request becomes a command.
    SaveMsg m;
    memset(&m, 0, sizeof(m));
    memcpy(m.name, world + NAME_OFFSET, NAME_BYTES - 1);
    m.zip = world[ZIP_OFFSET];
    *(unsigned short*)(world + REQUEST_OFFSET) = 0;
    *(int*)(world + COUNT_OFFSET) = 0;
    if (!m.name[0]) {
        Logf("republicmp  SAVE: a save without a name - nothing is done");
        return;
    }
    Logf("republicmp  SAVE: [%s] requested - saved on every PC at the next boundary", m.name);
    Commands_Submit(LS_CMD_SAVE, &m, sizeof(m));
}

// This PC's next autosave slot, as the Late body writes it (314ec6..314ffe):
// the name, the save, the counter and its wrap, the options saved; the name
// the world had is put back. Its "autosaving" message is left out.
static bool Autosave()
{
    if (!g_configInt || !g_configIntPtr) return false;
    unsigned char* world = g_exeBase + WORLD_RVA;
    char name[NAME_BYTES], saved[NAME_BYTES];
    ULONGLONG started = GetTickCount64();
    __try {
        int* counter = g_configIntPtr(Config(), "AUTOSAVE");
        if (!counter) return false;
        _snprintf_s(name, sizeof(name), _TRUNCATE, "save/autosave%d", *counter + 1);
        // A zipped autosave is packed by the save routine itself (42ccc6).
        if (!g_configInt(Config(), "AUTOSAVE_TO_ZIP") && !MakeSaveFolder(name))
            Logf("republicmp  SAVE: [%s] - its folder could not be made", name);
        memcpy(saved, world + NAME_OFFSET, NAME_BYTES);
        memset(world + NAME_OFFSET, 0, NAME_BYTES);
        memcpy(world + NAME_OFFSET, name, strlen(name));
        *(int*)world = 0; // the autosave timer
        world[8] = 0;
        ((SaveFn)(g_exeBase + SAVE_RVA))(world);
        memcpy(world + NAME_OFFSET, saved, NAME_BYTES);
        ++*counter;
        int slots = g_configInt(Config(), "AUTOSAVE_NUM");
        if (*counter >= kAutosaveSlots[slots >= 0 && slots < 3 ? slots : 0]) *counter = 0;
        if (g_configSave) g_configSave(Config());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  SAVE: autosave faulted");
        return false;
    }
    Logf("republicmp  SAVE: [%s] autosaved after tick %llu (%llu ms)", name, Determinism_StepCount(),
         GetTickCount64() - started);
    return true;
}

// Every PC at the boundary (the render thread has finished: the frame waits
// for it at its start). The steps of the Late body's save (3151e9..31520f);
// its "saved" message is left out.
bool SharedSave_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(SaveMsg)) return false;
    SaveMsg m;
    memcpy(&m, data, sizeof(m));
    m.name[NAME_BYTES - 1] = 0;
    if (m.autosave) return Autosave();
    if (!m.name[0]) return false;
    unsigned char* world = g_exeBase + WORLD_RVA;
    if (!MakeSaveFolder(m.name)) Logf("republicmp  SAVE: [%s] - its folder could not be made", m.name);
    ULONGLONG started = GetTickCount64();
    __try {
        memset(world + NAME_OFFSET, 0, NAME_BYTES);
        memcpy(world + NAME_OFFSET, m.name, strlen(m.name));
        world[ZIP_OFFSET] = m.zip;
        *(int*)(world + COUNT_OFFSET) = 0;
        *(int*)world = 0;  // the autosave timer
        world[8] = 0;
        ((SaveFn)(g_exeBase + SAVE_RVA))(world);
        world[PACKED_OFFSET] = world[ZIP_OFFSET];
        *(unsigned short*)(world + REQUEST_OFFSET) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  SAVE: [%s] faulted while saving", m.name);
        return false;
    }
    Logf("republicmp  SAVE: [%s] saved after tick %llu (%llu ms)", m.name, Determinism_StepCount(),
         GetTickCount64() - started);
    return true;
}
