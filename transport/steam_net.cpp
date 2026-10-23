// SPDX-License-Identifier: GPL-3.0-only
// See steam_net.h. Headers: Valve's GameNetworkingSockets (BSD-3,
// third_party/gns), which declare the same interfaces the Steam client serves.
#include "steam_net.h"
#include "../core/profile.h"
#include "../third_party/gns/steam/isteamnetworkingsockets.h"
#include "../third_party/gns/steam/isteamnetworkingutils.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

#define STEAM_LINK_TAG (1ull << 62)            // never set in a SOCKET (a small kernel handle)
#define STEAM_MAX_MESSAGE (256 * 1024)         // under k_cbMaxSteamNetworkingSocketsMessageSizeSend (512 KiB)
#define STEAM_START_RATE (1024 * 1024)         // bytes/s; clean between two PCs ~220 ms apart (#36 spike)
#define STEAM_MIN_RATE (256 * 1024)            // the library's own default
#define STEAM_SEND_BUFFER (4 * 1024 * 1024)    // queued, not yet sent, per connection
#define STEAM_FRIENDS_VERSION "SteamFriends017" // the version the game itself asks for
#define STEAM_JOIN_REQUESTED_CALLBACK 337      // GameRichPresenceJoinRequested_t (k_iSteamFriendsCallbacks + 37)
#define STEAM_CONNECT_PREFIX "+republicmp steam:" // our rich-presence "connect" string

typedef int (*Fn_GetHSteamUser)();
typedef void* (*Fn_FindInterface)(int user, const char* version);
typedef bool (*Fn_SetRichPresence)(void* friends, const char* key, const char* value);
class CallbackBase;
typedef void (*Fn_RegisterCallback)(CallbackBase* cb, int id);

// steam_api's CCallbackBase as the game's 2019 DLL dispatches it: MSVC puts the
// Run overloads in reverse order (slot 0 Run(void*, bool, uint64), slot 1
// Run(void*), slot 2 the size); flags at +0x08, id at +0x0C.
class CallbackBase {
public:
    virtual void Run(void* data) = 0;
    virtual void Run(void* data, bool ioFailure, uint64 call) = 0;
    virtual int GetCallbackSizeBytes() = 0;
    uint8 flags = 0;
    int id = 0;
};

struct JoinRequested { // GameRichPresenceJoinRequested_t
    uint64 friendId;
    char connect[256];
};

struct Conn {
    HSteamNetConnection h = k_HSteamNetConnection_Invalid;
    unsigned long long peer = 0;
    int vport = -1;                // the port it came in on (-1: we connected)
    bool taken = false;            // handed to a caller (under g_steamLock)
    std::atomic<DWORD> ioTimeoutMs{0};
    std::atomic<bool> gone{false}; // closed by the peer or failed; what arrived stays readable
    std::atomic<bool> shut{false}; // shut down or closed by us: every call fails
    std::mutex rx;                 // one reader at a time, so messages are appended in order
    std::vector<char> buf;         // received, not read yet (from off)
    size_t off = 0;
    int rate = STEAM_START_RATE;   // the rate controller's (service thread only)
    int basePing = 0;
};
typedef std::shared_ptr<Conn> ConnPtr;

// One Steam listen socket per port, open for good: closing it would drop every
// connection accepted on it, and the host stops listening once a session is full.
struct Port {
    HSteamListenSocket ls = k_HSteamListenSocket_Invalid;
    NetLink owner = NET_NO_LINK; // the TCP listener it serves; NET_NO_LINK: refuse players
    std::deque<ConnPtr> ready;   // connected, waiting for SteamNet_Accept
};

static std::mutex g_steamLock; // the maps below; never held across a Steam call
static std::unordered_map<HSteamNetConnection, ConnPtr> g_conns;
static std::unordered_map<int, Port> g_ports; // by virtual port
static char g_joinRequest[64];
static ISteamNetworkingSockets* g_sockets;
static ISteamNetworkingUtils* g_utils;
static void* g_friends;
static Fn_SetRichPresence p_SetRichPresence;
static unsigned long long g_localId;
static int g_maxRate;

static ULONGLONG Now() { return GetTickCount64(); }
static NetLink ToLink(HSteamNetConnection h) { return (NetLink)(STEAM_LINK_TAG | h); }
// Steam wants small virtual port numbers: 5556 -> 556, 5560 -> 560.
static int VirtualPort(unsigned short port) { return port % 1000; }

bool SteamNet_IsLink(NetLink link)
{
    return link != NET_NO_LINK && (link & STEAM_LINK_TAG) != 0;
}

static ConnPtr Find(NetLink link)
{
    if (!SteamNet_IsLink(link)) return nullptr;
    std::lock_guard<std::mutex> l(g_steamLock);
    auto it = g_conns.find((HSteamNetConnection)(link & 0xFFFFFFFFu));
    return it == g_conns.end() ? nullptr : it->second;
}

bool SteamNet_ParseId(const char* text, unsigned long long* id)
{
    if (!text) return false;
    while (*text == ' ') ++text;
    if (_strnicmp(text, "steam:", 6) == 0) text += 6;
    if (strncmp(text, "7656119", 7) != 0) return false; // individual accounts
    char* end;
    unsigned long long v = _strtoui64(text, &end, 10);
    if (end - text != 17) return false;
    while (*end == ' ') ++end;
    if (*end) return false;
    *id = v;
    return true;
}

// Steam's main-thread callback (the game pumps SteamAPI_RunCallbacks): only
// keeps the request; SteamNet_TakeJoinRequest hands it on.
class JoinCallback : public CallbackBase {
public:
    void Run(void* data) override
    {
        char connect[sizeof(JoinRequested::connect) + 1] = {};
        memcpy(connect, ((const JoinRequested*)data)->connect, sizeof(JoinRequested::connect));
        const char* arg = strstr(connect, "steam:");
        unsigned long long id;
        if (!arg || !SteamNet_ParseId(arg, &id)) return;
        std::lock_guard<std::mutex> l(g_steamLock);
        snprintf(g_joinRequest, sizeof(g_joinRequest), "steam:%llu", id);
    }
    void Run(void* data, bool, uint64) override { Run(data); }
    int GetCallbackSizeBytes() override { return sizeof(JoinRequested); }
};
static JoinCallback g_joinCallback;

bool SteamNet_TakeJoinRequest(char* out, size_t outSize)
{
    std::lock_guard<std::mutex> l(g_steamLock);
    if (!g_joinRequest[0]) return false;
    strncpy_s(out, outSize, g_joinRequest, _TRUNCATE);
    g_joinRequest[0] = 0;
    return true;
}

// Friends see "Join Game" (and can be invited from the overlay) while we host.
static void SetJoinable(bool on)
{
    if (!g_friends || !p_SetRichPresence) return;
    char value[64];
    snprintf(value, sizeof(value), STEAM_CONNECT_PREFIX "%llu", g_localId);
    p_SetRichPresence(g_friends, "connect", on ? value : "");
}

static void CloseUnclaimed(const std::vector<ConnPtr>& conns, const char* why)
{
    for (const ConnPtr& c : conns) g_sockets->CloseConnection(c->h, 0, why, false);
}

// Runs on the service thread, from ISteamNetworkingSockets::RunCallbacks (the
// game's 2019 steam_api never runs function callbacks itself).
static void OnStatus(SteamNetConnectionStatusChangedCallback_t* s)
{
    HSteamNetConnection h = s->m_hConn;
    ESteamNetworkingConnectionState state = s->m_info.m_eState;
    if (state == k_ESteamNetworkingConnectionState_Connecting && s->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
        // A player knocking on one of our ports: accept while it is served.
        bool served = false;
        {
            std::lock_guard<std::mutex> l(g_steamLock);
            for (auto& it : g_ports) {
                if (it.second.ls != s->m_info.m_hListenSocket || it.second.owner == NET_NO_LINK) continue;
                ConnPtr c = std::make_shared<Conn>();
                c->h = h;
                c->peer = s->m_info.m_identityRemote.GetSteamID64();
                c->vport = it.first;
                g_conns[h] = c;
                served = true;
            }
        }
        if (served && g_sockets->AcceptConnection(h) == k_EResultOK) return;
        {
            std::lock_guard<std::mutex> l(g_steamLock);
            g_conns.erase(h);
        }
        g_sockets->CloseConnection(h, 0, "not listening", false);
        return;
    }

    ConnPtr c;
    std::vector<ConnPtr> unclaimed;
    {
        std::lock_guard<std::mutex> l(g_steamLock);
        auto it = g_conns.find(h);
        if (it == g_conns.end()) return;
        c = it->second;
        if (state == k_ESteamNetworkingConnectionState_Connected && c->vport >= 0 && !c->taken) {
            auto port = g_ports.find(c->vport);
            if (port != g_ports.end() && port->second.owner != NET_NO_LINK) {
                port->second.ready.push_back(c);
                return;
            }
            unclaimed.push_back(c); // the host stopped listening meanwhile
            g_conns.erase(it);
        } else if (state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                   state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
            c->gone = true;
            if (!c->taken) { // left before anyone took it: nobody else will close it
                auto port = g_ports.find(c->vport);
                if (port != g_ports.end()) {
                    auto& q = port->second.ready;
                    q.erase(std::remove(q.begin(), q.end(), c), q.end());
                }
                unclaimed.push_back(c);
                g_conns.erase(it);
            }
        } else {
            return;
        }
    }
    if (c->gone)
        Logf("republicmp  NETWORK: Steam link to %llu lost: %s (reason %d)", c->peer, s->m_info.m_szEndDebug,
             s->m_info.m_eEndReason);
    CloseUnclaimed(unclaimed, "not taken");
}

// Once a second: the library does not estimate bandwidth (min = max is a fixed
// rate), so raise the rate of a backlogged connection while it stays clean
// and halve it when packets drop or the ping climbs - a queue building on the
// way shows as ping before it shows as loss. Only a bulk transfer (the world)
// ever backlogs; lockstep's small messages never do.
static void AdjustRates()
{
    static ULONGLONG next;
    if (Now() < next) return;
    next = Now() + 1000;
    std::vector<ConnPtr> conns;
    {
        std::lock_guard<std::mutex> l(g_steamLock);
        for (auto& it : g_conns) conns.push_back(it.second);
    }
    for (ConnPtr& c : conns) {
        if (c->gone || c->shut) continue;
        SteamNetConnectionRealTimeStatus_t st{};
        if (g_sockets->GetConnectionRealTimeStatus(c->h, &st, 0, nullptr) != k_EResultOK ||
            st.m_eState != k_ESteamNetworkingConnectionState_Connected)
            continue;
        if (st.m_nPing > 0 && (!c->basePing || st.m_nPing < c->basePing)) c->basePing = st.m_nPing;
        float quality = st.m_flConnectionQualityLocal; // each -1 while unknown
        if (st.m_flConnectionQualityRemote >= 0 && (quality < 0 || st.m_flConnectionQualityRemote < quality))
            quality = st.m_flConnectionQualityRemote;
        bool congested = (quality >= 0 && quality < 0.95f) || (c->basePing && st.m_nPing > c->basePing * 3 / 2 + 20);
        bool backlogged = st.m_cbPendingReliable > c->rate / 4;
        int rate = c->rate;
        if (congested) rate = std::max(STEAM_MIN_RATE, rate / 2);
        else if (backlogged && quality >= 0.98f) rate = std::min(g_maxRate, rate + rate / 4);
        if (rate == c->rate) continue;
        g_utils->SetConnectionConfigValueInt32(c->h, k_ESteamNetworkingConfig_SendRateMin, rate);
        g_utils->SetConnectionConfigValueInt32(c->h, k_ESteamNetworkingConfig_SendRateMax, rate);
        Logf("republicmp  NETWORK: Steam link to %llu: send rate %d KiB/s (ping %d ms, base %d, quality %.2f, pending %d KiB)",
             c->peer, rate / 1024, st.m_nPing, c->basePing, quality, st.m_cbPendingReliable / 1024);
        c->rate = rate;
    }
}

static DWORD WINAPI ServiceThread(LPVOID)
{
    for (;;) {
        g_sockets->RunCallbacks();
        AdjustRates();
        Sleep(5);
    }
}

void SteamNet_Bind(const TsmHost* host)
{
    TsmBind(host);
    // steam_api64.dll is an import of the exe, so it is loaded by now; the
    // callback map needs no SteamAPI_Init, and registering here comes before
    // the main thread ever dispatches it.
    HMODULE api = GetModuleHandleA("steam_api64.dll");
    Fn_RegisterCallback reg = api ? (Fn_RegisterCallback)GetProcAddress(api, "SteamAPI_RegisterCallback") : nullptr;
    if (reg) {
        g_joinCallback.id = STEAM_JOIN_REQUESTED_CALLBACK; // SteamAPI_RegisterCallback sets it too
        reg(&g_joinCallback, STEAM_JOIN_REQUESTED_CALLBACK);
    }
    else Logf("republicmp  NETWORK: no steam_api64.dll - Steam Join Game off");
}

static bool InitOnce()
{
    HMODULE api = GetModuleHandleA("steam_api64.dll");
    Fn_GetHSteamUser getUser = api ? (Fn_GetHSteamUser)GetProcAddress(api, "SteamAPI_GetHSteamUser") : nullptr;
    Fn_FindInterface find = api ? (Fn_FindInterface)GetProcAddress(api, "SteamInternal_FindOrCreateUserInterface") : nullptr;
    p_SetRichPresence = api ? (Fn_SetRichPresence)GetProcAddress(api, "SteamAPI_ISteamFriends_SetRichPresence") : nullptr;
    int user = getUser && find ? getUser() : 0;
    if (!user) {
        Logf("republicmp  NETWORK: the game runs without Steam - TCP only");
        return false;
    }
    g_sockets = (ISteamNetworkingSockets*)find(user, STEAMNETWORKINGSOCKETS_INTERFACE_VERSION);
    g_utils = (ISteamNetworkingUtils*)find(user, STEAMNETWORKINGUTILS_INTERFACE_VERSION);
    g_friends = find(user, STEAM_FRIENDS_VERSION);
    if (!g_sockets || !g_utils) {
        Logf("republicmp  NETWORK: the Steam client has no %s / %s - TCP only", STEAMNETWORKINGSOCKETS_INTERFACE_VERSION,
             STEAMNETWORKINGUTILS_INTERFACE_VERSION);
        return false;
    }
    g_maxRate = std::clamp(Profile_GetInt("steam", "max_rate_kib", 16384), 256, 65536) * 1024;
    g_utils->SetGlobalCallback_SteamNetConnectionStatusChanged(OnStatus);
    g_utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendBufferSize, STEAM_SEND_BUFFER);
    g_utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMin, STEAM_START_RATE);
    g_utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMax, STEAM_START_RATE);
    SteamNetworkingIdentity me;
    me.Clear();
    if (g_sockets->GetIdentity(&me)) g_localId = me.GetSteamID64();
    g_utils->InitRelayNetworkAccess(); // the relay takes a few seconds; start now
    // The blocking reads poll Steam with Sleep(1), which is ~15.6 ms at the
    // default timer resolution; the game's XAudio2 asks for 1 ms too.
    timeBeginPeriod(1);
    HANDLE t = CreateThread(nullptr, 0, ServiceThread, nullptr, 0, nullptr);
    if (!t) {
        Logf("republicmp  NETWORK: no Steam service thread - TCP only");
        return false;
    }
    CloseHandle(t);
    Logf("republicmp  NETWORK: Steam links ready, SteamID64 %llu, send rate up to %d KiB/s", g_localId, g_maxRate / 1024);
    return true;
}

bool SteamNet_Init()
{
    static std::once_flag once;
    static bool ok;
    std::call_once(once, [] { ok = InitOnce(); });
    return ok;
}

unsigned long long SteamNet_LocalId()
{
    return SteamNet_Init() ? g_localId : 0;
}

void SteamNet_Listen(NetLink tcpListener, unsigned short port)
{
    if (!SteamNet_Init()) return;
    int vport = VirtualPort(port);
    bool exists;
    {
        std::lock_guard<std::mutex> l(g_steamLock);
        exists = g_ports.count(vport) != 0;
    }
    if (!exists) {
        HSteamListenSocket ls = g_sockets->CreateListenSocketP2P(vport, 0, nullptr);
        if (ls == k_HSteamListenSocket_Invalid) {
            Logf("republicmp  NETWORK: Steam listening on virtual port %d failed", vport);
            return;
        }
        std::lock_guard<std::mutex> l(g_steamLock);
        g_ports[vport].ls = ls;
    }
    {
        std::lock_guard<std::mutex> l(g_steamLock);
        g_ports[vport].owner = tcpListener;
    }
    SetJoinable(true);
    Logf("republicmp  NETWORK: Steam listening on virtual port %d", vport);
}

void SteamNet_StopListen(NetLink tcpListener)
{
    std::vector<ConnPtr> unclaimed;
    bool any = false, found = false;
    {
        std::lock_guard<std::mutex> l(g_steamLock);
        for (auto& it : g_ports) {
            if (it.second.owner != tcpListener) {
                any |= it.second.owner != NET_NO_LINK;
                continue;
            }
            found = true;
            it.second.owner = NET_NO_LINK;
            it.second.ready.clear();
            for (auto c = g_conns.begin(); c != g_conns.end();) {
                if (c->second->vport == it.first && !c->second->taken) {
                    unclaimed.push_back(c->second);
                    c = g_conns.erase(c);
                } else {
                    ++c;
                }
            }
        }
    }
    if (!found) return;
    CloseUnclaimed(unclaimed, "host stopped listening");
    if (!any) SetJoinable(false);
}

NetLink SteamNet_Accept(NetLink tcpListener, DWORD ioTimeoutMs)
{
    ConnPtr c;
    {
        std::lock_guard<std::mutex> l(g_steamLock);
        for (auto& it : g_ports) {
            if (it.second.owner != tcpListener || it.second.ready.empty()) continue;
            c = it.second.ready.front();
            it.second.ready.pop_front();
            c->taken = true;
            break;
        }
    }
    if (!c) return NET_NO_LINK;
    c->ioTimeoutMs = ioTimeoutMs;
    Logf("republicmp  NETWORK: client connected, Steam %llu", c->peer);
    return ToLink(c->h);
}

NetLink SteamNet_Connect(unsigned long long id, unsigned short port, DWORD connectTimeoutMs, DWORD ioTimeoutMs,
                         volatile bool* cancelFlag)
{
    if (!SteamNet_Init()) return NET_NO_LINK;
    // A first route through the relay can take seconds (lockstep asks for 5).
    connectTimeoutMs = std::max<DWORD>(connectTimeoutMs, 10000);
    SteamNetworkingIdentity peer;
    peer.Clear();
    peer.SetSteamID64(id);
    HSteamNetConnection h = g_sockets->ConnectP2P(peer, VirtualPort(port), 0, nullptr);
    if (h == k_HSteamNetConnection_Invalid) {
        Logf("republicmp  NETWORK: Steam connect to %llu refused", id);
        return NET_NO_LINK;
    }
    ConnPtr c = std::make_shared<Conn>();
    c->h = h;
    c->peer = id;
    c->taken = true;
    c->ioTimeoutMs = ioTimeoutMs;
    {
        std::lock_guard<std::mutex> l(g_steamLock);
        g_conns[h] = c;
    }
    Logf("republicmp  NETWORK: connecting to Steam %llu, virtual port %d (timeout %u ms)...", id, VirtualPort(port),
         connectTimeoutMs);
    // Polled rather than told by the callback, which may run before the map
    // above has the connection.
    ULONGLONG start = Now();
    for (;;) {
        SteamNetConnectionInfo_t info{};
        if (!g_sockets->GetConnectionInfo(h, &info)) break;
        if (info.m_eState == k_ESteamNetworkingConnectionState_Connected) {
            Logf("republicmp  NETWORK: connected to Steam %llu", id);
            return ToLink(h);
        }
        if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
            info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
            Logf("republicmp  NETWORK: Steam connect to %llu failed: %s (reason %d)", id, info.m_szEndDebug,
                 info.m_eEndReason);
            break;
        }
        if (cancelFlag && *cancelFlag) break;
        if (Now() - start >= connectTimeoutMs) {
            Logf("republicmp  NETWORK: TIMEOUT - no Steam connection to %llu within %u ms", id, connectTimeoutMs);
            break;
        }
        Sleep(20);
    }
    SteamNet_Close(ToLink(h));
    return NET_NO_LINK;
}

void SteamNet_SetTimeouts(NetLink link, DWORD timeoutMs)
{
    if (ConnPtr c = Find(link)) c->ioTimeoutMs = timeoutMs;
}

void SteamNet_PeerAddress(NetLink link, char* out, size_t outSize)
{
    out[0] = 0;
    if (ConnPtr c = Find(link)) snprintf(out, outSize, "steam:%llu", c->peer);
}

// Moves what has arrived into the buffer; caller holds c->rx. True when
// something came.
static bool Pull(Conn* c)
{
    SteamNetworkingMessage_t* msgs[32];
    int n = g_sockets->ReceiveMessagesOnConnection(c->h, msgs, 32);
    for (int i = 0; i < n; ++i) {
        const char* p = (const char*)msgs[i]->m_pData;
        c->buf.insert(c->buf.end(), p, p + msgs[i]->m_cbSize);
        msgs[i]->Release();
    }
    return n > 0;
}

static size_t Buffered(const Conn* c) { return c->buf.size() - c->off; }

bool SteamNet_Send(NetLink link, const void* data, size_t size)
{
    ConnPtr c = Find(link);
    if (!c) return false;
    const char* p = (const char*)data;
    ULONGLONG last = Now();
    while (size) {
        if (c->shut || c->gone) {
            Logf("republicmp  NETWORK: send on a closed Steam link (%zu bytes left)", size);
            return false;
        }
        uint32 n = (uint32)std::min(size, (size_t)STEAM_MAX_MESSAGE);
        EResult r = g_sockets->SendMessageToConnection(c->h, p, n, k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
        if (r == k_EResultOK) {
            p += n;
            size -= n;
            last = Now();
            continue;
        }
        if (r != k_EResultLimitExceeded) { // anything but a full send buffer
            Logf("republicmp  NETWORK: Steam send failed, result %d", r);
            return false;
        }
        DWORD timeout = c->ioTimeoutMs;
        if (timeout && Now() - last > timeout) {
            Logf("republicmp  NETWORK: TIMEOUT - Steam send stalled (%zu bytes left)", size);
            return false;
        }
        Sleep(2);
    }
    return true;
}

bool SteamNet_Recv(NetLink link, void* data, size_t size)
{
    ConnPtr c = Find(link);
    if (!c) return false;
    std::lock_guard<std::mutex> rx(c->rx);
    ULONGLONG last = Now();
    for (;;) {
        if (c->shut) return false;
        if (Buffered(c.get()) >= size) {
            memcpy(data, c->buf.data() + c->off, size);
            c->off += size;
            if (c->off == c->buf.size()) {
                c->buf.clear();
                c->off = 0;
            } else if (c->off > 1024 * 1024) { // reads that straddle messages: drop what was read
                c->buf.erase(c->buf.begin(), c->buf.begin() + c->off);
                c->off = 0;
            }
            return true;
        }
        if (Pull(c.get())) {
            last = Now();
            continue;
        }
        if (c->gone) {
            // The last message may have landed between the Pull above and the close.
            if (Pull(c.get())) continue;
            Logf("republicmp  NETWORK: the Steam peer closed the connection (%zu of %zu bytes)", Buffered(c.get()), size);
            return false;
        }
        DWORD timeout = c->ioTimeoutMs;
        if (timeout && Now() - last > timeout) {
            Logf("republicmp  NETWORK: TIMEOUT - Steam receive stalled (%zu of %zu bytes)", Buffered(c.get()), size);
            return false;
        }
        Sleep(1);
    }
}

bool SteamNet_PeerGone(NetLink link)
{
    ConnPtr c = Find(link);
    if (!c || c->shut) return true;
    std::unique_lock<std::mutex> rx(c->rx, std::try_to_lock);
    if (!rx.owns_lock()) return false; // someone is reading it: alive
    bool gone = c->gone; // read first: data that came before the close still counts
    if (Buffered(c.get()) || Pull(c.get())) return false; // pending data counts as alive
    return gone;
}

bool SteamNet_WaitReadable(NetLink link, DWORD waitMs, volatile bool* cancelFlag)
{
    ConnPtr c = Find(link);
    if (!c) return false;
    ULONGLONG start = Now();
    for (;;) {
        if (cancelFlag && *cancelFlag) return false;
        if (c->shut || c->gone) return true; // the connection closing counts as readable
        {
            std::lock_guard<std::mutex> rx(c->rx);
            if (Buffered(c.get()) || Pull(c.get())) return true;
        }
        if (Now() - start >= waitMs) return false;
        Sleep(5);
    }
}

void SteamNet_Shutdown(NetLink link)
{
    ConnPtr c = Find(link);
    if (!c || c->shut.exchange(true)) return;
    {
        // Out of the map now: the freed handle may come back for a new player,
        // and the late Close of this link must not find that one.
        std::lock_guard<std::mutex> l(g_steamLock);
        auto it = g_conns.find(c->h);
        if (it != g_conns.end() && it->second == c) g_conns.erase(it);
    }
    g_sockets->CloseConnection(c->h, 0, "shutdown", true); // linger: what was sent still arrives
}

void SteamNet_Close(NetLink link)
{
    ConnPtr c;
    {
        std::lock_guard<std::mutex> l(g_steamLock);
        auto it = g_conns.find((HSteamNetConnection)(link & 0xFFFFFFFFu));
        if (!SteamNet_IsLink(link) || it == g_conns.end()) return;
        c = it->second;
        g_conns.erase(it);
    }
    // After Shutdown the handle is already freed, and Steam may have reused it.
    if (!c->shut.exchange(true)) g_sockets->CloseConnection(c->h, 0, "closed", true);
}
