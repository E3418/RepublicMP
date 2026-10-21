// SPDX-License-Identifier: GPL-3.0-only
// See profile.h.
#include "profile.h"
#include <string.h>

static char g_nick[PROFILE_NICK_BYTES] = "";
static volatile int g_maxPlayers = PROFILE_MAX_PLAYERS;
static CRITICAL_SECTION g_profileLock;

void Profile_Bind(const TsmHost* host)
{
    TsmBind(host);
    InitializeCriticalSection(&g_profileLock);
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
    strcpy_s(g_nick, clean);
    LeaveCriticalSection(&g_profileLock);
}

int Profile_GetMaxPlayers()
{
    return g_maxPlayers;
}

void Profile_SetMaxPlayers(int players)
{
    g_maxPlayers = players < PROFILE_MIN_PLAYERS ? PROFILE_MIN_PLAYERS : players > PROFILE_MAX_PLAYERS ? PROFILE_MAX_PLAYERS : players;
}
