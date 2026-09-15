// Second, separate ring buffer of status lines - for verbose/technical stuff
// that would clutter the main log (see ui_log.h), e.g. net/build_sync.cpp's
// per-connection and per-placement events. Rendered in the Debug window, see
// ui/debugui.cpp. Identical shape to ui_log.h/.cpp on purpose - kept as two
// small separate files instead of one parameterized one, for clarity.
#pragma once

#include <string>
#include <vector>

void DebugLog_Add(const char* line);
void DebugLog_Clear();

// Thread-safe snapshot for rendering - copies out under a lock, safe to call
// from the render thread (Present hook) while network threads append
// concurrently via DebugLog_Add.
std::vector<std::string> DebugLog_Snapshot();
