// Minimal TCP transport with timeouts: host accepts one client, client
// connects, either side sends/receives a whole file. Protocol: 8-byte
// little-endian file size, then the file bytes. Blocking sockets - only
// called from a background thread, never the game thread.
#pragma once

#include "tesmio_plugin.h"
#include <winsock2.h>
#include <ws2tcpip.h>

void Network_Bind(const TsmHost* host);
bool NetworkInit();

// Listens on port (all interfaces). acceptTimeoutMs bounds the wait for a
// client (0 = wait indefinitely - used by save_sync's Host flow, which should
// sit idle until someone actually wants to join, not give up after some
// arbitrary time), ioTimeoutMs is the send/recv timeout on the resulting
// connection.
//
// cancelFlag: optional, used by the Disconnect/"End Hosting" button.
// Internally this polls in short (~200ms) steps rather than one single
// blocking select()/accept(), so *cancelFlag can be checked between them,
// same pattern as hooks/save_load.h's WaitForSaveToFinish. If *cancelFlag
// becomes true, returns INVALID_SOCKET early (reaction time ~200ms)
// instead of waiting for a client or for acceptTimeoutMs to elapse.
// nullptr keeps a "wait it out" behavior for a caller with no cancel path.
// This is what lets save_sync's host flow sit here (potentially
// indefinitely, nobody connected yet) WITHOUT the game ever being paused,
// while still reacting to Disconnect immediately instead of only once a
// client shows up.
SOCKET NetworkHostAcceptOne(unsigned short port,
                            DWORD acceptTimeoutMs = 60000,
                            DWORD ioTimeoutMs = 30000,
                            volatile bool* cancelFlag = nullptr);

// Connects to hostIp:port. connectTimeoutMs bounds the connect itself,
// ioTimeoutMs is the send/recv timeout on the resulting connection.
//
// cancelFlag: same idea as NetworkHostAcceptOne() above - the connect-wait is
// polled in ~200ms steps instead of one single select() call, so Disconnect
// reacts within ~200ms instead of having to wait out the full
// connectTimeoutMs. nullptr (default) keeps the old behavior.
SOCKET NetworkConnect(const char* hostIp, unsigned short port,
                      DWORD connectTimeoutMs = 15000,
                      DWORD ioTimeoutMs = 30000,
                      volatile bool* cancelFlag = nullptr);

typedef void (*NetworkProgressFn)(unsigned long long done, unsigned long long total, void* userData);

bool NetworkSendFile(SOCKET sock, const char* filePath,
                     NetworkProgressFn onProgress = nullptr, void* userData = nullptr);
bool NetworkReceiveFile(SOCKET sock, const char* outFilePath,
                        NetworkProgressFn onProgress = nullptr, void* userData = nullptr);

// Raw fixed-size send/recv (loops until all of it is transferred or an error
// occurs). Used directly by modules that exchange small structs instead of
// whole files, e.g. build_sync.
bool NetworkSendExact(SOCKET sock, const void* data, size_t size);
bool NetworkRecvExact(SOCKET sock, void* data, size_t size);

void NetworkCloseSocket(SOCKET sock);
