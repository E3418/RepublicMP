// SPDX-License-Identifier: GPL-3.0-only
// Proxy DLL for integrating a multiplayer mod into a game's executable file, experimental version by E3418 (XINPUT9_1_0.DLL)

#pragma comment(linker, "/export:XInputGetCapabilities=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\XINPUT9_1_0.XInputGetCapabilities,@2")
#pragma comment(linker, "/export:XInputGetDSoundAudioDeviceGuids=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\XINPUT9_1_0.XInputGetDSoundAudioDeviceGuids,@3")
#pragma comment(linker, "/export:XInputGetState=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\XINPUT9_1_0.XInputGetState,@4")
#pragma comment(linker, "/export:XInputSetState=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\XINPUT9_1_0.XInputSetState,@5")