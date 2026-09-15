// Road/pipe (and eventually rail/footpath/water/fence/electric) network
// sync. Same idea as build_sync.h (buildings) but for the drag-based
// infrastructure tools - see hooks/detect_spawn.cpp's drag-capture comments
// for how this data is captured on the sending side.
//
// Unlike buildings, we don't have a single "place this with these params"
// engine call to hand off to (g_origUniversal). Instead we drive the same
// three primitives a real mouse-drag uses ourselves:
//   FUN_1404fc040(manager, startPoint, ...)   - drag start
//   FUN_1404fcaf0(manager, point, type, variant, ...)  - ONCE, final point only
//   FUN_140518a80(manager, ...)               - commit
// fcaf0 is called ONCE with the final captured point, not once per waypoint -
// it recomputes the WHOLE route from
// drag-start to whatever single point it's given (bounded by the curve
// accumulator), it doesn't append a segment, so looping it over every
// captured point replays the router's own output back into itself and
// produces self-intersecting zigzags on curved paths. See
// RoadSync_ApplySegment below and the long comment above it in
// road_sync.cpp for exactly which constant args are safe to pass (worked
// out from decompiles of all three functions plus the road/pipe
// tool-select wrappers that call them).
//
// Own dedicated TCP connection (own port), same convention as save_sync/
// build_sync - each sync module gets its own socket rather than multiplexing
// one connection, so a bug/crash in one doesn't take the others down.
#pragma once

#include "tesmio_plugin.h"
#include "../mp_config.h" // REPUBLICMP_HOST_IP - one shared IP for every network module

#define ROAD_SYNC_PORT 5558
#define ROAD_SYNC_TEST_HOST_IP REPUBLICMP_HOST_IP

#define ROAD_SYNC_MAX_POINTS 64

enum RoadSyncCategory {
    ROAD_SYNC_CATEGORY_ROAD = 0, // thisPtr+0xf7a0 manager
    ROAD_SYNC_CATEGORY_PIPE = 1, // thisPtr+0xf858 manager
};

// Fixed-size wire message - simplest possible framing (same convention as
// BuildSyncMsg), even though most segments use far fewer than
// ROAD_SYNC_MAX_POINTS of the array. pointCount says how many are valid.
//
// curveValue: the manager+0x48 "road curve" accumulator (see
// detect_spawn.cpp's ToolWaypointSnapshot comment) captured at commit time
// on the SENDING machine. Without it, straight segments arrive fine but
// curved ones flatten out on the receiving side: FUN_1404fc040 (drag
// start) resets/defaults this accumulator on the manager it initializes,
// so the replaying side would call FUN_1404fcaf0 with curve=0/default
// regardless of what the original drag actually used - see
// RoadSync_ApplySegment in road_sync.cpp for where this gets written back
// before the per-waypoint replay loop.
struct RoadSyncMsg {
    int category; // RoadSyncCategory
    int typeId;
    int variant;
    int pointCount; // 2..ROAD_SYNC_MAX_POINTS - anything outside that range is refused, not sent/applied
    float curveValue;
    float points[ROAD_SYNC_MAX_POINTS][3];
};

void RoadSync_Bind(const TsmHost* host);

// Called from detect_spawn.cpp's ToolDragPoll_Tick when a LOCAL road/pipe
// drag just committed with a valid captured snapshot. No-op if no road-sync
// connection is active, or if pointCount is out of [2, ROAD_SYNC_MAX_POINTS].
// curveValue: the manager's curve accumulator (manager+0x48) at commit time -
// see the RoadSyncMsg comment above for why this needs to travel with the points.
void RoadSync_SendSegment(int category, int typeId, int variant,
                          const float points[][3], int pointCount, float curveValue);

// Called every frame from the Present hook (game thread). Applies any
// segments received from the network since the last call.
void RoadSync_Tick();

// Replays a segment on THIS machine's own manager, by directly driving the
// same three engine primitives a real mouse drag uses. Shared by RoadSync_Tick
// (remote segments arriving over the network) and detect_spawn.cpp's F11
// local self-test hotkey (replays your own last-captured segment, offset,
// against your own local manager - no second machine/connection needed) - so
// the exact same replay code path gets exercised safely before ever trusting
// it with real network data. No-op (with a log line) if pointCount is out of
// [2, ROAD_SYNC_MAX_POINTS] or the local manager handle reads as null.
// curveValue: see the RoadSyncMsg comment above - written to the local
// manager's own curve accumulator (offset +0x48) right after the drag-start
// call, before the per-waypoint replay loop, so FUN_1404fcaf0 routes with
// the same curvature the original drag used instead of whatever default
// FUN_1404fc040 leaves the accumulator at.
void RoadSync_ApplySegment(int category, int typeId, int variant,
                           const float points[][3], int pointCount, float curveValue);

void RoadSync_StartHost();

// hostIp: pass nullptr (or empty) to keep using whatever IP was last set
// (ROAD_SYNC_TEST_HOST_IP / mp_config.h by default); pass a real IP string
// to override it for this and future calls.
void RoadSync_StartClient(const char* hostIp = nullptr);

// Force-closes an active connection so Host/Connect can be pressed again.
void RoadSync_Reset();

bool RoadSync_IsConnected();
bool RoadSync_IsConnecting();
bool RoadSync_IsHostRole(); // true after RoadSync_StartHost(), false after RoadSync_StartClient() - meaningless while fully Idle
const char* RoadSync_GetTargetIp(); // current/default client target IP, for prefilling a UI field
