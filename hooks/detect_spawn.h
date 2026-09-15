// Building-placement detection + debug synthetic spawn - public interface.
#pragma once

#include "tesmio_plugin.h"

// The single building-placement hook target (RVA in SOVIET64.exe).
#define HOOK_UNIVERSAL_RVA 0x2BADE0

extern const unsigned char kUniversalExpect[21]; // function prologue bytes

typedef void (*UniversalFn)(__int64 param_1, char* param_2, char param_3, char param_4,
                             unsigned __int64 param_5, unsigned int param_6,
                             char param_7, char param_8);
extern UniversalFn g_origUniversal;

void HookedUniversal(__int64 param_1, char* param_2, char param_3, char param_4,
                      unsigned __int64 param_5, unsigned int param_6,
                      char param_7, char param_8);

DWORD WINAPI HeartbeatThread(LPVOID);
DWORD WINAPI DebugSpawnThread(LPVOID); // F9: synthetic building spawn for testing

// Called every frame from the Present hook (game thread). Services F9
// requests (debug spawn + thisPtr diagnostics) off the fixed
// placement-manager address, independent of any real HookedUniversal call.
void DetectSpawn_Tick();

// Last locally-captured drag-commit snapshot (see ToolDragPoll_Tick in
// detect_spawn.cpp), exposed for ui/debugui.cpp's self-test button.
// outPoints must point at a float[64][3] buffer (ROAD_SYNC_MAX_POINTS, see
// net/road_sync.h). Returns false (buffer untouched) if nothing valid has
// been captured yet this session.
// outCurveValue: the manager's curve accumulator (+0x48) at commit time -
// needed by RoadSync_ApplySegment to reproduce a curved path instead of a
// flattened one, see road_sync.h's RoadSyncMsg comment.
bool DetectSpawn_GetLastRoadSnapshot(int* outTypeId, int* outVariant, float outPoints[][3], int* outCount, float* outCurveValue);
bool DetectSpawn_GetLastPipeSnapshot(int* outTypeId, int* outVariant, float outPoints[][3], int* outCount, float* outCurveValue);

// True while the LOCAL player has an active road/pipe/rail/etc drag in
// progress (thisPtr+0xd2ac, one flag shared across every path tool).
// net/road_sync.cpp's RoadSync_Tick() uses this to avoid applying an
// incoming network segment onto the same manager object the local player
// is actively dragging on - see its comment for why that collision
// corrupts both sides' state.
bool DetectSpawn_IsToolDragActive();

void DetectSpawn_Bind(const TsmHost* host);
