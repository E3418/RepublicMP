// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP by E3418 and Sohn (juniorsohn); all authors in README.md.
// Republics: each republic's own money.
//
// The game has one pair of balances (world +0x588 rubles, +0x580 dollars) and
// no idea of an owner, and everything it does pays from them and tests them
// for "not enough money". So the pair holds whichever account is in use:
//
//   between the frame's phases  this PC's republic (the HUD, a click's test)
//   while a command is made     the republic of the player who made it
//   while a tick runs           both added up: what the tick pays or earns has
//                               no owner yet and is split in two
//
// That third case is the next step: paying buildings and vehicles from their
// own republic's account. Until then Wallets_LogState shows how much of the
// money goes through it.
//
// The accounts are kept per half of the map, not per player, so a host who
// picks the other side keeps the right one. They go into the save's
// republicmp.ini; a save without them starts each from the save's balances, the
// same numbers on every PC. East vs West mirrors them (the East's rubles are
// the West's dollars and the other way round); Equal gives both the same.
//
// All of this runs on the main thread, in a Republics session only; outside one
// every call does nothing.
#pragma once

#include "host.h"

void Wallets_Bind(const ModHost* host);

// Determinism's load hook, after the game's load and the save's file were read.
// session: this load starts or re-syncs a lockstep session.
void Wallets_AfterLoad(bool session);
// The session ended. The accounts stay until the next load: a host who plays on
// alone and takes a new player keeps both.
void Wallets_SessionEnd();

// A command of `player` is being made. Between the two the balances are theirs.
void Wallets_CommandBegin(int player);
void Wallets_CommandEnd();

// The tick's own code runs between the two. Calling End with no Begin is fine.
void Wallets_SimBegin();
void Wallets_SimEnd();

// The accounts are in use.
bool Wallets_On();
// Half 0 and half 1: rubles and dollars. False when not in use.
bool Wallets_Read(double* rub0, double* usd0, double* rub1, double* usd1);
// A line in the log: both accounts and the money the ticks split so far.
void Wallets_LogState();
