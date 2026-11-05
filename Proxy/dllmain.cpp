// SPDX-License-Identifier: GPL-3.0-only
// Proxy DLL for integrating a multiplayer mod into a game's executable file, experimental version by E3418 (XINPUT9_1_0.DLL)
#include <windows.h>
#include "dll_loader.h"
#include "dll_log.h"
#include <stdio.h>

static HMODULE g_self;

static DWORD WINAPI ProxyThread(LPVOID) {
	if (!Loader_Init(g_self)) return 0;

	char buf[128];
	sprintf_s(buf, "proxy loaded, pid %lu", GetCurrentProcessId());
	Log_Write(buf);

	Log_Close();
	return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
	switch (reason) {
	case DLL_PROCESS_ATTACH: {
		g_self = module;
		DisableThreadLibraryCalls(module);
		HANDLE thread = CreateThread(nullptr, 0, ProxyThread, nullptr, 0, nullptr);
		if (thread) CloseHandle(thread);
		break;
	}
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}