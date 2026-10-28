// SPDX-License-Identifier: GPL-3.0-only
// See weather.h.
#include "weather.h"
#include "world.h"

#define W_SNOWING 0xE28
#define W_TEMPERATURE 0xE30
#define W_WIND 0xE60
#define W_SKY 0xE70     // 0 clear, 1 dawn / dusk, 2 night, 3 rain
#define W_RAIN 0xE74    // the rain's strength (333bd0)
#define W_WETNESS 0xE78 // the roads' wetness (5cfea0)
#define SKY_RVA 0x333BD0     // (world, init): the sky and the rain
#define WEATHER_RVA 0x5CFEA0 // (world, init): temperature, wind, snow, wetness

typedef void (*WeatherFn)(void* world, unsigned long long init);

void Weather_Bind(const TsmHost* host)
{
    TsmBind(host);
}

void Weather_StartShared()
{
    BYTE* w = g_exeBase + GAME_WORLD_RVA;
    __try {
        float before = *(float*)(w + W_TEMPERATURE);
        // What the initializations leave as they find it: a clear sky, no
        // rain, dry roads - the same on every PC before the shared draws.
        *(int*)(w + W_SKY) = 0;
        *(float*)(w + W_RAIN) = 0.0f;
        *(float*)(w + W_WETNESS) = 0.0f;
        ((WeatherFn)(g_exeBase + SKY_RVA))(w, 1);
        ((WeatherFn)(g_exeBase + WEATHER_RVA))(w, 1);
        Logf("republicmp  WEATHER: started for the session - %.2f C (was %.2f), wind %.2f, snowing %d, sky %d",
             *(float*)(w + W_TEMPERATURE), before, *(float*)(w + W_WIND), *(int*)(w + W_SNOWING), *(int*)(w + W_SKY));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  WEATHER: could not be started for the session");
    }
}
