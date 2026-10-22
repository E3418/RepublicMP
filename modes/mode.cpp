// SPDX-License-Identifier: GPL-3.0-only
// See mode.h.
#include "mode.h"

static const RmpMode* g_republics = nullptr;

void Modes_SetRepublics(const RmpMode* mode)
{
    g_republics = mode;
}

const RmpMode* Modes_Republics()
{
    return g_republics;
}

void Modes_Frame()
{
    if (g_republics && g_republics->frame) g_republics->frame();
}

bool Modes_WantsOverlay()
{
    return g_republics && g_republics->drawOverlay && g_republics->wantsOverlay && g_republics->wantsOverlay();
}

void Modes_DrawOverlay()
{
    if (Modes_WantsOverlay()) g_republics->drawOverlay();
}
