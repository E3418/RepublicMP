// SPDX-License-Identifier: GPL-3.0-only
// See world.h.
#include "world.h"
#include <math.h>
#include <string.h>

#define MOUSE_POINT_OFFSET 0xF78
#define DAY_OFFSET 0x590
#define YEAR_OFFSET 0x594
#define DAY_PROGRESS_OFFSET 0x59C
#define DOLLARS_OFFSET 0x580
#define RUBLES_OFFSET 0x588

void World_Bind(const TsmHost* host)
{
    TsmBind(host);
}

unsigned char* World_Object()
{
    return g_exeBase + GAME_WORLD_RVA;
}

bool World_IsLoaded()
{
    __try {
        return *(__int64*)(g_exeBase + GAME_ROAD_NETWORK_RVA) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool World_Buildings(unsigned char*** begin, unsigned char*** end)
{
    __try {
        *begin = *(unsigned char***)(g_exeBase + GAME_BUILDINGS_BEGIN_RVA);
        *end = *(unsigned char***)(g_exeBase + GAME_BUILDINGS_END_RVA);
        return *begin && *end >= *begin;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int World_BuildingCategory(const unsigned char* building)
{
    __try {
        const unsigned char* type = *(const unsigned char* const*)(building + GAME_BUILDING_TYPE_OFFSET);
        return type ? *(const int*)(type + GAME_TYPE_CATEGORY_OFFSET) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool World_BuildingPos(const unsigned char* building, float out[3])
{
    __try {
        memcpy(out, building + GAME_BUILDING_POS_OFFSET, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool World_MousePoint(float out[3])
{
    __try {
        memcpy(out, World_Object() + MOUSE_POINT_OFFSET, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]) && (out[0] != 0 || out[1] != 0 || out[2] != 0);
}

void World_Date(int* day, int* year, float* progress)
{
    unsigned char* w = World_Object();
    if (day) *day = *(int*)(w + DAY_OFFSET);
    if (year) *year = *(int*)(w + YEAR_OFFSET);
    if (progress) *progress = *(float*)(w + DAY_PROGRESS_OFFSET);
}

void World_Money(double* rubles, double* dollars)
{
    unsigned char* w = World_Object();
    if (rubles) *rubles = *(double*)(w + RUBLES_OFFSET);
    if (dollars) *dollars = *(double*)(w + DOLLARS_OFFSET);
}
