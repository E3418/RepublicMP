// See save_sync.h.
#include "save_sync.h"
#include "../hooks/game_speed.h"
#include "../hooks/save_load.h"
#include "../save/save_zip.h"
#include "../ui/ui_log.h"
#include "../ui/sync_block.h"
#include "network.h"
#include "build_sync.h" // auto-start building sync right after the world loads - see the two call sites below
#include "road_sync.h" // same auto-start treatment - roads/pipes ride along with building sync
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

#pragma comment(lib, "User32.lib")

// Fixed local name for the downloaded zip and its extraction folder on the client.
#define SAVE_SYNC_CLIENT_STAGING_NAME "mp_syncsave_incoming"

void SaveSync_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// Connection state. "Busy" spans the whole blocking flow, not just the connect.
static volatile bool g_busy = false;
static volatile bool g_isHost = false;
static char g_targetIp[64] = SAVE_SYNC_TEST_HOST_IP; // client target - overridable from the UI, see SaveSync_StartClient

// Set by SaveSync_RequestDisconnect; reset at the start of every run.
static volatile bool g_disconnectRequested = false;

// True while a host is listening with no client connected (nothing paused).
static volatile bool g_hostAwaitingClient = false;

// Writes to tesmioloader.log and the in-game UI log.
void SaveStatus(const char* fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    Logf("republicmp  STATUS: %s", body); // generic tag - save_sync.cpp and build_sync.cpp both call this
    UiLog_Add(body); // same line also reaches the in-game overlay log (ui/main_menu.cpp) - one pipe, no other call site changed
}

static int g_lastPercent = -1;
static int g_lastLoggedDecile = -1;
static const char* g_progressLabel = "progress";

// Progress label shown in the UI log, e.g. "sending the save: 40%".
static void ResetProgressBar(const char* label)
{
    g_lastPercent = -1;
    g_lastLoggedDecile = -1;
    g_progressLabel = label;
}

static void PrintProgressBar(unsigned long long done, unsigned long long total)
{
    if (total == 0) return;

    int percent = (int)((done * 100) / total);

    // Report in ~20% steps so the UI log is informative without flooding.
    int decile = percent / 20;
    if (decile != g_lastLoggedDecile) {
        g_lastLoggedDecile = decile;
        SaveStatus("%s: %d%%", g_progressLabel, percent);
    }

    g_lastPercent = percent;
}

// Status messages the client sends after the transfer (extracting/loading/ready).
struct SaveSyncStatusMsg {
    char text[64];
    unsigned char isFinalReady; // 1 = "go ahead and unpause", this is the last message
};

static void SendRemoteStatus(SOCKET sock, const char* text, bool isFinalReady)
{
    SaveSyncStatusMsg msg = {};
    strncpy_s(msg.text, text, _TRUNCATE);
    msg.isFinalReady = isFinalReady ? 1 : 0;
    NetworkSendExact(sock, &msg, sizeof(msg)); // best-effort - a dropped status line isn't fatal
}

// Logs SaveSyncStatusMsg-s from sock until "ready" arrives or the connection
// drops. Returns true only if "ready" was received.
static bool ReceiveRemoteStatusUntilReady(SOCKET sock)
{
    for (;;) {
        SaveSyncStatusMsg msg = {};
        if (!NetworkRecvExact(sock, &msg, sizeof(msg))) {
            return false;
        }
        msg.text[sizeof(msg.text) - 1] = 0;
        SaveStatus("[HOST] client: %s", msg.text);
        if (msg.isFinalReady) {
            return true;
        }
    }
}

static void NetworkProgressCb(unsigned long long done, unsigned long long total, void* /*userData*/)
{
    PrintProgressBar(done, total);
}

static void ZipProgressCb(size_t done, size_t total, void* /*userData*/)
{
    PrintProgressBar((unsigned long long)done, (unsigned long long)total);
}

static bool GetGameSaveDir(char* outPath, size_t outSize, const char* saveName)
{
    char exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(g_exe, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        SaveStatus("GetModuleFileNameA failed");
        return false;
    }

    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) {
        SaveStatus("no '\\' found in exe path [%s]", exePath);
        return false;
    }
    *lastSlash = '\0';

    int written = _snprintf_s(outPath, outSize, _TRUNCATE, "%s\\media_soviet\\save\\%s", exePath, saveName);
    if (written < 0) {
        SaveStatus("save path didn't fit in the buffer");
        return false;
    }
    return true;
}

// A save name that will not collide with the player's own saves.
static void GenerateRandomSaveName(char* out, size_t outSize)
{
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)time(nullptr));
        seeded = true;
    }
    unsigned tag = (unsigned)GetTickCount() ^ ((unsigned)rand() << 16) ^ (unsigned)rand();
    _snprintf_s(out, outSize, _TRUNCATE, "mp_syncsave_%08x", tag);
}

static void RunHostSendSaveFlow()
{
    // Nothing is paused or saved until a client actually connects; the game keeps
    // running while the host is listening.
    if (g_disconnectRequested) {
        SaveStatus("[HOST] disconnect requested before starting, aborting");
        return;
    }

    SaveStatus("[HOST] listening on port %d - waiting for a player to connect "
               "(game keeps running, nothing paused yet)...", SAVE_SYNC_PORT);

    if (!NetworkInit()) {
        SaveStatus("[HOST] network init failed, aborting");
        return;
    }

    // acceptTimeoutMs = 0: wait for a client as long as it takes (cancellable).
    // ioTimeoutMs = 0: the client may take a while to extract and load the save.
    g_hostAwaitingClient = true;
    SOCKET sock = NetworkHostAcceptOne(SAVE_SYNC_PORT, 0, 0, &g_disconnectRequested);
    g_hostAwaitingClient = false;

    if (sock == INVALID_SOCKET) {
        if (g_disconnectRequested) {
            SaveStatus("[HOST] closed - no player connected");
        } else {
            SaveStatus("[HOST] listening failed, aborting");
        }
        return;
    }

    // A client connected: pause, save, zip and send the current game.
    SaveStatus("[HOST] a player connected - pausing and preparing the save...");
    SyncBlock_SetActive(true, "Preparing save for the new player...");
    EnsureGamePaused();

    char liveSaveName[64];
    GenerateRandomSaveName(liveSaveName, sizeof(liveSaveName));

    char saveDir[MAX_PATH] = {};
    char zipPath[MAX_PATH] = {};
    bool haveSaveDir = false;

    if (!GetGameSaveDir(saveDir, sizeof(saveDir), liveSaveName)) {
        NetworkCloseSocket(sock);
        SyncBlock_SetActive(false, nullptr);
        EnsureGameUnpaused();
        return;
    }

    if (g_disconnectRequested) {
        SaveStatus("[HOST] disconnected, aborting");
        NetworkCloseSocket(sock);
        SyncBlock_SetActive(false, nullptr);
        EnsureGameUnpaused();
        return;
    }

    // The engine's save writer does not create the destination folder (the Save
    // menu does that separately), so create it before triggering the save.
    if (!CreateDirectoryA(saveDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        SaveStatus("[HOST] couldn't create save folder '%s' (error %lu), aborting", saveDir, GetLastError());
        NetworkCloseSocket(sock);
        SyncBlock_SetActive(false, nullptr);
        EnsureGameUnpaused();
        return;
    }

    if (TriggerSaveGame(liveSaveName)) {
        SaveStatus("[HOST] saving current game as '%s'...", liveSaveName);
        if (!WaitForSaveToFinish(0, &g_disconnectRequested)) {
            SaveStatus("[HOST] disconnected while saving, aborting");
            NetworkCloseSocket(sock);
            SyncBlock_SetActive(false, nullptr);
            EnsureGameUnpaused();
            return;
        }
        haveSaveDir = true;
    } else {
        // Fallback: send the existing test save instead of a fresh snapshot.
        SaveStatus("[HOST] no live-save hook yet - sending the existing '%s' save instead", SAVE_SYNC_TEST_SAVE_NAME);
        haveSaveDir = GetGameSaveDir(saveDir, sizeof(saveDir), SAVE_SYNC_TEST_SAVE_NAME);
    }

    if (!haveSaveDir) {
        NetworkCloseSocket(sock);
        SyncBlock_SetActive(false, nullptr);
        EnsureGameUnpaused();
        return;
    }
    _snprintf_s(zipPath, sizeof(zipPath), _TRUNCATE, "%s.republicmp.zip", saveDir);

    if (g_disconnectRequested) {
        SaveStatus("[HOST] disconnected, aborting");
        NetworkCloseSocket(sock);
        SyncBlock_SetActive(false, nullptr);
        EnsureGameUnpaused();
        return;
    }

    SaveStatus("[HOST] zipping the save...");
    ResetProgressBar("zipping the save");
    if (!ZipSaveDirectory(saveDir, zipPath, ZipProgressCb)) {
        SaveStatus("[HOST] zip failed, aborting");
        NetworkCloseSocket(sock);
        SyncBlock_SetActive(false, nullptr);
        EnsureGameUnpaused();
        return;
    }

    if (g_disconnectRequested) {
        SaveStatus("[HOST] disconnected, aborting");
        NetworkCloseSocket(sock);
        SyncBlock_SetActive(false, nullptr);
        EnsureGameUnpaused();
        return;
    }

    SaveStatus("[HOST] save packaged as '%s' - sending to the connected player...", liveSaveName);
    SyncBlock_SetActive(true, "Sending save to the new player...");
    ResetProgressBar("sending the save");
    bool ok = NetworkSendFile(sock, zipPath, NetworkProgressCb);

    if (ok) {
        // Wait until the client reports it has loaded so both sides resume together.
        SaveStatus("[HOST] waiting for the client to finish loading...");
        if (ReceiveRemoteStatusUntilReady(sock)) {
            SaveStatus("[HOST] client is ready, unpausing");

            // The client's world is loaded - start building and road sync with the same
            // role/IP (no-op if already connected).
            if (!g_disconnectRequested) {
                BuildSync_StartHost();
                RoadSync_StartHost();
            }
        } else {
            SaveStatus("[HOST] never heard back from the client (timed out/disconnected) - unpausing anyway");
        }
    }

    NetworkCloseSocket(sock);
    SyncBlock_SetActive(false, nullptr);
    EnsureGameUnpaused();

    SaveStatus("[HOST] done, success=%d", ok);
}

static void RunClientReceiveSaveFlow()
{
    if (g_disconnectRequested) {
        SaveStatus("[CLIENT] disconnect requested before starting, aborting");
        return;
    }

    SaveStatus("[CLIENT] connecting - pausing...");
    // Connect is the join request: the client is blocked from here on.
    SyncBlock_SetActive(true, "Connecting to host...");

    char saveDir[MAX_PATH] = {};
    if (!GetGameSaveDir(saveDir, sizeof(saveDir), SAVE_SYNC_CLIENT_STAGING_NAME)) {
        SyncBlock_SetActive(false, nullptr);
        return;
    }

    char zipPath[MAX_PATH] = {};
    _snprintf_s(zipPath, sizeof(zipPath), _TRUNCATE, "%s.republicmp.zip", saveDir);

    char destDir[MAX_PATH] = {};
    _snprintf_s(destDir, sizeof(destDir), _TRUNCATE, "%s%s", saveDir, SAVE_SYNC_RECEIVED_SUFFIX);

    EnsureGamePaused();

    if (!NetworkInit()) {
        SaveStatus("[CLIENT] network init failed, aborting");
        SyncBlock_SetActive(false, nullptr);
        EnsureGameUnpaused();
        return;
    }

    SaveStatus("[CLIENT] connecting to host %s:%d...", g_targetIp, SAVE_SYNC_PORT);
    // ioTimeoutMs = 0: status messages follow the extract+load, which can take a
    // while. The connect wait is cancellable (~200ms).
    SOCKET sock = NetworkConnect(g_targetIp, SAVE_SYNC_PORT, 15000, 0, &g_disconnectRequested);
    if (sock == INVALID_SOCKET) {
        SaveStatus("[CLIENT] couldn't connect, aborting");
        SyncBlock_SetActive(false, nullptr);
        EnsureGameUnpaused();
        return;
    }

    if (g_disconnectRequested) {
        SaveStatus("[CLIENT] disconnected right after connecting, closing and aborting");
        NetworkCloseSocket(sock);
        SyncBlock_SetActive(false, nullptr);
        EnsureGameUnpaused();
        return;
    }

    SyncBlock_SetActive(true, "Receiving save from host...");

    SaveStatus("[CLIENT] connected, receiving...");
    ResetProgressBar("receiving the save");
    bool ok = NetworkReceiveFile(sock, zipPath, NetworkProgressCb);
    // The socket stays open for the final "ready" message.

    if (ok) {
        SaveStatus("[CLIENT] extracting...");
        SendRemoteStatus(sock, "extracting the save...", false);
        ResetProgressBar("extracting the save");
        ok = UnzipToDirectory(zipPath, destDir, ZipProgressCb);
        if (!ok) {
            SendRemoteStatus(sock, "extraction failed, giving up", true);
        }
    }

    if (ok && g_disconnectRequested) {
        SaveStatus("[CLIENT] disconnected after extraction, not loading");
        SendRemoteStatus(sock, "cancelled by the player, not loading", true);
        ok = false;
    }

    if (ok) {
        char receivedSaveName[MAX_PATH] = {};
        _snprintf_s(receivedSaveName, sizeof(receivedSaveName), _TRUNCATE, "%s%s",
                    SAVE_SYNC_CLIENT_STAGING_NAME, SAVE_SYNC_RECEIVED_SUFFIX);
        TriggerLoadSave(receivedSaveName);

        SaveStatus("[CLIENT] waiting for the game to finish loading...");
        SendRemoteStatus(sock, "loading into the world...", false);
        // No timeout; Disconnect stops the wait (not the load already triggered).
        bool loaded = WaitForLoadToFinish(0, &g_disconnectRequested);
        if (loaded) {
            SaveStatus("[CLIENT] load finished");

            // World loaded - start building and road sync (no-op if already connected).
            if (!g_disconnectRequested) {
                BuildSync_StartClient(g_targetIp);
                RoadSync_StartClient(g_targetIp);
            }

            // Tell the host it can unpause, so both sides resume together.
            SendRemoteStatus(sock, "loaded, ready", true);
        } else {
            SaveStatus("[CLIENT] disconnected while waiting for the load to finish");
            SendRemoteStatus(sock, "cancelled by the player while loading", true);
        }
    }

    NetworkCloseSocket(sock);
    SyncBlock_SetActive(false, nullptr);
    EnsureGameUnpaused();

    SaveStatus("[CLIENT] done, success=%d, extracted to [%s]", ok, destDir);
}

// Entry points for the UI; g_busy prevents two overlapping runs.

static DWORD WINAPI HostFlowThread(LPVOID)
{
    RunHostSendSaveFlow();
    g_busy = false;
    return 0;
}

static DWORD WINAPI ClientFlowThread(LPVOID)
{
    RunClientReceiveSaveFlow();
    g_busy = false;
    return 0;
}

void SaveSync_StartHost()
{
    if (g_busy) return;
    g_disconnectRequested = false; // fresh run - don't inherit a stale cancel from a previous disconnected session
    g_isHost = true;
    g_busy = true;
    CreateThread(nullptr, 0, HostFlowThread, nullptr, 0, nullptr);
}

void SaveSync_StartClient(const char* hostIp)
{
    if (g_busy) return;
    g_disconnectRequested = false; // see SaveSync_StartHost() above
    if (hostIp && hostIp[0]) {
        strncpy_s(g_targetIp, hostIp, _TRUNCATE);
    }
    g_isHost = false;
    g_busy = true;
    CreateThread(nullptr, 0, ClientFlowThread, nullptr, 0, nullptr);
}

void SaveSync_RequestDisconnect()
{
    // Disconnect leaves the whole session: close the build and road sync
    // connections too, which outlive the save-sync run itself.
    if (BuildSync_IsConnected() || BuildSync_IsConnecting()) {
        BuildSync_Reset();
    }
    if (RoadSync_IsConnected() || RoadSync_IsConnecting()) {
        RoadSync_Reset();
    }

    if (!g_busy) {
        SaveStatus("[DISCONNECT] nothing running");
        return;
    }

    SaveStatus("[DISCONNECT] requested - stopping at the next safe point "
               "(reacts within ~200ms almost everywhere now, including while "
               "listening/connecting - see network.h's NetworkHostAcceptOne/"
               "NetworkConnect cancelFlag)");
    g_disconnectRequested = true;
}

bool SaveSync_IsBusy()      { return g_busy; }
bool SaveSync_IsHostRole()  { return g_isHost; }
bool SaveSync_IsHostAwaitingClient() { return g_hostAwaitingClient; }
const char* SaveSync_GetTargetIp() { return g_targetIp; }
