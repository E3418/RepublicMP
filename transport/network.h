// SPDX-License-Identifier: GPL-3.0-only
// Minimal blocking transport shared by the sync modules: accept clients,
// connect to a host, exchange fixed-size structs or whole files (8-byte
// little-endian size, then the bytes). Only ever called from worker threads
// or for small sends with a send timeout.
//
// Callers hold links, not sockets: a link is a connection or a listener the
// transport hands out. A link is TCP, or Steam P2P through Valve's relay
// (steam_net.h) when the host is given as a SteamID64; a listener takes both.
#pragma once

#include "tesmio_plugin.h"
#include <winsock2.h>
#include <ws2tcpip.h>

// A connection or a listener; NET_NO_LINK for none.
typedef UINT_PTR NetLink;
#define NET_NO_LINK ((NetLink)INVALID_SOCKET)

void Network_Bind(const TsmHost* host);
bool NetworkInit();

// A listening socket kept open across accepts (the lockstep host takes
// several players, and new ones while a session runs): NET_NO_LINK when the
// port could not be bound.
NetLink NetworkListen(unsigned short port, int backlog);
// The next waiting client within waitMs (0 = only one already waiting), or
// NET_NO_LINK. The connection gets ioTimeoutMs and TCP keepalive.
NetLink NetworkAcceptReady(NetLink listenSock, DWORD waitMs, DWORD ioTimeoutMs);
// The send/recv timeout of a connection (0 = none).
void NetworkSetTimeouts(NetLink sock, DWORD timeoutMs);
// The remote address of a connection, "" when unknown.
void NetworkPeerAddress(NetLink sock, char* out, size_t outSize);

// Connects to hostIp:port, or over Steam when hostIp is a SteamID64 ("7656..."
// or "steam:7656..."; port picks the Steam virtual port). connectTimeoutMs
// bounds the wait, ioTimeoutMs is the send/recv timeout of the connection
// (0 = none); the wait returns NET_NO_LINK early when *cancelFlag becomes true.
NetLink NetworkConnect(const char* hostIp, unsigned short port,
                       DWORD connectTimeoutMs = 15000,
                       DWORD ioTimeoutMs = 30000,
                       volatile bool* cancelFlag = nullptr);

typedef void (*NetworkProgressFn)(unsigned long long done, unsigned long long total, void* userData);

bool NetworkSendFile(NetLink sock, const char* filePath,
                     NetworkProgressFn onProgress = nullptr, void* userData = nullptr);
bool NetworkReceiveFile(NetLink sock, const char* outFilePath,
                        NetworkProgressFn onProgress = nullptr, void* userData = nullptr);

// Sends/receives exactly `size` bytes; false on error or timeout.
bool NetworkSendExact(NetLink sock, const void* data, size_t size);
bool NetworkRecvExact(NetLink sock, void* data, size_t size);

// Non-blocking: true when the connection has been closed or has failed
// (including a keepalive timeout). Pending data counts as alive and is left
// unread.
bool NetworkPeerGone(NetLink sock);
// Waits up to waitMs for something to read (data, or the connection closing),
// checking the cancel flag in short steps; false: nothing came, or cancelled.
bool NetworkWaitReadable(NetLink sock, DWORD waitMs, volatile bool* cancelFlag);

// Ends the connection both ways without freeing it, so a thread blocked in a
// receive on it returns; NetworkClose frees it afterwards.
void NetworkShutdown(NetLink sock);
void NetworkClose(NetLink sock);

// Our SteamID64, for players to connect to; 0 without Steam.
unsigned long long NetworkSteamId();
// This PC's IPv4 addresses (loopback left out), for players on the LAN or a
// VPN to connect to; how many were written. Any thread.
int NetworkLocalAddresses(char (*out)[16], int max);
// A friend's Steam "Join Game" or accepted invite while we are idle: the host
// to connect to ("steam:<id>"), once; false when there is none. Friends see
// Join Game while we listen.
bool NetworkTakeJoinRequest(char* host, size_t hostSize);
