// SPDX-License-Identifier: GPL-3.0-only
// Minimal blocking TCP transport shared by the sync modules: accept one
// client, connect to a host, exchange fixed-size structs or whole files
// (8-byte little-endian size, then the bytes). Only ever called from worker
// threads or for small sends with a send timeout.
#pragma once

#include "tesmio_plugin.h"
#include <winsock2.h>
#include <ws2tcpip.h>

void Network_Bind(const TsmHost* host);
bool NetworkInit();

// Listens on port and accepts one client. acceptTimeoutMs bounds the wait
// (0 = no limit); ioTimeoutMs is the send/recv timeout of the connection.
// The wait is polled in ~200ms steps and returns INVALID_SOCKET early when
// *cancelFlag becomes true.
SOCKET NetworkHostAcceptOne(unsigned short port,
                            DWORD acceptTimeoutMs = 60000,
                            DWORD ioTimeoutMs = 30000,
                            volatile bool* cancelFlag = nullptr);

// A listening socket kept open across accepts (the lockstep host takes
// several players, and new ones while a session runs): INVALID_SOCKET when the
// port could not be bound.
SOCKET NetworkListen(unsigned short port, int backlog);
// The next waiting client within waitMs (0 = only one already waiting), or
// INVALID_SOCKET. The connection gets ioTimeoutMs and TCP keepalive.
SOCKET NetworkAcceptReady(SOCKET listenSock, DWORD waitMs, DWORD ioTimeoutMs);
// The send/recv timeout of a connection (0 = none).
void NetworkSetTimeouts(SOCKET sock, DWORD timeoutMs);
// The remote address of a connection, "" when unknown.
void NetworkPeerAddress(SOCKET sock, char* out, size_t outSize);

// Connects to hostIp:port, same timeout and cancel semantics.
SOCKET NetworkConnect(const char* hostIp, unsigned short port,
                      DWORD connectTimeoutMs = 15000,
                      DWORD ioTimeoutMs = 30000,
                      volatile bool* cancelFlag = nullptr);

typedef void (*NetworkProgressFn)(unsigned long long done, unsigned long long total, void* userData);

bool NetworkSendFile(SOCKET sock, const char* filePath,
                     NetworkProgressFn onProgress = nullptr, void* userData = nullptr);
bool NetworkReceiveFile(SOCKET sock, const char* outFilePath,
                        NetworkProgressFn onProgress = nullptr, void* userData = nullptr);

// Sends/receives exactly `size` bytes; false on error or timeout.
bool NetworkSendExact(SOCKET sock, const void* data, size_t size);
bool NetworkRecvExact(SOCKET sock, void* data, size_t size);

// Non-blocking: true when the connection has been closed or has failed
// (including a keepalive timeout). Pending data counts as alive and is left
// unread.
bool NetworkPeerGone(SOCKET sock);
// Waits up to waitMs for something to read (data, or the connection closing),
// checking the cancel flag in short steps; false: nothing came, or cancelled.
bool NetworkWaitReadable(SOCKET sock, DWORD waitMs, volatile bool* cancelFlag);

void NetworkCloseSocket(SOCKET sock);
