// SPDX-License-Identifier: GPL-3.0-only
// See build_modes.h.
#include "build_modes.h"
#include "commands.h"
#include "../determinism/determinism.h"
#include "../world/build_mode.h"
#include <string.h>

#define MAX_PLAYERS 16
enum { USE_VIEW, USE_SIM, USE_COMMAND };

static BuildMode g_modes[MAX_PLAYERS]; // every player's, alike on every PC
static BuildMode g_own = {};           // what this PC's interface last showed
static BuildMode g_kept = {};          // this player's, while the world holds another's
static int g_use = USE_VIEW;
static bool g_ready = false;
static unsigned g_load = 0, g_keptLoad = 0;

void BuildModes_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

// A session's start or a load: every player has the world's mode.
static bool Ensure()
{
    if (!Commands_Capturing()) {
        g_ready = false;
        return false;
    }
    unsigned load = Determinism_WorldLoads();
    if (!g_ready || load != g_load) {
        g_own = BuildMode_Read();
        for (BuildMode& m : g_modes) m = g_own;
        g_load = load;
        g_ready = true;
        g_use = USE_VIEW;
        char text[96];
        Logf("republicmp  BUILD MODE: every player starts with: %s", BuildMode_Describe(g_own, text, sizeof(text)));
    }
    return true;
}

static void Enter(int use, const BuildMode& mode)
{
    g_kept = BuildMode_Read();
    g_keptLoad = g_load;
    BuildMode_Write(mode);
    g_use = use;
}

// A load under way took the world over: nothing of this player's goes back into it.
static void Leave()
{
    if (g_keptLoad == Determinism_WorldLoads()) BuildMode_Write(g_kept);
    g_use = USE_VIEW;
}

void BuildModes_Collect()
{
    if (!Ensure() || g_use != USE_VIEW) return;
    BuildMode now = BuildMode_Read();
    if (BuildMode_Equal(now, g_own)) return;
    g_own = now;
    Commands_Submit(LS_CMD_BUILD_MODE, &now, sizeof(now));
}

void BuildModes_SimBegin()
{
    if (Ensure() && g_use == USE_VIEW) Enter(USE_SIM, g_modes[0]);
}

void BuildModes_SimEnd()
{
    if (g_use == USE_SIM) Leave();
}

void BuildModes_CommandBegin(int player)
{
    if (Ensure() && g_use == USE_VIEW) Enter(USE_COMMAND, g_modes[player >= 0 && player < MAX_PLAYERS ? player : 0]);
}

void BuildModes_CommandEnd()
{
    if (g_use == USE_COMMAND) Leave();
}

bool BuildModes_ApplyCommand(int player, const void* data, size_t size)
{
    if (size != sizeof(BuildMode) || player < 0 || player >= MAX_PLAYERS || !Ensure()) return false;
    memcpy(&g_modes[player], data, sizeof(BuildMode));
    char text[96];
    Logf("republicmp  BUILD MODE: player %d: %s", player, BuildMode_Describe(g_modes[player], text, sizeof(text)));
    return true;
}
