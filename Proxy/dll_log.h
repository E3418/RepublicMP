// SPDX-License-Identifier: GPL-3.0-only
// Proxy DLL for integrating a multiplayer mod into a game's executable file, experimental version by E3418 (XINPUT9_1_0.DLL)

#pragma once
#include <windows.h>

bool Log_Open(const wchar_t* path);
void Log_Write(const char* text);
void Log_WritePath(const char* label, const wchar_t* path);
void Log_Close();