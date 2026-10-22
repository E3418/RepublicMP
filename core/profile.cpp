// SPDX-License-Identifier: GPL-3.0-only
// See profile.h.
#include "profile.h"
#include <stdio.h>
#include <string.h>

static char g_nick[PROFILE_NICK_BYTES] = "";
static volatile int g_maxPlayers = PROFILE_MAX_PLAYERS;
static CRITICAL_SECTION g_profileLock;
static char g_file[MAX_PATH] = ""; // RepublicMP.profile.ini next to the DLL ("" when unknown)

static void Clean(const char* in, char* out, size_t outSize);

static void FindFile()
{
    HMODULE self = nullptr;
    char path[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&FindFile, &self) ||
        !GetModuleFileNameA(self, path, MAX_PATH))
        return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    slash[1] = 0;
    _snprintf_s(g_file, sizeof(g_file), _TRUNCATE, "%sRepublicMP.profile.ini", path);
}

static void Save()
{
    if (!g_file[0]) return;
    char players[8];
    _snprintf_s(players, sizeof(players), _TRUNCATE, "%d", (int)g_maxPlayers);
    EnterCriticalSection(&g_profileLock);
    WritePrivateProfileStringA("profile", "nick", g_nick, g_file);
    LeaveCriticalSection(&g_profileLock);
    WritePrivateProfileStringA("profile", "max_players", players, g_file);
}

void Profile_Bind(const TsmHost* host)
{
    TsmBind(host);
    InitializeCriticalSection(&g_profileLock);
    FindFile();
    if (!g_file[0]) return;
    char nick[PROFILE_NICK_BYTES] = {};
    GetPrivateProfileStringA("profile", "nick", "", nick, sizeof(nick), g_file);
    Clean(nick, g_nick, sizeof(g_nick));
    int players = (int)GetPrivateProfileIntA("profile", "max_players", PROFILE_MAX_PLAYERS, g_file);
    g_maxPlayers = players < PROFILE_MIN_PLAYERS ? PROFILE_MIN_PLAYERS : players > PROFILE_MAX_PLAYERS ? PROFILE_MAX_PLAYERS : players;
}

// Printable ASCII only, trimmed: the name travels in fixed fields and the log.
static void Clean(const char* in, char* out, size_t outSize)
{
    size_t n = 0;
    for (const char* c = in ? in : ""; *c && n + 1 < outSize; ++c)
        if (*c >= 0x20 && *c < 0x7F) out[n++] = *c;
    while (n && out[n - 1] == ' ') --n;
    out[n] = 0;
    size_t lead = 0;
    while (out[lead] == ' ') ++lead;
    if (lead) memmove(out, out + lead, n - lead + 1);
}

const char* Profile_GetNick()
{
    return g_nick[0] ? g_nick : "Player";
}

void Profile_SetNick(const char* nick)
{
    char clean[PROFILE_NICK_BYTES];
    Clean(nick, clean, sizeof(clean));
    EnterCriticalSection(&g_profileLock);
    bool changed = strcmp(g_nick, clean) != 0;
    strcpy_s(g_nick, clean);
    LeaveCriticalSection(&g_profileLock);
    if (changed) Save();
}

int Profile_GetMaxPlayers()
{
    return g_maxPlayers;
}

void Profile_SetMaxPlayers(int players)
{
    int clamped = players < PROFILE_MIN_PLAYERS ? PROFILE_MIN_PLAYERS : players > PROFILE_MAX_PLAYERS ? PROFILE_MAX_PLAYERS : players;
    if (clamped == g_maxPlayers) return;
    g_maxPlayers = clamped;
    Save();
}

int Profile_GetInt(const char* section, const char* key, int def)
{
    if (!g_file[0]) FindFile();
    if (!g_file[0]) return def;
    return (int)GetPrivateProfileIntA(section, key, def, g_file);
}

void Profile_SetInt(const char* section, const char* key, int value)
{
    if (!g_file[0]) FindFile();
    if (!g_file[0] || Profile_GetInt(section, key, value + 1) == value) return;
    char text[16];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%d", value);
    WritePrivateProfileStringA(section, key, text, g_file);
}
