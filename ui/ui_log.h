// Shared ring buffer of status lines for the in-game log window
// (see MainMenu::Draw in main_menu.cpp). Fed from net/save_sync.cpp's
// SaveStatus() - every existing [BUILD_SYNC]/[HOST]/[CLIENT] status line
// already routed through there reaches the overlay for free.
#pragma once

#include <string>
#include <vector>

void UiLog_Add(const char* line);
void UiLog_Clear();

// Thread-safe snapshot for rendering - copies out under a lock, safe to call
// from the render thread (Present hook) while network threads append
// concurrently via UiLog_Add.
std::vector<std::string> UiLog_Snapshot();
