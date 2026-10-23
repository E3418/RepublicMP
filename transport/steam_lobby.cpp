// SPDX-License-Identifier: GPL-3.0-only
// See steam_lobby.h. Calls the flat exports of the game's steam_api64.dll on
// the interface versions they were built for (their vtable offsets match
// SteamMatchMaking009, SteamFriends017, SteamUtils009). Confirmed by
// disassembly: the exports pass every argument straight through, a CSteamID
// argument travels as a uint64, and the ones returning a CSteamID return a
// uint64 in RAX.
#include "steam_lobby.h"
#include "steam_net.h"
#include "../core/version.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define LOBBY_MATCHMAKING_VERSION "SteamMatchMaking009"
#define LOBBY_FRIENDS_VERSION "SteamFriends017"
#define LOBBY_UTILS_VERSION "SteamUtils009"
#define LOBBY_CREATED_CALLBACK 513    // LobbyCreated_t (k_iSteamMatchmakingCallbacks + 13)
#define LOBBY_MATCH_LIST_CALLBACK 510 // LobbyMatchList_t (k_iSteamMatchmakingCallbacks + 10)
#define LOBBY_TYPE_FRIENDS_ONLY 1
#define LOBBY_TYPE_PUBLIC 2
#define LOBBY_COMPARE_EQUAL 0
#define LOBBY_DISTANCE_WORLDWIDE 3
#define FRIEND_FLAG_IMMEDIATE 0x04
#define GAME_APP_ID 784150
#define LOBBY_KEY "republicmp" // our lobbies carry the protocol under it
#define LOBBY_WAIT_MS 5000     // friends' lobby data comes in within this

typedef unsigned long long u64;
struct FriendGameInfo { // FriendGameInfo_t
    u64 gameId;         // CGameID: the app id in the low 24 bits
    unsigned ip;
    unsigned short gamePort, queryPort;
    u64 lobby;
};
struct LobbyCreated { // LobbyCreated_t
    int result;       // EResult, 1 = OK
    u64 lobby;
};
struct LobbyMatchList { // LobbyMatchList_t
    unsigned count;
};

static struct {
    u64 (*CreateLobby)(void*, int type, int maxMembers);
    void (*LeaveLobby)(void*, u64 lobby);
    bool (*SetLobbyData)(void*, u64 lobby, const char* key, const char* value);
    const char* (*GetLobbyData)(void*, u64 lobby, const char* key);
    bool (*SetLobbyType)(void*, u64 lobby, int type);
    u64 (*GetLobbyOwner)(void*, u64 lobby);
    u64 (*RequestLobbyList)(void*);
    void (*AddStringFilter)(void*, const char* key, const char* value, int comparison);
    void (*AddDistanceFilter)(void*, int distance);
    void (*AddResultCountFilter)(void*, int count);
    u64 (*GetLobbyByIndex)(void*, int index);
    bool (*RequestLobbyData)(void*, u64 lobby);
    const char* (*GetPersonaName)(void*);
    int (*GetFriendCount)(void*, int flags);
    u64 (*GetFriendByIndex)(void*, int index, int flags);
    bool (*GetFriendGamePlayed)(void*, u64 id, FriendGameInfo* out);
    bool (*IsApiCallCompleted)(void*, u64 call, bool* failed);
    bool (*GetApiCallResult)(void*, u64 call, void* out, int size, int callbackId, bool* failed);
} p;
static void* g_mm;
static void* g_friends;
static void* g_utils;

// Hosting: what should be listed, and the lobby that lists it.
static bool g_wanted = false;
static bool g_public = false;
static std::map<std::string, std::string> g_data;   // what the lobby should carry
static std::map<std::string, std::string> g_sent;   // what it carries
static u64 g_createCall = 0;
static u64 g_lobby = 0;
static int g_lobbyType = 0;
static ULONGLONG g_retryAt = 0;
static ULONGLONG g_nextUpdate = 0; // the lobby's type and data, once a second
static ULONGLONG g_nextCheck = 0;  // that the lobby still exists, every 30 s

// Browsing.
struct Candidate {
    u64 lobby;
    bool friendGame;
    bool done;
};
static u64 g_listCall = 0;
static ULONGLONG g_browseUntil = 0;
static std::vector<Candidate> g_candidates;
static std::vector<LobbyGame> g_games;

static ULONGLONG Now() { return GetTickCount64(); }

void SteamLobby_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static bool InitOnce()
{
    g_mm = SteamNet_Interface(LOBBY_MATCHMAKING_VERSION);
    g_friends = SteamNet_Interface(LOBBY_FRIENDS_VERSION);
    g_utils = SteamNet_Interface(LOBBY_UTILS_VERSION);
#define EXPORT(field, name) (*(void**)&p.field = SteamNet_Export(name)) != nullptr
    bool ok = g_mm && g_friends && g_utils && EXPORT(CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby") &&
              EXPORT(LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby") &&
              EXPORT(SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData") &&
              EXPORT(GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData") &&
              EXPORT(SetLobbyType, "SteamAPI_ISteamMatchmaking_SetLobbyType") &&
              EXPORT(GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner") &&
              EXPORT(RequestLobbyList, "SteamAPI_ISteamMatchmaking_RequestLobbyList") &&
              EXPORT(AddStringFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter") &&
              EXPORT(AddDistanceFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter") &&
              EXPORT(AddResultCountFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter") &&
              EXPORT(GetLobbyByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex") &&
              EXPORT(RequestLobbyData, "SteamAPI_ISteamMatchmaking_RequestLobbyData") &&
              EXPORT(GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName") &&
              EXPORT(GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount") &&
              EXPORT(GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex") &&
              EXPORT(GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed") &&
              EXPORT(IsApiCallCompleted, "SteamAPI_ISteamUtils_IsAPICallCompleted") &&
              EXPORT(GetApiCallResult, "SteamAPI_ISteamUtils_GetAPICallResult");
#undef EXPORT
    Logf("republicmp  LOBBY: Steam lobbies %s", ok ? "ready" : "unavailable - no game list");
    return ok;
}

static bool Ready()
{
    static int state = -1; // render thread only
    if (state < 0) state = InitOnce() ? 1 : 0;
    return state == 1;
}

// The result of a Steam call once it is in: 1 done, 0 not yet, -1 failed.
static int CallResult(u64 call, void* out, int size, int callbackId)
{
    bool failed = false;
    if (!p.IsApiCallCompleted(g_utils, call, &failed)) return 0;
    if (failed || !p.GetApiCallResult(g_utils, call, out, size, callbackId, &failed) || failed) return -1;
    return 1;
}

static std::string Protocol()
{
    char text[16];
    snprintf(text, sizeof(text), "%d", REPUBLICMP_PROTOCOL);
    return text;
}

void SteamLobby_Advertise(bool publicGame, const char* name, const char* mode, int players, int maxPlayers)
{
    if (!Ready()) return;
    g_wanted = true;
    g_public = publicGame;
    char num[32];
    g_data[LOBBY_KEY] = Protocol();
    g_data["version"] = REPUBLICMP_VERSION;
    g_data["name"] = name && name[0] ? name : p.GetPersonaName(g_friends);
    g_data["mode"] = mode ? mode : "";
    snprintf(num, sizeof(num), "%d", players);
    g_data["players"] = num;
    snprintf(num, sizeof(num), "%d", maxPlayers);
    g_data["max"] = num;
    snprintf(num, sizeof(num), "%llu", SteamNet_LocalId());
    g_data["host"] = num;
}

void SteamLobby_StopAdvertising()
{
    g_wanted = false;
}

static void TickHosting()
{
    if (g_createCall) {
        LobbyCreated r = {};
        int done = CallResult(g_createCall, &r, sizeof(r), LOBBY_CREATED_CALLBACK);
        if (!done) return;
        g_createCall = 0;
        if (done < 0 || r.result != 1 || !r.lobby) {
            Logf("republicmp  LOBBY: making the lobby failed (result %d) - retrying in 10 s", r.result);
            g_retryAt = Now() + 10000;
            return;
        }
        g_lobby = r.lobby;
        g_sent.clear();
        g_nextUpdate = 0;
        g_nextCheck = Now() + 30000;
        Logf("republicmp  LOBBY: listed as %s", g_lobbyType == LOBBY_TYPE_PUBLIC ? "public" : "friends only");
    }
    if (!g_wanted) {
        if (g_lobby) {
            p.LeaveLobby(g_mm, g_lobby);
            g_lobby = 0;
            Logf("republicmp  LOBBY: no longer listed");
        }
        return;
    }
    int type = g_public ? LOBBY_TYPE_PUBLIC : LOBBY_TYPE_FRIENDS_ONLY;
    if (!g_lobby) {
        if (Now() < g_retryAt) return;
        int max = std::max(1, atoi(g_data["max"].c_str()));
        g_createCall = p.CreateLobby(g_mm, type, max);
        g_lobbyType = type;
        if (!g_createCall) g_retryAt = Now() + 10000; // Steam offline: no call was made
        return;
    }
    if (Now() < g_nextUpdate) return;
    g_nextUpdate = Now() + 1000;
    // After a reconnect to Steam the lobby may be gone on Valve's side while
    // we still hold its id: no longer its owner, make a new one.
    if (Now() >= g_nextCheck) {
        g_nextCheck = Now() + 30000;
        if (p.GetLobbyOwner(g_mm, g_lobby) != SteamNet_LocalId()) {
            Logf("republicmp  LOBBY: the lobby is gone - listing again");
            p.LeaveLobby(g_mm, g_lobby);
            g_lobby = 0;
            return;
        }
    }
    if (type != g_lobbyType && p.SetLobbyType(g_mm, g_lobby, type)) g_lobbyType = type;
    // The relay place changes rarely; it is measured a few seconds after start.
    char ping[1024];
    if (SteamNet_PingLocation(ping, sizeof(ping))) g_data["ping"] = ping;
    for (auto& kv : g_data) {
        auto sent = g_sent.find(kv.first);
        if (sent != g_sent.end() && sent->second == kv.second) continue;
        if (p.SetLobbyData(g_mm, g_lobby, kv.first.c_str(), kv.second.c_str())) g_sent[kv.first] = kv.second;
    }
}

void SteamLobby_Refresh()
{
    if (!Ready() || g_listCall) return;
    g_candidates.clear();
    g_games.clear();
    // Public games of this protocol...
    p.AddStringFilter(g_mm, LOBBY_KEY, Protocol().c_str(), LOBBY_COMPARE_EQUAL);
    p.AddDistanceFilter(g_mm, LOBBY_DISTANCE_WORLDWIDE);
    p.AddResultCountFilter(g_mm, 50);
    g_listCall = p.RequestLobbyList(g_mm);
    // ...and the lobbies friends are in (friends-only ones are never in the list).
    int n = p.GetFriendCount(g_friends, FRIEND_FLAG_IMMEDIATE);
    for (int i = 0; i < n; ++i) {
        u64 id = p.GetFriendByIndex(g_friends, i, FRIEND_FLAG_IMMEDIATE);
        FriendGameInfo info = {};
        if (!id || !p.GetFriendGamePlayed(g_friends, id, &info)) continue;
        if ((info.gameId & 0xFFFFFF) != GAME_APP_ID || !info.lobby) continue;
        p.RequestLobbyData(g_mm, info.lobby);
        g_candidates.push_back({ info.lobby, true, false });
    }
    g_browseUntil = Now() + LOBBY_WAIT_MS;
}

bool SteamLobby_Refreshing()
{
    return g_listCall || Now() < g_browseUntil;
}

static void TickBrowsing()
{
    if (g_listCall) {
        LobbyMatchList r = {};
        int done = CallResult(g_listCall, &r, sizeof(r), LOBBY_MATCH_LIST_CALLBACK);
        if (done) {
            g_listCall = 0;
            if (done < 0) Logf("republicmp  LOBBY: the game list request failed");
            for (unsigned i = 0; done > 0 && i < r.count; ++i)
                if (u64 lobby = p.GetLobbyByIndex(g_mm, (int)i)) g_candidates.push_back({ lobby, false, false });
            g_browseUntil = std::max(g_browseUntil, Now() + 1000); // a late answer is still read
        }
    }
    if (Now() >= g_browseUntil && !g_listCall) return;
    std::string protocol = Protocol();
    unsigned long long me = SteamNet_LocalId();
    for (Candidate& c : g_candidates) {
        if (c.done) continue;
        const char* mark = p.GetLobbyData(g_mm, c.lobby, LOBBY_KEY);
        if (!mark || !mark[0]) continue; // its data has not come in yet
        c.done = true;
        if (protocol != mark) continue; // another build: it could not be joined
        LobbyGame g = {};
        g.host = _strtoui64(p.GetLobbyData(g_mm, c.lobby, "host"), nullptr, 10);
        if (!g.host || g.host == me) continue;
        bool dup = false;
        for (LobbyGame& e : g_games) dup |= e.host == g.host;
        if (dup) continue;
        strncpy_s(g.name, p.GetLobbyData(g_mm, c.lobby, "name"), _TRUNCATE);
        strncpy_s(g.mode, p.GetLobbyData(g_mm, c.lobby, "mode"), _TRUNCATE);
        g.players = atoi(p.GetLobbyData(g_mm, c.lobby, "players"));
        g.maxPlayers = atoi(p.GetLobbyData(g_mm, c.lobby, "max"));
        g.pingMs = SteamNet_EstimatePing(p.GetLobbyData(g_mm, c.lobby, "ping"));
        g.friendGame = c.friendGame;
        g_games.push_back(g);
    }
}

int SteamLobby_Games(LobbyGame* out, int max)
{
    int n = std::min(max, (int)g_games.size());
    for (int i = 0; i < n; ++i) out[i] = g_games[i];
    return n;
}

void SteamLobby_Tick()
{
    if (!g_wanted && !g_lobby && !g_createCall && !g_listCall && Now() >= g_browseUntil) return;
    if (!Ready()) return;
    TickHosting();
    TickBrowsing();
}
