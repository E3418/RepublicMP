// SPDX-License-Identifier: GPL-3.0-only
// See new_city.h.
#include "new_city.h"
#include "commands.h"
#include <string.h>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define CITY_LIST 0x10348
#define MAX_CITIES 4096
#define MAKE_CITY 0x32F450     // (world, float position[3], name): a new city record
#define ASSIGN_CITY 0x32FF40   // (world, float position[3]): the buildings around take the new city
#define MAX_CHARS 0x40
#define MAGIC 0x31595443

#pragma pack(push, 1)
struct NewCityMsg {
    unsigned magic;
    float position[3];
    unsigned chars;
    unsigned short name[MAX_CHARS];
};
#pragma pack(pop)

static bool g_assignTaken = false; // the tool's 32ff40 after a captured 32f450

void NewCity_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }

static bool Read(U64 position, U64 name, NewCityMsg& m)
{
    __try {
        memcpy(m.position, (const void*)position, sizeof(m.position));
        const unsigned short* t = (const unsigned short*)name;
        unsigned n = 0;
        while (n < MAX_CHARS && t[n]) n++;
        if (n >= MAX_CHARS) return false;
        memcpy(m.name, t, n * 2);
        m.chars = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool NewCity_Capture(U64 position, U64 name)
{
    NewCityMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    if (!position || !name || !Read(position, name, m)) {
        Logf("republicmp  NEW CITY: not shared - the position or the name could not be read; nothing is done");
        g_assignTaken = true;
        return true;
    }
    Commands_Submit(LS_CMD_NEW_CITY, &m, sizeof(m));
    g_assignTaken = true;
    return true;
}

bool NewCity_TakeAssign()
{
    bool taken = g_assignTaken;
    g_assignTaken = false;
    return taken;
}

bool NewCity_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(NewCityMsg)) return false;
    NewCityMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.chars >= MAX_CHARS) return false;
    unsigned short name[MAX_CHARS + 1] = {};
    memcpy(name, m.name, m.chars * 2);
    U64 world = (U64)World();
    unsigned cities = 0;
    const char* why = nullptr;
    __try {
        // 32f450 may lift the position it is given (y below 0); the tool gives 32ff40 its own copy.
        float made[3], assigned[3];
        memcpy(made, m.position, sizeof(made));
        memcpy(assigned, m.position, sizeof(assigned));
        typedef void (*MakeFn)(U64, float*, unsigned short*);
        typedef void (*AssignFn)(U64, float*);
        ((MakeFn)(g_exeBase + MAKE_CITY))(world, made, name);
        ((AssignFn)(g_exeBase + ASSIGN_CITY))(world, assigned);
        U64 b = *(U64*)(world + CITY_LIST), e = *(U64*)(world + CITY_LIST + 8);
        if (b && e >= b && (e - b) / 8 <= MAX_CITIES) cities = (unsigned)((e - b) / 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "the game's routine faulted";
    }
    Logf("republicmp  NEW CITY: at (%.1f, %.1f, %.1f), %u characters: %s (%u cities)", m.position[0], m.position[1],
         m.position[2], m.chars, why ? why : "made", cities);
    return why == nullptr;
}
