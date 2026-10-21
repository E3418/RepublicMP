// SPDX-License-Identifier: GPL-3.0-only
// See shared_save.h.
#include "shared_save.h"
#include "commands.h"
#include "../determinism/determinism.h"
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

#pragma pack(push, 1)
struct SaveMsg {
    char name[NAME_BYTES];
    unsigned char zip;
    unsigned char pad[3];
};
#pragma pack(pop)

typedef void (*SaveFn)(unsigned char* world);

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

void SharedSave_Bind(const TsmHost* host)
{
    TsmBind(host);
}

void SharedSave_BeforeLate(unsigned char* world)
{
    if (!Determinism_SessionRunning() || !world[REQUEST_OFFSET] || *(int*)(world + LEAVE_OFFSET)) return;
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

// Every PC at the boundary (the render thread has finished: the frame waits
// for it at its start). The steps of the Late body's save (3151e9..31520f);
// its "saved" message is left out.
bool SharedSave_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(SaveMsg)) return false;
    SaveMsg m;
    memcpy(&m, data, sizeof(m));
    m.name[NAME_BYTES - 1] = 0;
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
