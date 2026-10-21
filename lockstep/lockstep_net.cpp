// SPDX-License-Identifier: GPL-3.0-only
// See lockstep_net.h.
#include "lockstep_net.h"
#include "commands.h"
#include "../determinism/determinism.h"
#include "../transport/network.h"
#include <deque>
#include <vector>
#include <string.h>
#include <stdio.h>

void LockstepNet_Bind(const TsmHost* host)
{
    TsmBind(host);
}

enum LockstepMsgType {
    LS_GRANT = 1, // host -> player: LsGrant
    LS_CMD = 2,   // player -> host: LsCommandHead + payload
    LS_CMDS = 3,  // host -> player: LsCommandHead + payload
    LS_ACK = 4,   // player -> host: unsigned long long completed
    LS_HASH = 5,  // player -> host: LsHash
    LS_RESYNC = 6, // host -> player: the world is sent again (no payload)
    LS_PAUSE = 7, // player -> host: a pause toggle
    LS_SPEED = 8, // player -> host: int speed
    LS_STATE = 9, // host -> player: LsState
    LS_BYE = 10,  // either way: leaving
    LS_PING = 11,  // either way: still here (LsPing; a request is answered with the same time)
    LS_DESYNC = 12, // host -> player: the first state difference (text)
    LS_HELLO = 13,  // player -> host, first: LsHello
    LS_PLAYERS = 14, // host -> player: LsPlayers (once a second)
};

#pragma pack(push, 1)
struct LsHeader { unsigned type; unsigned size; };
struct LsGrant { unsigned long long tick; int speed; };
struct LsCommandHead { unsigned long long afterTick; int kind; };
struct LsHash { unsigned long long tick; StateSummary s; };
struct LsState { int paused; int speed; };
struct LsPing { unsigned long long sentMs; int reply; };
struct LsHello { char magic[8]; char nick[PROFILE_NICK_BYTES]; };
struct LsPlayerEntry { char nick[PROFILE_NICK_BYTES]; int rttMs; unsigned behind; unsigned char host, self, pad[2]; };
struct LsPlayers { unsigned count; LsPlayerEntry e[PROFILE_MAX_PLAYERS]; };
#pragma pack(pop)

#define LS_MAX_PAYLOAD (1u << 20)
#define LS_HASH_KEEP 32
#define LS_HELLO_TIMEOUT_MS 5000
static const char kHelloMagic[8] = "RPMPLSH";

struct LsMessage {
    int peer;
    unsigned type;
    std::vector<BYTE> data;
};

// One connection: on the host a player, on a player the host (slot 0).
struct Peer {
    SOCKET sock;
    HANDLE thread;
    volatile bool connected;
    char nick[PROFILE_NICK_BYTES];
    char ip[48];
    unsigned long long completed; // host: the player's ticks run (ACK)
    volatile ULONGLONG lastReceived;
    volatile ULONGLONG rttMs;
    volatile bool rttKnown;
    LsHash early[LS_HASH_KEEP];   // host: summaries that came before the host's own
    int earlyCount;
    unsigned long long lastMatched;
    CRITICAL_SECTION sendLock;
};

static Peer g_peers[LOCKSTEP_MAX_PEERS];
static int g_peerSlots = 0; // slots used this session
static bool g_isHost = false;
static CRITICAL_SECTION g_inLock, g_listLock;
static bool g_locksReady = false;
static std::deque<LsMessage> g_inbox;

// Host side.
static int g_pauseToggles = 0;
static int g_speedRequest = 0;
static LsState g_sentState = { 1, 1 }; // repeated with every ping: a player that joined later gets it too
static bool g_stateSent = false;
struct OwnHash { unsigned long long tick; StateSummary s; bool valid; };
static OwnHash g_ownHashes[LS_HASH_KEEP];
static char g_desync[256] = "";
static unsigned long long g_desyncTick = 0; // the tick of g_desync

// Player side.
static std::deque<LsGrant> g_grants;
static volatile bool g_resyncReceived = false; // set by the receive thread
static bool g_hostStateKnown = false;
static LsState g_hostState = { 1, 1 };
static LsPlayers g_list = {};                  // the host's last player list (g_listLock)

// Both sides: the host's waits for a player (the admission window full), the
// ticks granted so far and the player's grants not run yet (for the interface).
static volatile ULONGLONG g_lastStallMs = 0;
static volatile LONG g_stalls = 0;
static unsigned long long g_lastGranted = 0;
static volatile LONG g_grantBacklog = 0;

static void Locks()
{
    if (g_locksReady) return;
    InitializeCriticalSection(&g_inLock);
    InitializeCriticalSection(&g_listLock);
    for (Peer& p : g_peers) {
        InitializeCriticalSection(&p.sendLock);
        p.sock = INVALID_SOCKET;
        p.thread = nullptr;
        p.connected = false;
    }
    g_locksReady = true;
}

static bool Send(Peer& p, unsigned type, const void* a, size_t na, const void* b = nullptr, size_t nb = 0)
{
    if (!p.connected) return false;
    LsHeader h = { type, (unsigned)(na + nb) };
    EnterCriticalSection(&p.sendLock);
    bool ok = NetworkSendExact(p.sock, &h, sizeof(h)) && (!na || NetworkSendExact(p.sock, a, na)) &&
              (!nb || NetworkSendExact(p.sock, b, nb));
    LeaveCriticalSection(&p.sendLock);
    if (!ok && p.connected) {
        p.connected = false;
        Logf("republicmp  LOCKSTEP: send to %s failed - the connection is lost", p.nick);
    }
    return ok;
}

// Host: to every connected player.
static void Broadcast(unsigned type, const void* a, size_t na, const void* b = nullptr, size_t nb = 0)
{
    for (int i = 0; i < g_peerSlots; i++) Send(g_peers[i], type, a, na, b, nb);
}

static DWORD WINAPI RecvThread(LPVOID param)
{
    int index = (int)(intptr_t)param;
    Peer& peer = g_peers[index];
    while (peer.connected) {
        LsHeader h;
        if (!NetworkRecvExact(peer.sock, &h, sizeof(h)) || h.size > LS_MAX_PAYLOAD) break;
        LsMessage m;
        m.peer = index;
        m.type = h.type;
        m.data.resize(h.size);
        if (h.size && !NetworkRecvExact(peer.sock, m.data.data(), h.size)) break;
        peer.lastReceived = GetTickCount64();
        if (h.type == LS_PING) {
            if (m.data.size() == sizeof(LsPing)) {
                LsPing p = *(const LsPing*)m.data.data();
                if (p.reply) {
                    ULONGLONG now = GetTickCount64();
                    peer.rttMs = now >= p.sentMs ? now - p.sentMs : 0;
                    peer.rttKnown = true;
                } else {
                    p.reply = 1;
                    Send(peer, LS_PING, &p, sizeof(p));
                }
            }
            continue;
        }
        if (h.type == LS_DESYNC) {
            size_t n = m.data.size() < sizeof(g_desync) - 1 ? m.data.size() : sizeof(g_desync) - 1;
            memcpy(g_desync, m.data.data(), n);
            g_desync[n] = 0;
            Logf("republicmp  LOCKSTEP: the host reports - %s", g_desync);
            unsigned long long tick = 0;
            if (sscanf_s(g_desync, "the games differ after tick %llu", &tick) == 1) Determinism_DumpMoneyTrace(tick);
            continue;
        }
        if (h.type == LS_PLAYERS) {
            if (m.data.size() == sizeof(LsPlayers)) {
                EnterCriticalSection(&g_listLock);
                memcpy(&g_list, m.data.data(), sizeof(g_list));
                LeaveCriticalSection(&g_listLock);
            }
            continue;
        }
        if (h.type == LS_BYE) {
            Logf(g_isHost ? "republicmp  LOCKSTEP: %s left" : "republicmp  LOCKSTEP: the host left", peer.nick);
            break;
        }
        // Noted here, not in the game thread's poll: the host closes the
        // connection right after it.
        if (h.type == LS_RESYNC) {
            g_resyncReceived = true;
            continue;
        }
        EnterCriticalSection(&g_inLock);
        g_inbox.push_back(std::move(m));
        LeaveCriticalSection(&g_inLock);
    }
    if (peer.connected)
        Logf(g_isHost ? "republicmp  LOCKSTEP: the connection to %s is closed" : "republicmp  LOCKSTEP: the connection is closed", peer.nick);
    peer.connected = false;
    return 0;
}

static void ResetState()
{
    EnterCriticalSection(&g_inLock);
    g_inbox.clear();
    LeaveCriticalSection(&g_inLock);
    for (Peer& p : g_peers) {
        p.connected = false;
        p.sock = INVALID_SOCKET;
        p.thread = nullptr;
        p.nick[0] = 0;
        p.ip[0] = 0;
        p.completed = 0;
        p.lastReceived = GetTickCount64();
        p.rttMs = 0;
        p.rttKnown = false;
        p.earlyCount = 0;
        p.lastMatched = 0;
    }
    g_peerSlots = 0;
    g_pauseToggles = 0;
    g_speedRequest = 0;
    g_stateSent = false;
    memset(g_ownHashes, 0, sizeof(g_ownHashes));
    g_desync[0] = 0;
    g_desyncTick = 0;
    g_grants.clear();
    g_hostStateKnown = false;
    g_hostState.paused = 1;
    g_hostState.speed = 1;
    g_resyncReceived = false;
    EnterCriticalSection(&g_listLock);
    memset(&g_list, 0, sizeof(g_list));
    LeaveCriticalSection(&g_listLock);
    g_lastStallMs = 0;
    g_stalls = 0;
    g_lastGranted = 0;
    g_grantBacklog = 0;
}

static void StartPeer(SOCKET s, const char* nick, const char* ip)
{
    Peer& p = g_peers[g_peerSlots];
    p.sock = s;
    strncpy_s(p.nick, nick && nick[0] ? nick : "Player", _TRUNCATE);
    strncpy_s(p.ip, ip ? ip : "", _TRUNCATE);
    p.lastReceived = GetTickCount64();
    p.connected = true;
    p.thread = CreateThread(nullptr, 0, RecvThread, (LPVOID)(intptr_t)g_peerSlots, 0, nullptr);
    g_peerSlots++;
}

int LockstepNet_HostAccept(int expected, DWORD timeoutMs, volatile bool* cancel, int (*stillExpected)(void* ctx), void* ctx)
{
    Locks();
    LockstepNet_Close();
    if (!NetworkInit()) return 0;
    ResetState();
    g_isHost = true;
    if (expected > LOCKSTEP_MAX_PEERS) expected = LOCKSTEP_MAX_PEERS;
    SOCKET listener = NetworkListen(LOCKSTEP_PORT, LOCKSTEP_MAX_PEERS);
    if (listener == INVALID_SOCKET) return 0;
    ULONGLONG start = GetTickCount64();
    while (g_peerSlots < expected && !(cancel && *cancel) && GetTickCount64() - start < timeoutMs) {
        if (stillExpected) {
            int now = stillExpected(ctx);
            if (now < expected) {
                Logf("republicmp  LOCKSTEP: %d player(s) gave up before connecting - waiting for %d", expected - now, now);
                expected = now;
                continue;
            }
        }
        SOCKET s = NetworkAcceptReady(listener, 200, LS_HELLO_TIMEOUT_MS);
        if (s == INVALID_SOCKET) continue;
        // A player opens with its name; anything else is not one of ours.
        LsHeader h = {};
        LsHello hello = {};
        if (!NetworkRecvExact(s, &h, sizeof(h)) || h.type != LS_HELLO || h.size != sizeof(hello) ||
            !NetworkRecvExact(s, &hello, sizeof(hello)) || memcmp(hello.magic, kHelloMagic, sizeof(kHelloMagic)) != 0) {
            Logf("republicmp  LOCKSTEP: a connection without a player's hello - closed");
            NetworkCloseSocket(s);
            continue;
        }
        hello.nick[PROFILE_NICK_BYTES - 1] = 0;
        NetworkSetTimeouts(s, 0);
        char ip[48];
        NetworkPeerAddress(s, ip, sizeof(ip));
        StartPeer(s, hello.nick, ip);
        Logf("republicmp  LOCKSTEP: %s (%s) connected - %d of %d", hello.nick, ip, g_peerSlots, expected);
    }
    NetworkCloseSocket(listener);
    // The first ones waited for the others: their silence so far is not a drop.
    for (int i = 0; i < g_peerSlots; i++) g_peers[i].lastReceived = GetTickCount64();
    if (g_peerSlots) Logf("republicmp  LOCKSTEP: connected as host with %d player(s)", g_peerSlots);
    return g_peerSlots;
}

bool LockstepNet_SessionStarted()
{
    EnterCriticalSection(&g_listLock);
    bool started = g_list.count > 0;
    LeaveCriticalSection(&g_listLock);
    return started;
}

bool LockstepNet_Join(const char* hostIp, DWORD timeoutMs, volatile bool* cancel)
{
    Locks();
    LockstepNet_Close();
    if (!NetworkInit()) return false;
    ULONGLONG start = GetTickCount64();
    for (;;) {
        SOCKET s = NetworkConnect(hostIp, LOCKSTEP_PORT, 5000, 0, cancel);
        if (s != INVALID_SOCKET) {
            ResetState();
            g_isHost = false;
            LsHeader h = { LS_HELLO, sizeof(LsHello) };
            LsHello hello = {};
            memcpy(hello.magic, kHelloMagic, sizeof(kHelloMagic));
            strncpy_s(hello.nick, Profile_GetNick(), _TRUNCATE);
            if (!NetworkSendExact(s, &h, sizeof(h)) || !NetworkSendExact(s, &hello, sizeof(hello))) {
                NetworkCloseSocket(s);
                return false;
            }
            StartPeer(s, "host", hostIp);
            Logf("republicmp  LOCKSTEP: connected as player");
            return true;
        }
        if ((cancel && *cancel) || GetTickCount64() - start > timeoutMs) return false;
        Sleep(500); // the host listens once its own load is done
    }
}

void LockstepNet_Close()
{
    Locks();
    for (int i = 0; i < g_peerSlots; i++) {
        Peer& p = g_peers[i];
        if (p.sock != INVALID_SOCKET) {
            if (p.connected) Send(p, LS_BYE, nullptr, 0);
            p.connected = false;
            NetworkCloseSocket(p.sock);
            p.sock = INVALID_SOCKET;
        }
        if (p.thread) {
            WaitForSingleObject(p.thread, 5000);
            CloseHandle(p.thread);
            p.thread = nullptr;
        }
    }
    g_peerSlots = 0;
}

int LockstepNet_PeerCount()
{
    int n = 0;
    for (int i = 0; i < g_peerSlots; i++)
        if (g_peers[i].connected) n++;
    return n;
}

bool LockstepNet_IsConnected() { return LockstepNet_PeerCount() > 0; }
bool LockstepNet_IsHost() { return g_isHost; }
const char* LockstepNet_Desync() { return g_desync; }

unsigned long long LockstepNet_LastMatchedTick()
{
    unsigned long long matched = 0;
    bool any = false;
    for (int i = 0; i < g_peerSlots; i++) {
        const Peer& p = g_peers[i];
        if (!p.connected) continue;
        if (!any || p.lastMatched < matched) matched = p.lastMatched;
        any = true;
    }
    return any ? matched : 0;
}

// ---------------------------------------------------------------------------
// State comparison (host)
// ---------------------------------------------------------------------------

// After the first difference only that tick is still compared: the log tells
// which players differ and which match (one PC alone or everyone).
static void Compare(int index, const LsHash& theirs)
{
    Peer& peer = g_peers[index];
    if (g_desync[0] && theirs.tick != g_desyncTick) return;
    const OwnHash* mine = nullptr;
    for (const OwnHash& h : g_ownHashes)
        if (h.valid && h.tick == theirs.tick) mine = &h;
    if (!mine) {
        if (peer.earlyCount < LS_HASH_KEEP) peer.early[peer.earlyCount++] = theirs;
        return;
    }
    const StateSummary& a = mine->s;
    const StateSummary& b = theirs.s;
    char what[160] = "";
    if (a.hashClock != b.hashClock) strcat_s(what, "clock/money ");
    if (a.hashBuildings != b.hashBuildings) strcat_s(what, "buildings ");
    if (a.hashVehicles != b.hashVehicles) strcat_s(what, "vehicles ");
    if (a.hashCitizens != b.hashCitizens) strcat_s(what, "citizens ");
    if (g_desync[0]) {
        if (what[0]) Logf("republicmp  LOCKSTEP:   %s differs too after tick %llu: %s(rubles %.4f dollars %.4f)", peer.nick,
                          theirs.tick, what, b.rub, b.usd);
        else Logf("republicmp  LOCKSTEP:   %s matches the host after tick %llu", peer.nick, theirs.tick);
        return;
    }
    if (!what[0]) {
        peer.lastMatched = theirs.tick;
        return;
    }
    g_desyncTick = theirs.tick;
    for (int i = 0; i < g_peerSlots; i++)
        if (i != index && g_peers[i].connected && g_peers[i].lastMatched >= theirs.tick)
            Logf("republicmp  LOCKSTEP:   %s matches the host after tick %llu", g_peers[i].nick, theirs.tick);
    _snprintf_s(g_desync, sizeof(g_desync), _TRUNCATE, "the games differ after tick %llu: %s(%s)", theirs.tick, what, peer.nick);
    Logf("republicmp  LOCKSTEP: DESYNC - %s (host: buildings %d vehicles %d citizens %d, %s: %d %d %d)", g_desync,
         a.buildings, a.vehicles, a.citizens, peer.nick, b.buildings, b.vehicles, b.citizens);
    Logf("republicmp  LOCKSTEP:   host day %d %.4f rubles %.4f dollars %.4f, %s day %d %.4f rubles %.4f dollars %.4f", a.day,
         a.dayProgress, a.rub, a.usd, peer.nick, b.day, b.dayProgress, b.rub, b.usd);
    Broadcast(LS_DESYNC, g_desync, strlen(g_desync)); // every player shows it too
    Determinism_DumpMoneyTrace(theirs.tick);
}

void LockstepNet_StateAfter(unsigned long long tick, const StateSummary& s)
{
    if (!LockstepNet_IsConnected()) return;
    if (!g_isHost) {
        LsHash h = { tick, s };
        Send(g_peers[0], LS_HASH, &h, sizeof(h));
        return;
    }
    OwnHash& slot = g_ownHashes[(tick / LOCKSTEP_HASH_TICKS) % LS_HASH_KEEP];
    slot.tick = tick;
    slot.s = s;
    slot.valid = true;
    // A player summary that came first (it should not, the players are behind).
    for (int p = 0; p < g_peerSlots; p++) {
        Peer& peer = g_peers[p];
        for (int i = 0; i < peer.earlyCount; i++) {
            if (peer.early[i].tick != tick) continue;
            LsHash h = peer.early[i];
            peer.early[i] = peer.early[--peer.earlyCount];
            Compare(p, h);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Game thread
// ---------------------------------------------------------------------------

void LockstepNet_Poll()
{
    if (!g_locksReady) return;
    std::deque<LsMessage> in;
    EnterCriticalSection(&g_inLock);
    in.swap(g_inbox);
    LeaveCriticalSection(&g_inLock);
    for (LsMessage& m : in) {
        const BYTE* p = m.data.data();
        size_t n = m.data.size();
        Peer& peer = g_peers[m.peer];
        switch (m.type) {
        case LS_GRANT:
            if (n == sizeof(LsGrant)) g_grants.push_back(*(const LsGrant*)p);
            g_grantBacklog = (LONG)g_grants.size();
            break;
        case LS_CMD:
            if (n >= sizeof(LsCommandHead))
                Commands_SubmitRemote(((const LsCommandHead*)p)->kind, p + sizeof(LsCommandHead), n - sizeof(LsCommandHead));
            break;
        case LS_CMDS:
            if (n >= sizeof(LsCommandHead))
                Commands_Receive(((const LsCommandHead*)p)->kind, ((const LsCommandHead*)p)->afterTick,
                                 p + sizeof(LsCommandHead), n - sizeof(LsCommandHead));
            break;
        case LS_ACK:
            if (n == sizeof(unsigned long long) && *(const unsigned long long*)p > peer.completed)
                peer.completed = *(const unsigned long long*)p;
            break;
        case LS_HASH:
            if (n == sizeof(LsHash)) Compare(m.peer, *(const LsHash*)p);
            break;
        case LS_PAUSE:
            g_pauseToggles++;
            break;
        case LS_SPEED:
            if (n == sizeof(int)) g_speedRequest = *(const int*)p;
            break;
        case LS_STATE:
            if (n == sizeof(LsState)) {
                g_hostState = *(const LsState*)p;
                g_hostStateKnown = true;
            }
            break;
        }
    }
}

bool LockstepNet_MayRun(unsigned long long tick)
{
    bool any = false, may = true;
    for (int i = 0; i < g_peerSlots; i++) {
        const Peer& p = g_peers[i];
        if (!p.connected) continue;
        any = true;
        if (p.completed + LOCKSTEP_WINDOW < tick) may = false;
    }
    if (any && !may) {
        ULONGLONG now = GetTickCount64();
        if (now - g_lastStallMs > 1000) InterlockedIncrement(&g_stalls); // one per second of waiting
        g_lastStallMs = now;
    }
    return any && may;
}

void LockstepNet_Grant(unsigned long long tick, int speed)
{
    g_lastGranted = tick;
    LsGrant g = { tick, speed };
    Broadcast(LS_GRANT, &g, sizeof(g));
}

int LockstepNet_TakePauseToggles()
{
    int n = g_pauseToggles;
    g_pauseToggles = 0;
    return n;
}

int LockstepNet_TakeSpeedRequest()
{
    int s = g_speedRequest;
    g_speedRequest = 0;
    return s;
}

void LockstepNet_SendState(bool paused, int speed)
{
    LsState s = { paused ? 1 : 0, speed };
    g_sentState = s;
    g_stateSent = true;
    Broadcast(LS_STATE, &s, sizeof(s));
}

bool LockstepNet_TakeGrant(unsigned long long tick, int* speed)
{
    while (!g_grants.empty() && g_grants.front().tick < tick) g_grants.pop_front();
    if (g_grants.empty() || g_grants.front().tick != tick) return false;
    *speed = g_grants.front().speed;
    g_grants.pop_front();
    g_grantBacklog = (LONG)g_grants.size();
    return true;
}

// ---------------------------------------------------------------------------
// The players and the connection's health
// ---------------------------------------------------------------------------

static unsigned long long Behind(const Peer& p)
{
    return g_lastGranted > p.completed ? g_lastGranted - p.completed : 0;
}

// Host: the list as one player sees it (its own entry marked).
static void FillList(LsPlayers& l, int forPeer)
{
    memset(&l, 0, sizeof(l));
    LsPlayerEntry& host = l.e[l.count++];
    strncpy_s(host.nick, Profile_GetNick(), _TRUNCATE);
    host.host = 1;
    for (int i = 0; i < g_peerSlots && l.count < PROFILE_MAX_PLAYERS; i++) {
        const Peer& p = g_peers[i];
        if (!p.connected) continue;
        LsPlayerEntry& e = l.e[l.count++];
        strncpy_s(e.nick, p.nick, _TRUNCATE);
        e.rttMs = p.rttKnown ? (int)p.rttMs : -1;
        e.behind = (unsigned)Behind(p);
        e.self = i == forPeer ? 1 : 0;
    }
}

void LockstepNet_Ping()
{
    LsPing ping = { GetTickCount64(), 0 };
    if (!g_isHost) {
        Send(g_peers[0], LS_PING, &ping, sizeof(ping));
        return;
    }
    for (int i = 0; i < g_peerSlots; i++) {
        Peer& p = g_peers[i];
        if (!p.connected) continue;
        Send(p, LS_PING, &ping, sizeof(ping));
        if (g_stateSent) Send(p, LS_STATE, &g_sentState, sizeof(g_sentState));
        LsPlayers l;
        FillList(l, i);
        Send(p, LS_PLAYERS, &l, sizeof(l));
    }
}

int LockstepNet_DropSilent(unsigned long long ms)
{
    int dropped = 0;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_peerSlots; i++) {
        Peer& p = g_peers[i];
        if (!p.connected || now - p.lastReceived <= ms) continue;
        Logf("republicmp  LOCKSTEP: %s stopped answering - dropped", p.nick);
        p.connected = false;
        shutdown(p.sock, SD_BOTH); // its receive thread ends
        dropped++;
    }
    return dropped;
}

int LockstepNet_Players(LockstepPlayer* out, int max)
{
    int n = 0;
    if (g_isHost) {
        if (n < max) {
            LockstepPlayer& me = out[n++];
            memset(&me, 0, sizeof(me));
            strncpy_s(me.nick, Profile_GetNick(), _TRUNCATE);
            me.host = me.self = true;
        }
        for (int i = 0; i < g_peerSlots && n < max; i++) {
            const Peer& p = g_peers[i];
            if (!p.connected) continue;
            LockstepPlayer& e = out[n++];
            memset(&e, 0, sizeof(e));
            strncpy_s(e.nick, p.nick, _TRUNCATE);
            strncpy_s(e.ip, p.ip, _TRUNCATE);
            e.rttMs = p.rttKnown ? (int)p.rttMs : -1;
            e.behind = Behind(p);
        }
        return n;
    }
    LsPlayers l;
    EnterCriticalSection(&g_listLock);
    l = g_list;
    LeaveCriticalSection(&g_listLock);
    for (unsigned i = 0; i < l.count && i < PROFILE_MAX_PLAYERS && n < max; i++) {
        LockstepPlayer& e = out[n++];
        memset(&e, 0, sizeof(e));
        l.e[i].nick[PROFILE_NICK_BYTES - 1] = 0;
        strncpy_s(e.nick, l.e[i].nick, _TRUNCATE);
        e.rttMs = l.e[i].host ? 0 : l.e[i].rttMs;
        e.behind = l.e[i].behind;
        e.host = l.e[i].host != 0;
        e.self = l.e[i].self != 0;
    }
    return n;
}

bool LockstepNet_RoundTrip(unsigned long long* ms)
{
    bool known = false;
    *ms = 0;
    for (int i = 0; i < g_peerSlots; i++) {
        const Peer& p = g_peers[i];
        if (!p.connected || !p.rttKnown) continue;
        if (!known || p.rttMs > *ms) *ms = p.rttMs;
        known = true;
    }
    return known;
}

LockstepFlow LockstepNet_Flow()
{
    LockstepFlow f = {};
    f.stalls = (int)g_stalls;
    f.msSinceStall = g_lastStallMs ? GetTickCount64() - g_lastStallMs : ~0ull;
    if (g_isHost) {
        for (int i = 0; i < g_peerSlots; i++)
            if (g_peers[i].connected && Behind(g_peers[i]) > f.behind) f.behind = Behind(g_peers[i]);
    } else {
        f.behind = (unsigned long long)g_grantBacklog;
    }
    return f;
}

ULONGLONG LockstepNet_MsSinceReceived()
{
    ULONGLONG now = GetTickCount64(), quiet = 0;
    for (int i = 0; i < g_peerSlots; i++)
        if (g_peers[i].connected && now - g_peers[i].lastReceived > quiet) quiet = now - g_peers[i].lastReceived;
    return quiet;
}

void LockstepNet_SendResync()
{
    Broadcast(LS_RESYNC, nullptr, 0);
}

bool LockstepNet_TakeResync()
{
    if (!g_resyncReceived) return false;
    g_resyncReceived = false;
    return true;
}

void LockstepNet_Ack(unsigned long long completed)
{
    Send(g_peers[0], LS_ACK, &completed, sizeof(completed));
}

void LockstepNet_SendPauseToggle()
{
    Send(g_peers[0], LS_PAUSE, nullptr, 0);
}

void LockstepNet_SendSpeed(int speed)
{
    Send(g_peers[0], LS_SPEED, &speed, sizeof(speed));
}

bool LockstepNet_HostState(bool* paused, int* speed)
{
    if (!g_hostStateKnown) return false;
    *paused = g_hostState.paused != 0;
    *speed = g_hostState.speed;
    return true;
}

void LockstepNet_SendCommand(int kind, unsigned long long afterTick, const void* data, size_t size)
{
    LsCommandHead h = { afterTick, kind };
    if (g_isHost) Broadcast(LS_CMDS, &h, sizeof(h), data, size);
    else Send(g_peers[0], LS_CMD, &h, sizeof(h), data, size);
}
