// SPDX-License-Identifier: GPL-3.0-only
// Lockstep channel: one ordered TCP connection per player, to the host, that
// carries the shared simulation. The host takes up to LOCKSTEP_MAX_PEERS
// players (a star: the players never talk to each other).
//
// All PCs load the same save with the determinism layer on (session/
// save_sync), so their simulations start equal; from there they stay equal
// if all run the same ticks with the same commands between them:
//
//  - the host admits the ticks: when it runs tick T it sends GRANT(T, speed)
//    to every player, and a player runs tick T only once that grant has
//    arrived. The host stays at most LOCKSTEP_WINDOW ticks ahead of the
//    slowest player (ACK), so the slowest PC sets the pace;
//  - a player's actions (lockstep/commands) go to the host (CMD); the host
//    orders them with its own and the other players', makes each after its
//    current tick and sends it to every player (CMDS, with that tick). TCP
//    keeps the order: a command made after tick N always arrives before
//    GRANT(N + 1), so every player makes it at the same point;
//  - pause and speed are the host's: a player's keys and buttons are requests
//    (PAUSE, SPEED), the state comes back for the buttons (STATE). A selected
//    construction tool does not stop the ticks (it acts in a private tool
//    scope);
//  - every LOCKSTEP_HASH_TICKS ticks each player sends its state summary
//    (HASH, determinism/state_hash) and the host compares it with its own;
//    the first difference is reported to everyone;
//  - a player that leaves is dropped and the others go on; the host sends the
//    player list (names, ping, how far behind) to everyone once a second.
//
// Received messages are queued by one receive thread per connection and
// handled on the game thread (LockstepNet_Poll, at the start of a frame).
// Sends come from the game thread and the session thread.
#pragma once

#include "tesmio_plugin.h"
#include "../determinism/state_hash.h"
#include "../core/profile.h"

#define LOCKSTEP_PORT 5560
#define LOCKSTEP_WINDOW 10      // ticks the host may run ahead of the slowest player
#define LOCKSTEP_HASH_TICKS 60  // ticks between state comparisons
#define LOCKSTEP_MAX_PEERS (PROFILE_MAX_PLAYERS - 1) // players besides the host

void LockstepNet_Bind(const TsmHost* host);

// Session thread. Host: takes `expected` players' connections (each opens
// with its name) within timeoutMs; returns how many came. stillExpected
// (optional) gives how many are still coming, so a player who gave up is not
// waited for. Player: connects to the host. Both close a previous session first.
int LockstepNet_HostAccept(int expected, DWORD timeoutMs, volatile bool* cancel,
                           int (*stillExpected)(void* ctx) = nullptr, void* ctx = nullptr);
bool LockstepNet_Join(const char* hostIp, DWORD timeoutMs, volatile bool* cancel);
// Player: the host's first player list came - every player is connected and
// the session runs (the host sends none while it still waits for some).
bool LockstepNet_SessionStarted();
void LockstepNet_Close();
// Host: at least one player is connected; player: connected to the host.
bool LockstepNet_IsConnected();
// Commands/ticks wait until the host has finished accepting its roster.
// A player is ready after its own connection and hello have been set up.
bool LockstepNet_CommandsReady();
bool LockstepNet_IsHost();
// Host: the players connected now.
int LockstepNet_PeerCount();
// The first state difference found (host; the players get it from the host), or "".
const char* LockstepNet_Desync();
// Host: the last tick whose state summaries matched with every player (0 = none compared yet).
unsigned long long LockstepNet_LastMatchedTick();

// Game thread, start of a frame: handles everything received.
void LockstepNet_Poll();

// Host (game thread).
bool LockstepNet_MayRun(unsigned long long tick);
void LockstepNet_Grant(unsigned long long tick, int speed);
int LockstepNet_TakePauseToggles();
int LockstepNet_TakeSpeedRequest(); // 0 = none
// Any thread: tells the players the world is sent again (a re-sync).
void LockstepNet_SendResync();
// Any thread: a keepalive (host: also the player list), and the time since
// the other side sent anything (host: the quietest player).
void LockstepNet_Ping();
unsigned long long LockstepNet_MsSinceReceived();
// Host: drops the players silent for longer than ms; returns how many.
int LockstepNet_DropSilent(unsigned long long ms);
// The round trip of the last answered ping (host: the slowest player; false
// until one came back).
bool LockstepNet_RoundTrip(unsigned long long* ms);
// How the ticks flow: host - the slowest player's ticks behind the last
// grant, and the seconds the host waited for a player (the window full);
// player - grants received but not run yet. Any thread.
struct LockstepFlow {
    unsigned long long behind;
    int stalls;                      // seconds with a wait since the session started
    unsigned long long msSinceStall; // ~0 when it never waited
};
LockstepFlow LockstepNet_Flow();
void LockstepNet_SendState(bool paused, int speed);

// The players of the session, the host first (host: its own view; player:
// the host's last list). Any thread.
struct LockstepPlayer {
    char nick[PROFILE_NICK_BYTES];
    char ip[48];          // host only: the player's address
    int rttMs;            // -1 unknown (the host's own entry: 0)
    unsigned long long behind;
    bool host;
    bool self;            // this PC
};
int LockstepNet_Players(LockstepPlayer* out, int max);

// Player (game thread).
bool LockstepNet_TakeGrant(unsigned long long tick, int* speed);
void LockstepNet_Ack(unsigned long long completed);
// Any thread: the host announced a re-sync (taken once).
bool LockstepNet_TakeResync();
void LockstepNet_SendPauseToggle();
void LockstepNet_SendSpeed(int speed);
// The host's pause and speed for the buttons; false until it sent them.
bool LockstepNet_HostState(bool* paused, int* speed);

// Both, any thread: this PC's cursor or ping (lockstep/presence) - not a
// command, the simulation never sees it.
void LockstepNet_SendMark(int kind, const float pos[3]);
// Game thread: a line of the chat (ui/chat), to every other player.
void LockstepNet_SendChat(const char* text);

// Both: commands (host: `player`'s, made after `afterTick`, to every player;
// player: to the host, which stamps who sent it).
void LockstepNet_SendCommand(int kind, unsigned long long afterTick, int player, const void* data, size_t size);
// Both, after tick `tick`: the state summary for the comparison.
void LockstepNet_StateAfter(unsigned long long tick, const StateSummary& s);
