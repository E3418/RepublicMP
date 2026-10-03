// See save_sync.h.
#include "save_sync.h"
#include "../version.h"
#include "../hooks/game_speed.h"
#include "../hooks/save_load.h"
#include "../save/save_zip.h"
#include "../ui/ui_log.h"
#include "../ui/sync_block.h"
#include "../third_party/miniz/miniz.h" // mz_crc32 for the build check
#include "network.h"
#include "build_sync.h"
#include "road_sync.h"
#include "terrain_sync.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <vector>

#pragma comment(lib, "User32.lib")

// Fixed local name for the downloaded zip and its extraction folder on the client.
#define SAVE_SYNC_CLIENT_STAGING_NAME "mp_syncsave_incoming"

// How long the live sync channels get to connect after the world is loaded.
#define CHANNELS_CONNECT_TIMEOUT_MS 30000
#define CHANNELS_RETRY_MS 1000

#define JOIN_STEPS 7

void SaveSync_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static volatile SessionState g_state = SESSION_IDLE;
static volatile bool g_isHost = false;
static char g_targetIp[64] = SAVE_SYNC_TEST_HOST_IP; // client target - overridable from the UI
static char g_peerIp[64] = "";
static char g_lastError[256] = "";

// Set by SaveSync_RequestDisconnect; reset at the start of every session.
static volatile bool g_disconnectRequested = false;

// The join connection in use, so Disconnect can break a blocking recv.
static volatile SOCKET g_activeSock = INVALID_SOCKET;

static void SetState(SessionState s) { g_state = s; }

static void SetPeerIp(const char* ip)
{
    EnterCriticalSection(&g_lock);
    strncpy_s(g_peerIp, ip ? ip : "", _TRUNCATE);
    LeaveCriticalSection(&g_lock);
}

static void SetSessionError(const char* fmt, ...)
{
    char body[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_lock);
    strncpy_s(g_lastError, body, _TRUNCATE);
    LeaveCriticalSection(&g_lock);
}

// Writes to tesmioloader.log and the in-game UI log.
void SaveStatus(const char* fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    Logf("republicmp  STATUS: %s", body);
    UiLog_Add(body);
}

// ---------------------------------------------------------------------------
// Build identity
// ---------------------------------------------------------------------------

static unsigned int g_dllCrc = 0;
static bool g_dllCrcDone = false;

// CRC32 of this RepublicMP.dll file on disk: two players with the same file
// have the same build, whatever its version string says.
unsigned int SaveSync_GetLocalDllCrc()
{
    if (g_dllCrcDone) return g_dllCrc;
    g_dllCrcDone = true;

    HMODULE self = nullptr;
    char path[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&SaveSync_GetLocalDllCrc, &self) ||
        !GetModuleFileNameA(self, path, MAX_PATH)) {
        Logf("republicmp  SESSION: couldn't locate RepublicMP.dll for the build check");
        return 0;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        Logf("republicmp  SESSION: couldn't read [%s] for the build check", path);
        return 0;
    }
    std::vector<unsigned char> buf(64 * 1024);
    mz_ulong crc = MZ_CRC32_INIT;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
        crc = mz_crc32(crc, buf.data(), n);
    fclose(f);
    g_dllCrc = (unsigned int)crc;
    Logf("republicmp  SESSION: this build: version %s, protocol %d, dll crc %08X",
         REPUBLICMP_VERSION, REPUBLICMP_PROTOCOL, g_dllCrc);
    return g_dllCrc;
}

const char* SaveSync_GetLocalVersion() { return REPUBLICMP_VERSION; }

// ---------------------------------------------------------------------------
// Wire messages
// ---------------------------------------------------------------------------

struct SessionHello {
    char magic[8];     // "RPMPSES"
    int protocol;      // REPUBLICMP_PROTOCOL
    char version[16];  // REPUBLICMP_VERSION
    unsigned int dllCrc;
};

// Step reports in both directions after the hello.
enum SessionMsgCode {
    MSG_STEP = 0,        // step/percent/text update
    MSG_FILE_FOLLOWS = 1, // host -> client: the save file comes next
    MSG_READY = 2,       // client -> host: world loaded
    MSG_ABORT = 3,       // either way: giving up, text says why
};
struct SessionMsg {
    int code;
    int step;
    int percent;
    char text[64];
};

static bool SendMsg(SOCKET sock, int code, int step, int percent, const char* text)
{
    SessionMsg m = {};
    m.code = code;
    m.step = step;
    m.percent = percent;
    strncpy_s(m.text, text ? text : "", _TRUNCATE);
    return NetworkSendExact(sock, &m, sizeof(m));
}

static bool RecvMsg(SOCKET sock, SessionMsg* m)
{
    if (!NetworkRecvExact(sock, m, sizeof(*m))) return false;
    m->text[sizeof(m->text) - 1] = 0;
    return true;
}

static void MakeHello(SessionHello* h)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "RPMPSES", 8);
    h->protocol = REPUBLICMP_PROTOCOL;
    strncpy_s(h->version, REPUBLICMP_VERSION, _TRUNCATE);
    h->dllCrc = SaveSync_GetLocalDllCrc();
}

// Compares the peer's hello with ours; fills why on a mismatch.
static bool HelloMatches(const SessionHello& peer, char* why, size_t whySize)
{
    if (memcmp(peer.magic, "RPMPSES", 8) != 0) {
        _snprintf_s(why, whySize, _TRUNCATE, "the other side is not a compatible RepublicMP (old build?)");
        return false;
    }
    char peerVersion[17] = {};
    memcpy(peerVersion, peer.version, 16);
    if (peer.protocol != REPUBLICMP_PROTOCOL || strcmp(peerVersion, REPUBLICMP_VERSION) != 0 ||
        peer.dllCrc != SaveSync_GetLocalDllCrc()) {
        _snprintf_s(why, whySize, _TRUNCATE,
                    "different RepublicMP builds - yours %s (protocol %d, crc %08X), theirs %s (protocol %d, crc %08X). "
                    "Both players need the same RepublicMP.dll",
                    REPUBLICMP_VERSION, REPUBLICMP_PROTOCOL, SaveSync_GetLocalDllCrc(),
                    peerVersion, peer.protocol, peer.dllCrc);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Live sync channels
// ---------------------------------------------------------------------------

static bool AllChannelsConnected()
{
    return BuildSync_IsConnected() && RoadSync_IsConnected() && TerrainSync_IsConnected();
}

// Closes every channel, connected or still waiting for its peer.
static void ResetChannels()
{
    BuildSync_Reset();
    RoadSync_Reset();
    TerrainSync_Reset();
}

static void StartChannelsHost()
{
    BuildSync_StartHost();
    RoadSync_StartHost();
    TerrainSync_StartHost();
}

// Re-tries every channel that is neither connected nor connecting (the host
// may not be listening yet when the first attempt goes out).
static void StartChannelsClient()
{
    if (!BuildSync_IsConnected() && !BuildSync_IsConnecting()) BuildSync_StartClient(g_targetIp);
    if (!RoadSync_IsConnected() && !RoadSync_IsConnecting()) RoadSync_StartClient(g_targetIp);
    if (!TerrainSync_IsConnected() && !TerrainSync_IsConnecting()) TerrainSync_StartClient(g_targetIp);
}

// Waits until all channels are up. Client side keeps re-trying.
static bool WaitForChannels(bool client)
{
    ULONGLONG start = GetTickCount64();
    ULONGLONG lastTry = 0;
    while (!g_disconnectRequested) {
        if (AllChannelsConnected()) return true;
        ULONGLONG now = GetTickCount64();
        if (now - start > CHANNELS_CONNECT_TIMEOUT_MS) return false;
        if (client && now - lastTry >= CHANNELS_RETRY_MS) {
            StartChannelsClient();
            lastTry = now;
        }
        Sleep(100);
    }
    return false;
}

// Blocks while the session is live. Returns why it ended.
static const char* MonitorLiveSession()
{
    for (;;) {
        if (g_disconnectRequested) return nullptr;
        if (!BuildSync_IsConnected()) return "building sync connection lost";
        if (!RoadSync_IsConnected()) return "road sync connection lost";
        if (!TerrainSync_IsConnected()) return "terrain sync connection lost";
        Sleep(200);
    }
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

// Shared by the progress callbacks: the step's bar, plus (optionally) a
// relay of the percentage to the other side every 10%.
struct ProgressCtx {
    SOCKET relaySock; // INVALID_SOCKET = do not relay
    int step;
    const char* text;
    int lastRelayed;
};

static void OnProgress(ProgressCtx* ctx, unsigned long long done, unsigned long long total)
{
    if (total == 0) return;
    int percent = (int)((done * 100) / total);
    SyncBlock_SetPercent(percent);
    if (ctx && ctx->relaySock != INVALID_SOCKET && percent / 10 != ctx->lastRelayed / 10) {
        ctx->lastRelayed = percent;
        SendMsg(ctx->relaySock, MSG_STEP, ctx->step, percent, ctx->text);
    }
}

static void NetworkProgressCb(unsigned long long done, unsigned long long total, void* userData)
{
    OnProgress((ProgressCtx*)userData, done, total);
}

static void ZipProgressCb(size_t done, size_t total, void* userData)
{
    OnProgress((ProgressCtx*)userData, (unsigned long long)done, (unsigned long long)total);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

static bool GetGameSaveDir(char* outPath, size_t outSize, const char* saveName)
{
    char exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(g_exe, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) return false;
    *lastSlash = '\0';
    return _snprintf_s(outPath, outSize, _TRUNCATE, "%s\\media_soviet\\save\\%s", exePath, saveName) >= 0;
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

static void PeerIpOf(SOCKET sock, char* out, size_t outSize)
{
    sockaddr_in addr = {};
    int len = sizeof(addr);
    out[0] = 0;
    if (getpeername(sock, (sockaddr*)&addr, &len) == 0)
        inet_ntop(AF_INET, &addr.sin_addr, out, outSize);
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

static void HostStep(SOCKET sock, int step, const char* text, int percent = -1)
{
    SyncBlock_SetStep(step, JOIN_STEPS, text, percent);
    SaveStatus("[HOST] step %d/%d: %s", step, JOIN_STEPS, text);
    (void)sock;
}

// One join, from the accepted connection to the lifted sync screen. The
// caller's session loop decides what happens next. Returns true when the
// player is in the game with all channels up.
static bool RunHostJoin(SOCKET sock)
{
    g_activeSock = sock;
    SyncBlock_SetActive(true, "A player is connecting");
    EnsureGamePaused();
    bool joined = false;

    do {
        // 1. Versions.
        HostStep(sock, 1, "A player is connecting - checking versions");
        SessionHello mine, theirs;
        MakeHello(&mine);
        if (!NetworkRecvExact(sock, &theirs, sizeof(theirs))) {
            SetSessionError("the player dropped during the version check");
            break;
        }
        NetworkSendExact(sock, &mine, sizeof(mine)); // always, so the client can show what differs
        char why[256];
        if (!HelloMatches(theirs, why, sizeof(why))) {
            SetSessionError("join refused: %s", why);
            SaveStatus("[HOST] join refused: %s", why);
            break;
        }

        // 2. Save.
        char liveSaveName[64];
        GenerateRandomSaveName(liveSaveName, sizeof(liveSaveName));
        char saveDir[MAX_PATH] = {};
        if (!GetGameSaveDir(saveDir, sizeof(saveDir), liveSaveName)) {
            SetSessionError("couldn't build the save path");
            SendMsg(sock, MSG_ABORT, 0, -1, "host couldn't save the world");
            break;
        }
        HostStep(sock, 2, "Saving the world");
        SendMsg(sock, MSG_STEP, 2, -1, "saving the world");
        // The engine's save writer does not create the destination folder.
        if (!CreateDirectoryA(saveDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            SetSessionError("couldn't create the save folder (error %lu)", GetLastError());
            SendMsg(sock, MSG_ABORT, 0, -1, "host couldn't save the world");
            break;
        }
        TriggerSaveGame(liveSaveName);
        if (!WaitForSaveToFinish(0, &g_disconnectRequested)) {
            SendMsg(sock, MSG_ABORT, 0, -1, "host stopped hosting");
            break;
        }

        // 3. Pack.
        char zipPath[MAX_PATH] = {};
        _snprintf_s(zipPath, sizeof(zipPath), _TRUNCATE, "%s.republicmp.zip", saveDir);
        HostStep(sock, 3, "Packing the save", 0);
        SendMsg(sock, MSG_STEP, 3, 0, "packing the save");
        ProgressCtx packCtx = { sock, 3, "packing the save", -100 };
        if (!ZipSaveDirectory(saveDir, zipPath, ZipProgressCb, &packCtx)) {
            SetSessionError("packing the save failed");
            SendMsg(sock, MSG_ABORT, 0, -1, "host couldn't pack the save");
            break;
        }
        if (g_disconnectRequested) {
            SendMsg(sock, MSG_ABORT, 0, -1, "host stopped hosting");
            break;
        }

        // 4. Send.
        HostStep(sock, 4, "Sending the world to the player", 0);
        SendMsg(sock, MSG_FILE_FOLLOWS, 4, -1, "");
        ProgressCtx sendCtx = { INVALID_SOCKET, 4, nullptr, 0 };
        if (!NetworkSendFile(sock, zipPath, NetworkProgressCb, &sendCtx)) {
            SetSessionError("sending the save failed - connection lost");
            break;
        }

        // 5-6. The player unpacks and loads; its steps arrive as messages.
        HostStep(sock, 5, "Waiting for the player - unpacking", 0);
        bool ready = false;
        for (;;) {
            SessionMsg m;
            if (!RecvMsg(sock, &m)) {
                SetSessionError("the player dropped while loading");
                break;
            }
            if (m.code == MSG_READY) { ready = true; break; }
            if (m.code == MSG_ABORT) {
                SetSessionError("the player gave up: %s", m.text);
                SaveStatus("[HOST] the player gave up: %s", m.text);
                break;
            }
            if (m.code == MSG_STEP) {
                char text[96];
                _snprintf_s(text, sizeof(text), _TRUNCATE, "Waiting for the player - %s", m.text);
                if (m.step == 5) SyncBlock_SetStep(5, JOIN_STEPS, text, m.percent);
                else HostStep(sock, m.step, text, m.percent);
            }
        }
        if (!ready) break;

        // 7. Live sync channels.
        HostStep(sock, 7, "Starting live sync");
        StartChannelsHost();
        if (!WaitForChannels(false)) {
            if (!g_disconnectRequested) SetSessionError("the live sync channels didn't connect");
            ResetChannels();
            break;
        }
        joined = true;
    } while (false);

    g_activeSock = INVALID_SOCKET;
    NetworkCloseSocket(sock);
    SyncBlock_SetActive(false, nullptr);
    EnsureGameUnpaused();
    return joined;
}

static void RunHostSession()
{
    if (!NetworkInit()) {
        SetSessionError("network init failed");
        return;
    }

    while (!g_disconnectRequested) {
        SetState(SESSION_HOST_LISTENING);
        SetPeerIp("");
        SaveStatus("[HOST] hosting on port %d - waiting for a player (the game keeps running)", SAVE_SYNC_PORT);

        // No accept timeout (cancellable); no I/O timeout - the player may
        // take a while to unpack and load.
        SOCKET sock = NetworkHostAcceptOne(SAVE_SYNC_PORT, 0, 0, &g_disconnectRequested);
        if (sock == INVALID_SOCKET) {
            if (!g_disconnectRequested) SetSessionError("couldn't listen on port %d (already in use?)", SAVE_SYNC_PORT);
            break;
        }

        char peer[64];
        PeerIpOf(sock, peer, sizeof(peer));
        SetPeerIp(peer);
        SetState(SESSION_HOST_JOINING);
        SetSessionError("");
        SaveStatus("[HOST] %s is joining - both games are paused until it is loaded", peer);

        if (!RunHostJoin(sock)) {
            if (g_disconnectRequested) break;
            SaveStatus("[HOST] the join failed (%s) - waiting for a player again", SaveSync_GetLastError());
            continue;
        }

        SetState(SESSION_HOST_IN_GAME);
        SaveStatus("[HOST] %s is in the game - live sync running", peer);
        const char* why = MonitorLiveSession();
        ResetChannels();
        if (!why) break; // Stop hosting
        SetSessionError("%s left: %s", peer, why);
        SaveStatus("[HOST] %s left (%s) - waiting for a player again", peer, why);
    }

    ResetChannels();
    SetPeerIp("");
    SaveStatus("[HOST] stopped hosting");
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

static void ClientStep(int step, const char* text, int percent = -1)
{
    SyncBlock_SetStep(step, JOIN_STEPS, text, percent);
    SaveStatus("[CLIENT] step %d/%d: %s", step, JOIN_STEPS, text);
}

// Returns true when the player is in the host's game with all channels up.
static bool RunClientJoin()
{
    SyncBlock_SetActive(true, "Connecting to the host");
    EnsureGamePaused();

    SOCKET sock = INVALID_SOCKET;
    bool joined = false;

    do {
        char saveDir[MAX_PATH] = {};
        if (!GetGameSaveDir(saveDir, sizeof(saveDir), SAVE_SYNC_CLIENT_STAGING_NAME)) {
            SetSessionError("couldn't build the save path");
            break;
        }
        char zipPath[MAX_PATH] = {};
        _snprintf_s(zipPath, sizeof(zipPath), _TRUNCATE, "%s.republicmp.zip", saveDir);
        char destDir[MAX_PATH] = {};
        _snprintf_s(destDir, sizeof(destDir), _TRUNCATE, "%s%s", saveDir, SAVE_SYNC_RECEIVED_SUFFIX);

        // 1. Connect + versions.
        ClientStep(1, "Connecting to the host - checking versions");
        if (!NetworkInit()) {
            SetSessionError("network init failed");
            break;
        }
        sock = NetworkConnect(g_targetIp, SAVE_SYNC_PORT, 15000, 0, &g_disconnectRequested);
        if (sock == INVALID_SOCKET) {
            if (!g_disconnectRequested)
                SetSessionError("couldn't reach %s:%d - is the host hosting (and not already playing with someone)? is the IP right?", g_targetIp, SAVE_SYNC_PORT);
            break;
        }
        g_activeSock = sock;
        SessionHello mine, theirs;
        MakeHello(&mine);
        if (!NetworkSendExact(sock, &mine, sizeof(mine)) || !NetworkRecvExact(sock, &theirs, sizeof(theirs))) {
            SetSessionError("the host dropped during the version check (old build, or already has a player?)");
            break;
        }
        char why[256];
        if (!HelloMatches(theirs, why, sizeof(why))) {
            SetSessionError("join refused: %s", why);
            SaveStatus("[CLIENT] join refused: %s", why);
            break;
        }

        // 2-3. The host saves and packs; its steps arrive as messages.
        ClientStep(2, "Waiting for the host - saving the world");
        bool fileFollows = false;
        for (;;) {
            SessionMsg m;
            if (!RecvMsg(sock, &m)) {
                SetSessionError("the host dropped while preparing the world");
                break;
            }
            if (m.code == MSG_FILE_FOLLOWS) { fileFollows = true; break; }
            if (m.code == MSG_ABORT) {
                SetSessionError("the host gave up: %s", m.text);
                break;
            }
            if (m.code == MSG_STEP) {
                char text[96];
                _snprintf_s(text, sizeof(text), _TRUNCATE, "Waiting for the host - %s", m.text);
                SyncBlock_SetStep(m.step, JOIN_STEPS, text, m.percent);
            }
        }
        if (!fileFollows) break;

        // 4. Receive.
        ClientStep(4, "Receiving the world", 0);
        ProgressCtx recvCtx = { INVALID_SOCKET, 4, nullptr, 0 };
        if (!NetworkReceiveFile(sock, zipPath, NetworkProgressCb, &recvCtx)) {
            SetSessionError("receiving the world failed - connection lost");
            break;
        }

        // 5. Unpack (relayed to the host).
        ClientStep(5, "Unpacking", 0);
        SendMsg(sock, MSG_STEP, 5, 0, "unpacking");
        ProgressCtx unpackCtx = { sock, 5, "unpacking", -100 };
        if (!UnzipToDirectory(zipPath, destDir, ZipProgressCb, &unpackCtx)) {
            SetSessionError("unpacking the world failed");
            SendMsg(sock, MSG_ABORT, 0, -1, "unpacking failed");
            break;
        }
        if (g_disconnectRequested) {
            SendMsg(sock, MSG_ABORT, 0, -1, "the player cancelled");
            break;
        }

        // 6. Load.
        ClientStep(6, "Loading the world");
        SendMsg(sock, MSG_STEP, 6, -1, "loading the world");
        char receivedSaveName[MAX_PATH] = {};
        _snprintf_s(receivedSaveName, sizeof(receivedSaveName), _TRUNCATE, "%s%s",
                    SAVE_SYNC_CLIENT_STAGING_NAME, SAVE_SYNC_RECEIVED_SUFFIX);
        TriggerLoadSave(receivedSaveName);
        if (!WaitForLoadToFinish(0, &g_disconnectRequested)) {
            SendMsg(sock, MSG_ABORT, 0, -1, "the player cancelled while loading");
            break;
        }

        // 7. Ready -> the host starts listening on the channels; connect them.
        ClientStep(7, "Starting live sync");
        SendMsg(sock, MSG_READY, 7, -1, "");
        if (!WaitForChannels(true)) {
            if (!g_disconnectRequested) SetSessionError("the live sync channels didn't connect");
            ResetChannels();
            break;
        }
        joined = true;
    } while (false);

    g_activeSock = INVALID_SOCKET;
    if (sock != INVALID_SOCKET) NetworkCloseSocket(sock);
    SyncBlock_SetActive(false, nullptr);
    EnsureGameUnpaused();
    return joined;
}

static void RunClientSession()
{
    SetPeerIp(g_targetIp);
    SetState(SESSION_CLIENT_JOINING);
    if (!RunClientJoin()) {
        ResetChannels();
        if (g_disconnectRequested) SaveStatus("[CLIENT] cancelled");
        else SaveStatus("[CLIENT] couldn't join: %s", SaveSync_GetLastError());
        return;
    }

    SetState(SESSION_CLIENT_IN_GAME);
    SaveStatus("[CLIENT] in the game at %s - live sync running", g_targetIp);
    const char* why = MonitorLiveSession();
    ResetChannels();
    if (why) {
        SetSessionError("disconnected from the host: %s", why);
        SaveStatus("[CLIENT] disconnected from the host (%s)", why);
    } else {
        SaveStatus("[CLIENT] left the session");
    }
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

static DWORD WINAPI HostSessionThread(LPVOID)
{
    RunHostSession();
    SetState(SESSION_IDLE);
    return 0;
}

static DWORD WINAPI ClientSessionThread(LPVOID)
{
    RunClientSession();
    SetPeerIp("");
    SetState(SESSION_IDLE);
    return 0;
}

void SaveSync_StartHost()
{
    if (g_state != SESSION_IDLE) return;
    g_disconnectRequested = false;
    g_isHost = true;
    SetSessionError("");
    SetState(SESSION_HOST_LISTENING);
    CreateThread(nullptr, 0, HostSessionThread, nullptr, 0, nullptr);
}

void SaveSync_StartClient(const char* hostIp)
{
    if (g_state != SESSION_IDLE) return;
    g_disconnectRequested = false;
    if (hostIp && hostIp[0]) {
        strncpy_s(g_targetIp, hostIp, _TRUNCATE);
    }
    g_isHost = false;
    SetSessionError("");
    SetState(SESSION_CLIENT_JOINING);
    CreateThread(nullptr, 0, ClientSessionThread, nullptr, 0, nullptr);
}

void SaveSync_RequestDisconnect()
{
    if (g_state == SESSION_IDLE) {
        // Channels opened by hand from the Debug window.
        ResetChannels();
        return;
    }
    SaveStatus(g_isHost ? "[HOST] stopping..." : "[CLIENT] disconnecting...");
    g_disconnectRequested = true;
    // Wakes a blocking recv on the join connection (closed by its owner).
    SOCKET s = g_activeSock;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    ResetChannels();
}

SessionState SaveSync_GetState() { return g_state; }
bool SaveSync_IsBusy()           { return g_state != SESSION_IDLE; }
bool SaveSync_IsHostRole()       { return g_isHost; }
bool SaveSync_IsHostAwaitingClient() { return g_state == SESSION_HOST_LISTENING; }
const char* SaveSync_GetTargetIp() { return g_targetIp; }

// The returned buffers are only ever rewritten whole by the session thread;
// the UI reads them once per frame.
const char* SaveSync_GetPeerIp()    { return g_peerIp; }
const char* SaveSync_GetLastError() { return g_lastError; }
