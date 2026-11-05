// SPDX-License-Identifier: GPL-3.0-only
// This player's settings, set in the RepublicMP and Debug windows. They are
// remembered by themselves in RepublicMP\RepublicMP.profile.ini next to RepublicMP.dll
// (written when one changes, read at start; nobody has to edit it, and a
// missing file or key means the default):
//
//   [profile] nick         the name the other players see (empty: "Player");
//                          a name, not an identity - two players may share one
//             max_players  the most players a lockstep session takes, the
//                          host included (2..4); a host that is full stops
//                          listening
//             dlc_check    refuse a join when the DLCs differ (session/save_sync)
//             desync_resync seconds after a desync before the host re-syncs
//                          (-1 off, 0 at once; session/save_sync)
//             money_tolerance the host ignores money differences under 1 ruble
//                          or dollar (lockstep/lockstep_net, off by default)
//             workshop_sync download the host's Workshop items at a join (session/workshop)
//   [overlay] show_cursors, ping_key (a virtual-key code; lockstep/presence)
//   [debug]   money_trace (determinism), field_diff (lockstep/building_settings),
//             terrain_log (world/terrain_hook),
//             construction_log (determinism/construction_view),
//             site_watch (determinism/site_watch),
//             frame_timing (core/frame_timing),
//             storage_trace (determinism/state_hash, off unless remembered),
//             sound_log (lockstep/remote_sounds)
//
// Each module reads its own setting when it is bound and writes it when it
// is changed (Profile_GetInt / Profile_SetInt).
#pragma once

#include "host.h"

#define PROFILE_NICK_BYTES 32
#define PROFILE_MIN_PLAYERS 2
#define PROFILE_MAX_PLAYERS 4

void Profile_Bind(const ModHost* host);

// Never empty: "Player" when none is set.
const char* Profile_GetNick();
void Profile_SetNick(const char* nick);

int Profile_GetMaxPlayers();
void Profile_SetMaxPlayers(int players);

// Any other setting: `def` when the file or the key is missing. Any time
// (the file is found on first use).
int Profile_GetInt(const char* section, const char* key, int def);
void Profile_SetInt(const char* section, const char* key, int value);
