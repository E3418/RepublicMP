// SPDX-License-Identifier: GPL-3.0-only
// This player's session settings, set in the RepublicMP window. They are
// remembered by themselves in RepublicMP.profile.ini next to RepublicMP.dll
// (written when one changes, read at start; nobody has to edit it):
//
//   nick         the name the other players see (empty: "Player"); a name,
//                not an identity - two players may share one
//   max players  the most players a lockstep session takes, the host
//                included (2..4); a host that is full stops listening
#pragma once

#include "tesmio_plugin.h"

#define PROFILE_NICK_BYTES 32
#define PROFILE_MIN_PLAYERS 2
#define PROFILE_MAX_PLAYERS 4

void Profile_Bind(const TsmHost* host);

// Never empty: "Player" when none is set.
const char* Profile_GetNick();
void Profile_SetNick(const char* nick);

int Profile_GetMaxPlayers();
void Profile_SetMaxPlayers(int players);
