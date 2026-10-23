// SPDX-License-Identifier: GPL-3.0-only
// Steam lobbies as the list of games (#38). A host advertises its session in a
// Steam lobby - friends only or public - that carries what the list shows;
// players never join the lobby, they connect to the host's SteamID64 over the
// Steam transport (steam_net.h). A browser lists the public lobbies of this
// protocol and the lobbies its Steam friends are in. Render thread only.
#pragma once

#include "tesmio_plugin.h"

struct LobbyGame {
    unsigned long long host; // the SteamID64 to connect to
    char name[64];
    char mode[24];
    int players, maxPlayers;
    int pingMs;              // estimated through Valve's relay; -1 unknown
    bool friendGame;         // hosted by a Steam friend
};

void SteamLobby_Bind(const TsmHost* host);

// Hosting: call every frame while the session should be listed (the lobby is
// made, kept current and switched between friends only and public); Stop
// leaves it. name may be empty (the Steam name is used).
void SteamLobby_Advertise(bool publicGame, const char* name, const char* mode, int players, int maxPlayers);
void SteamLobby_StopAdvertising();

// Browsing: Refresh asks Steam again; the list fills in over a few seconds.
void SteamLobby_Refresh();
bool SteamLobby_Refreshing();
int SteamLobby_Games(LobbyGame* out, int max);

// Every frame: polls Steam's answers.
void SteamLobby_Tick();
