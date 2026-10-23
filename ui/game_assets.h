// SPDX-License-Identifier: GPL-3.0-only
// The game's own look for the mod's pages: its textures (media_soviet\...,
// PNG or DDS) and its bitmap fonts (media_soviet\fontsbitmap, BMFont v3 with
// DXT5 pages), drawn through ImGui's draw lists. Render thread only; loaded on
// first use and kept for the process.
#pragma once

#include "imgui.h"

enum GameFont {
    GAME_FONT_SQUARE, // the menu rows
    GAME_FONT_BURAN,  // titles
    GAME_FONT_ARIAL,  // plain text
    GAME_FONT_COUNT
};

// A texture by its path under media_soviet ("menu\\strip_wide.png"); 0 when
// it could not be loaded. size gets its pixel size.
ImTextureID GameAssets_Texture(const char* path, ImVec2* size = nullptr);

// Text in a game font at a pixel height; nothing when the font is missing.
ImVec2 GameText_Size(GameFont font, float height, const char* text);
void GameText_Draw(ImDrawList* drawList, GameFont font, float height, ImVec2 pos, ImU32 color, const char* text);
