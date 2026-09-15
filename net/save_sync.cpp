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

// Client-local staging name: purely a filename prefix for the downloaded zip
// and its extraction folder on the CLIENT's own disk - it never has to match
// whatever name the host used for its own save (the zip bytes are sent as an
// anonymous blob, see network.h). Kept as one fixed name rather than
// randomized, since the client doesn't need uniqueness the way the host's
// on-disk save name does.
#define SAVE_SYNC_CLIENT_STAGING_NAME "mp_syncsave_incoming"

void SaveSync_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// UI/hotkey-shared connection state - see save_sync.h. "Busy" spans the whole
// blocking flow (pause -> zip/transfer -> unzip/load -> unpause), not just
// the initial connect, since RunHostSendSaveFlow/RunClientReceiveSaveFlow
// below are one continuous synchronous run with no separate "connected and
// idling" state.
static volatile bool g_busy = false;
static volatile bool g_isHost = false;
static char g_targetIp[64] = SAVE_SYNC_TEST_HOST_IP; // client target - overridable from the UI, see SaveSync_StartClient

// See save_sync.h's SaveSync_RequestDisconnect() for the full
// contract/limitations. Reset to false at the start of every new
// Host/Client run (SaveSync_StartHost/StartClient below) so a stale flag
// from a previous disconnected run can't immediately kill the next one.
static volatile bool g_disconnectRequested = false;

// true from the moment RunHostSendSaveFlow starts listening until a real
// client actually connects (or hosting is closed) - the game is NOT paused
// during this window (see the restructured flow below). ui/main_menu.cpp
// reads this to show "End Hosting"/"Close" instead of "Disconnect" while
// there's nobody to disconnect FROM yet, just an open listening socket.
static volatile bool g_hostAwaitingClient = false;

// Writes to tesmioloader.log (Logf) and the in-game log (UiLog_Add) below.
// The two in-game log panels (ui/ui_log.cpp for this, plus ui/debug_log.cpp
// for build_sync.cpp) are sufficient on their own; there is no separate
// console window.
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

// label shows up in the normal (non-debug) UI log via SaveStatus below - e.g.
// "sending the save: 40%" - so the user waiting in the menu knows what's
// actually happening instead of just staring at "Busy".
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

    // Coarser ~20% steps - frequent enough to explain the wait in the
    // in-game log, not so frequent it floods it. The old console-only
    // ASCII bar (printf, every 1%) was dropped with the console itself.
    int decile = percent / 20;
    if (decile != g_lastLoggedDecile) {
        g_lastLoggedDecile = decile;
        SaveStatus("%s: %d%%", g_progressLabel, percent);
    }

    g_lastPercent = percent;
}

// Small status messages the client sends over the same socket after the file
// transfer finishes, so the host's console shows what the client is doing
// (extracting/loading/ready) instead of going silent until one final byte.
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

// Reads SaveSyncStatusMsg-s off sock and prints each one, until the final
// "ready" one arrives or the connection times out/drops. Returns true only
// if the final ready message was actually seen.
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

// "Random enough" - just needs to not collide with a real save name, this
// isn't a name anyone needs to remember or type. GetTickCount()'s low 32 bits
// XORed with rand() (seeded once from the current time) is plenty.
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
    // Pausing/saving/zipping happens ONLY once a real client has actually
    // connected, not the moment Host is pressed. Host just opens a
    // listening socket and returns - the game keeps running completely
    // normally while nobody has joined yet. This also keeps
    // Disconnect/"End Hosting" cheap: since nothing is paused/locked during
    // this wait, there's nothing risky about closing it, and
    // NetworkHostAcceptOne polls in short steps (see network.h) so that
    // close is near-instant instead of having to wait for a client.
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

    // acceptTimeoutMs = 0: wait indefinitely for a client (in short polled
    // steps internally, see network.h) - there's no reason to give up on
    // hosting on a timer, and nothing is locked while we wait so there's no
    // cost to waiting as long as it takes.
    // ioTimeoutMs = 0: the recv further down waits for the client to extract
    // and load the save, which can take a while on a slow machine.
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

    // A real player just connected - NOW, and only now, do we touch the game
    // at all: pause, snapshot, zip, send. Everything above this point never
    // paused anything and never wrote a single save file.
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

    // The engine's own save writer (FUN_14042cc70, see hooks/save_load.h)
    // opens every subsystem file with a plain fopen("wb") and never creates
    // the destination folder itself - a real Save-menu click does that as a
    // separate step before triggering the write, which is bypassed here
    // since only the trigger flag is poked directly. Without this, every
    // file fails with "Cannot open ... for save ..." because the folder is
    // simply missing (not a disk-space issue - plenty of free space, and
    // the error is "Cannot open", not a write/space error).
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
        // TriggerSaveGame isn't implemented yet (see hooks/save_load.h) - fall
        // back to whatever's already saved on disk under the fixed test name,
        // so the rest of the pipeline (zip/send/receive/load) still works
        // end-to-end. This is NOT a fresh snapshot of the current game.
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
        // Don't unpause the instant the file lands - the client still needs to
        // extract and load it. Print each status line it sends as it arrives,
        // and wait for the final "ready" one so both sides resume together
        // instead of the host running ahead alone.
        SaveStatus("[HOST] waiting for the client to finish loading...");
        if (ReceiveRemoteStatusUntilReady(sock)) {
            SaveStatus("[HOST] client is ready, unpausing");

            // Auto-starts building sync using the same role/IP as
            // ui/debugui.cpp's manual "Enable building sync" button,
            // instead of needing a second click. This is the natural point
            // for it on the host side: the client has just confirmed its
            // world finished loading, so both sides' placement-manager
            // objects exist and are ready to receive remote placements.
            // Idempotent (no-op if already connected/connecting from a
            // manual click), so the manual button is still safe to press
            // afterwards.
            if (!g_disconnectRequested) {
                BuildSync_StartHost();
                // Same auto-start treatment as building sync, at the same
                // moment - road/pipe sync (net/road_sync.cpp) otherwise
                // stays manual-only (its own Host##rs/Connect##rs buttons
                // in the Debug window). Idempotent, same as BuildSync_StartHost.
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
    // Pressing Connect IS the "hey, I want to join" signal - it's what makes
    // the host's blocking accept() above return and kick off its own side of
    // the flow. So the client is blocked/paused from this point on too, not
    // just once bytes start arriving.
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
    // ioTimeoutMs = 0 (no timeout): only matters for the status messages
    // sent back after the extract+load, which can take a while - the file
    // recv itself is chunked and fast either way. cancelFlag:
    // NetworkConnect() polls in short steps internally (see network.h), so
    // Disconnect reacts within ~200ms instead of having to wait out the
    // full 15s connectTimeoutMs.
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
    // Socket stays open - we still need to send the "I'm loaded" ready byte
    // on it below, closed once at the end of this function.

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
        // no timeout - wait however long the load actually takes; cancelFlag
        // lets Disconnect break out of this poll within ~100ms (see
        // hooks/save_load.h's WaitForLoadToFinish doc) - note this does NOT
        // stop the load that's already been triggered above, the engine's
        // own dispatcher will still finish it on its own next frame either
        // way; it only stops US from waiting on/reacting to it further.
        bool loaded = WaitForLoadToFinish(0, &g_disconnectRequested);
        if (loaded) {
            SaveStatus("[CLIENT] load finished");

            // Mirrors the host-side call in RunHostSendSaveFlow above; see
            // its comment for why right after the world finishes loading is
            // the right moment. Idempotent, same as the host side.
            if (!g_disconnectRequested) {
                BuildSync_StartClient(g_targetIp);
                // See the matching host-side comment in RunHostSendSaveFlow -
                // road/pipe sync now auto-starts alongside building sync here too.
                RoadSync_StartClient(g_targetIp);
            }

            // Only now tell the host it can unpause - so both sides resume
            // close together instead of the host running ahead while we're
            // still loading.
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

// UI-driven equivalents of the O/I hotkeys - see save_sync.h. Both the
// hotkey thread below and the UI (ui/main_menu.cpp) call these, so there's
// exactly one place that decides "is a run allowed right now" (g_busy guards
// against a hotkey-mash or a UI double-click starting two overlapping runs).

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
    // Always tear down building sync, busy or not - Disconnect means "leave
    // the whole session", and a build_sync connection can be sitting there
    // connected/connecting independently of whether a save_sync run is still
    // "busy" (that flag clears the moment the one-shot save/load flow
    // finishes, but build_sync's own connection - auto-started at the end of
    // that flow now, see RunHostSendSaveFlow/RunClientReceiveSaveFlow - stays
    // open long after).
    if (BuildSync_IsConnected() || BuildSync_IsConnecting()) {
        BuildSync_Reset();
    }
    // Same reasoning as the BuildSync_Reset() above - road/pipe sync is
    // auto-started alongside building sync, so Disconnect needs to tear it
    // down too, not just leave it dangling.
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
