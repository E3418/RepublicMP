// See network.h.
#include "network.h"
#include <cstdio>
#include <algorithm> // std::min - NOMINMAX is defined project-wide, so windows.h's min/max macros are gone

#pragma comment(lib, "ws2_32.lib")

void Network_Bind(const TsmHost* host)
{
    TsmBind(host);
}

bool NetworkInit()
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        Logf("republicmp  NETWORK: WSAStartup failed, code=%d", result);
        return false;
    }
    Logf("republicmp  NETWORK: WinSock initialized (%d.%d)",
         LOBYTE(wsaData.wVersion), HIBYTE(wsaData.wVersion));
    return true;
}

// Windows SO_RCVTIMEO/SO_SNDTIMEO take a DWORD (ms), not a struct timeval.
static bool NetworkSetSocketTimeouts(SOCKET sock, DWORD timeoutMs)
{
    if (sock == INVALID_SOCKET) {
        Logf("republicmp  NETWORK: NetworkSetSocketTimeouts() got INVALID_SOCKET");
        return false;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs)) == SOCKET_ERROR) {
        Logf("republicmp  NETWORK: setsockopt(SO_RCVTIMEO) failed, WSAGetLastError=%d", WSAGetLastError());
        return false;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs)) == SOCKET_ERROR) {
        Logf("republicmp  NETWORK: setsockopt(SO_SNDTIMEO) failed, WSAGetLastError=%d", WSAGetLastError());
        return false;
    }
    return true;
}

SOCKET NetworkHostAcceptOne(unsigned short port, DWORD acceptTimeoutMs, DWORD ioTimeoutMs, volatile bool* cancelFlag)
{
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        Logf("republicmp  NETWORK: socket() failed, WSAGetLastError=%d", WSAGetLastError());
        return INVALID_SOCKET;
    }

    BOOL reuse = TRUE;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Logf("republicmp  NETWORK: bind(:%u) failed, WSAGetLastError=%d", port, WSAGetLastError());
        closesocket(listenSock);
        return INVALID_SOCKET;
    }
    if (listen(listenSock, 1) == SOCKET_ERROR) {
        Logf("republicmp  NETWORK: listen(:%u) failed, WSAGetLastError=%d", port, WSAGetLastError());
        closesocket(listenSock);
        return INVALID_SOCKET;
    }

    if (acceptTimeoutMs == 0)
        Logf("republicmp  NETWORK: listening on :%u, waiting for a client (no timeout)...", port);
    else
        Logf("republicmp  NETWORK: listening on :%u, waiting for a client (timeout %u ms)...", port, acceptTimeoutMs);

    // accept() ignores SO_RCVTIMEO: wait with select() in short steps so the
    // cancel flag is checked between them. acceptTimeoutMs == 0 = no limit.
    const DWORD pollStepMs = 200;
    DWORD waited = 0;
    for (;;) {
        if (cancelFlag && *cancelFlag) {
            Logf("republicmp  NETWORK: accept on :%u cancelled (no client yet)", port);
            closesocket(listenSock);
            return INVALID_SOCKET;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);
        timeval stepTimeout;
        stepTimeout.tv_sec = pollStepMs / 1000;
        stepTimeout.tv_usec = (pollStepMs % 1000) * 1000;

        int selectResult = select(0, &readSet, nullptr, nullptr, &stepTimeout);
        if (selectResult == SOCKET_ERROR) {
            Logf("republicmp  NETWORK: select() failed, WSAGetLastError=%d", WSAGetLastError());
            closesocket(listenSock);
            return INVALID_SOCKET;
        }
        if (selectResult > 0) {
            break; // a client is waiting - fall through to accept() below
        }

        // selectResult == 0: nobody yet this step, keep polling.
        if (acceptTimeoutMs != 0) {
            waited += pollStepMs;
            if (waited >= acceptTimeoutMs) {
                Logf("republicmp  NETWORK: TIMEOUT - no client after %u ms", acceptTimeoutMs);
                closesocket(listenSock);
                return INVALID_SOCKET;
            }
        }
    }

    sockaddr_in clientAddr = {};
    int clientAddrLen = sizeof(clientAddr);
    SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &clientAddrLen);

    closesocket(listenSock);

    if (clientSock == INVALID_SOCKET) {
        Logf("republicmp  NETWORK: accept() failed, WSAGetLastError=%d", WSAGetLastError());
        return INVALID_SOCKET;
    }

    char clientIp[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
    Logf("republicmp  NETWORK: client connected, %s:%u", clientIp, ntohs(clientAddr.sin_port));

    NetworkSetSocketTimeouts(clientSock, ioTimeoutMs);
    return clientSock;
}

SOCKET NetworkConnect(const char* hostIp, unsigned short port, DWORD connectTimeoutMs, DWORD ioTimeoutMs, volatile bool* cancelFlag)
{
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        Logf("republicmp  NETWORK: socket() failed, WSAGetLastError=%d", WSAGetLastError());
        return INVALID_SOCKET;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, hostIp, &addr.sin_addr) != 1) {
        Logf("republicmp  NETWORK: couldn't parse IP [%s]", hostIp);
        closesocket(sock);
        return INVALID_SOCKET;
    }

    Logf("republicmp  NETWORK: connecting to %s:%u (timeout %u ms)...", hostIp, port, connectTimeoutMs);

    // Standard non-blocking connect-with-timeout pattern.
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);

    int connectResult = connect(sock, (sockaddr*)&addr, sizeof(addr));
    if (connectResult == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            Logf("republicmp  NETWORK: connect() failed, WSAGetLastError=%d", err);
            closesocket(sock);
            return INVALID_SOCKET;
        }

        // Polled in short steps so the cancel flag is checked; the result is read
        // from the same select() call that reported completion.
        const DWORD pollStepMs = 200;
        DWORD waited = 0;
        fd_set writeSet;
        fd_set exceptSet;
        for (;;) {
            if (cancelFlag && *cancelFlag) {
                Logf("republicmp  NETWORK: connect to %s:%u cancelled", hostIp, port);
                closesocket(sock);
                return INVALID_SOCKET;
            }

            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            FD_ZERO(&exceptSet);
            FD_SET(sock, &exceptSet);

            DWORD stepMs = std::min(pollStepMs, connectTimeoutMs - waited);
            timeval stepTimeout;
            stepTimeout.tv_sec = stepMs / 1000;
            stepTimeout.tv_usec = (stepMs % 1000) * 1000;

            int selectResult = select(0, nullptr, &writeSet, &exceptSet, &stepTimeout);
            if (selectResult == SOCKET_ERROR) {
                Logf("republicmp  NETWORK: select() failed, WSAGetLastError=%d", WSAGetLastError());
                closesocket(sock);
                return INVALID_SOCKET;
            }
            if (selectResult > 0) {
                break; // writeSet/exceptSet now hold this call's real result
            }

            waited += stepMs;
            if (waited >= connectTimeoutMs) {
                Logf("republicmp  NETWORK: TIMEOUT - couldn't connect to %s:%u within %u ms", hostIp, port, connectTimeoutMs);
                closesocket(sock);
                return INVALID_SOCKET;
            }
        }

        if (FD_ISSET(sock, &exceptSet)) {
            // select() succeeded; the connect error itself is in SO_ERROR (e.g.
            // WSAECONNREFUSED when nothing is listening).
            int soError = 0;
            int soErrorLen = sizeof(soError);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soError, &soErrorLen);
            Logf("republicmp  NETWORK: connect() failed (SO_ERROR=%d)", soError);
            closesocket(sock);
            return INVALID_SOCKET;
        }

        int soError = 0;
        int soErrorLen = sizeof(soError);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soError, &soErrorLen);
        if (soError != 0) {
            Logf("republicmp  NETWORK: connect() failed (SO_ERROR=%d)", soError);
            closesocket(sock);
            return INVALID_SOCKET;
        }
    }

    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);

    Logf("republicmp  NETWORK: connected to %s:%u", hostIp, port);
    NetworkSetSocketTimeouts(sock, ioTimeoutMs);
    return sock;
}

bool NetworkSendExact(SOCKET sock, const void* data, size_t size)
{
    const char* bytes = (const char*)data;
    size_t sent = 0;
    while (sent < size) {
        int chunk = send(sock, bytes + sent, (int)std::min(size - sent, (size_t)INT_MAX), 0);
        if (chunk == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                Logf("republicmp  NETWORK: TIMEOUT - send() stalled (sent %zu of %zu bytes)", sent, size);
            } else {
                Logf("republicmp  NETWORK: send() failed, WSAGetLastError=%d", err);
            }
            return false;
        }
        if (chunk == 0) {
            Logf("republicmp  NETWORK: send() returned 0 (connection closed?)");
            return false;
        }
        sent += (size_t)chunk;
    }
    return true;
}

bool NetworkRecvExact(SOCKET sock, void* data, size_t size)
{
    char* bytes = (char*)data;
    size_t received = 0;
    while (received < size) {
        int chunk = recv(sock, bytes + received, (int)std::min(size - received, (size_t)INT_MAX), 0);
        if (chunk == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                Logf("republicmp  NETWORK: TIMEOUT - recv() stalled (received %zu of %zu bytes)", received, size);
            } else {
                Logf("republicmp  NETWORK: recv() failed, WSAGetLastError=%d", err);
            }
            return false;
        }
        if (chunk == 0) {
            Logf("republicmp  NETWORK: connection closed early (received %zu of %zu bytes)", received, size);
            return false;
        }
        received += (size_t)chunk;
    }
    return true;
}

bool NetworkSendFile(SOCKET sock, const char* filePath, NetworkProgressFn onProgress, void* userData)
{
    FILE* f = fopen(filePath, "rb");
    if (!f) {
        Logf("republicmp  NETWORK: couldn't open [%s] for reading", filePath);
        return false;
    }

    _fseeki64(f, 0, SEEK_END);
    unsigned long long fileSize = (unsigned long long)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);

    if (!NetworkSendExact(sock, (const char*)&fileSize, sizeof(fileSize))) {
        fclose(f);
        return false;
    }

    Logf("republicmp  NETWORK: sending [%s], %llu bytes...", filePath, fileSize);

    char buffer[65536];
    unsigned long long sentTotal = 0;
    while (sentTotal < fileSize) {
        size_t toRead = (size_t)std::min((unsigned long long)sizeof(buffer), fileSize - sentTotal);
        size_t bytesRead = fread(buffer, 1, toRead, f);
        if (bytesRead == 0) {
            Logf("republicmp  NETWORK: fread() returned 0 early (sent %llu of %llu bytes)", sentTotal, fileSize);
            fclose(f);
            return false;
        }
        if (!NetworkSendExact(sock, buffer, bytesRead)) {
            fclose(f);
            return false;
        }
        sentTotal += bytesRead;
        if (onProgress) onProgress(sentTotal, fileSize, userData);
    }

    fclose(f);
    Logf("republicmp  NETWORK: finished sending [%s], %llu bytes", filePath, sentTotal);
    return true;
}

bool NetworkReceiveFile(SOCKET sock, const char* outFilePath, NetworkProgressFn onProgress, void* userData)
{
    unsigned long long fileSize = 0;
    if (!NetworkRecvExact(sock, (char*)&fileSize, sizeof(fileSize))) {
        return false;
    }

    FILE* f = fopen(outFilePath, "wb");
    if (!f) {
        Logf("republicmp  NETWORK: couldn't open [%s] for writing", outFilePath);
        return false;
    }

    Logf("republicmp  NETWORK: receiving [%s], expecting %llu bytes...", outFilePath, fileSize);

    char buffer[65536];
    unsigned long long receivedTotal = 0;
    while (receivedTotal < fileSize) {
        size_t toRead = (size_t)std::min((unsigned long long)sizeof(buffer), fileSize - receivedTotal);
        if (!NetworkRecvExact(sock, buffer, toRead)) {
            fclose(f);
            return false;
        }
        if (fwrite(buffer, 1, toRead, f) != toRead) {
            Logf("republicmp  NETWORK: fwrite() didn't write everything (disk full?)");
            fclose(f);
            return false;
        }
        receivedTotal += toRead;
        if (onProgress) onProgress(receivedTotal, fileSize, userData);
    }

    fclose(f);
    Logf("republicmp  NETWORK: finished receiving [%s], %llu bytes", outFilePath, receivedTotal);
    return true;
}

void NetworkCloseSocket(SOCKET sock)
{
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
}
