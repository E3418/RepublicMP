// See sync_check.h.
#include "sync_check.h"
#include "build_sync.h"
#include "save_sync.h"
#include "../hooks/state_hash.h"
#include "../hooks/determinism.h"
#include "../ui/sync_block.h"
#include <math.h>

#define HOUR_LENGTH 2.5f        // day progress units per game hour (60 per day)
#define HISTORY 64              // host: own summaries kept for matching

void SyncCheck_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static CRITICAL_SECTION g_checkLock;
static long long g_lastHourKey = -1;      // main thread
static SyncCheckWire g_history[HISTORY];   // host: own summaries, ring
static int g_historyNext = 0;
static SyncCheckWire g_pending;            // client: summary waiting to be sent
static bool g_hasPending = false;
static SyncDrift g_drift = {};
static volatile int g_autoPercent = 0;

static long long HourKey(int year, int day, int hour)
{
    return ((long long)year * 400 + day) * 24 + hour;
}

// Main thread, after every simulation step.
static void OnStep()
{
    if (!BuildSync_IsConnected() || SyncBlock_IsActive()) {
        g_lastHourKey = -1;
        return;
    }
    StateSummary s;
    // Cheap check first: only the clock decides whether this step opens a new hour.
    __try {
        BYTE* w = g_exeBase + 0x9D4F10;
        int day = *(int*)(w + 0x590), year = *(int*)(w + 0x594);
        int hour = (int)(*(float*)(w + 0x59c) / HOUR_LENGTH);
        long long key = HourKey(year, day, hour);
        if (key == g_lastHourKey) return;
        bool first = g_lastHourKey < 0;
        g_lastHourKey = key;
        if (first) return; // joined mid-hour - start with the next full hour
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!StateHash_Compute(&s)) return;

    SyncCheckWire wire = {};
    wire.year = s.year;
    wire.day = s.day;
    wire.hour = (int)(s.dayProgress / HOUR_LENGTH);
    wire.buildings = s.buildings;
    wire.vehicles = s.vehicles;
    wire.citizens = s.citizens;
    wire.storage = s.storageTotal;
    wire.cargo = s.cargoTotal;
    wire.production = s.productionTotal;

    EnterCriticalSection(&g_checkLock);
    if (BuildSync_IsHostRole()) {
        g_history[g_historyNext] = wire;
        g_historyNext = (g_historyNext + 1) % HISTORY;
    } else {
        g_pending = wire;
        g_hasPending = true;
    }
    LeaveCriticalSection(&g_checkLock);
}

void SyncCheck_Install()
{
    InitializeCriticalSection(&g_checkLock);
    Determinism_AddStepObserver(&OnStep);
}

void SyncCheck_Tick()
{
    SyncCheckWire w;
    bool send = false;
    EnterCriticalSection(&g_checkLock);
    if (g_hasPending) {
        w = g_pending;
        g_hasPending = false;
        send = true;
    }
    LeaveCriticalSection(&g_checkLock);
    if (send && !BuildSync_IsHostRole()) BuildSync_SendState(w);
}

static float Percent(double a, double b)
{
    double base = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    if (base < 1.0) base = 1.0;
    return (float)(fabs(a - b) / base * 100.0);
}

void SyncCheck_OnRemote(const SyncCheckWire& c)
{
    if (!BuildSync_IsHostRole()) return;
    SyncCheckWire h;
    bool found = false;
    EnterCriticalSection(&g_checkLock);
    for (int i = 0; i < HISTORY; i++) {
        const SyncCheckWire& e = g_history[i];
        if (e.year == c.year && e.day == c.day && e.hour == c.hour && (e.year || e.day)) {
            h = e;
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_checkLock);
    if (!found) {
        Logf("republicmp  SYNC_CHECK: no own summary for year %d day %d hour %d - skipped", c.year, c.day, c.hour);
        return;
    }

    SyncDrift d = {};
    d.valid = true;
    d.year = c.year; d.day = c.day; d.hour = c.hour;
    d.citizens = Percent(h.citizens, c.citizens);
    d.vehicles = Percent(h.vehicles, c.vehicles);
    d.storage = Percent(h.storage + h.cargo, c.storage + c.cargo);
    d.production = Percent(h.production, c.production);
    d.total = d.citizens;
    if (d.vehicles > d.total) d.total = d.vehicles;
    if (d.storage > d.total) d.total = d.storage;
    d.hostVehicles = h.vehicles; d.clientVehicles = c.vehicles;
    d.hostCitizens = h.citizens; d.clientCitizens = c.citizens;
    EnterCriticalSection(&g_checkLock);
    g_drift = d;
    LeaveCriticalSection(&g_checkLock);

    Logf("republicmp  SYNC_CHECK: day %d hour %d drift %.1f%% (citizens %d/%d, vehicles %d/%d, storage %.0f/%.0f, buildings %d/%d)",
         c.day, c.hour, d.total, h.citizens, c.citizens, h.vehicles, c.vehicles, h.storage + h.cargo,
         c.storage + c.cargo, h.buildings, c.buildings);

    int limit = g_autoPercent;
    if (limit > 0 && d.total > (float)limit && !SaveSync_IsResyncing()) {
        Logf("republicmp  SYNC_CHECK: drift %.1f%% above %d%% - re-syncing", d.total, limit);
        SaveSync_RequestResync();
    }
}

SyncDrift SyncCheck_GetDrift()
{
    EnterCriticalSection(&g_checkLock);
    SyncDrift d = g_drift;
    LeaveCriticalSection(&g_checkLock);
    return d;
}

void SyncCheck_SetAutoResyncPercent(int percent) { g_autoPercent = percent > 0 ? percent : 0; }
int SyncCheck_GetAutoResyncPercent() { return g_autoPercent; }
