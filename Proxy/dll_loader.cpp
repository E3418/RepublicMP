// SPDX-License-Identifier: GPL-3.0-only
// Proxy DLL for integrating a multiplayer mod into a game's executable file, experimental version by E3418 (XINPUT9_1_0.DLL)

#include "dll_loader.h"
#include "dll_log.h"
#include <stdio.h>
#include <wchar.h>

bool Loader_Init(HMODULE self) {
	wchar_t path[MAX_PATH];
	if (!GetModuleFileNameW(self, path, MAX_PATH)) return false;

	wchar_t* slash = wcsrchr(path, L'\\');
	if (!slash) return false;
	*slash = 0;

	wchar_t dir[MAX_PATH + 64];
	swprintf_s(dir, L"%ls\\RepublicMP", path);
	CreateDirectoryW(dir, nullptr);
	swprintf_s(dir, L"%ls\\RepublicMP\\logs", path);
	CreateDirectoryW(dir, nullptr);

	wchar_t logPath[MAX_PATH + 64];
	swprintf_s(logPath, L"%ls\\RepublicMP\\logs\\proxy.log", path);
	return Log_Open(logPath);
}