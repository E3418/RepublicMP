// SPDX-License-Identifier: GPL-3.0-only
// Proxy DLL for integrating a multiplayer mod into a game's executable file, experimental version by E3418 (XINPUT9_1_0.DLL)
#include <windows.h>
#include "dll_entry.h"
#include "dll_loader.h"
#include "dll_log.h"
#include <stdio.h>

// Runs on the game's main thread, before the game's first instruction.
static void OnGameEntry() {
	char buf[128];
	sprintf_s(buf, "game entry reached, exe base %p, engine base %p", GetModuleHandleW(nullptr), GetModuleHandleW(L"C3DDLL64.dll"));
	Log_Write(buf);

	Loader_StartMod();

	Log_Write("handing over to the game");
	Log_Close();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
	switch (reason) {
	case DLL_PROCESS_ATTACH: {
		DisableThreadLibraryCalls(module);
		if (!Loader_IsGame()) break;
		if (!Loader_Init(module)) break;

		char buf[64];
		sprintf_s(buf, "proxy loaded, pid %lu", GetCurrentProcessId());
		Log_Write(buf);

		wchar_t exePath[MAX_PATH];
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		Log_WritePath("exe", exePath);

		if (!Entry_Hook(OnGameEntry)) {
			Log_Write("could not hook the game's entry point, the mod will not start");
			Log_Close();
		}
		break;
	}
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}
