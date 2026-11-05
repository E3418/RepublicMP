// SPDX-License-Identifier: GPL-3.0-only
// Proxy DLL for integrating a multiplayer mod into a game's executable file, experimental version by E3418 (XINPUT9_1_0.DLL)
#include <windows.h>
#include <stdio.h>
#include <string.h>

#pragma comment(linker, "/export:XInputGetCapabilities=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\XINPUT9_1_0.XInputGetCapabilities,@2")
#pragma comment(linker, "/export:XInputGetDSoundAudioDeviceGuids=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\XINPUT9_1_0.XInputGetDSoundAudioDeviceGuids,@3")
#pragma comment(linker, "/export:XInputGetState=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\XINPUT9_1_0.XInputGetState,@4")
#pragma comment(linker, "/export:XInputSetState=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\XINPUT9_1_0.XInputSetState,@5")

static HMODULE g_self;

static DWORD WINAPI ProxyThread(LPVOID LpParam) {
	wchar_t path[MAX_PATH];
	if (!GetModuleFileNameW(g_self, path, MAX_PATH)) return 0;

	wchar_t*slash = wcsrchr(path, L'\\');
	if (!slash) return 0;
	*slash = 0; 

	wchar_t dir[MAX_PATH + 64];
	swprintf_s(dir, L"%ls\\RepublicMP", path);
	CreateDirectoryW(dir, nullptr);

	swprintf_s(dir, L"%ls\\RepublicMP\\logs", path);
	CreateDirectoryW(dir, nullptr);

	wchar_t logPath[MAX_PATH + 64];		
	swprintf_s(logPath, L"%ls\\RepublicMP\\logs\\proxy.log", path);

	HANDLE file = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

	if (file == INVALID_HANDLE_VALUE) return 0;

	SYSTEMTIME t;
	GetLocalTime(&t);

	char buf[512];
	sprintf_s(buf, "[%02d:%02d:%02d] proxy loaded, pid %lu\r\n",
		t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());

	DWORD written = 0;
	WriteFile(file, buf, (DWORD)strlen(buf), &written, nullptr);

	CloseHandle(file); 
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