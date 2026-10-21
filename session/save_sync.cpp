// SPDX-License-Identifier: GPL-3.0-only
// See save_sync.h.
#include "save_sync.h"
#include "../core/version.h"
#include "game_speed.h"
#include "sync_check.h"
#include "../game/save_load.h"
#include "../save/save_zip.h"
#include "../ui/ui_log.h"
#include "../ui/sync_block.h"
#include "../third_party/miniz/miniz.h" // mz_crc32 for the build check
#include "../transport/network.h"
#include "../world_sync/buildings/build_sync.h"
#include "../world_sync/networks/road_sync.h"
#include "../world_sync/terrain/terrain_sync.h"
#include "../determinism/determinism.h"
#include "../lockstep/lockstep_net.h"
#include "../core/profile.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <system_error>

#pragma comment(lib, "User32.lib")

// Fixed local name for the downloaded zip and its extraction folder on the client.
#define SAVE_SYNC_CLIENT_STAGING_NAME "mp_syncsave_incoming"

// How long the live sync channels get to connect after the world is loaded.
#define CHANNELS_CONNECT_TIMEOUT_MS 30000
#define CHANNELS_RETRY_MS 1000
// A single dropped channel (network hiccup, Debug window Disconnect) gets
// this long to reconnect before the session is given up.
#define CHANNEL_RECONNECT_TIMEOUT_MS 20000

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

// GetTickCount64 when the current session was started (Host / Connect).
static volatile ULONGLONG g_sessionStartTick = 0;

// Re-sync (see save_sync.h).
#define RESYNC_ANNOUNCE_TIMEOUT_MS 3000 // host: the game thread sends the announcement within this
#define RESYNC_ANNOUNCE_FLUSH_MS 500    // host: lets the announcement leave before the channels close
#define RESYNC_REJOIN_TIMEOUT_MS 30000  // host: how long the player has to come back; client: how long it retries
#define RESYNC_RETRY_MS 500
static volatile bool g_resyncRequested = false; // host: button or timer
static volatile bool g_resyncSendNow = false;   // host: the game thread sends the announcement
static volatile bool g_resyncSent = false;
static volatile bool g_resyncAnnounced = false; // client: the host announced one
static volatile bool g_resyncing = false;
static volatile int g_autoResyncMinutes = 0;
static volatile int g_desyncResyncSeconds = 60;    // lockstep: re-sync this long after a desync (-1 off)
static volatile ULONGLONG g_desyncSince = 0;       // GetTickCount64 of the desync this session saw (0 none)
static volatile ULONGLONG g_lastWorldSentTick = 0; // host: when the player last got the world
// Lockstep: both PCs load the sent save with the determinism layer on and run
// the same ticks (lockstep/lockstep_net) instead of the live sync channels.
// The host chooses (lockstep by default); the player follows the host's hello.
static volatile bool g_lockstepChosen = true;
static volatile bool g_dlcCheck = true; // refuse a join when the DLCs differ (both players' choice)
static volatile bool g_lockstep = false; // the current session's mode
#define LOCKSTEP_CONNECT_TIMEOUT_MS 180000 // host: how long the loaded players get to connect
#define HELLO_ANSWER_TIMEOUT_MS 60000      // client: the host is taking other players in
#define LOCKSTEP_SILENCE_MS 20000          // a game that sent nothing (not even a keepalive) is gone
static const char* const kResyncReason = "re-sync";

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
    int lockstep;      // host: the session runs in lockstep
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
    h->lockstep = g_lockstepChosen ? 1 : 0;
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
    LockstepNet_Close();
    Determinism_EndSession();
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

// Names of the channels that are down, e.g. "roads, terrain".
static void DownChannels(char* out, size_t outSize)
{
    out[0] = 0;
    if (!BuildSync_IsConnected()) strcat_s(out, outSize, "buildings ");
    if (!RoadSync_IsConnected()) strcat_s(out, outSize, "roads ");
    if (!TerrainSync_IsConnected()) strcat_s(out, outSize, "terrain ");
    size_t n = strlen(out);
    if (n) out[n - 1] = 0;
}

static bool ResyncDue()
{
    if (g_resyncRequested) return true;
    int minutes = g_autoResyncMinutes;
    return minutes > 0 && GetTickCount64() - g_lastWorldSentTick >= (ULONGLONG)minutes * 60000;
}

// Host: has the game thread send the announcement and waits until it is out.
// False (re-sync dropped) if it could not be sent.
static bool AnnounceResync()
{
    g_resyncRequested = false;
    g_resyncSent = false;
    g_resyncSendNow = true;
    ULONGLONG start = GetTickCount64();
    while (!g_resyncSent && !g_disconnectRequested && GetTickCount64() - start < RESYNC_ANNOUNCE_TIMEOUT_MS)
        Sleep(50);
    g_resyncSendNow = false;
    if (!g_resyncSent) {
        SaveStatus("[HOST] couldn't announce the re-sync - skipped");
        g_lastWorldSentTick = GetTickCount64(); // the timer tries again a full interval later
        return false;
    }
    Sleep(RESYNC_ANNOUNCE_FLUSH_MS);
    g_resyncing = true;
    return true;
}

// Blocks while the session is live. Returns why it ended (nullptr = the
// local player stopped it, kResyncReason = a re-sync).
//
// All three channels gone at once = the other game closed the session (or
// is gone). Only some gone = a hiccup or a Debug-window Disconnect: those
// channels are reconnected (host listens again, client retries) and the
// session ends only if they are not back within CHANNEL_RECONNECT_TIMEOUT_MS.
// Messages that were in flight on a dropped channel are lost.
// Lockstep: the one channel. The host re-syncs on request, on the timer, or
// on a state difference while "Re-sync on drift" is set (any limit: in
// lockstep the games either match or they do not); the announcement goes
// over the lockstep channel and both sides take the usual re-sync path.
static const char* MonitorLockstep(bool client)
{
    bool reported = false;
    ULONGLONG lastPing = 0;
    g_desyncSince = 0;
    while (LockstepNet_IsConnected()) {
        if (g_disconnectRequested) return nullptr;
        ULONGLONG now = GetTickCount64();
        if (now - lastPing >= 1000) {
            LockstepNet_Ping();
            lastPing = now;
        }
        // A game that froze or crashed keeps its socket open while Windows
        // reports it: it is gone once it stays silent.
        if (LockstepNet_MsSinceReceived() > LOCKSTEP_SILENCE_MS) return "the other game stopped answering";
        if (client && LockstepNet_TakeResync()) {
            SaveSync_OnResyncAnnounced();
            return kResyncReason;
        }
        const char* desync = LockstepNet_Desync();
        if (desync[0] && !reported) {
            reported = true;
            g_desyncSince = now;
            SaveStatus("[LOCKSTEP] %s", desync);
        }
        // In lockstep the games match or not: a re-sync comes on request, or a set time after a desync.
        int delay = g_desyncResyncSeconds;
        bool onDesync = desync[0] && delay >= 0 && now - g_desyncSince >= (ULONGLONG)delay * 1000;
        if (!client && (g_resyncRequested || onDesync)) {
            g_resyncRequested = false;
            SaveStatus(onDesync ? "[HOST] the games differ - re-syncing" : "[HOST] re-syncing");
            LockstepNet_SendResync();
            Sleep(RESYNC_ANNOUNCE_FLUSH_MS);
            g_resyncing = true;
            return kResyncReason;
        }
        Sleep(200);
    }
    // The announcement may arrive right before the host closes the channel.
    if (client && LockstepNet_TakeResync()) {
        SaveSync_OnResyncAnnounced();
        return kResyncReason;
    }
    return g_disconnectRequested ? nullptr : "the lockstep connection closed";
}

static const char* MonitorLiveSession(bool client)
{
    if (g_lockstep) return MonitorLockstep(client);
    static char s_why[160];
    ULONGLONG downSince = 0;
    ULONGLONG lastTry = 0;
    for (;;) {
        if (g_disconnectRequested) return nullptr;
        if (client && g_resyncAnnounced) return kResyncReason;

        bool b = BuildSync_IsConnected(), r = RoadSync_IsConnected(), t = TerrainSync_IsConnected();
        if (b && r && t) {
            if (downSince) {
                SaveStatus("live sync restored after %llus", (GetTickCount64() - downSince) / 1000);
                downSince = 0;
            }
            if (!client && ResyncDue() && AnnounceResync()) return kResyncReason;
            Sleep(200);
            continue;
        }

        // The announcement is noted before its connection is seen closing.
        if (client && g_resyncAnnounced) return kResyncReason;

        char down[64];
        DownChannels(down, sizeof(down));
        if (!b && !r && !t) {
            // When the peer really leaves, the three drop within ~100 ms of
            // each other; the reconnect branch below covers that gap.
            _snprintf_s(s_why, sizeof(s_why), _TRUNCATE, "all live sync connections closed");
            return s_why;
        }

        ULONGLONG now = GetTickCount64();
        if (!downSince) {
            downSince = now;
            SaveStatus("live sync channel(s) dropped: %s - reconnecting", down);
        }
        if (now - downSince > CHANNEL_RECONNECT_TIMEOUT_MS) {
            _snprintf_s(s_why, sizeof(s_why), _TRUNCATE, "couldn't reconnect: %s", down);
            return s_why;
        }
        if (client) {
            if (now - lastTry >= CHANNELS_RETRY_MS) {
                StartChannelsClient();
                lastTry = now;
            }
        } else {
            StartChannelsHost(); // no-op for channels already connected or listening
        }
        Sleep(200);
    }
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

// A player in a host's join (several come together: a re-sync brings
// everyone back at once).
struct Joiner {
    SOCKET sock;
    char ip[64];
    char nick[PROFILE_NICK_BYTES];
    bool alive;
};

// Shared by the progress callbacks: the step's bar, plus (optionally) a
// relay of the percentage to the other side every 10% (the client to the
// host, or the host to every player of a join).
struct ProgressCtx {
    SOCKET relaySock;              // INVALID_SOCKET = do not relay
    std::vector<Joiner>* relayTo;  // nullptr = none
    int step;
    const char* text;
    int lastRelayed;
};

static void OnProgress(ProgressCtx* ctx, unsigned long long done, unsigned long long total)
{
    if (total == 0) return;
    int percent = (int)((done * 100) / total);
    SyncBlock_SetPercent(percent);
    if (!ctx || percent / 10 == ctx->lastRelayed / 10) return;
    ctx->lastRelayed = percent;
    if (ctx->relaySock != INVALID_SOCKET) SendMsg(ctx->relaySock, MSG_STEP, ctx->step, percent, ctx->text);
    if (ctx->relayTo)
        for (Joiner& j : *ctx->relayTo)
            if (j.alive) SendMsg(j.sock, MSG_STEP, ctx->step, percent, ctx->text);
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

// ---------------------------------------------------------------------------
// DLCs
// ---------------------------------------------------------------------------
//
// The game's DLCs are folders media_soviet\dlcN (buildings, vehicles,
// terrains). A player without one cannot draw the other's DLC map, buildings
// and vehicles, and in lockstep the vehicle type table (commands name types by
// their index in it) differs. After the version check both sides send their
// list and their checker choice; the join is refused when the lists differ
// and both players keep the checker on.

struct SessionDlcs {
    char magic[8];  // "RPMPDLC"
    int check;      // this player's checker is on
    char list[248]; // the dlcN folder names, sorted, comma-separated
};

static void ListDlcs(char* out, size_t outSize)
{
    out[0] = 0;
    char exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(g_exe, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return;
    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) return;
    *lastSlash = '\0';
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::string(exePath) + "\\media_soviet", ec)) {
        std::string name = e.path().filename().string();
        if (_strnicmp(name.c_str(), "dlc", 3) != 0 || !e.is_directory(ec)) continue;
        if (std::filesystem::directory_iterator(e.path(), ec) == std::filesystem::directory_iterator()) continue; // empty
        for (char& c : name) c = (char)tolower((unsigned char)c);
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        if (out[0]) strncat_s(out, outSize, ",", _TRUNCATE);
        strncat_s(out, outSize, n.c_str(), _TRUNCATE);
    }
}

static void MakeDlcs(SessionDlcs* d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->magic, "RPMPDLC", 8);
    d->check = g_dlcCheck ? 1 : 0;
    ListDlcs(d->list, sizeof(d->list));
}

// Both sides decide alike: false refuses the join (why says what differs).
static bool DlcsMatch(const SessionDlcs& mine, const SessionDlcs& theirs, bool host, char* why, size_t whySize)
{
    char peer[sizeof(theirs.list) + 1] = {};
    memcpy(peer, theirs.list, sizeof(theirs.list));
    const char* hostList = host ? mine.list : peer;
    const char* playerList = host ? peer : mine.list;
    Logf("republicmp  SESSION: DLCs - host [%s], player [%s]", hostList[0] ? hostList : "none", playerList[0] ? playerList : "none");
    if (strcmp(mine.list, peer) == 0) return true;
    bool refuse = mine.check && theirs.check;
    _snprintf_s(why, whySize, _TRUNCATE,
                "the DLCs differ - host [%s], player [%s]. DLC maps, buildings and vehicles cannot be shown without the DLC%s",
                hostList[0] ? hostList : "none", playerList[0] ? playerList : "none",
                refuse ? "; install the same DLCs, or untick \"Check DLCs\" on either PC to play a map that needs none"
                       : " (the DLC check is off - playing anyway)");
    if (!refuse) {
        Logf("republicmp  SESSION: %s", why);
        SaveStatus("%s %s", host ? "[HOST]" : "[CLIENT]", why);
    }
    return !refuse;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
//
// After the DLC check both sides send the name their player chose (core/
// profile; "Player" when none): only for showing who is who.

struct SessionNick {
    char magic[8]; // "RPMPNIK"
    char nick[PROFILE_NICK_BYTES];
};

// Sends this player's name and takes the other side's; false: the connection is gone.
static bool ExchangeNicks(SOCKET sock, char* theirs, size_t theirsSize)
{
    SessionNick mine = {}, other = {};
    memcpy(mine.magic, "RPMPNIK", 8);
    strncpy_s(mine.nick, Profile_GetNick(), _TRUNCATE);
    if (!NetworkSendExact(sock, &mine, sizeof(mine)) || !NetworkRecvExact(sock, &other, sizeof(other)) ||
        memcmp(other.magic, "RPMPNIK", 8) != 0)
        return false;
    other.nick[PROFILE_NICK_BYTES - 1] = 0;
    strncpy_s(theirs, theirsSize, other.nick[0] ? other.nick : "Player", _TRUNCATE);
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

// The host's newest join save. Each join (and so each re-sync) writes a new
// one; every older one is deleted, so they do not pile up - also those left
// by earlier runs of the game (one per run had stayed, ~150 MB each).
static bool IsHostJoinSaveName(const std::string& name)
{
    const char* prefix = "mp_syncsave_";
    if (name.size() != strlen(prefix) + 8 || name.compare(0, strlen(prefix), prefix) != 0) return false;
    for (size_t i = strlen(prefix); i < name.size(); i++)
        if (!isxdigit((unsigned char)name[i])) return false;
    return true;
}

static void KeepOnlyNewestHostSave(const char* newSaveDir)
{
    std::error_code ec;
    std::filesystem::path keep(newSaveDir);
    std::filesystem::directory_iterator it(keep.parent_path(), ec), end;
    int deleted = 0;
    for (; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string name = it->path().filename().string();
        if (!IsHostJoinSaveName(name) || _stricmp(name.c_str(), keep.filename().string().c_str()) == 0) continue;
        std::error_code rm;
        std::filesystem::remove_all(it->path(), rm);
        if (rm) Logf("republicmp  SESSION: couldn't delete the old join save [%s]: %s", name.c_str(), rm.message().c_str());
        else deleted++;
    }
    if (deleted) Logf("republicmp  SESSION: deleted %d old join save(s)", deleted);
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

#define GATHER_MORE_MS 3000 // after the first player, others who connect this soon join with it

static void HostStep(int step, const char* text, int percent = -1)
{
    SyncBlock_SetStep(step, JOIN_STEPS, text, percent);
    SaveStatus("[HOST] step %d/%d: %s", step, JOIN_STEPS, text);
}

static int AliveCount(const std::vector<Joiner>& js)
{
    int n = 0;
    for (const Joiner& j : js)
        if (j.alive) n++;
    return n;
}

static const char* NameOf(const Joiner& j)
{
    return j.nick[0] ? j.nick : j.ip;
}

static void DropJoiner(Joiner& j)
{
    if (j.sock != INVALID_SOCKET) NetworkCloseSocket(j.sock);
    j.sock = INVALID_SOCKET;
    j.alive = false;
}

// The join connections in use, so Disconnect can break their blocking recvs.
static SOCKET g_joinSocks[PROFILE_MAX_PLAYERS];
static int g_joinSockCount = 0;

static void SetJoinSockets(const std::vector<Joiner>* js)
{
    EnterCriticalSection(&g_lock);
    g_joinSockCount = 0;
    if (js)
        for (const Joiner& j : *js)
            if (j.alive && g_joinSockCount < PROFILE_MAX_PLAYERS) g_joinSocks[g_joinSockCount++] = j.sock;
    LeaveCriticalSection(&g_lock);
}

// The players' names for the status lines, e.g. "Bob, Alice".
static void SetPeerNames(const std::vector<Joiner>& js)
{
    char names[64] = "";
    for (const Joiner& j : js) {
        if (!j.alive) continue;
        if (names[0]) strncat_s(names, ", ", _TRUNCATE);
        strncat_s(names, NameOf(j), _TRUNCATE);
    }
    SetPeerIp(names);
}

static void SetPeerNamesFromLockstep()
{
    LockstepPlayer players[PROFILE_MAX_PLAYERS];
    int n = LockstepNet_Players(players, PROFILE_MAX_PLAYERS);
    char names[64] = "";
    for (int i = 0; i < n; i++) {
        if (players[i].self) continue;
        if (names[0]) strncat_s(names, ", ", _TRUNCATE);
        strncat_s(names, players[i].nick, _TRUNCATE);
    }
    SetPeerIp(names);
}

static void SendAll(std::vector<Joiner>& js, int code, int step, int percent, const char* text)
{
    for (Joiner& j : js)
        if (j.alive && !SendMsg(j.sock, code, step, percent, text)) {
            SaveStatus("[HOST] %s dropped", NameOf(j));
            DropJoiner(j);
        }
}

// Versions, DLCs and names with one player; false: refused or gone (said why).
static bool CheckJoiner(Joiner& j)
{
    SessionHello mine, theirs;
    MakeHello(&mine);
    if (!NetworkRecvExact(j.sock, &theirs, sizeof(theirs))) {
        SetSessionError("%s dropped during the version check", j.ip);
        SaveStatus("[HOST] %s dropped during the version check", j.ip);
        return false;
    }
    NetworkSendExact(j.sock, &mine, sizeof(mine)); // always, so the client can show what differs
    char why[256];
    if (!HelloMatches(theirs, why, sizeof(why))) {
        SetSessionError("join refused: %s", why);
        SaveStatus("[HOST] %s: join refused: %s", j.ip, why);
        return false;
    }
    SessionDlcs myDlcs, theirDlcs;
    MakeDlcs(&myDlcs);
    if (!NetworkSendExact(j.sock, &myDlcs, sizeof(myDlcs)) || !NetworkRecvExact(j.sock, &theirDlcs, sizeof(theirDlcs)) ||
        memcmp(theirDlcs.magic, "RPMPDLC", 8) != 0) {
        SetSessionError("%s dropped during the DLC check", j.ip);
        return false;
    }
    char dlcWhy[512];
    if (!DlcsMatch(myDlcs, theirDlcs, true, dlcWhy, sizeof(dlcWhy))) {
        SetSessionError("join refused: %s", dlcWhy);
        SaveStatus("[HOST] %s: join refused: %s", j.ip, dlcWhy);
        return false;
    }
    if (!ExchangeNicks(j.sock, j.nick, sizeof(j.nick))) {
        SetSessionError("%s dropped during the name exchange", j.ip);
        return false;
    }
    return true;
}

// One player's copy of the world, sent on its own thread.
struct WorldSend {
    Joiner* j;
    const char* zipPath;
    volatile LONG percent;
    bool ok;
    HANDLE thread;
};

static void WorldSendProgress(unsigned long long done, unsigned long long total, void* userData)
{
    if (total) ((WorldSend*)userData)->percent = (LONG)((done * 100) / total);
}

static DWORD WINAPI WorldSendThread(LPVOID param)
{
    WorldSend* w = (WorldSend*)param;
    w->ok = SendMsg(w->j->sock, MSG_FILE_FOLLOWS, 4, -1, "") &&
            NetworkSendFile(w->j->sock, w->zipPath, WorldSendProgress, w);
    return 0;
}

// Sends the packed world to every player of the join at the same time (one
// after another, the last one would wait for all the others' downloads); the
// bar shows the slowest. Those it could not reach are dropped.
static void SendWorldToAll(std::vector<Joiner>& js, const char* zipPath)
{
    std::vector<WorldSend> sends;
    for (Joiner& j : js)
        if (j.alive) sends.push_back({ &j, zipPath, 0, false, nullptr });
    char names[96] = "";
    for (WorldSend& w : sends) {
        if (names[0]) strncat_s(names, ", ", _TRUNCATE);
        strncat_s(names, NameOf(*w.j), _TRUNCATE);
        w.thread = CreateThread(nullptr, 0, WorldSendThread, &w, 0, nullptr);
        if (!w.thread) WorldSendThread(&w);
    }
    char text[128];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "Sending the world to %s", names);
    HostStep(4, text, 0);
    for (;;) {
        bool running = false;
        int slowest = 100;
        for (WorldSend& w : sends) {
            if (w.thread && WaitForSingleObject(w.thread, 0) == WAIT_TIMEOUT) running = true;
            if (w.percent < slowest) slowest = (int)w.percent;
        }
        SyncBlock_SetPercent(slowest);
        if (!running) break;
        Sleep(200);
    }
    for (WorldSend& w : sends) {
        if (w.thread) CloseHandle(w.thread);
        if (w.ok) continue;
        SaveStatus("[HOST] sending the world to %s failed - connection lost", NameOf(*w.j));
        DropJoiner(*w.j);
    }
}

// The players of a join still waiting for the lockstep connection: one whose
// join connection closed has given up.
static int JoinersStillThere(void* ctx)
{
    int n = 0;
    for (Joiner& j : *(std::vector<Joiner>*)ctx) {
        if (j.alive && NetworkPeerGone(j.sock)) {
            SaveStatus("[HOST] %s gave up waiting", NameOf(j));
            j.alive = false; // closed with the others when the join ends
        }
        if (j.alive) n++;
    }
    return n;
}

// One join of one or more players (a re-sync brings everyone back at once,
// and a new player comes with them), from their connections to the lifted
// sync screen: one save, packed once, sent to each. Returns how many are in
// the game (lockstep: connected to the lockstep channel; replay: the one
// player, with all channels up). The connections are closed.
static int RunHostJoin(std::vector<Joiner>& js)
{
    SetJoinSockets(&js);
    bool several = js.size() > 1;
    SyncBlock_SetActive(true, g_resyncing ? "Re-syncing the world" : several ? "Players are connecting" : "A player is connecting");
    EnsureGamePaused();
    int joined = 0;
    char madeSaveDir[MAX_PATH] = {}; // cleaned up below
    char madeZip[MAX_PATH] = {};

    do {
        // 1. Versions, DLCs and names, one player after another.
        HostStep(1, several ? "Players are connecting - checking versions" : "A player is connecting - checking versions");
        for (Joiner& j : js)
            if (j.alive && !CheckJoiner(j)) DropJoiner(j);
        if (!AliveCount(js)) break;
        SetPeerNames(js);

        // 2. Save.
        char liveSaveName[64];
        GenerateRandomSaveName(liveSaveName, sizeof(liveSaveName));
        char saveDir[MAX_PATH] = {};
        if (!GetGameSaveDir(saveDir, sizeof(saveDir), liveSaveName)) {
            SetSessionError("couldn't build the save path");
            SendAll(js, MSG_ABORT, 0, -1, "host couldn't save the world");
            break;
        }
        HostStep(2, "Saving the world");
        SendAll(js, MSG_STEP, 2, -1, "saving the world");
        // The engine's save writer does not create the destination folder.
        if (!CreateDirectoryA(saveDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            SetSessionError("couldn't create the save folder (error %lu)", GetLastError());
            SendAll(js, MSG_ABORT, 0, -1, "host couldn't save the world");
            break;
        }
        strncpy_s(madeSaveDir, saveDir, _TRUNCATE);
        TriggerSaveGame(liveSaveName);
        if (!WaitForSaveToFinish(0, &g_disconnectRequested)) {
            SendAll(js, MSG_ABORT, 0, -1, "host stopped hosting");
            break;
        }

        // 3. Pack.
        char zipPath[MAX_PATH] = {};
        _snprintf_s(zipPath, sizeof(zipPath), _TRUNCATE, "%s.republicmp.zip", saveDir);
        HostStep(3, "Packing the save", 0);
        SendAll(js, MSG_STEP, 3, 0, "packing the save");
        ProgressCtx packCtx = { INVALID_SOCKET, &js, 3, "packing the save", -100 };
        strncpy_s(madeZip, zipPath, _TRUNCATE);
        if (!ZipSaveDirectory(saveDir, zipPath, ZipProgressCb, &packCtx)) {
            SetSessionError("packing the save failed");
            SendAll(js, MSG_ABORT, 0, -1, "host couldn't pack the save");
            break;
        }
        if (g_disconnectRequested) {
            SendAll(js, MSG_ABORT, 0, -1, "host stopped hosting");
            break;
        }

        // 4. Send, to every player at once.
        SendWorldToAll(js, zipPath);
        if (!AliveCount(js)) {
            SetSessionError("sending the save failed - connection lost");
            break;
        }

        // Lockstep: this PC loads the same save while the players unpack and
        // load their copies; their step messages wait in the sockets.
        if (g_lockstep) {
            HostStep(5, several ? "Loading the shared world while the players load it" : "Loading the shared world while the player loads it");
            if (!Determinism_BeginSession(true)) {
                SetSessionError("lockstep is not available - see the log (determinism layer)");
                break;
            }
            TriggerLoadSave(liveSaveName);
            if (!WaitForLoadToFinish(0, &g_disconnectRequested) || !Determinism_SessionRunning()) {
                if (!g_disconnectRequested) SetSessionError("loading the shared world failed");
                ResetChannels();
                break;
            }
        }

        // 5-6. Each player unpacks and loads; its steps arrive as messages.
        for (Joiner& j : js) {
            if (!j.alive) continue;
            bool ready = false;
            for (;;) {
                SessionMsg m;
                if (!RecvMsg(j.sock, &m)) {
                    SaveStatus("[HOST] %s dropped while loading", NameOf(j));
                    break;
                }
                if (m.code == MSG_READY) { ready = true; break; }
                if (m.code == MSG_ABORT) {
                    SaveStatus("[HOST] %s gave up: %s", NameOf(j), m.text);
                    break;
                }
                if (m.code == MSG_STEP) {
                    char text[128];
                    _snprintf_s(text, sizeof(text), _TRUNCATE, "Waiting for %s - %s", NameOf(j), m.text);
                    if (m.step == 5) SyncBlock_SetStep(5, JOIN_STEPS, text, m.percent);
                    else HostStep(m.step, text, m.percent);
                }
            }
            if (!ready) DropJoiner(j);
        }
        int ready = AliveCount(js);
        if (!ready) {
            SetSessionError(several ? "no player finished loading" : "the player dropped while loading");
            if (g_lockstep) ResetChannels();
            break;
        }

        // 7. Lockstep: all loaded the same save; the players' channels connect.
        if (g_lockstep) {
            HostStep(7, ready > 1 ? "Waiting for the players' lockstep connections" : "Waiting for the player's lockstep connection");
            joined = LockstepNet_HostAccept(ready, LOCKSTEP_CONNECT_TIMEOUT_MS, &g_disconnectRequested, JoinersStillThere, &js);
            if (!joined) {
                if (!g_disconnectRequested) SetSessionError("the lockstep connection didn't arrive - TCP port %d must reach the host", LOCKSTEP_PORT);
                ResetChannels();
            } else if (joined < ready) {
                SaveStatus("[HOST] %d of %d player(s) opened the lockstep connection - going on without the rest", joined, ready);
            }
            break;
        }

        // 7. Replay: the live sync channels (one player).
        HostStep(7, "Starting live sync");
        StartChannelsHost();
        if (!WaitForChannels(false)) {
            if (!g_disconnectRequested) SetSessionError("the live sync channels didn't connect - TCP ports %d-%d must reach the host too (forward them like port %d)", BUILD_SYNC_PORT, TERRAIN_SYNC_PORT, SAVE_SYNC_PORT);
            ResetChannels();
            break;
        }
        joined = 1;
    } while (false);

    // The zip only existed to be sent.
    if (madeZip[0]) DeleteFileA(madeZip);
    if (madeSaveDir[0]) KeepOnlyNewestHostSave(madeSaveDir);

    SetJoinSockets(nullptr);
    for (Joiner& j : js) DropJoiner(j);
    SyncBlock_SetActive(false, nullptr);
    // Joined players start paused on every PC - any player resumes when all
    // are ready (issue #18); the speed sync carries it over.
    if (!joined) EnsureGameUnpaused();
    return joined;
}

// Replay mode: one player at a time.
static void RunHostSessionReplay()
{
    bool rejoin = false; // a re-sync: the player is expected back right away
    while (!g_disconnectRequested) {
        SetState(SESSION_HOST_LISTENING);
        if (!rejoin) {
            SetPeerIp("");
            SaveStatus("[HOST] hosting on port %d - waiting for a player (the game keeps running)", SAVE_SYNC_PORT);
        }

        // No accept timeout (cancellable) unless the player is coming back
        // from a re-sync; no I/O timeout - the player may take a while to
        // unpack and load.
        SOCKET sock = NetworkHostAcceptOne(SAVE_SYNC_PORT, rejoin ? RESYNC_REJOIN_TIMEOUT_MS : 0, 0,
                                           &g_disconnectRequested);
        if (sock == INVALID_SOCKET) {
            if (g_disconnectRequested) break;
            if (rejoin) {
                rejoin = false;
                g_resyncing = false;
                SetSessionError("the player didn't come back for the re-sync");
                SaveStatus("[HOST] the player didn't come back for the re-sync - waiting for a player again");
                continue;
            }
            SetSessionError("couldn't listen on port %d (already in use?)", SAVE_SYNC_PORT);
            break;
        }

        std::vector<Joiner> js(1);
        js[0].sock = sock;
        NetworkPeerAddress(sock, js[0].ip, sizeof(js[0].ip));
        js[0].nick[0] = 0;
        js[0].alive = true;
        SetPeerIp(js[0].ip);
        SetState(SESSION_HOST_JOINING);
        SetSessionError("");
        SaveStatus(rejoin ? "[HOST] %s is back - re-syncing, both games are paused until it is loaded"
                          : "[HOST] %s is joining - both games are paused until it is loaded", js[0].ip);

        bool joined = RunHostJoin(js) > 0;
        rejoin = false;
        g_resyncing = false;
        if (!joined) {
            if (g_disconnectRequested) break;
            SaveStatus("[HOST] the join failed (%s) - waiting for a player again", SaveSync_GetLastError());
            continue;
        }
        g_lastWorldSentTick = GetTickCount64();
        g_resyncRequested = false; // one asked for during the join is already done

        char peer[64];
        strncpy_s(peer, g_peerIp, _TRUNCATE);
        SetState(SESSION_HOST_IN_GAME);
        SaveStatus("[HOST] %s is in the game - live sync running, paused until either player resumes", peer);
        const char* why = MonitorLiveSession(false);
        ResetChannels();
        if (!why) break; // Stop hosting
        if (why == kResyncReason) {
            rejoin = true;
            SaveStatus("[HOST] re-syncing - waiting for %s to reconnect", peer);
            continue;
        }
        SetSessionError("%s left: %s", peer, why);
        SaveStatus("[HOST] %s left (%s) - waiting for a player again", peer, why);
    }
}

// Lockstep: up to Profile_GetMaxPlayers() - 1 players. The join port stays
// open while there is room (a full session stops listening); a player who
// connects during the game joins through a re-sync of everyone, so all load
// the same save again.
static SOCKET g_listener = INVALID_SOCKET;
static const char* const kJoinReason = "a player joins";

static bool OpenListener()
{
    if (g_listener == INVALID_SOCKET) g_listener = NetworkListen(SAVE_SYNC_PORT, PROFILE_MAX_PLAYERS);
    return g_listener != INVALID_SOCKET;
}

static void CloseListener(const char* why)
{
    if (g_listener == INVALID_SOCKET) return;
    NetworkCloseSocket(g_listener);
    g_listener = INVALID_SOCKET;
    Logf("republicmp  SESSION: %s - not listening on port %d", why, SAVE_SYNC_PORT);
}

static void TakeJoiner(SOCKET s, std::vector<Joiner>& batch)
{
    Joiner j = {};
    j.sock = s;
    NetworkPeerAddress(s, j.ip, sizeof(j.ip));
    j.alive = true;
    batch.push_back(j);
}

// The addresses of the players in the game (host side).
static std::vector<std::string> PlayerIps()
{
    std::vector<std::string> ips;
    LockstepPlayer players[PROFILE_MAX_PLAYERS];
    int n = LockstepNet_Players(players, PROFILE_MAX_PLAYERS);
    for (int i = 0; i < n; i++)
        if (!players[i].self) ips.push_back(players[i].ip);
    return ips;
}

// How many of the expected addresses have no connection in the batch yet
// (two players behind one address count twice).
static int MissingOf(const std::vector<Joiner>& batch, const std::vector<std::string>& expect)
{
    std::vector<bool> used(batch.size(), false);
    int missing = 0;
    for (const std::string& ip : expect) {
        bool found = false;
        for (size_t i = 0; i < batch.size() && !found; i++)
            if (!used[i] && ip == batch[i].ip) used[i] = found = true;
        if (!found) missing++;
    }
    return missing;
}

// Takes players into the batch, at most `room`. expect: the players coming
// back for a re-sync, waited for by address for timeoutMs, with their places
// kept (a newcomer is let in only while there is room besides them); none: a
// first player without a time limit, then GATHER_MORE_MS for others who
// connect with it.
static void Gather(std::vector<Joiner>& batch, int room, const std::vector<std::string>* expect, DWORD timeoutMs)
{
    ULONGLONG start = GetTickCount64(), firstAt = batch.empty() ? 0 : start;
    while (!g_disconnectRequested && (int)batch.size() < room) {
        ULONGLONG now = GetTickCount64();
        if (expect && (!MissingOf(batch, *expect) || now - start >= timeoutMs)) break;
        if (!expect && firstAt && now - firstAt >= GATHER_MORE_MS) break;
        SOCKET s = NetworkAcceptReady(g_listener, 200, 0);
        if (s == INVALID_SOCKET) continue;
        int missingBefore = expect ? MissingOf(batch, *expect) : 0;
        TakeJoiner(s, batch);
        if (expect && MissingOf(batch, *expect) == missingBefore && (int)batch.size() + missingBefore > room) {
            SaveStatus("[HOST] %s connects - no room left besides the players coming back", batch.back().ip);
            DropJoiner(batch.back());
            batch.pop_back();
            continue;
        }
        if (!firstAt) firstAt = GetTickCount64();
        SaveStatus("[HOST] %s connects", batch.back().ip);
    }
}

// Host, players in the game: returns why the game ended here (nullptr: Stop
// hosting), kResyncReason / kJoinReason (a newcomer is in `batch`).
static const char* MonitorLockstepHost(std::vector<Joiner>& batch)
{
    static char s_why[96];
    bool reported = false;
    ULONGLONG lastPing = 0;
    int players = LockstepNet_PeerCount();
    g_desyncSince = 0;
    while (!g_disconnectRequested) {
        ULONGLONG now = GetTickCount64();
        if (now - lastPing >= 1000) {
            LockstepNet_Ping();
            lastPing = now;
        }
        // A game that froze or crashed keeps its socket open while Windows
        // reports it: it is gone once it stays silent.
        LockstepNet_DropSilent(LOCKSTEP_SILENCE_MS);
        int count = LockstepNet_PeerCount();
        if (count != players) {
            if (count < players) SaveStatus("[HOST] a player left - %d player(s) in the game", count);
            players = count;
            SetPeerNamesFromLockstep();
        }
        if (!count) {
            _snprintf_s(s_why, sizeof(s_why), _TRUNCATE, "every player left");
            return s_why;
        }
        const char* desync = LockstepNet_Desync();
        if (desync[0] && !reported) {
            reported = true;
            g_desyncSince = now;
            SaveStatus("[LOCKSTEP] %s", desync);
        }
        // In lockstep the games match or not: a re-sync comes on request, or a set time after a desync.
        int delay = g_desyncResyncSeconds;
        bool onDesync = desync[0] && delay >= 0 && now - g_desyncSince >= (ULONGLONG)delay * 1000;
        if (g_resyncRequested || onDesync) {
            g_resyncRequested = false;
            SaveStatus(onDesync ? "[HOST] the games differ - re-syncing" : "[HOST] re-syncing");
            return kResyncReason;
        }
        // Room for more: a new player joins through a re-sync of everyone.
        if (count < Profile_GetMaxPlayers() - 1) {
            if (OpenListener()) {
                SOCKET s = NetworkAcceptReady(g_listener, 0, 0);
                if (s != INVALID_SOCKET) {
                    TakeJoiner(s, batch);
                    SaveStatus("[HOST] %s connects - re-syncing everyone so all load the same world", batch.back().ip);
                    return kJoinReason;
                }
            }
        } else {
            CloseListener("no room for more players");
        }
        Sleep(200);
    }
    return nullptr;
}

static void RunHostSessionLockstep()
{
    std::vector<Joiner> batch;
    std::vector<std::string> comingBack; // the players expected back for a re-sync (addresses)
    while (!g_disconnectRequested) {
        int room = Profile_GetMaxPlayers() - 1;
        if (!OpenListener()) {
            SetSessionError("couldn't listen on port %d (already in use?)", SAVE_SYNC_PORT);
            break;
        }
        SetState(SESSION_HOST_LISTENING);
        if (!comingBack.empty()) {
            Gather(batch, room, &comingBack, RESYNC_REJOIN_TIMEOUT_MS);
            int missing = MissingOf(batch, comingBack);
            if (missing) SaveStatus("[HOST] %d of %d player(s) came back for the re-sync", (int)comingBack.size() - missing, (int)comingBack.size());
        } else {
            SetPeerIp("");
            SaveStatus("[HOST] hosting on port %d - waiting for players (up to %d, the game keeps running)", SAVE_SYNC_PORT, room);
            Gather(batch, room, nullptr, 0);
        }
        comingBack.clear();
        if (g_disconnectRequested) break;
        if (batch.empty()) {
            g_resyncing = false;
            SetSessionError("nobody came back for the re-sync");
            SaveStatus("[HOST] nobody came back for the re-sync - waiting for players again");
            continue;
        }

        SetState(SESSION_HOST_JOINING);
        SetSessionError("");
        SetPeerNames(batch);
        SaveStatus(g_resyncing ? "[HOST] re-syncing %d player(s) - every game is paused until all are loaded"
                               : "[HOST] %d player(s) joining - every game is paused until all are loaded", (int)batch.size());
        // A player who connected now would wait unanswered until this join
        // is done: the port opens again once the game runs.
        CloseListener("taking players in");
        int joined = RunHostJoin(batch);
        batch.clear();
        g_resyncing = false;
        if (!joined) {
            if (g_disconnectRequested) break;
            SaveStatus("[HOST] the join failed (%s) - waiting for players again", SaveSync_GetLastError());
            continue;
        }
        g_lastWorldSentTick = GetTickCount64();
        g_resyncRequested = false; // one asked for during the join is already done

        SetState(SESSION_HOST_IN_GAME);
        SetPeerNamesFromLockstep();
        SaveStatus("[HOST] in the game with %s - lockstep running, paused until a player resumes", g_peerIp);
        const char* why = MonitorLockstepHost(batch);
        if (!why) break; // Stop hosting
        if (why == kResyncReason || why == kJoinReason) {
            comingBack = PlayerIps();
            LockstepNet_SendResync();
            Sleep(RESYNC_ANNOUNCE_FLUSH_MS);
            ResetChannels();
            g_resyncing = true;
            SaveStatus("[HOST] re-syncing - waiting for %d player(s) to reconnect", (int)comingBack.size());
            continue;
        }
        ResetChannels();
        SetSessionError("%s", why);
        SaveStatus("[HOST] %s - waiting for players again", why);
    }
    for (Joiner& j : batch) DropJoiner(j);
    CloseListener("stopped hosting");
}

static void RunHostSession()
{
    if (!NetworkInit()) {
        SetSessionError("network init failed");
        return;
    }
    g_lockstep = g_lockstepChosen;
    if (g_lockstep) RunHostSessionLockstep();
    else RunHostSessionReplay();
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

// WaitForLoadToFinish abort check: the host sends nothing while the client
// loads, so anything readable here is the connection closing or failing.
static bool JoinHostGone(void* sockPtr)
{
    return NetworkPeerGone(*(SOCKET*)sockPtr);
}

// Returns true when the player is in the host's game with all channels up.
// rejoin: coming back for a re-sync - the host may need a moment to listen
// again, so refused connects are retried.
static bool RunClientJoin(bool rejoin)
{
    g_resyncAnnounced = false;
    SyncBlock_SetActive(true, rejoin ? "Re-syncing with the host" : "Connecting to the host");
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
        ULONGLONG connectStart = GetTickCount64();
        for (;;) {
            sock = NetworkConnect(g_targetIp, SAVE_SYNC_PORT, 15000, 0, &g_disconnectRequested);
            if (sock != INVALID_SOCKET || !rejoin || g_disconnectRequested) break;
            if (GetTickCount64() - connectStart > RESYNC_REJOIN_TIMEOUT_MS) break;
            SyncBlock_SetStep(1, JOIN_STEPS, "Re-sync: waiting for the host to open for the players again", -1);
            Sleep(RESYNC_RETRY_MS);
        }
        if (sock == INVALID_SOCKET) {
            if (!g_disconnectRequested)
                SetSessionError("couldn't reach %s:%d - is the host hosting, with room for you? is the IP right? (while players are loading in, the host takes no one else - try again in a minute)", g_targetIp, SAVE_SYNC_PORT);
            break;
        }
        g_activeSock = sock;
        // A Disconnect that came while connecting found no socket to shut
        // down; without this the whole world would still be downloaded.
        if (g_disconnectRequested) break;
        SessionHello mine, theirs;
        MakeHello(&mine);
        if (!NetworkSendExact(sock, &mine, sizeof(mine))) {
            SetSessionError("the host dropped during the version check (old build?)");
            break;
        }
        // The host answers once it takes this player in: right away, or after
        // the players of a re-sync are back. A longer wait says why.
        if (!NetworkWaitReadable(sock, 1500, &g_disconnectRequested) && !g_disconnectRequested) {
            ClientStep(1, rejoin ? "Re-sync: connected, waiting for the other players to come back"
                                 : "Connected, waiting for the host to take you in (other players are connecting)");
        }
        if (!NetworkWaitReadable(sock, HELLO_ANSWER_TIMEOUT_MS, &g_disconnectRequested)) {
            if (!g_disconnectRequested)
                SetSessionError("the host didn't answer - it may be busy with other players joining; try again in a minute");
            break;
        }
        if (!NetworkRecvExact(sock, &theirs, sizeof(theirs))) {
            SetSessionError("the host dropped during the version check (old build, or the session is full?)");
            break;
        }
        char why[256];
        if (!HelloMatches(theirs, why, sizeof(why))) {
            SetSessionError("join refused: %s", why);
            SaveStatus("[CLIENT] join refused: %s", why);
            break;
        }
        SessionDlcs myDlcs, theirDlcs;
        MakeDlcs(&myDlcs);
        if (!NetworkSendExact(sock, &myDlcs, sizeof(myDlcs)) || !NetworkRecvExact(sock, &theirDlcs, sizeof(theirDlcs)) ||
            memcmp(theirDlcs.magic, "RPMPDLC", 8) != 0) {
            SetSessionError("the host dropped during the DLC check");
            break;
        }
        char dlcWhy[512];
        if (!DlcsMatch(myDlcs, theirDlcs, false, dlcWhy, sizeof(dlcWhy))) {
            SetSessionError("join refused: %s", dlcWhy);
            SaveStatus("[CLIENT] join refused: %s", dlcWhy);
            break;
        }
        char hostNick[PROFILE_NICK_BYTES];
        if (!ExchangeNicks(sock, hostNick, sizeof(hostNick))) {
            SetSessionError("the host dropped during the name exchange");
            break;
        }
        char peer[64];
        _snprintf_s(peer, sizeof(peer), _TRUNCATE, "%s (%s)", hostNick, g_targetIp);
        SetPeerIp(peer);
        g_lockstep = theirs.lockstep != 0;
        if (g_lockstep) SaveStatus("[CLIENT] %s runs this session in lockstep", hostNick);

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
        ProgressCtx recvCtx = { INVALID_SOCKET, nullptr, 4, nullptr, 0 };
        if (!NetworkReceiveFile(sock, zipPath, NetworkProgressCb, &recvCtx)) {
            SetSessionError("receiving the world failed - connection lost");
            break;
        }

        // 5. Unpack (relayed to the host).
        ClientStep(5, "Unpacking", 0);
        SendMsg(sock, MSG_STEP, 5, 0, "unpacking");
        ProgressCtx unpackCtx = { sock, nullptr, 5, "unpacking", -100 };
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
        if (g_lockstep && !Determinism_BeginSession(false)) {
            SetSessionError("lockstep is not available - see the log (determinism layer)");
            SendMsg(sock, MSG_ABORT, 0, -1, "lockstep is not available on the player");
            break;
        }
        TriggerLoadSave(receivedSaveName);
        if (!WaitForLoadToFinish(0, &g_disconnectRequested, JoinHostGone, &sock)) {
            if (g_disconnectRequested) SendMsg(sock, MSG_ABORT, 0, -1, "the player cancelled while loading");
            else SetSessionError("the host dropped while the world was loading");
            break;
        }

        // 7. Ready -> the host starts listening on the channels; connect them.
        if (g_lockstep) {
            ClientStep(7, "Waiting for the host to load the shared world");
            SendMsg(sock, MSG_READY, 7, -1, "");
            if (!Determinism_SessionRunning()) {
                SetSessionError("loading the shared world failed");
                break;
            }
            // In short tries, watching the join connection: the host listens
            // once every player has loaded (a slow download keeps the others
            // waiting), and closes it when it crashed or gave up on this one.
            bool lockstepUp = false, hostGone = false;
            while (!g_disconnectRequested) {
                if (LockstepNet_Join(g_targetIp, 6000, &g_disconnectRequested)) {
                    lockstepUp = true;
                    break;
                }
                if (JoinHostGone(&sock)) {
                    hostGone = true;
                    break;
                }
            }
            if (!lockstepUp) {
                if (hostGone) SetSessionError("the host dropped while the players were loading the shared world");
                else if (!g_disconnectRequested) SetSessionError("couldn't open the lockstep connection to %s:%d", g_targetIp, LOCKSTEP_PORT);
                break;
            }
            // The game starts once every player is connected (the host's
            // player list says so); until then nothing is done here that a
            // player still connecting would miss.
            SyncBlock_SetStep(7, JOIN_STEPS, "Waiting for the other players to connect", -1);
            ULONGLONG lastPing = 0;
            while (!g_disconnectRequested && LockstepNet_IsConnected() && !LockstepNet_SessionStarted()) {
                ULONGLONG now = GetTickCount64();
                if (now - lastPing >= 1000) {
                    LockstepNet_Ping();
                    lastPing = now;
                }
                Sleep(100);
            }
            if (!LockstepNet_SessionStarted()) {
                if (!g_disconnectRequested) SetSessionError("the host dropped while waiting for the other players");
                break;
            }
            joined = true;
            break;
        }
        ClientStep(7, "Starting live sync");
        SendMsg(sock, MSG_READY, 7, -1, "");
        if (!WaitForChannels(true)) {
            if (!g_disconnectRequested) SetSessionError("the live sync channels didn't connect - TCP ports %d-%d of the host are not reachable (only port %d is forwarded?)", BUILD_SYNC_PORT, TERRAIN_SYNC_PORT, SAVE_SYNC_PORT);
            ResetChannels();
            break;
        }
        joined = true;
    } while (false);

    g_activeSock = INVALID_SOCKET;
    if (sock != INVALID_SOCKET) NetworkCloseSocket(sock);
    SyncBlock_SetActive(false, nullptr);
    // Joined: stays paused until either player resumes (see RunHostJoin).
    if (!joined) EnsureGameUnpaused();
    return joined;
}

static void RunClientSession()
{
    SetPeerIp(g_targetIp);
    bool rejoin = false;
    for (;;) {
        SetState(SESSION_CLIENT_JOINING);
        bool joined = RunClientJoin(rejoin);
        g_resyncing = false;
        if (!joined) {
            ResetChannels();
            if (g_disconnectRequested) SaveStatus("[CLIENT] cancelled");
            else SaveStatus("[CLIENT] couldn't join: %s", SaveSync_GetLastError());
            return;
        }

        SetState(SESSION_CLIENT_IN_GAME);
        SaveStatus("[CLIENT] in the game of %s - live sync running, paused until a player resumes", g_peerIp);
        const char* why = MonitorLiveSession(true);
        ResetChannels();
        if (why == kResyncReason) {
            rejoin = true;
            SaveStatus("[CLIENT] the host is re-syncing the world - reconnecting");
            continue;
        }
        if (why) {
            SetSessionError("disconnected from the host: %s", why);
            SaveStatus("[CLIENT] disconnected from the host (%s)", why);
        } else {
            SaveStatus("[CLIENT] left the session");
        }
        return;
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

// The world's road network (world+0xf7a0): made with every world, an empty
// map too; deleted and zeroed when the world is torn down (0x9952C,
// 0x29564E) - back in the main menu, or while another save loads.
#define WORLD_ROAD_NETWORK_RVA 0x9E46B0
// Out of the world this long in a session ends it (a load's teardown is
// shorter, and joins/re-syncs are not checked at all).
#define OUT_OF_WORLD_GRACE_MS 1500

bool SaveSync_IsWorldLoaded()
{
    __try {
        return *(__int64*)((uintptr_t)g_exe + WORLD_ROAD_NETWORK_RVA) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SaveSync_StartHost()
{
    if (g_state != SESSION_IDLE) return;
    if (!SaveSync_IsWorldLoaded()) {
        // A joining player gets the host's world: there has to be one.
        SaveStatus("[HOST] no world loaded - load a city first, then host");
        SetSessionError("load a city before hosting");
        return;
    }
    g_disconnectRequested = false;
    g_isHost = true;
    g_resyncRequested = false;
    g_resyncing = false;
    g_sessionStartTick = GetTickCount64();
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
    g_resyncing = false;
    g_sessionStartTick = GetTickCount64();
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
    // Wakes a blocking recv on the join connections (closed by their owner).
    SOCKET s = g_activeSock;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_joinSockCount; i++) shutdown(g_joinSocks[i], SD_BOTH);
    LeaveCriticalSection(&g_lock);
    ResetChannels();
}

void SaveSync_RequestResync()
{
    if (g_isHost && g_state == SESSION_HOST_IN_GAME) g_resyncRequested = true;
}

void SaveSync_SetDesyncResyncSeconds(int seconds) { g_desyncResyncSeconds = seconds < 0 ? -1 : seconds; }
int SaveSync_GetDesyncResyncSeconds() { return g_desyncResyncSeconds; }

ULONGLONG SaveSync_GetMsUntilDesyncResync()
{
    int delay = g_desyncResyncSeconds;
    ULONGLONG since = g_desyncSince;
    if (!g_isHost || !since || delay < 0 || !LockstepNet_Desync()[0]) return 0;
    ULONGLONG elapsed = GetTickCount64() - since, wait = (ULONGLONG)delay * 1000;
    return elapsed >= wait ? 1 : wait - elapsed;
}

void SaveSync_SetAutoResyncMinutes(int minutes)
{
    g_autoResyncMinutes = minutes > 0 ? minutes : 0;
}

int SaveSync_GetAutoResyncMinutes() { return g_autoResyncMinutes; }

ULONGLONG SaveSync_GetMsUntilAutoResync()
{
    int minutes = g_autoResyncMinutes;
    if (!g_isHost || g_state != SESSION_HOST_IN_GAME || minutes <= 0) return 0;
    ULONGLONG interval = (ULONGLONG)minutes * 60000;
    ULONGLONG elapsed = GetTickCount64() - g_lastWorldSentTick;
    return elapsed >= interval ? 1 : interval - elapsed;
}

ULONGLONG SaveSync_GetMsSinceWorldSent()
{
    if (!g_isHost || g_state != SESSION_HOST_IN_GAME) return 0;
    return GetTickCount64() - g_lastWorldSentTick;
}

bool SaveSync_IsResyncing() { return g_resyncing; }

void SaveSync_OnResyncAnnounced()
{
    g_resyncing = true;
    g_resyncAnnounced = true;
}

// Leaving the world (main menu, another save) ends the session: the other
// player would otherwise stay connected to a game that is gone. Joins and
// re-syncs load a world themselves and are not checked.
static void CheckStillInWorld()
{
    static ULONGLONG s_outSince = 0;
    SessionState st = g_state;
    bool watched = st == SESSION_HOST_LISTENING || st == SESSION_HOST_IN_GAME || st == SESSION_CLIENT_IN_GAME;
    if (!watched || g_disconnectRequested || SaveSync_IsWorldLoaded()) {
        s_outSince = 0;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (!s_outSince) {
        s_outSince = now;
        return;
    }
    if (now - s_outSince < OUT_OF_WORLD_GRACE_MS) return;
    s_outSince = 0;
    bool host = g_isHost;
    SaveStatus(host ? "[HOST] left the world - stopping hosting" : "[CLIENT] left the world - disconnecting");
    SaveSync_RequestDisconnect();
    SetSessionError(host ? "hosting stopped: you left the world" : "disconnected: you left the world");
}

void SaveSync_Tick()
{
    CheckStillInWorld();
    if (!g_resyncSendNow || g_resyncSent) return;
    if (!BuildSync_IsConnected()) return; // AnnounceResync gives up after its timeout
    BuildSync_SendResync();
    if (BuildSync_IsConnected()) g_resyncSent = true;
}

SessionState SaveSync_GetState() { return g_state; }
void SaveSync_SetLockstep(bool on) { g_lockstepChosen = on; }
void SaveSync_SetDlcCheck(bool on) { g_dlcCheck = on; }
bool SaveSync_GetDlcCheck() { return g_dlcCheck; }
bool SaveSync_GetLockstep() { return g_lockstepChosen; }
bool SaveSync_IsLockstepSession() { return g_lockstep && g_state != SESSION_IDLE; }
bool SaveSync_IsBusy()           { return g_state != SESSION_IDLE; }
bool SaveSync_IsHostRole()       { return g_isHost; }
ULONGLONG SaveSync_GetSessionAgeMs() { return g_state == SESSION_IDLE ? 0 : GetTickCount64() - g_sessionStartTick; }
bool SaveSync_IsHostAwaitingClient() { return g_state == SESSION_HOST_LISTENING; }
const char* SaveSync_GetTargetIp() { return g_targetIp; }

// The returned buffers are only ever rewritten whole by the session thread;
// the UI reads them once per frame.
const char* SaveSync_GetPeerIp()    { return g_peerIp; }
const char* SaveSync_GetLastError() { return g_lastError; }
