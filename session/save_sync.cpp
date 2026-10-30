// SPDX-License-Identifier: GPL-3.0-only
// See save_sync.h.
#include "save_sync.h"
#include "../core/version.h"
#include "game_speed.h"
#include "../game/save_load.h"
#include "../save/save_zip.h"
#include "../ui/ui_log.h"
#include "../ui/sync_block.h"
#include "../third_party/miniz/miniz.h" // mz_crc32 for the build check
#include "../transport/network.h"
#include "../determinism/determinism.h"
#include "../game/world.h"
#include "../lockstep/lockstep_net.h"
#include "../core/profile.h"
#include "../core/guard.h"
#include "workshop.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <wctype.h>

#pragma comment(lib, "User32.lib")

// Fixed local name for the downloaded zip and its extraction folder on the client.
#define SAVE_SYNC_CLIENT_STAGING_NAME "mp_syncsave_incoming"

#define JOIN_STEPS 7

static void LoadSettings();

void SaveSync_Bind(const TsmHost* host)
{
    TsmBind(host);
    LoadSettings();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static volatile SessionState g_state = SESSION_IDLE;
static volatile bool g_isHost = false;
static volatile int g_gameMode = SESSION_MODE_COOP; // SaveSync_GetGameMode
static volatile int g_gameVariant = SESSION_REPUBLICS_EAST_WEST; // SaveSync_GetGameVariant
static volatile int g_hostSide = 0; // SaveSync_GetHostSide
static char g_targetIp[64] = SAVE_SYNC_TEST_HOST_IP; // client target - overridable from the UI
static char g_peerIp[64] = "";
static char g_lastError[256] = "";

// Set by SaveSync_RequestDisconnect; reset at the start of every session.
static volatile bool g_disconnectRequested = false;

// The join connection in use, so Disconnect can break a blocking recv.
static volatile NetLink g_activeSock = NET_NO_LINK;

// GetTickCount64 when the current session was started (Host / Connect).
static volatile ULONGLONG g_sessionStartTick = 0;

// Re-sync (see save_sync.h).
#define RESYNC_ANNOUNCE_FLUSH_MS 500    // host: lets the announcement leave before the channels close
#define RESYNC_REJOIN_TIMEOUT_MS 30000  // host: how long the players have to come back; client: how long it retries
#define RESYNC_RETRY_MS 500
#define JOIN_RETRY_TIMEOUT_MS 180000 // client, first join: how long a refused connect is retried (the host
                                     // loads its city, or takes another player in with its port closed)
static volatile bool g_resyncRequested = false; // host: the button
static volatile bool g_resyncing = false;
static volatile int g_desyncResyncSeconds = 60;    // re-sync this long after a desync (-1 off)
static volatile ULONGLONG g_desyncSince = 0;       // GetTickCount64 of the desync this session saw (0 none)
static volatile ULONGLONG g_lastWorldSentTick = 0; // host: when the players last got the world
// Every PC loads the sent save with the determinism layer on and runs the
// same ticks (lockstep/lockstep_net).
static volatile bool g_dlcCheck = true; // refuse a join when the DLCs differ (both players' choice)
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
    // The log too: an error shown only on the page left nothing behind.
    if (body[0]) Logf("republicmp  SESSION: error shown - %s", body);
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
    int gameMode;      // the host's SESSION_MODE_*, its variant << 8, its side << 16; a player's is not read
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

static bool SendMsg(NetLink sock, int code, int step, int percent, const char* text)
{
    SessionMsg m = {};
    m.code = code;
    m.step = step;
    m.percent = percent;
    strncpy_s(m.text, text ? text : "", _TRUNCATE);
    return NetworkSendExact(sock, &m, sizeof(m));
}

static bool RecvMsg(NetLink sock, SessionMsg* m)
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
    h->gameMode = g_gameMode | g_gameVariant << 8 | g_hostSide << 16;
}

// Compares the peer's hello with ours; fills why on a mismatch.
static bool HelloMatches(const SessionHello& peer, char* why, size_t whySize)
{
    if (memcmp(peer.magic, "RPMPSES", 8) != 0) {
        _snprintf_s(why, whySize, _TRUNCATE, "not a compatible RepublicMP - an old build?");
        return false;
    }
    char peerVersion[17] = {};
    memcpy(peerVersion, peer.version, 16);
    if (peer.protocol != REPUBLICMP_PROTOCOL || strcmp(peerVersion, REPUBLICMP_VERSION) != 0 ||
        peer.dllCrc != SaveSync_GetLocalDllCrc()) {
        _snprintf_s(why, whySize, _TRUNCATE,
                    "different builds: yours %s (%08X, protocol %d), theirs %s (%08X, protocol %d). Use the same RepublicMP.dll",
                    REPUBLICMP_VERSION, SaveSync_GetLocalDllCrc(), REPUBLICMP_PROTOCOL,
                    peerVersion, peer.dllCrc, peer.protocol);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The lockstep channel
// ---------------------------------------------------------------------------

static void CloseLockstep()
{
    LockstepNet_Close();
    Determinism_EndSession();
}

// Player, in the game: blocks while the session is live. Returns why it
// ended (nullptr = the local player stopped it, kResyncReason = a re-sync the
// host announced).
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

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

// A player in a host's join (several come together: a re-sync brings
// everyone back at once).
struct Joiner {
    NetLink sock;
    char ip[64];
    char nick[PROFILE_NICK_BYTES];
    bool alive;
};

// Shared by the progress callbacks: the step's bar, plus (optionally) a
// relay of the percentage to the other side every 10% (the client to the
// host, or the host to every player of a join).
struct ProgressCtx {
    NetLink relaySock;             // NET_NO_LINK = do not relay
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
    if (ctx->relaySock != NET_NO_LINK) SendMsg(ctx->relaySock, MSG_STEP, ctx->step, percent, ctx->text);
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

// Wide names and a walk that never throws: path::string() throws on a name
// the code page cannot hold, and one such file or folder anywhere in
// media_soviet ended the player's game at the join's DLC check.
static void ListDlcs(char* out, size_t outSize)
{
    out[0] = 0;
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(g_exe, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return;
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash) return;
    *lastSlash = L'\0';
    std::vector<std::string> names;
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(exePath) / L"media_soviet", ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (name.size() < 3 || _wcsnicmp(name.c_str(), L"dlc", 3) != 0) continue;
        std::error_code dirEc;
        if (!it->is_directory(dirEc) || dirEc) continue;
        if (std::filesystem::directory_iterator(it->path(), dirEc) == std::filesystem::directory_iterator()) continue; // empty
        std::string ascii;
        for (wchar_t c : name) ascii += c < 128 ? (char)towlower(c) : '?';
        names.push_back(ascii);
    }
    if (ec) Logf("republicmp  SESSION: the DLC folders could not be listed completely: %s", ec.message().c_str());
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
                "different DLCs: host [%s], player [%s]. %s",
                hostList[0] ? hostList : "none", playerList[0] ? playerList : "none",
                refuse ? "Install the same DLCs, or set Check DLCs to OFF" : "The DLC check is off - playing anyway");
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

// ---------------------------------------------------------------------------
// Workshop items (session/workshop)
// ---------------------------------------------------------------------------
//
// After the DLCs both send their Steam Workshop list; the player compares,
// tells the host whether it goes on (and why not), and downloads what it
// lacks. Each side's Sync Workshop is its own: a host with it off offers
// nothing, a player with it off downloads nothing - neither refuses a join.

struct SessionWorkshopHead {
    char magic[8];  // "RPMPWSL"
    unsigned count; // WorkshopItem entries that follow
    unsigned steam; // WORKSHOP_LIST_*
};
enum { WORKSHOP_LIST_NONE = 0, WORKSHOP_LIST_SENT = 1, WORKSHOP_LIST_NOT_OFFERED = 2 }; // no Steam / a list / Sync Workshop off
struct SessionWorkshopVerdict {
    char magic[8]; // "RPMPWSV"
    int ok;        // the player goes on
    char why[248]; // why not, or what it was warned about
};

static bool SendWorkshop(NetLink sock, const std::vector<WorkshopItem>& items, unsigned state)
{
    SessionWorkshopHead h = {};
    memcpy(h.magic, "RPMPWSL", 8);
    h.count = state == WORKSHOP_LIST_SENT ? (unsigned)items.size() : 0;
    h.steam = state;
    return NetworkSendExact(sock, &h, sizeof(h)) &&
           (!h.count || NetworkSendExact(sock, items.data(), h.count * sizeof(WorkshopItem)));
}

static bool RecvWorkshop(NetLink sock, std::vector<WorkshopItem>& items, unsigned* state)
{
    SessionWorkshopHead h = {};
    if (!NetworkRecvExact(sock, &h, sizeof(h)) || memcmp(h.magic, "RPMPWSL", 8) != 0 || h.count > 20000) return false;
    items.resize(h.count);
    *state = h.steam;
    return items.empty() || NetworkRecvExact(sock, items.data(), items.size() * sizeof(WorkshopItem));
}

// Player: whether to go on with the host's items. With Sync Workshop on,
// what it lacks opens the choice screen (ui/workshop_panel) and this join
// ends; the screen downloads and joins again, or joins again without them.
// Only that screen stops a join: the differences are otherwise only said.
static bool WorkshopVerdict(const std::vector<WorkshopItem>& host, const std::vector<WorkshopItem>& mine, unsigned hostState,
                            bool mySteam, char* why, size_t whySize)
{
    why[0] = 0;
    if (hostState == WORKSHOP_LIST_NOT_OFFERED) {
        _snprintf_s(why, whySize, _TRUNCATE, "the host does not offer its Workshop items (its Sync Workshop is off)");
        return true;
    }
    if (hostState != WORKSHOP_LIST_SENT || !mySteam) {
        _snprintf_s(why, whySize, _TRUNCATE, "Workshop items not compared (no Steam Workshop on one side)");
        return true;
    }
    WorkshopDiff d = Workshop_Compare(host, mine);
    Logf("republicmp  SESSION: Workshop - host %zu item(s), player %zu; missing %zu, older %zu, extra %zu", host.size(),
         mine.size(), d.missing.size(), d.older.size(), d.extra.size());
    if (!d.missing.empty()) Logf("republicmp  SESSION:   missing: %s", Workshop_Ids(Workshop_IdsOf(d.missing), 200).c_str());
    if (!d.older.empty()) Logf("republicmp  SESSION:   older here: %s", Workshop_Ids(Workshop_IdsOf(d.older), 200).c_str());
    if (!d.extra.empty()) Logf("republicmp  SESSION:   extra here (the host has not): %s", Workshop_Ids(d.extra, 200).c_str());
    // What the player declined on the screen is not offered again.
    std::vector<WorkshopItem> need;
    size_t declined = 0;
    for (const std::vector<WorkshopItem>* list : { &d.missing, &d.older })
        for (const WorkshopItem& i : *list) {
            if (Workshop_Declined(i.id)) declined++;
            else need.push_back(i);
        }
    size_t lacking = need.size();
    if (!lacking && declined) {
        _snprintf_s(why, whySize, _TRUNCATE, "joined without %zu of the host's Workshop item(s) (the player's choice)", declined);
        return true;
    }
    if (lacking && Workshop_GetSync()) {
        Workshop_Offer(need, d.extra.size());
        _snprintf_s(why, whySize, _TRUNCATE, "choosing %zu Workshop item(s) of the host to download", need.size());
        return false;
    }
    if (lacking) {
        _snprintf_s(why, whySize, _TRUNCATE, "%zu Workshop item(s) of the host missing or older here - not downloaded "
                    "(Sync Workshop is off): %s", lacking, Workshop_Ids(Workshop_IdsOf(need), 5).c_str());
        return true;
    }
    if (!d.extra.empty())
        _snprintf_s(why, whySize, _TRUNCATE, "%zu Workshop item(s) here the host has not - they may change the content: %s",
                    d.extra.size(), Workshop_Ids(d.extra, 5).c_str());
    return true;
}

struct SessionNick {
    char magic[8]; // "RPMPNIK"
    char nick[PROFILE_NICK_BYTES];
};

// Sends this player's name and takes the other side's; false: the connection is gone.
static bool ExchangeNicks(NetLink sock, char* theirs, size_t theirsSize)
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
// Wide: the save folder also holds the player's own saves, whose names the
// code page may not hold (path::string() throws on them).
static bool IsHostJoinSaveName(const std::wstring& name)
{
    const wchar_t* prefix = L"mp_syncsave_";
    size_t n = wcslen(prefix);
    if (name.size() != n + 8 || name.compare(0, n, prefix) != 0) return false;
    for (size_t i = n; i < name.size(); i++)
        if (!iswxdigit(name[i])) return false;
    return true;
}

static void KeepOnlyNewestHostSave(const char* newSaveDir)
{
    std::error_code ec;
    std::filesystem::path keep(newSaveDir);
    const std::wstring keepName = keep.filename().wstring();
    std::filesystem::directory_iterator it(keep.parent_path(), ec), end;
    int deleted = 0;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code dirEc;
        if (!it->is_directory(dirEc) || dirEc) continue;
        const std::wstring name = it->path().filename().wstring();
        if (!IsHostJoinSaveName(name) || _wcsicmp(name.c_str(), keepName.c_str()) == 0) continue;
        std::error_code rm;
        std::filesystem::remove_all(it->path(), rm);
        if (rm) Logf("republicmp  SESSION: couldn't delete the old join save [%ls]: %s", name.c_str(), rm.message().c_str());
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
    if (j.sock != NET_NO_LINK) NetworkClose(j.sock);
    j.sock = NET_NO_LINK;
    j.alive = false;
}

// The join connections in use, so Disconnect can break their blocking recvs.
static NetLink g_joinSocks[PROFILE_MAX_PLAYERS];
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
    // With Sync Workshop off the host offers nothing (session/workshop).
    std::vector<WorkshopItem> myItems, theirItems;
    unsigned myState = !Workshop_GetSync() ? WORKSHOP_LIST_NOT_OFFERED
                       : Workshop_List(myItems) ? WORKSHOP_LIST_SENT : WORKSHOP_LIST_NONE;
    unsigned theirState = WORKSHOP_LIST_NONE;
    SessionWorkshopVerdict verdict = {};
    if (!SendWorkshop(j.sock, myItems, myState) || !RecvWorkshop(j.sock, theirItems, &theirState) ||
        !NetworkRecvExact(j.sock, &verdict, sizeof(verdict)) || memcmp(verdict.magic, "RPMPWSV", 8) != 0) {
        SetSessionError("%s dropped during the Workshop check", j.ip);
        return false;
    }
    verdict.why[sizeof(verdict.why) - 1] = 0;
    if (!verdict.ok) {
        SetSessionError("join refused: %s", verdict.why);
        SaveStatus("[HOST] %s: join refused: %s", j.ip, verdict.why);
        return false;
    }
    if (verdict.why[0]) SaveStatus("[HOST] %s: %s", j.ip, verdict.why);
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
    w->ok = false;
    Guard_Call("sending the world", [w] {
        w->ok = SendMsg(w->j->sock, MSG_FILE_FOLLOWS, 4, -1, "") &&
                NetworkSendFile(w->j->sock, w->zipPath, WorldSendProgress, w);
    });
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
// the game (connected to the lockstep channel). The connections are closed.
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
        ProgressCtx packCtx = { NET_NO_LINK, &js, 3, "packing the save", -100 };
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

        // This PC loads the same save while the players unpack and load
        // their copies; their step messages wait in the sockets.
        HostStep(5, several ? "Loading the shared world while the players load it" : "Loading the shared world while the player loads it");
        if (!Determinism_BeginSession(true)) {
            SetSessionError("lockstep is not available - see the log (determinism layer)");
            break;
        }
        TriggerLoadSave(liveSaveName);
        if (!WaitForLoadToFinish(0, &g_disconnectRequested) || !Determinism_SessionRunning()) {
            if (!g_disconnectRequested) SetSessionError("loading the shared world failed");
            CloseLockstep();
            break;
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
            CloseLockstep();
            break;
        }

        // 7. All loaded the same save; the players' lockstep connections come.
        HostStep(7, ready > 1 ? "Waiting for the players' lockstep connections" : "Waiting for the player's lockstep connection");
        joined = LockstepNet_HostAccept(ready, LOCKSTEP_CONNECT_TIMEOUT_MS, &g_disconnectRequested, JoinersStillThere, &js);
        if (!joined) {
            if (!g_disconnectRequested) SetSessionError("the player's game link didn't arrive - TCP port %d must reach this PC", LOCKSTEP_PORT);
            CloseLockstep();
        } else if (joined < ready) {
            SaveStatus("[HOST] %d of %d player(s) opened the lockstep connection - going on without the rest", joined, ready);
        }
    } while (false);

    // The zip only existed to be sent.
    if (madeZip[0]) DeleteFileA(madeZip);
    if (madeSaveDir[0]) KeepOnlyNewestHostSave(madeSaveDir);

    SetJoinSockets(nullptr);
    for (Joiner& j : js) DropJoiner(j);
    SyncBlock_SetActive(false, nullptr);
    // Joined players start paused on every PC - any player resumes when all
    // are ready (issue #18).
    if (!joined) EnsureGameUnpaused();
    return joined;
}

// The host: up to SaveSync_MaxPlayers() - 1 players. The join port stays
// open while there is room (a full session stops listening); a player who
// connects during the game joins through a re-sync of everyone, so all load
// the same save again.
static NetLink g_listener = NET_NO_LINK;
static const char* const kJoinReason = "a player joins";

static bool OpenListener()
{
    if (g_listener == NET_NO_LINK) g_listener = NetworkListen(SAVE_SYNC_PORT, PROFILE_MAX_PLAYERS);
    return g_listener != NET_NO_LINK;
}

static void CloseListener(const char* why)
{
    if (g_listener == NET_NO_LINK) return;
    NetworkClose(g_listener);
    g_listener = NET_NO_LINK;
    Logf("republicmp  SESSION: %s - not listening on port %d", why, SAVE_SYNC_PORT);
}

static void TakeJoiner(NetLink s, std::vector<Joiner>& batch)
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
        NetLink s = NetworkAcceptReady(g_listener, 200, 0);
        if (s == NET_NO_LINK) continue;
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
        // The games match or not: a re-sync comes on request, or a set time after a desync.
        int delay = g_desyncResyncSeconds;
        bool onDesync = desync[0] && delay >= 0 && now - g_desyncSince >= (ULONGLONG)delay * 1000;
        if (g_resyncRequested || onDesync) {
            g_resyncRequested = false;
            SaveStatus(onDesync ? "[HOST] the games differ - re-syncing" : "[HOST] re-syncing");
            return kResyncReason;
        }
        // Room for more: a new player joins through a re-sync of everyone.
        if (count < SaveSync_MaxPlayers() - 1) {
            if (OpenListener()) {
                NetLink s = NetworkAcceptReady(g_listener, 0, 0);
                if (s != NET_NO_LINK) {
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

static void RunHostSessionPlayers()
{
    std::vector<Joiner> batch;
    std::vector<std::string> comingBack; // the players expected back for a re-sync (addresses)
    while (!g_disconnectRequested) {
        int room = SaveSync_MaxPlayers() - 1;
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
            CloseLockstep();
            g_resyncing = true;
            SaveStatus("[HOST] re-syncing - waiting for %d player(s) to reconnect", (int)comingBack.size());
            continue;
        }
        CloseLockstep();
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
    RunHostSessionPlayers();
    CloseLockstep();
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
    return NetworkPeerGone(*(NetLink*)sockPtr);
}

// Returns true when the player is in the host's game with the lockstep
// connection up. rejoin: coming back for a re-sync - the host may need a
// moment to listen again, so refused connects are retried.
static bool RunClientJoin(bool rejoin)
{
    SyncBlock_SetActive(true, rejoin ? "Re-syncing with the host" : "Connecting to the host");
    EnsureGamePaused();

    NetLink sock = NET_NO_LINK;
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
        // Refused connects are retried: for a re-sync the host opens again in a
        // moment; for a first join it may still be loading its city, or taking
        // another player in (its port is closed until that player is in).
        ULONGLONG connectStart = GetTickCount64();
        ULONGLONG retryFor = rejoin ? RESYNC_REJOIN_TIMEOUT_MS : JOIN_RETRY_TIMEOUT_MS;
        for (;;) {
            sock = NetworkConnect(g_targetIp, SAVE_SYNC_PORT, 15000, 0, &g_disconnectRequested);
            if (sock != NET_NO_LINK || g_disconnectRequested) break;
            if (GetTickCount64() - connectStart > retryFor) break;
            SyncBlock_SetStep(1, JOIN_STEPS,
                              rejoin ? "Re-sync: waiting for the host to open for the players again"
                                     : "Waiting for the host - it is loading, or letting another player in",
                              -1);
            Sleep(RESYNC_RETRY_MS);
        }
        if (sock == NET_NO_LINK) {
            if (!g_disconnectRequested)
                SetSessionError("can't reach %s - check the address and that the host is hosting with a free slot. Try again in a minute", g_targetIp);
            break;
        }
        g_activeSock = sock;
        // A Disconnect that came while connecting found no socket to shut
        // down; without this the whole world would still be downloaded.
        if (g_disconnectRequested) break;
        SessionHello mine, theirs;
        MakeHello(&mine);
        if (!NetworkSendExact(sock, &mine, sizeof(mine))) {
            SetSessionError("the host closed the link at the version check - another build?");
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
                SetSessionError("the host didn't answer - it may be letting others in. Try again in a minute");
            break;
        }
        if (!NetworkRecvExact(sock, &theirs, sizeof(theirs))) {
            SetSessionError("the host closed the link - the game may be full, or on another build");
            break;
        }
        char why[256];
        if (!HelloMatches(theirs, why, sizeof(why))) {
            SetSessionError("join refused: %s", why);
            SaveStatus("[CLIENT] join refused: %s", why);
            break;
        }
        g_gameMode = (theirs.gameMode & 0xFF) == SESSION_MODE_REPUBLICS ? SESSION_MODE_REPUBLICS : SESSION_MODE_COOP;
        g_gameVariant = (theirs.gameMode >> 8 & 0xFF) == SESSION_REPUBLICS_EQUAL ? SESSION_REPUBLICS_EQUAL
                                                                                 : SESSION_REPUBLICS_EAST_WEST;
        g_hostSide = (theirs.gameMode >> 16 & 0xFF) == 1 ? 1 : 0;
        SaveStatus("[CLIENT] the host plays %s", g_gameMode != SESSION_MODE_REPUBLICS ? "co-op"
                                                 : g_gameVariant == SESSION_REPUBLICS_EQUAL ? "Republics, equal"
                                                                                            : "Republics, East vs West");
        SyncBlock_SetStep(1, JOIN_STEPS, "Checking the DLCs", -1);
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
        SyncBlock_SetStep(1, JOIN_STEPS, "Checking the Workshop items", -1);
        std::vector<WorkshopItem> myItems, hostItems;
        bool mySteam = Workshop_List(myItems);
        unsigned hostState = WORKSHOP_LIST_NONE;
        if (!SendWorkshop(sock, myItems, mySteam ? WORKSHOP_LIST_SENT : WORKSHOP_LIST_NONE) ||
            !RecvWorkshop(sock, hostItems, &hostState)) {
            SetSessionError("the host dropped during the Workshop check");
            break;
        }
        SessionWorkshopVerdict verdict = {};
        memcpy(verdict.magic, "RPMPWSV", 8);
        verdict.ok = WorkshopVerdict(hostItems, myItems, hostState, mySteam, verdict.why, sizeof(verdict.why)) ? 1 : 0;
        bool sent = NetworkSendExact(sock, &verdict, sizeof(verdict));
        if (!verdict.ok) {
            // The choice screen says what to do; nothing on the page.
            if (Workshop_Phase() == WORKSHOP_CLOSED) SetSessionError("%s", verdict.why);
            SaveStatus("[CLIENT] %s", verdict.why);
            break;
        }
        if (!sent) {
            SetSessionError("the host dropped during the Workshop check");
            break;
        }
        if (verdict.why[0]) SaveStatus("[CLIENT] %s", verdict.why);
        char hostNick[PROFILE_NICK_BYTES];
        if (!ExchangeNicks(sock, hostNick, sizeof(hostNick))) {
            SetSessionError("the host dropped during the name exchange");
            break;
        }
        char peer[64];
        _snprintf_s(peer, sizeof(peer), _TRUNCATE, "%s (%s)", hostNick, g_targetIp);
        SetPeerIp(peer);

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
        ProgressCtx recvCtx = { NET_NO_LINK, nullptr, 4, nullptr, 0 };
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
        if (!Determinism_BeginSession(false)) {
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

        // 7. Loaded -> the lockstep connection to the host.
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
            if (hostGone) SetSessionError("the host left while the players were loading");
            else if (!g_disconnectRequested) SetSessionError("couldn't open the game link to %s (port %d)", g_targetIp, LOCKSTEP_PORT);
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
            // The host may have said why (the games' content differs).
            if (!g_disconnectRequested)
                SetSessionError("%s", LockstepNet_Desync()[0] ? LockstepNet_Desync() : "the host dropped while waiting for the other players");
            break;
        }
        joined = true;
    } while (false);

    g_activeSock = NET_NO_LINK;
    if (sock != NET_NO_LINK) NetworkClose(sock);
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
            CloseLockstep();
            if (g_disconnectRequested) SaveStatus("[CLIENT] cancelled");
            else if (Workshop_Phase() != WORKSHOP_CLOSED) SaveStatus("[CLIENT] the join waits for the Workshop screen");
            else SaveStatus("[CLIENT] couldn't join: %s", SaveSync_GetLastError());
            return;
        }

        SetState(SESSION_CLIENT_IN_GAME);
        SaveStatus("[CLIENT] in the game of %s - lockstep running, paused until a player resumes", g_peerIp);
        const char* why = MonitorLockstep(true);
        CloseLockstep();
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

// After an exception left a session (core/guard.h): what the session's own
// ends do, each step guarded too - the game goes on outside a session.
static void EndFailedSession()
{
    SetSessionError("the session stopped on an error - see the log (GUARD)");
    SaveStatus("[%s] the session stopped on an error (see the GUARD line above)", g_isHost ? "HOST" : "CLIENT");
    Guard_Call("the failed session's cleanup", [] {
        NetLink s = g_activeSock;
        g_activeSock = NET_NO_LINK;
        if (s != NET_NO_LINK) NetworkClose(s);
        if (g_listener != NET_NO_LINK) {
            NetworkClose(g_listener);
            g_listener = NET_NO_LINK;
        }
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_joinSockCount; i++) NetworkShutdown(g_joinSocks[i]);
        g_joinSockCount = 0;
        LeaveCriticalSection(&g_lock);
        CloseLockstep();
        SyncBlock_SetActive(false, nullptr);
        EnsureGameUnpaused();
    });
}

static DWORD WINAPI HostSessionThread(LPVOID)
{
    if (!Guard_Call("the host session", [] { RunHostSession(); })) EndFailedSession();
    SetState(SESSION_IDLE);
    return 0;
}

static DWORD WINAPI ClientSessionThread(LPVOID)
{
    if (!Guard_Call("the join", [] { RunClientSession(); })) EndFailedSession();
    SetPeerIp("");
    SetState(SESSION_IDLE);
    return 0;
}

// Out of the world this long in a session ends it (a load's teardown is
// shorter, and joins/re-syncs are not checked at all).
#define OUT_OF_WORLD_GRACE_MS 1500

// The world's road network (world+0xf7a0) is made with every world, an empty
// map too, and zeroed when the world is torn down (0x9952C, 0x29564E) - back
// in the main menu, or while another save loads (game/world).
bool SaveSync_IsWorldLoaded()
{
    return World_IsLoaded();
}

int SaveSync_GetGameMode() { return g_gameMode; }
int SaveSync_GetGameVariant() { return g_gameVariant; }
int SaveSync_GetHostSide() { return g_hostSide; }

int SaveSync_MaxPlayers()
{
    return g_gameMode == SESSION_MODE_REPUBLICS ? SESSION_REPUBLICS_PLAYERS : Profile_GetMaxPlayers();
}

void SaveSync_StartHost(int gameMode)
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
    g_gameMode = gameMode;
    g_gameVariant = Profile_GetInt("profile", "republics_variant", 0) == SESSION_REPUBLICS_EQUAL
                        ? SESSION_REPUBLICS_EQUAL
                        : SESSION_REPUBLICS_EAST_WEST;
    g_hostSide = Profile_GetInt("profile", "republics_side", 0) == 1 ? 1 : 0;
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
    g_gameMode = SESSION_MODE_COOP; // the host's hello says
    g_resyncing = false;
    g_sessionStartTick = GetTickCount64();
    SetSessionError("");
    SetState(SESSION_CLIENT_JOINING);
    CreateThread(nullptr, 0, ClientSessionThread, nullptr, 0, nullptr);
}

void SaveSync_RequestDisconnect()
{
    if (g_state == SESSION_IDLE) return;
    SaveStatus(g_isHost ? "[HOST] stopping..." : "[CLIENT] disconnecting...");
    g_disconnectRequested = true;
    // Wakes a blocking recv on the join connections (closed by their owner).
    NetLink s = g_activeSock;
    if (s != NET_NO_LINK) NetworkShutdown(s);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_joinSockCount; i++) NetworkShutdown(g_joinSocks[i]);
    LeaveCriticalSection(&g_lock);
    CloseLockstep();
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

bool SaveSync_IsResyncing() { return g_resyncing; }

void SaveSync_OnResyncAnnounced()
{
    g_resyncing = true;
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
    // A friend's Steam "Join Game" or an accepted invite: join that host.
    char host[64];
    if (NetworkTakeJoinRequest(host, sizeof(host))) {
        if (g_state == SESSION_IDLE) SaveSync_StartClient(host);
        else Logf("republicmp  SESSION: Steam join request for %s ignored - already in a session", host);
    }
}

SessionState SaveSync_GetState() { return g_state; }
static void LoadSettings() { g_dlcCheck = Profile_GetInt("profile", "dlc_check", 1) != 0; }

void SaveSync_SetDlcCheck(bool on)
{
    g_dlcCheck = on;
    Profile_SetInt("profile", "dlc_check", on ? 1 : 0);
}
bool SaveSync_GetDlcCheck() { return g_dlcCheck; }
bool SaveSync_IsBusy()           { return g_state != SESSION_IDLE; }
bool SaveSync_IsHostRole()       { return g_isHost; }
ULONGLONG SaveSync_GetSessionAgeMs() { return g_state == SESSION_IDLE ? 0 : GetTickCount64() - g_sessionStartTick; }
bool SaveSync_IsHostAwaitingClient() { return g_state == SESSION_HOST_LISTENING; }
const char* SaveSync_GetTargetIp() { return g_targetIp; }

// The returned buffers are only ever rewritten whole by the session thread;
// the UI reads them once per frame.
const char* SaveSync_GetPeerIp()    { return g_peerIp; }
const char* SaveSync_GetLastError() { return g_lastError; }
