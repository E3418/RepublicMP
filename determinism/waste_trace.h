// SPDX-License-Identifier: GPL-3.0-only
// A trace of the waste trucks' job searches, for the desync reports.
//
// Each waste office's Early pass 1c4e20 asks 19e330 (a building with waste for an
// empty truck, at 1c5413) or 19e750 (where a full truck unloads, at 1c5251). A tester's
// desyncs had one truck loaded and driving on one PC and standing empty on the other,
// with one more search there. Each search is noted with what it answered and a hash of
// the waste it could see; two PCs' lines show the first search that differs, and whether
// the inputs or the answer parted. It only reads and draws no random numbers.
#pragma once

#include "host.h"

void WasteTrace_Bind(const ModHost* host);

// The relays instead of the two calls.
unsigned __int64 WasteTrace_FindWaste(unsigned __int64 world, unsigned __int64 office, float capacity, unsigned __int64 list,
                                      unsigned char flag, unsigned __int64* resource, unsigned __int64 list2);
unsigned __int64 WasteTrace_FindUnload(unsigned __int64 world, unsigned __int64 vehicle, unsigned __int64 resource, float radius,
                                       unsigned char flag);

// The searches of the ticks before `tick`, into the log.
void WasteTrace_Dump(unsigned long long tick);
