// SPDX-License-Identifier: GPL-3.0-only
// See republics.h.
#include "republics.h"
#include "../../api/republicmp_api.h"
#include "imgui.h"

static void DrawPage()
{
    ImGui::TextWrapped("Each player builds their own republic on one map, split in two. "
                       "You see the neighbour's buildings, vehicles and citizens live, and trade with them at the border.");
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.25f, 1.0f), "In development - not playable yet.");
}

static const RmpMode kRepublics = {
    "Republics",
    "Each player their own republic, 2 players",
    "One map split in two - in development",
    DrawPage,
    nullptr, // frame
    nullptr, // wantsOverlay
    nullptr, // drawOverlay
};

void Republics_Bind(const TsmHost* host)
{
    TsmBind(host);
    Modes_SetRepublics(&kRepublics);
}
