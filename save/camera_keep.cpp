// SPDX-License-Identifier: GPL-3.0-only
// See camera_keep.h.
#include "camera_keep.h"
#include "mod_save.h"
#include "../game/camera.h"
#include "../game/world.h"
#include "../core/profile.h"
#include "../transport/network.h"
#include "../determinism/determinism.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#define VIEW_FLOATS ((int)(sizeof(CameraView) / sizeof(float)))
#define MAX_ENTRIES 16 // other players' entries kept from the last load

struct KeptEntry {
    std::string key, value;
};

#define LIVE_MAX_AGE_MS (10 * 60 * 1000) // a view older than this is not brought back

static CameraView g_live;       // this PC's view right before a session's load
static bool g_liveValid = false;
static CameraView g_last;       // the last view read on a frame or step
static ULONGLONG g_lastAt = 0;  // when (0: none since the last load)
static CameraView g_saved;      // this player's view in the save just loaded
static bool g_savedValid = false;
static std::vector<KeptEntry> g_others;
static bool g_failLogged = false; // one line per load when the view cannot be read

// The values Camera_GetView refused, once per load: which of them is off.
static void LogUnreadable(const char* where, const CameraView& v)
{
    if (g_failLogged) return;
    g_failLogged = true;
    Logf("republicmp  CAMERA: the view could not be read (%s, world %d): target %g,%g,%g zoom %g/%g position %g,%g,%g "
         "look-at %g,%g,%g",
         where, World_IsLoaded() ? 1 : 0, v.target[0], v.target[1], v.target[2], v.zoomGoal, v.zoom, v.position[0],
         v.position[1], v.position[2], v.lookAt[0], v.lookAt[1], v.lookAt[2]);
}

// "steam_<id>", or "nick_<nick>" with only letters, digits, '-' and '_'.
static void PlayerKey(char* out, size_t outSize)
{
    unsigned long long steam = NetworkSteamId();
    if (steam) {
        _snprintf_s(out, outSize, _TRUNCATE, "steam_%llu", steam);
        return;
    }
    std::string nick = Profile_GetNick();
    for (char& c : nick)
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) c = '_';
    _snprintf_s(out, outSize, _TRUNCATE, "nick_%s", nick.c_str());
}

static void Write()
{
    char key[64];
    PlayerKey(key, sizeof(key));
    CameraView v = {};
    if (World_IsLoaded() && Camera_GetView(&v)) ModSave_PutFloats(key, &v.target[0], VIEW_FLOATS);
    else if (g_lastAt) ModSave_PutFloats(key, &g_last.target[0], VIEW_FLOATS);
    else LogUnreadable("save", v);
    for (const KeptEntry& e : g_others)
        if (e.key != key) ModSave_Put(e.key.c_str(), e.value.c_str());
}

static void KeepOther(const char* key, const char* value, void* ctx)
{
    if (strcmp(key, (const char*)ctx) && g_others.size() < MAX_ENTRIES) g_others.push_back({ key, value });
}

static void Read(bool found)
{
    g_others.clear();
    g_savedValid = false;
    if (!found) return;
    char key[64];
    PlayerKey(key, sizeof(key));
    g_savedValid = ModSave_GetFloats(key, &g_saved.target[0], VIEW_FLOATS);
    ModSave_ForEach(KeepOther, key);
}

// Every frame of a session and every step outside one (main thread): the
// view right before a load is not readable at the load itself.
static void Sample()
{
    CameraView v;
    if (World_IsLoaded() && Camera_GetView(&v)) {
        g_last = v;
        g_lastAt = GetTickCount64();
    }
}

void CameraKeep_Bind(const ModHost* host)
{
    ModHost_Bind(host);
    ModSave_Add("camera", Write, Read);
    Determinism_AddFrameObserver(Sample);
    Determinism_AddStepObserver(Sample);
}

void CameraKeep_BeforeLoad(bool session)
{
    g_liveValid = false;
    CameraView v = {};
    if (session && World_IsLoaded() && Camera_GetView(&v)) {
        g_live = v;
        g_liveValid = true;
    } else if (session && g_lastAt && GetTickCount64() - g_lastAt < LIVE_MAX_AGE_MS) {
        g_live = g_last;
        g_liveValid = true;
    } else if (session) {
        LogUnreadable("before the load", v);
    }
    g_lastAt = 0; // a view of the world before this load
}

void CameraKeep_AfterLoad()
{
    const CameraView* v = g_liveValid ? &g_live : g_savedValid ? &g_saved : nullptr;
    if (v) {
        bool ok = Camera_SetView(*v);
        Logf("republicmp  CAMERA: %s at (%.0f, %.0f, %.0f) %s", g_liveValid ? "the view before the load" : "this player's saved view",
             v->position[0], v->position[1], v->position[2], ok ? "restored" : "could not be restored");
    }
    g_liveValid = g_savedValid = false;
    g_failLogged = false;
}
