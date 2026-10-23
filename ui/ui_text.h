// SPDX-License-Identifier: GPL-3.0-only
// The mod's page in the game's language: the language the game is set to
// (media_soviet\config.ini, "$TEXT LANGUAGE2 soviet<Language>.btf") picks a
// table compiled in ui_text.cpp. English is the key and the fallback, so a
// text with no translation, or a language with no table, stays English.
#pragma once

// The text in the game's language (UTF-8); the English text itself when there
// is no translation.
const char* T(const char* english);

// The game's language uses Cyrillic letters (its menu font is then "Buran").
bool UiText_IsCyrillic();
