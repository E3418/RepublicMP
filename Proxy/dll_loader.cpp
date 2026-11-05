// SPDX-License-Identifier: GPL-3.0-only
// Proxy DLL for integrating a multiplayer mod into a game's executable file, experimental version by E3418 (XINPUT9_1_0.DLL)

#include "dll_loader.h"
#include "dll_log.h"
#include <stdio.h>
#include <wchar.h>

static wchar_t g_gameDir[MAX_PATH];

bool Loader_IsGame() {
	wchar_t path[MAX_PATH];
	if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
	const wchar_t* slash = wcsrchr(path, L'\\');
	return slash && _wcsicmp(slash + 1, L"SOVIET64.exe") == 0;
}

bool Loader_Init(HMODULE self) {
	wchar_t path[MAX_PATH];
	if (!GetModuleFileNameW(self, path, MAX_PATH)) return false;

	wchar_t* slash = wcsrchr(path, L'\\');
	if (!slash) return false;
	*slash = 0;

	wcscpy_s(g_gameDir, path);

	wchar_t modDir[MAX_PATH + 16];
	swprintf_s(modDir, L"%ls\\RepublicMP", path);
	CreateDirectoryW(modDir, nullptr);

	wchar_t logs[MAX_PATH + 64];
	swprintf_s(logs, L"%ls\\logs", modDir);
	CreateDirectoryW(logs, nullptr);

	// A log that will not open is no reason to leave the player without the mod.
	wchar_t logPath[MAX_PATH + 64];
	swprintf_s(logPath, L"%ls\\proxy.log", logs);
	Log_Open(logPath);
	return true;
}

void Loader_StartMod() {
	// A name without a folder matches a module loaded from any folder, so this also
	// catches the mod started by another loader, Tesmio's plugins folder included.
	if (GetModuleHandleW(L"RepublicMP.dll")) {
		Log_Write("RepublicMP.dll is already loaded by something else, not starting it twice");
		return;
	}

	wchar_t path[MAX_PATH + 64];
	swprintf_s(path, L"%ls\\RepublicMP.dll", g_gameDir);
	Log_WritePath("mod", path);

	// The flag makes Windows look for the mod's own dependencies next to the mod.
	HMODULE mod = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	char buf[128];
	if (!mod) {
		sprintf_s(buf, "mod did not load, error %lu", GetLastError());
		Log_Write(buf);
		return;
	}
	sprintf_s(buf, "mod base %p", mod);
	Log_Write(buf);

	typedef int (*StartFn)();
	StartFn start = (StartFn)GetProcAddress(mod, "RepublicMP_Start");
	if (!start) {
		Log_Write("mod has no RepublicMP_Start export, nothing started");
		return;
	}

	int result = start();
	sprintf_s(buf, "RepublicMP_Start returned %d", result);
	Log_Write(buf);
}
