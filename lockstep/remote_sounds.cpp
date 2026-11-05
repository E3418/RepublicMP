// SPDX-License-Identifier: GPL-3.0-only
// See remote_sounds.h.
#include "remote_sounds.h"
#include "commands.h"
#include "../core/profile.h"

#define DLL_C3D "C3DDLL64.dll"
#define PLAY_2D "?PlaySound2D@C3D_SOUNDMANAGER@@QEAA_NPEAD_NM0@Z"
#define PLAY_3D "?PlaySound3D@C3D_SOUNDMANAGER@@QEAA_NPEADVC3DVECTOR3@@_NM@Z"
#define LOOP_2D "?Play@C3D_SOUND2D@@QEAAX_N@Z" // a looped sound the game keeps (GetSound2DForLoop)
#define LOOP_3D "?Play@C3D_SOUND3D@@QEAAX_N@Z"
#define WORLD_RVA 0x9D4F10
#define TOOL_LOOPS 0xD488      // world: vector of the tools' looped sounds (begin, end)
#define TOOL_LOOP_BYTES 0x110
#define TOOL_LOOP_ACTIVE 0x10C // byte: set by a tool's edit (3aa76b, 3abc75); the Late body's
                               // 3ab920 fades the loop in while it is set
#define MAX_TOOL_LOOPS 64

// A C3DVECTOR3 (12 bytes) is passed by value as a pointer to the caller's copy.
typedef bool (*Play2DFn)(void* manager, char* name, bool flag, float value, char* extra);
typedef bool (*Play3DFn)(void* manager, char* name, const void* position, bool flag, float value);
typedef void (*LoopFn)(void* sound, bool flag);

static Play2DFn o_play2D = nullptr;
static Play3DFn o_play3D = nullptr;
static LoopFn o_loop2D = nullptr, o_loop3D = nullptr;
static bool g_muted = true;
static bool g_silent = false; // RemoteSounds_Silence: nobody hears anything for now
static bool g_log = false; // profile "debug" "sound_log": every sound with the command it came in
static unsigned char g_active[MAX_TOOL_LOOPS];
static size_t g_activeCount = 0;

static bool LeaveOut(const char* kind, const char* name)
{
    bool remote = Commands_ApplyingRemote();
    bool out = g_silent || (g_muted && remote);
    if (g_log)
        Logf("republicmp  SOUND: %s [%s]%s%s", kind, name ? name : "?", Commands_Applying() ? (remote ? " in another player's command" : " in this player's command") : "",
             out ? " - left out" : "");
    return out;
}

static bool H_play2D(void* manager, char* name, bool flag, float value, char* extra)
{
    if (LeaveOut("2D", name)) return false;
    return o_play2D(manager, name, flag, value, extra);
}

static bool H_play3D(void* manager, char* name, const void* position, bool flag, float value)
{
    if (LeaveOut("3D", name)) return false;
    return o_play3D(manager, name, position, flag, value);
}

static void H_loop2D(void* sound, bool flag)
{
    if (!LeaveOut("2D loop", nullptr)) o_loop2D(sound, flag);
}

static void H_loop3D(void* sound, bool flag)
{
    if (!LeaveOut("3D loop", nullptr)) o_loop3D(sound, flag);
}

// The tool loops a command switched on play on the frames after it, outside
// the command: their switches go back to what they were.
static unsigned char* ToolLoops(size_t* count)
{
    unsigned char* world = g_exeBase + WORLD_RVA;
    unsigned char* begin = *(unsigned char**)(world + TOOL_LOOPS);
    unsigned char* end = *(unsigned char**)(world + TOOL_LOOPS + 8);
    size_t n = begin && end > begin ? (size_t)(end - begin) / TOOL_LOOP_BYTES : 0;
    *count = n < MAX_TOOL_LOOPS ? n : MAX_TOOL_LOOPS;
    return begin;
}

static void KeepToolLoops()
{
    g_activeCount = 0;
    __try {
        size_t n;
        unsigned char* loops = ToolLoops(&n);
        for (size_t i = 0; i < n; i++) g_active[i] = loops[i * TOOL_LOOP_BYTES + TOOL_LOOP_ACTIVE];
        g_activeCount = n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_activeCount = 0;
    }
}

void RemoteSounds_CommandBegin()
{
    g_activeCount = 0;
    if (g_muted) KeepToolLoops();
}

void RemoteSounds_CommandEnd()
{
    if (!g_activeCount) return;
    __try {
        size_t n;
        unsigned char* loops = ToolLoops(&n);
        for (size_t i = 0; i < n && i < g_activeCount; i++) {
            unsigned char& active = loops[i * TOOL_LOOP_BYTES + TOOL_LOOP_ACTIVE];
            if (active != g_active[i] && g_log) Logf("republicmp  SOUND: tool loop %zu switched on by another player's command - left out", i);
            active = g_active[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_activeCount = 0;
}

void RemoteSounds_Silence(bool on)
{
    g_silent = on;
    if (on) KeepToolLoops();
    else RemoteSounds_CommandEnd();
}

void RemoteSounds_Bind(const ModHost* host)
{
    ModHost_Bind(host);
    g_muted = Profile_GetInt("overlay", "remote_sounds", 1) != 0;
    g_log = Profile_GetInt("debug", "sound_log", 0) != 0;
}

bool RemoteSounds_Install()
{
    bool a = PatchIat(g_exe, DLL_C3D, PLAY_2D, (void*)&H_play2D, (void**)&o_play2D, "republicmp PlaySound2D");
    bool b = PatchIat(g_exe, DLL_C3D, PLAY_3D, (void*)&H_play3D, (void**)&o_play3D, "republicmp PlaySound3D");
    bool c = PatchIat(g_exe, DLL_C3D, LOOP_2D, (void*)&H_loop2D, (void**)&o_loop2D, "republicmp C3D_SOUND2D Play");
    bool d = PatchIat(g_exe, DLL_C3D, LOOP_3D, (void*)&H_loop3D, (void**)&o_loop3D, "republicmp C3D_SOUND3D Play");
    return a && b && c && d;
}

bool RemoteSounds_GetMuted() { return g_muted; }

void RemoteSounds_SetMuted(bool on)
{
    g_muted = on;
    Profile_SetInt("overlay", "remote_sounds", on ? 1 : 0);
}
