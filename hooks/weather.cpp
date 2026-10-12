// See weather.h.
#include "weather.h"
#include "determinism.h"
#include "../net/build_sync.h"
#include "../ui/sync_block.h"
#include <math.h>

#define WORLD_RVA 0x9D4F10
#define W_SNOWING 0xE28
#define W_SNOW_TIMER 0xE2C
#define W_TEMPERATURE 0xE30
#define W_TEMPERATURE_TARGET 0xE34
#define W_TEMPERATURE_TIMER 0xE38
#define W_WIND 0xE60
#define W_WIND_TARGET 0xE64
#define W_WIND_TIMER 0xE68
#define W_SKY 0xE70
#define W_WETNESS 0xE78
#define W_DAY_START_RESET 0x598      // zeroed by FUN_140333bd0 before a sky switch
#define SET_SKY_RVA 0x334020         // FUN_140334020(_, type, -1, blend)
#define SKY_BLEND 0.001f             // DAT_140909afc, the blend FUN_140333bd0 uses

void Weather_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static CRITICAL_SECTION g_weatherLock;
static WeatherWire g_received;
static bool g_hasReceived = false;
static ULONGLONG g_lastSendMs = 0;
static bool g_loggedFirst = false;

static BYTE* World() { return g_exeBase + WORLD_RVA; }

static bool ReadWeather(WeatherWire* w)
{
    __try {
        BYTE* p = World();
        w->temperature = *(float*)(p + W_TEMPERATURE);
        w->temperatureTarget = *(float*)(p + W_TEMPERATURE_TARGET);
        w->temperatureTimer = *(float*)(p + W_TEMPERATURE_TIMER);
        w->wind = *(float*)(p + W_WIND);
        w->windTarget = *(float*)(p + W_WIND_TARGET);
        w->windTimer = *(float*)(p + W_WIND_TIMER);
        w->snowing = *(int*)(p + W_SNOWING);
        w->snowTimer = *(float*)(p + W_SNOW_TIMER);
        w->sky = *(int*)(p + W_SKY);
        w->wetness = *(float*)(p + W_WETNESS);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

typedef void (*SetSkyFn)(__int64 unused, int type, int index, float blend);

static void WriteWeather(const WeatherWire& w)
{
    __try {
        BYTE* p = World();
        *(float*)(p + W_TEMPERATURE) = w.temperature;
        *(float*)(p + W_TEMPERATURE_TARGET) = w.temperatureTarget;
        *(float*)(p + W_TEMPERATURE_TIMER) = w.temperatureTimer;
        *(float*)(p + W_WIND) = w.wind;
        *(float*)(p + W_WIND_TARGET) = w.windTarget;
        *(float*)(p + W_WIND_TIMER) = w.windTimer;
        *(int*)(p + W_SNOWING) = w.snowing;
        *(float*)(p + W_SNOW_TIMER) = w.snowTimer;
        *(float*)(p + W_WETNESS) = w.wetness;
        int sky = *(int*)(p + W_SKY);
        if (sky != w.sky && w.sky >= 0 && w.sky <= 3) {
            *(int*)(p + W_SKY) = w.sky;
            *(int*)(p + W_DAY_START_RESET) = 0;
            ((SetSkyFn)(g_exeBase + SET_SKY_RVA))(0, w.sky, -1, SKY_BLEND);
            Logf("republicmp  WEATHER: sky %d -> %d (host)", sky, w.sky);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool Plausible(const WeatherWire& w)
{
    return isfinite(w.temperature) && isfinite(w.temperatureTarget) && isfinite(w.temperatureTimer) &&
           isfinite(w.wind) && isfinite(w.windTarget) && isfinite(w.windTimer) && isfinite(w.snowTimer) &&
           isfinite(w.wetness) && fabsf(w.temperature) < 100.0f && w.wetness >= 0.0f && w.wetness <= 1.5f;
}

// Main thread, right after a simulation step.
static void OnStep()
{
    if (!g_hasReceived) return;
    WeatherWire w;
    EnterCriticalSection(&g_weatherLock);
    w = g_received;
    g_hasReceived = false;
    LeaveCriticalSection(&g_weatherLock);
    if (BuildSync_IsHostRole() || !BuildSync_IsConnected() || SyncBlock_IsActive()) return;
    if (!g_loggedFirst) {
        g_loggedFirst = true;
        Logf("republicmp  WEATHER: following the host - %.1f°C, wind %.0f, sky %d, snowing %d", w.temperature,
             w.wind, w.sky, w.snowing);
    }
    WriteWeather(w);
}

void Weather_Install()
{
    InitializeCriticalSection(&g_weatherLock);
    Determinism_AddStepObserver(&OnStep);
}

void Weather_SyncTick()
{
    bool syncing = BuildSync_IsConnected() && !SyncBlock_IsActive();
    if (!syncing) {
        g_loggedFirst = false;
        return;
    }
    if (!BuildSync_IsHostRole()) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_lastSendMs < WEATHER_SYNC_INTERVAL_MS) return;
    g_lastSendMs = now;
    WeatherWire w;
    if (ReadWeather(&w)) BuildSync_SendWeather(w);
}

void Weather_OnRemote(const WeatherWire& w)
{
    if (BuildSync_IsHostRole() || !Plausible(w)) return;
    EnterCriticalSection(&g_weatherLock);
    g_received = w;
    g_hasReceived = true;
    LeaveCriticalSection(&g_weatherLock);
}
