// SPDX-License-Identifier: GPL-3.0-only
// See lockstep_net.h.
#include "lockstep_net.h"
#include "commands.h"
#include "presence.h"
#include "../ui/chat.h"
#include "../determinism/determinism.h"
#include "../determinism/citizen_trace.h"
#include "../transport/network.h"
#include "../core/guard.h"
#include "../session/workshop.h"
#include "../session/save_sync.h"
#include <deque>
#include <vector>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

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
    LS_MARK = 15,    // either way: LsMark, a cursor or ping (lockstep/presence; the host passes it on)
    LS_CHAT = 16,    // either way: LsChat, a line of the chat (ui/chat; the host passes it on)
};

#pragma pack(push, 1)
struct LsHeader { unsigned type; unsigned size; };
struct LsGrant { unsigned long long tick; int speed; };
struct LsCommandHead { unsigned long long afterTick; int kind; int player; }; // player: host -> player only
struct LsHash { unsigned long long tick; StateSummary s; };
struct LsState { int paused; int speed; };
struct LsPing { unsigned long long sentMs; int reply; };
struct LsHello { char magic[8]; char nick[PROFILE_NICK_BYTES]; WorkshopContent content; }; // content: session/workshop
struct LsPlayerEntry { char nick[PROFILE_NICK_BYTES]; int rttMs; unsigned behind; unsigned char host, self, pad[2]; };
struct LsPlayers { unsigned count; LsPlayerEntry e[PROFILE_MAX_PLAYERS]; };
struct LsMark { int player; int kind; float pos[3]; char nick[PROFILE_NICK_BYTES]; };
struct LsChat { int player; char nick[PROFILE_NICK_BYTES]; char text[CHAT_TEXT_BYTES]; };
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
    NetLink sock;
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
    bool differsAfterDesync;      // host, after a desync: the last comparison still differed
    CRITICAL_SECTION sendLock;
};

static Peer g_peers[LOCKSTEP_MAX_PEERS];
static volatile LONG g_commandsReady = 0;
static int g_peerSlots = 0; // slots used this session
static bool g_isHost = false;
static CRITICAL_SECTION g_inLock, g_listLock;
static CRITICAL_SECTION g_delayLock; // the latency emulation's queue
static HANDLE g_delayWake = nullptr;
static bool g_locksReady = false;
static std::deque<LsMessage> g_inbox;

// Host side.
static int g_pauseToggles = 0;
static int g_speedRequest = 0;
static LsState g_sentState = { 1, 1 }; // repeated with every ping: a player that joined later gets it too
static bool g_stateSent = false;
struct OwnHash { unsigned long long tick; StateSummary s; bool valid; };
static OwnHash g_ownHashes[LS_HASH_KEEP];

// The per-object records of the last hashed ticks (StateHash_*Records): at a
// desync each PC writes those of the tick that differed to its log, so the two
// logs name the objects that differ. Main thread writes; the player's receive
// thread reads when the host reports a desync.
#define LS_RECORD_KEEP 8
struct OwnRecords {
    unsigned long long tick = 0;
    bool valid = false;
    std::vector<BuildingRecord> buildings;
    std::vector<VehicleRecord> vehicles;
    std::vector<CitizenRecord> citizens;
    std::vector<StorageRecord> storage;
    bool storageEnabled = false, storageValid = false;
};
static OwnRecords g_records[LS_RECORD_KEEP];
static SRWLOCK g_recordsLock = SRWLOCK_INIT;

static void KeepRecords(unsigned long long tick)
{
    OwnRecords& r = g_records[(tick / LOCKSTEP_HASH_TICKS) % LS_RECORD_KEEP];
    AcquireSRWLockExclusive(&g_recordsLock);
    r.tick = tick;
    r.storageEnabled = StateHash_GetStorageTrace();
    r.storageValid = r.storageEnabled && StateHash_StorageRecords(r.storage);
    if (!r.storageEnabled) r.storage.clear();
    r.valid = StateHash_BuildingRecords(r.buildings) && StateHash_VehicleRecords(r.vehicles) &&
              StateHash_CitizenRecords(r.citizens);
    ReleaseSRWLockExclusive(&g_recordsLock);
}

// Lines gathered and written ~30 at a time: the loader writes each Logf
// through to the file.
struct RecordLines {
    char text[4000];
    size_t used = 0;
    int lines = 0;
    void Add(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(text + used, sizeof(text) - used, fmt, args);
        va_end(args);
        if (n < 0 || used + n + 2 >= sizeof(text)) {
            text[used] = 0;
            Flush();
            va_start(args, fmt);
            n = vsnprintf(text, sizeof(text) - 2, fmt, args);
            va_end(args);
            if (n < 0) return;
            used = (size_t)n < sizeof(text) - 2 ? (size_t)n : sizeof(text) - 3;
        } else {
            used += n;
        }
        text[used++] = '\r';
        text[used++] = '\n';
        text[used] = 0;
        if (++lines >= 30) Flush();
    }
    void Flush()
    {
        if (used) Logf("republicmp  DESYNC_RECORD:\r\n%s", text);
        used = 0;
        lines = 0;
    }
};

// One line per object of the subsystems named in `what` (the desync text up
// to its player's name: "buildings vehicles citizens"), in list order; floats
// with every digit (%.9g), as the hash takes their bits. The tick's records
// leave the ring first, so nothing waits for the log.
static void DumpRecords(unsigned long long tick, const char* what)
{
    char subsystems[160];
    strncpy_s(subsystems, what, _TRUNCATE);
    if (char* name = strchr(subsystems, '(')) *name = 0;
    OwnRecords r;
    AcquireSRWLockExclusive(&g_recordsLock);
    for (OwnRecords& k : g_records)
        if (k.valid && k.tick == tick) {
            r = std::move(k);
            k.valid = false;
        }
    ReleaseSRWLockExclusive(&g_recordsLock);
    // Who boarded, was hired or left since two hashes before (#55).
    CitizenTrace_Dump(tick > 2 * LOCKSTEP_HASH_TICKS ? tick - 2 * LOCKSTEP_HASH_TICKS : 0, tick);
    if (!r.valid) {
        Logf("republicmp  DESYNC_RECORD: none kept for tick %llu", tick);
        return;
    }
    RecordLines out;
    if (strstr(subsystems, "buildings")) {
        out.Add("tick %llu, %d buildings (index category x y z progress produced workers storage slots | each slot's "
                "entries:amount)", tick, (int)r.buildings.size());
        for (const BuildingRecord& b : r.buildings) {
            // Each slot's entry count and amount: two logs show which slot split and by how much.
            char slots[RECORD_SLOTS * 24] = "";
            size_t used = 0;
            for (int s = 0; s < b.slots && s < RECORD_SLOTS; s++) {
                int n = _snprintf_s(slots + used, sizeof(slots) - used, _TRUNCATE, " %d:%.9g", b.slotEntries[s], b.slotAmount[s]);
                if (n > 0) used += n;
            }
            out.Add("B %d %d %.9g %.9g %.9g %.9g %.9g %d %016llX %d |%s", b.index, b.category, b.pos[0], b.pos[1], b.pos[2],
                    b.progress, b.produced, b.workers, b.storage, b.slots, slots);
        }
    }
    if (strstr(subsystems, "buildings") && r.storageEnabled) {
        out.Add("tick %llu, storage resource entries: %s (building slot entry resource amount bits)",
                tick, r.storageValid ? "complete" : "unavailable: read or size limit");
        if (r.storageValid) for (const StorageRecord& s : r.storage) {
            unsigned bits = 0;
            memcpy(&bits, &s.amount, 4);
            out.Add("S %d %d %d [%s] %.9g %08X", s.building, s.slot, s.entry, s.resource, s.amount, bits);
        }
    }
    if (strstr(subsystems, "vehicles")) {
        out.Add("tick %llu, %d vehicles (index x y z cargo)", tick, (int)r.vehicles.size());
        for (const VehicleRecord& v : r.vehicles)
            out.Add("V %d %.9g %.9g %.9g %016llX", v.index, v.pos[0], v.pos[1], v.pos[2], v.cargo);
    }
    if (strstr(subsystems, "citizens")) {
        out.Add("tick %llu, %d citizens (n home age x y z)", tick, (int)r.citizens.size());
        for (size_t i = 0; i < r.citizens.size(); i++) {
            const CitizenRecord& c = r.citizens[i];
            out.Add("C %d %d %.9g %.9g %.9g %.9g", (int)i, c.home, c.age, c.pos[0], c.pos[1], c.pos[2]);
        }
    }
    out.Flush();
}
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
    InitializeCriticalSection(&g_delayLock);
    g_delayWake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    for (Peer& p : g_peers) {
        InitializeCriticalSection(&p.sendLock);
        p.sock = NET_NO_LINK;
        p.thread = nullptr;
        p.connected = false;
    }
    g_locksReady = true;
}

// A message goes out as one buffer - header and parts together - in one send:
// one TCP segment or one Steam reliable message instead of two or three.
#define SEND_STACK_BYTES 2048

static bool Send(Peer& p, unsigned type, const void* a, size_t na, const void* b = nullptr, size_t nb = 0)
{
    if (!p.connected) return false;
    LsHeader h = { type, (unsigned)(na + nb) };
    size_t total = sizeof(h) + na + nb;
    unsigned char stack[SEND_STACK_BYTES];
    std::vector<unsigned char> heap;
    unsigned char* buf = stack;
    if (total > sizeof(stack)) {
        heap.resize(total);
        buf = heap.data();
    }
    memcpy(buf, &h, sizeof(h));
    if (na) memcpy(buf + sizeof(h), a, na);
    if (nb) memcpy(buf + sizeof(h) + na, b, nb);
    EnterCriticalSection(&p.sendLock);
    bool ok = NetworkSendExact(p.sock, buf, total);
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

// A received message (receive thread, or the latency emulation's thread):
// pings, the desync report, the player list, marks and chat are handled
// here, the rest is queued for the game thread.
static void Dispatch(LsMessage& m)
{
    int index = m.peer;
    Peer& peer = g_peers[index];
    if (m.type == LS_PING) {
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
        return;
    }
    if (m.type == LS_DESYNC) {
        size_t n = m.data.size() < sizeof(g_desync) - 1 ? m.data.size() : sizeof(g_desync) - 1;
        memcpy(g_desync, m.data.data(), n);
        g_desync[n] = 0;
        Logf("republicmp  LOCKSTEP: the host reports - %s", g_desync);
        unsigned long long tick = 0;
        if (sscanf_s(g_desync, "the games differ after tick %llu", &tick) == 1) {
            Determinism_DumpMoneyTrace(tick);
            DumpRecords(tick, g_desync);
        }
        return;
    }
    if (m.type == LS_PLAYERS) {
        if (m.data.size() == sizeof(LsPlayers)) {
            EnterCriticalSection(&g_listLock);
            memcpy(&g_list, m.data.data(), sizeof(g_list));
            LeaveCriticalSection(&g_listLock);
        }
        return;
    }
    if (m.type == LS_MARK) {
        if (m.data.size() == sizeof(LsMark)) {
            LsMark k = *(const LsMark*)m.data.data();
            if (g_isHost) {
                // A player's: numbered and named by the host, passed on to the others.
                k.player = index + 1;
                strncpy_s(k.nick, peer.nick, _TRUNCATE);
                for (int i = 0; i < g_peerSlots; i++)
                    if (i != index) Send(g_peers[i], LS_MARK, &k, sizeof(k));
            }
            k.nick[PROFILE_NICK_BYTES - 1] = 0;
            Presence_Receive(k.player, k.kind, k.pos, k.nick);
        }
        return;
    }
    if (m.type == LS_CHAT) {
        if (m.data.size() == sizeof(LsChat)) {
            LsChat c = *(const LsChat*)m.data.data();
            if (g_isHost) {
                // A player's: numbered and named by the host, passed on to the others.
                c.player = index + 1;
                strncpy_s(c.nick, peer.nick, _TRUNCATE);
                for (int i = 0; i < g_peerSlots; i++)
                    if (i != index) Send(g_peers[i], LS_CHAT, &c, sizeof(c));
            }
            c.nick[PROFILE_NICK_BYTES - 1] = 0;
            c.text[CHAT_TEXT_BYTES - 1] = 0;
            Chat_Receive(c.player, c.nick, c.text);
        }
        return;
    }
    LockScope lock(g_inLock);
    g_inbox.push_back(std::move(m));
}

// ---------------------------------------------------------------------------
// Latency emulation (Debug window; off by default, not remembered)
// ---------------------------------------------------------------------------
//
// To test on a LAN what a high ping does: what this PC receives is handed on
// g_delayMs later, in the order it came - grants, commands, acks, hashes and
// the pings too, so the shown ping grows. Set on both PCs it adds twice that
// to the round trip. A leave and a re-sync are not held.

struct DelayedMessage {
    ULONGLONG due;
    LsMessage m;
};
static volatile LONG g_delayMs = 0;
static std::deque<DelayedMessage> g_delayed; // g_delayLock
static HANDLE g_delayThread = nullptr;

// Hands on what is due; the lock is held while a message is handled, so a
// message that comes meanwhile queues behind it and the order is kept.
static DWORD WINAPI DelayThread(LPVOID)
{
    for (;;) {
        DWORD wait = INFINITE;
        Guard_Call("the latency emulation", [&wait] {
            LockScope lock(g_delayLock);
            ULONGLONG now = GetTickCount64();
            while (!g_delayed.empty() && g_delayed.front().due <= now) {
                LsMessage m = std::move(g_delayed.front().m);
                g_delayed.pop_front();
                Guard_Call("a received lockstep message", [&m] { Dispatch(m); });
            }
            if (!g_delayed.empty()) wait = (DWORD)(g_delayed.front().due - now);
        });
        WaitForSingleObject(g_delayWake, wait);
    }
}

// Receive thread: true when the message was queued (the delay is on, or
// earlier ones still wait - they go first).
static bool Delay(LsMessage& m)
{
    LONG ms = g_delayMs;
    LockScope lock(g_delayLock);
    bool queued = ms > 0 || !g_delayed.empty();
    if (queued) {
        ULONGLONG due = GetTickCount64() + (ULONGLONG)ms;
        if (!g_delayed.empty() && g_delayed.back().due > due) due = g_delayed.back().due;
        g_delayed.push_back({ due, std::move(m) });
        if (!g_delayThread) g_delayThread = CreateThread(nullptr, 0, DelayThread, nullptr, 0, nullptr);
        SetEvent(g_delayWake);
    }
    return queued;
}

void LockstepNet_SetAddedDelay(int ms)
{
    if (ms < 0) ms = 0;
    InterlockedExchange(&g_delayMs, ms);
    if (ms) Logf("republicmp  LOCKSTEP: latency emulation on - what this PC receives is handed on %d ms later", ms);
    else Logf("republicmp  LOCKSTEP: latency emulation off");
}

int LockstepNet_GetAddedDelay() { return (int)g_delayMs; }

static void ReceiveLoop(int index)
{
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
        // One message that fails is lost, not the connection.
        Guard_Call("a received lockstep message", [&m] {
            if (!Delay(m)) Dispatch(m);
        });
    }
}

// A failure outside one message ends this connection as a lost one would.
static DWORD WINAPI RecvThread(LPVOID param)
{
    int index = (int)(intptr_t)param;
    Peer& peer = g_peers[index];
    Guard_Call("the lockstep receive thread", [index] { ReceiveLoop(index); });
    if (peer.connected)
        Logf(g_isHost ? "republicmp  LOCKSTEP: the connection to %s is closed" : "republicmp  LOCKSTEP: the connection is closed", peer.nick);
    peer.connected = false;
    return 0;
}

static void ResetState()
{
    InterlockedExchange(&g_commandsReady, 0);
    EnterCriticalSection(&g_delayLock); // the last session's held messages are dropped
    g_delayed.clear();
    LeaveCriticalSection(&g_delayLock);
    EnterCriticalSection(&g_inLock);
    g_inbox.clear();
    LeaveCriticalSection(&g_inLock);
    for (Peer& p : g_peers) {
        p.connected = false;
        p.sock = NET_NO_LINK;
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

// A send that cannot leave within this (a player whose link stalled) drops
// that player instead of holding the game thread and the other players.
#define LS_SEND_TIMEOUT_MS 15000

static void StartPeer(NetLink s, const char* nick, const char* ip)
{
    Peer& p = g_peers[g_peerSlots];
    NetworkSetMessageLink(s, LS_SEND_TIMEOUT_MS);
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
    NetLink listener = NetworkListen(LOCKSTEP_PORT, LOCKSTEP_MAX_PEERS);
    if (listener == NET_NO_LINK) return 0;
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
        NetLink s = NetworkAcceptReady(listener, 200, LS_HELLO_TIMEOUT_MS);
        if (s == NET_NO_LINK) continue;
        // A player opens with its name; anything else is not one of ours.
        LsHeader h = {};
        LsHello hello = {};
        if (!NetworkRecvExact(s, &h, sizeof(h)) || h.type != LS_HELLO || h.size != sizeof(hello) ||
            !NetworkRecvExact(s, &hello, sizeof(hello)) || memcmp(hello.magic, kHelloMagic, sizeof(kHelloMagic)) != 0) {
            Logf("republicmp  LOCKSTEP: a connection without a player's hello - closed");
            NetworkClose(s);
            continue;
        }
        hello.nick[PROFILE_NICK_BYTES - 1] = 0;
        NetworkSetTimeouts(s, 0);
        char ip[48];
        NetworkPeerAddress(s, ip, sizeof(ip));
        // The loaded content (session/workshop): other Workshop items, their
        // load order or DLCs give other type numbers, which commands use.
        // The vehicle types refuse (commands name them by number); the build
        // menu only warns until it is known to match on PCs that agree.
        char differs[256] = "";
        WorkshopContent mine = Workshop_Content();
        bool vehiclesDiffer = mine.vehicles != hello.content.vehicles || mine.vehicleHash != hello.content.vehicleHash;
        if (memcmp(&mine, &hello.content, sizeof(mine)) != 0)
            _snprintf_s(differs, sizeof(differs), _TRUNCATE,
                        "the games' content differs (vehicle types %u here, %u there; build menu %u here, %u there) - "
                        "the Workshop items, their load order or the DLCs",
                        mine.vehicles, hello.content.vehicles, mine.buildings, hello.content.buildings);
        if (differs[0]) {
            Logf("republicmp  LOCKSTEP: %s: %s (hashes %08X/%08X here, %08X/%08X there)", hello.nick, differs, mine.vehicleHash,
                 mine.buildingHash, hello.content.vehicleHash, hello.content.buildingHash);
            if (vehiclesDiffer && SaveSync_GetDlcCheck()) {
                // The player shows why, then its join ends.
                LsHeader h = { LS_DESYNC, (unsigned)strlen(differs) };
                NetworkSendExact(s, &h, sizeof(h));
                NetworkSendExact(s, differs, h.size);
                Logf("republicmp  LOCKSTEP: %s refused - set Check DLCs to OFF to play anyway", hello.nick);
                NetworkClose(s);
                continue;
            }
        }
        StartPeer(s, hello.nick, ip);
        if (differs[0]) Send(g_peers[g_peerSlots - 1], LS_DESYNC, differs, strlen(differs)); // playing anyway: said on its page
        Logf("republicmp  LOCKSTEP: %s (%s) connected - %d of %d", hello.nick, ip, g_peerSlots, expected);
    }
    NetworkClose(listener);
    // The first ones waited for the others: their silence so far is not a drop.
    for (int i = 0; i < g_peerSlots; i++) g_peers[i].lastReceived = GetTickCount64();
    // Publishing a peer early is needed by the receive threads, but the
    // command boundary must not consume its queue until all accepts finish.
    InterlockedExchange(&g_commandsReady, g_peerSlots > 0 ? 1 : 0);
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
        NetLink s = NetworkConnect(hostIp, LOCKSTEP_PORT, 5000, 0, cancel);
        if (s != NET_NO_LINK) {
            ResetState();
            g_isHost = false;
            struct {
                LsHeader h;
                LsHello hello;
            } first = {};
            first.h = { LS_HELLO, sizeof(LsHello) };
            memcpy(first.hello.magic, kHelloMagic, sizeof(kHelloMagic));
            strncpy_s(first.hello.nick, Profile_GetNick(), _TRUNCATE);
            first.hello.content = Workshop_Content();
            if (!NetworkSendExact(s, &first, sizeof(first))) {
                NetworkClose(s);
                return false;
            }
            StartPeer(s, "host", hostIp);
            InterlockedExchange(&g_commandsReady, 1);
            Logf("republicmp  LOCKSTEP: connected as player");
            return true;
        }
        if ((cancel && *cancel) || GetTickCount64() - start > timeoutMs) return false;
        Sleep(500); // the host listens once its own load is done
    }
}

void LockstepNet_Close()
{
    InterlockedExchange(&g_commandsReady, 0);
    Locks();
    for (int i = 0; i < g_peerSlots; i++) {
        Peer& p = g_peers[i];
        if (p.sock != NET_NO_LINK) {
            if (p.connected) Send(p, LS_BYE, nullptr, 0);
            p.connected = false;
            NetworkClose(p.sock);
            p.sock = NET_NO_LINK;
        }
        if (p.thread) {
            // The socket is closed, so its receive returns; the slot is used
            // again only once that thread has left (it reads the slot).
            DWORD r = WaitForSingleObject(p.thread, 5000);
            if (r != WAIT_OBJECT_0) {
                Logf("republicmp  LOCKSTEP: %s's receive thread has not ended after 5 s - waiting for it", p.nick);
                r = WaitForSingleObject(p.thread, 25000);
                if (r != WAIT_OBJECT_0) Logf("republicmp  LOCKSTEP: %s's receive thread still runs after 30 s", p.nick);
            }
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
bool LockstepNet_CommandsReady()
{
    return InterlockedCompareExchange(&g_commandsReady, 0, 0) != 0 && LockstepNet_IsConnected();
}
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
    if (g_desync[0] && theirs.tick != g_desyncTick) {
        // Later ticks: the games go on differing, or match again - a
        // difference of that moment only (something one PC alone listed then).
        bool differs = what[0] != 0;
        if (differs != peer.differsAfterDesync) {
            peer.differsAfterDesync = differs;
            if (differs) Logf("republicmp  LOCKSTEP:   %s differs again after tick %llu: %s", peer.nick, theirs.tick, what);
            else Logf("republicmp  LOCKSTEP:   %s matches the host again after tick %llu", peer.nick, theirs.tick);
        }
        return;
    }
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
    peer.differsAfterDesync = true;
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
    DumpRecords(theirs.tick, what);
}

void LockstepNet_StateAfter(unsigned long long tick, const StateSummary& s)
{
    if (!LockstepNet_IsConnected()) return;
    KeepRecords(tick);
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

// One received message on the game thread.
static void HandleMessage(LsMessage& m)
{
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
            Commands_SubmitRemote(((const LsCommandHead*)p)->kind, m.peer + 1, p + sizeof(LsCommandHead),
                                  n - sizeof(LsCommandHead));
        break;
    case LS_CMDS:
        if (n >= sizeof(LsCommandHead))
            Commands_Receive(((const LsCommandHead*)p)->kind, ((const LsCommandHead*)p)->player,
                             ((const LsCommandHead*)p)->afterTick,
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

void LockstepNet_Poll()
{
    if (!g_locksReady) return;
    std::deque<LsMessage> in;
    {
        LockScope lock(g_inLock);
        in.swap(g_inbox);
    }
    // One message that fails (core/guard.h) is lost, not the session.
    for (LsMessage& m : in) Guard_Call("a lockstep message", [&m] { HandleMessage(m); });
}

bool LockstepNet_MayRun(unsigned long long tick)
{
    if (!LockstepNet_CommandsReady()) return false;
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
        NetworkShutdown(p.sock); // its receive thread ends
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
    // Before any lockstep connection the list lock is not made yet (Locks):
    // a host still waiting for its first player has no list to show.
    if (!g_locksReady) return n;
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

void LockstepNet_SendMark(int kind, const float pos[3])
{
    if (!g_locksReady) return;
    LsMark k = { 0, kind, { pos[0], pos[1], pos[2] }, {} };
    strncpy_s(k.nick, Profile_GetNick(), _TRUNCATE);
    if (g_isHost) Broadcast(LS_MARK, &k, sizeof(k));
    else Send(g_peers[0], LS_MARK, &k, sizeof(k));
}

void LockstepNet_SendChat(const char* text)
{
    if (!g_locksReady) return;
    LsChat c = {};
    strncpy_s(c.nick, Profile_GetNick(), _TRUNCATE);
    strncpy_s(c.text, text, _TRUNCATE);
    if (g_isHost) Broadcast(LS_CHAT, &c, sizeof(c));
    else Send(g_peers[0], LS_CHAT, &c, sizeof(c));
}

void LockstepNet_SendCommand(int kind, unsigned long long afterTick, int player, const void* data, size_t size)
{
    LsCommandHead h = { afterTick, kind, player };
    if (g_isHost) Broadcast(LS_CMDS, &h, sizeof(h), data, size);
    else Send(g_peers[0], LS_CMD, &h, sizeof(h), data, size);
}
