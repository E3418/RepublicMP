// SPDX-License-Identifier: GPL-3.0-only
// See workshop.h. Calls the flat exports of the game's steam_api64.dll
// (SDK of STEAMUGC_INTERFACE_VERSION012: the instance pointer first).
#include "workshop.h"
#include "../transport/steam_net.h"
#include "../core/profile.h"
#include "../core/guard.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <stdio.h>
#include <string.h>

#define UGC_VERSION "STEAMUGC_INTERFACE_VERSION012"
#define WORKSHOP_MAX_ITEMS 20000
// EItemState
#define ITEM_SUBSCRIBED 1
#define ITEM_INSTALLED 4
#define ITEM_NEEDS_UPDATE 8
#define ITEM_DOWNLOADING 16
#define ITEM_DOWNLOAD_PENDING 32
#define SUBSCRIBE_WAIT_MS 30000   // not subscribed by then: private, removed or not on Steam
#define STALL_WAIT_MS 120000      // no byte for this long: given up

#define WORLD_RVA 0x9D4F10
#define VEHICLE_TYPES_OFFSET 0x12840 // inline records, 0x9f08 each, the name at +200
#define VEHICLE_TYPE_STRIDE 0x9F08
#define VEHICLE_TYPE_NAME 0x200
#define BUILD_ROWS_OFFSET 0xD280     // build menu rows, 0x2d0 each, the name at +0
#define BUILD_ROW_STRIDE 0x2D0

typedef unsigned (*Fn_GetNum)(void* self);
typedef unsigned (*Fn_GetSubscribed)(void* self, unsigned long long* ids, unsigned max);
typedef unsigned (*Fn_GetState)(void* self, unsigned long long id);
typedef bool (*Fn_InstallInfo)(void* self, unsigned long long id, unsigned long long* size, char* folder, unsigned folderSize,
                               unsigned* timeStamp);
typedef bool (*Fn_DownloadInfo)(void* self, unsigned long long id, unsigned long long* done, unsigned long long* total);
typedef unsigned long long (*Fn_Subscribe)(void* self, unsigned long long id);
typedef bool (*Fn_Download)(void* self, unsigned long long id, bool highPriority);

static void* g_ugc = nullptr;
static Fn_GetNum p_GetNum;
static Fn_GetSubscribed p_GetSubscribed;
static Fn_GetState p_GetState;
static Fn_InstallInfo p_InstallInfo;
static Fn_DownloadInfo p_DownloadInfo;
static Fn_Subscribe p_Subscribe;
static Fn_Download p_Download;
static bool g_sync = true;

// The screen's state and the downloads (g_workshopLock).
static CRITICAL_SECTION g_workshopLock;
static int g_phase = WORKSHOP_CLOSED;
static std::vector<WorkshopOffer> g_offers;
static std::vector<ULONGLONG> g_since, g_lastProgress; // per offer, while it runs
static size_t g_extra = 0;
static std::unordered_map<unsigned long long, std::string> g_names; // read once per run
static volatile LONG g_watching = 0;
static std::unordered_set<unsigned long long> g_declined; // not offered again in this run

void Workshop_Bind(const TsmHost* host)
{
    TsmBind(host);
    InitializeCriticalSection(&g_workshopLock);
    g_sync = Profile_GetInt("profile", "workshop_sync", 1) != 0;
}

bool Workshop_GetSync() { return g_sync; }

void Workshop_SetSync(bool on)
{
    g_sync = on;
    Profile_SetInt("profile", "workshop_sync", on ? 1 : 0);
}

static bool Ugc()
{
    if (g_ugc) return true;
    void* ugc = SteamNet_Interface(UGC_VERSION);
#define EXPORT(var, type, name) (var = (type)SteamNet_Export(name)) != nullptr
    bool ok = ugc && EXPORT(p_GetNum, Fn_GetNum, "SteamAPI_ISteamUGC_GetNumSubscribedItems") &&
              EXPORT(p_GetSubscribed, Fn_GetSubscribed, "SteamAPI_ISteamUGC_GetSubscribedItems") &&
              EXPORT(p_GetState, Fn_GetState, "SteamAPI_ISteamUGC_GetItemState") &&
              EXPORT(p_InstallInfo, Fn_InstallInfo, "SteamAPI_ISteamUGC_GetItemInstallInfo") &&
              EXPORT(p_DownloadInfo, Fn_DownloadInfo, "SteamAPI_ISteamUGC_GetItemDownloadInfo") &&
              EXPORT(p_Subscribe, Fn_Subscribe, "SteamAPI_ISteamUGC_SubscribeItem") &&
              EXPORT(p_Download, Fn_Download, "SteamAPI_ISteamUGC_DownloadItem");
#undef EXPORT
    if (ok) g_ugc = ugc;
    else {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            Logf("republicmp  WORKSHOP: Steam's Workshop interface is not available - no Workshop sync");
        }
    }
    return ok;
}

// The item's $ITEM_NAME "..." from its workshopconfig.ini.
static std::string ReadName(const char* folder)
{
    char path[MAX_PATH + 32];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\workshopconfig.ini", folder);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return "";
    char text[8192];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    const char* at = strstr(text, "$ITEM_NAME");
    if (!at) return "";
    at = strchr(at, '"');
    if (!at) return "";
    const char* end = strchr(++at, '"');
    if (!end) return "";
    return std::string(at, end - at);
}

static WorkshopItem Installed(unsigned long long id)
{
    WorkshopItem i = {};
    i.id = id;
    char folder[MAX_PATH] = "";
    unsigned stamp = 0;
    if ((p_GetState(g_ugc, id) & ITEM_INSTALLED) && p_InstallInfo(g_ugc, id, &i.size, folder, sizeof(folder), &stamp)) {
        i.updated = stamp;
        LockScope lock(g_workshopLock);
        auto name = g_names.find(id);
        if (name == g_names.end()) name = g_names.emplace(id, ReadName(folder)).first;
        strncpy_s(i.name, name->second.c_str(), _TRUNCATE);
    }
    return i;
}

bool Workshop_List(std::vector<WorkshopItem>& out)
{
    out.clear();
    if (!Ugc()) return false;
    unsigned n = p_GetNum(g_ugc);
    if (n > WORKSHOP_MAX_ITEMS) n = WORKSHOP_MAX_ITEMS;
    std::vector<unsigned long long> ids(n);
    n = n ? p_GetSubscribed(g_ugc, ids.data(), n) : 0;
    ids.resize(n);
    for (unsigned long long id : ids) out.push_back(Installed(id));
    return true;
}

WorkshopDiff Workshop_Compare(const std::vector<WorkshopItem>& host, const std::vector<WorkshopItem>& mine)
{
    WorkshopDiff d;
    std::unordered_map<unsigned long long, unsigned> have;
    for (const WorkshopItem& i : mine) have[i.id] = i.updated;
    std::unordered_set<unsigned long long> hostHas;
    for (const WorkshopItem& i : host) {
        hostHas.insert(i.id);
        auto it = have.find(i.id);
        if (it == have.end() || it->second == 0) d.missing.push_back(i);
        else if (i.updated > it->second) d.older.push_back(i);
    }
    for (const WorkshopItem& i : mine)
        if (!hostHas.count(i.id)) d.extra.push_back(i.id);
    return d;
}

std::string Workshop_Ids(const std::vector<unsigned long long>& ids, size_t shown)
{
    std::string s;
    for (size_t i = 0; i < ids.size() && i < shown; i++) {
        if (i) s += ", ";
        s += std::to_string(ids[i]);
    }
    if (ids.size() > shown) s += " (+" + std::to_string(ids.size() - shown) + ")";
    return s;
}

std::vector<unsigned long long> Workshop_IdsOf(const std::vector<WorkshopItem>& items)
{
    std::vector<unsigned long long> ids;
    for (const WorkshopItem& i : items) ids.push_back(i.id);
    return ids;
}

// ---------------------------------------------------------------------------
// The choice screen and the downloads
// ---------------------------------------------------------------------------

void Workshop_Offer(const std::vector<WorkshopItem>& need, size_t extra)
{
    LockScope lock(g_workshopLock);
    if (g_phase == WORKSHOP_DOWNLOADING) return; // the running downloads keep the screen
    g_offers.clear();
    for (const WorkshopItem& i : need) g_offers.push_back({ i, true, WORKSHOP_OFFERED, 0, 0 });
    g_extra = extra;
    g_phase = WORKSHOP_CHOOSE;
}

int Workshop_Phase() { return g_phase; }

void Workshop_Snapshot(std::vector<WorkshopOffer>& out, size_t* extra)
{
    LockScope lock(g_workshopLock);
    out = g_offers;
    *extra = g_extra;
}

void Workshop_Select(unsigned long long id, bool on)
{
    LockScope lock(g_workshopLock);
    for (WorkshopOffer& o : g_offers)
        if (o.item.id == id) o.selected = on;
}

void Workshop_SelectAll(bool on)
{
    LockScope lock(g_workshopLock);
    for (WorkshopOffer& o : g_offers) o.selected = on;
}

static void WatchOnce()
{
    ULONGLONG now = GetTickCount64();
    LockScope lock(g_workshopLock);
    bool running = false;
    for (size_t k = 0; k < g_offers.size(); k++) {
        WorkshopOffer& o = g_offers[k];
        if (o.state != WORKSHOP_RUNNING) continue;
        unsigned state = p_GetState(g_ugc, o.item.id);
        unsigned long long done = 0, total = 0;
        if (p_DownloadInfo(g_ugc, o.item.id, &done, &total) && (done != o.done || total != o.total)) {
            o.done = done;
            o.total = total;
            g_lastProgress[k] = now;
        }
        bool busy = state & (ITEM_NEEDS_UPDATE | ITEM_DOWNLOADING | ITEM_DOWNLOAD_PENDING);
        if ((state & ITEM_INSTALLED) && !busy) {
            o.state = WORKSHOP_INSTALLED;
            Logf("republicmp  WORKSHOP: item %llu installed (%s)", o.item.id, o.item.name);
        } else if ((!(state & ITEM_SUBSCRIBED) && now - g_since[k] > SUBSCRIBE_WAIT_MS) || now - g_lastProgress[k] > STALL_WAIT_MS) {
            o.state = WORKSHOP_FAILED;
            Logf("republicmp  WORKSHOP: item %llu (%s) could not be downloaded (state 0x%X) - private, removed or not on Steam?",
                 o.item.id, o.item.name, state);
        } else {
            running = true;
        }
    }
    if (!running && g_phase == WORKSHOP_DOWNLOADING) g_phase = WORKSHOP_DONE;
}

static DWORD WINAPI WatchThread(LPVOID)
{
    while (g_phase == WORKSHOP_DOWNLOADING) {
        Guard_Call("the Workshop downloads", [] { WatchOnce(); });
        Sleep(250);
    }
    InterlockedExchange(&g_watching, 0);
    int ok = 0, failed = 0;
    {
        LockScope lock(g_workshopLock);
        for (const WorkshopOffer& o : g_offers) {
            if (o.state == WORKSHOP_INSTALLED) ok++;
            if (o.state == WORKSHOP_FAILED) failed++;
        }
    }
    Logf("republicmp  WORKSHOP: downloads done - %d installed, %d could not be downloaded", ok, failed);
    return 0;
}

void Workshop_DownloadSelected()
{
    if (!Ugc()) return;
    std::vector<unsigned long long> ids;
    {
        LockScope lock(g_workshopLock);
        if (g_phase != WORKSHOP_CHOOSE) return;
        ULONGLONG now = GetTickCount64();
        g_since.assign(g_offers.size(), now);
        g_lastProgress.assign(g_offers.size(), now);
        for (WorkshopOffer& o : g_offers) {
            if (!o.selected) {
                g_declined.insert(o.item.id); // unticked: the player does not want it
                continue;
            }
            // All asked for at once; Steam downloads them.
            p_Subscribe(g_ugc, o.item.id);
            p_Download(g_ugc, o.item.id, true);
            o.state = WORKSHOP_RUNNING;
            ids.push_back(o.item.id);
        }
        if (ids.empty()) return;
        g_phase = WORKSHOP_DOWNLOADING;
    }
    Logf("republicmp  WORKSHOP: subscribing to and downloading %zu item(s) at once: %s", ids.size(), Workshop_Ids(ids).c_str());
    if (InterlockedCompareExchange(&g_watching, 1, 0) == 0) {
        HANDLE t = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        else InterlockedExchange(&g_watching, 0);
    }
}

void Workshop_Close()
{
    LockScope lock(g_workshopLock);
    // Running downloads go on in Steam; the screen comes back with the next join.
    g_phase = WORKSHOP_CLOSED;
}

void Workshop_DeclineOffered()
{
    LockScope lock(g_workshopLock);
    for (const WorkshopOffer& o : g_offers)
        if (o.state != WORKSHOP_INSTALLED) g_declined.insert(o.item.id);
}

bool Workshop_Declined(unsigned long long id)
{
    LockScope lock(g_workshopLock);
    return g_declined.count(id) != 0;
}

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

static void HashNames(BYTE* begin, BYTE* end, size_t stride, size_t nameOffset, unsigned* count, unsigned* hash)
{
    unsigned h = 2166136261u, n = 0;
    __try {
        for (BYTE* p = begin; p && p + stride <= end && n < 100000; p += stride, n++) {
            const char* name = (const char*)(p + nameOffset);
            for (int i = 0; i < 64 && name[i]; i++) h = (h ^ (unsigned char)name[i]) * 16777619u;
            h = (h ^ 0xFF) * 16777619u; // the name's end
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        h = 0;
    }
    *count = n;
    *hash = h;
}

WorkshopContent Workshop_Content()
{
    WorkshopContent c = {};
    BYTE* world = g_exeBase + WORLD_RVA;
    HashNames(*(BYTE**)(world + VEHICLE_TYPES_OFFSET), *(BYTE**)(world + VEHICLE_TYPES_OFFSET + 8), VEHICLE_TYPE_STRIDE,
              VEHICLE_TYPE_NAME, &c.vehicles, &c.vehicleHash);
    HashNames(*(BYTE**)(world + BUILD_ROWS_OFFSET), *(BYTE**)(world + BUILD_ROWS_OFFSET + 8), BUILD_ROW_STRIDE, 0, &c.buildings,
              &c.buildingHash);
    return c;
}
