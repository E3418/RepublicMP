// SPDX-License-Identifier: GPL-3.0-only
// Proxy DLL for integrating a multiplayer mod into a game's executable file, experimental version by E3418 (XINPUT9_1_0.DLL)

#include "dll_log.h"
#include <stdio.h>
#include <string.h>

static HANDLE g_file = INVALID_HANDLE_VALUE;

bool Log_Open(const wchar_t* path) {
	g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	return g_file != INVALID_HANDLE_VALUE;
}

void Log_Write(const char* text) {
	if (g_file == INVALID_HANDLE_VALUE) return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	char line[600];
	sprintf_s(line, "[%02d:%02d:%02d] %s\r\n", t.wHour, t.wMinute, t.wSecond, text);
	DWORD written = 0;
	WriteFile(g_file, line, (DWORD)strlen(line), &written, nullptr);
}

void Log_Close() {
	if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
	g_file = INVALID_HANDLE_VALUE;
}