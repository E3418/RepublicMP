// SPDX-License-Identifier: GPL-3.0-only
// Proxy DLL for integrating a multiplayer mod into a game's executable file, experimental version by E3418 (XINPUT9_1_0.DLL)

#include "dll_entry.h"
#include <windows.h>
#include <string.h>

// jmp qword ptr [rip+0], then the 8 byte address. It reaches anywhere in the address space.
static constexpr size_t JUMP_SIZE = 14;

static unsigned char g_saved[JUMP_SIZE];
static unsigned char* g_entry;
static void (*g_callback)();

static bool WriteCode(void* to, const void* from, size_t size) {
	DWORD old;
	if (!VirtualProtect(to, size, PAGE_EXECUTE_READWRITE, &old)) return false;
	memcpy(to, from, size);
	VirtualProtect(to, size, old, &old);
	FlushInstructionCache(GetCurrentProcess(), to, size);
	return true;
}

// Windows calls the exe's entry point with the PEB as its one argument, so this has the
// same shape. The original bytes go back first, which is why no trampoline is needed:
// the game's entry point runs exactly as it was built.
static DWORD WINAPI OnEntry(LPVOID peb) {
	WriteCode(g_entry, g_saved, JUMP_SIZE);
	g_callback();
	return ((LPTHREAD_START_ROUTINE)g_entry)(peb);
}

bool Entry_Hook(void (*callback)()) {
	BYTE* exe = (BYTE*)GetModuleHandleW(nullptr);
	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe;
	IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(exe + dos->e_lfanew);

	g_entry = exe + nt->OptionalHeader.AddressOfEntryPoint;
	g_callback = callback;
	memcpy(g_saved, g_entry, JUMP_SIZE);

	unsigned char jump[JUMP_SIZE] = { 0xFF, 0x25, 0, 0, 0, 0 };
	void* target = (void*)OnEntry;
	memcpy(jump + 6, &target, sizeof(target));
	return WriteCode(g_entry, jump, JUMP_SIZE);
}
