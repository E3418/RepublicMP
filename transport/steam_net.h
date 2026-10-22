// SPDX-License-Identifier: GPL-3.0-only
// Steam backend of the transport (network.h): P2P links through Valve's relay,
// so players connect by SteamID64 with no open port. Reached through the
// game's own steam_api64.dll (Feb 2019 SDK), which hands the modern
// SteamNetworkingSockets012 / Utils004 interfaces over from the Steam client
// (docs/findings.md, "Steam"). Only network.cpp calls these.
//
// A Steam link is the connection handle tagged with bit 62, which a SOCKET
// never has. Links behave like the TCP ones: blocking exact reads and writes
// over reliable messages, the same timeouts, Shutdown / Close. Every send goes
// out without Nagle delay; the send rate of each connection adapts to the link
// (only a backlogged transfer such as the world raises it).
#pragma once

#include "network.h"

// Registers the Steam "Join Game" callback; from the plugin's Bind, before the
// game's main thread pumps Steam callbacks.
void SteamNet_Bind(const TsmHost* host);
// Gets the interfaces and starts the relay and the service thread, once.
// False without Steam: links are then TCP only.
bool SteamNet_Init();
// Our SteamID64, 0 without Steam.
unsigned long long SteamNet_LocalId();
// "76561198..." or "steam:76561198...": a SteamID64 to connect to.
bool SteamNet_ParseId(const char* text, unsigned long long* id);
bool SteamNet_IsLink(NetLink link);

// Steam listening beside a TCP listener, on the same port number; friends see
// "Join Game" while any listener is open.
void SteamNet_Listen(NetLink tcpListener, unsigned short port);
void SteamNet_StopListen(NetLink tcpListener);
// A Steam player connected to that listener, or NET_NO_LINK; never waits.
NetLink SteamNet_Accept(NetLink tcpListener, DWORD ioTimeoutMs);
NetLink SteamNet_Connect(unsigned long long id, unsigned short port, DWORD connectTimeoutMs,
                         DWORD ioTimeoutMs, volatile bool* cancelFlag);

void SteamNet_SetTimeouts(NetLink link, DWORD timeoutMs);
void SteamNet_PeerAddress(NetLink link, char* out, size_t outSize);
bool SteamNet_Send(NetLink link, const void* data, size_t size);
bool SteamNet_Recv(NetLink link, void* data, size_t size);
bool SteamNet_PeerGone(NetLink link);
bool SteamNet_WaitReadable(NetLink link, DWORD waitMs, volatile bool* cancelFlag);
// Both tolerate a link that is already closed.
void SteamNet_Shutdown(NetLink link);
void SteamNet_Close(NetLink link);

// The host a friend's "Join Game" or an accepted invite asks for ("steam:<id>"),
// once; false when there is none.
bool SteamNet_TakeJoinRequest(char* out, size_t outSize);
