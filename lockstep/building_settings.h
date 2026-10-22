// SPDX-License-Identifier: GPL-3.0-only
// Building settings: what players set in a building's window, as lockstep
// commands.
//
// The fields in the table in building_settings.cpp are plain fields on the
// building object, all written directly by the window code - found through
// the window functions FUN_1406f8cc0 / FUN_140718fc0 / FUN_14071a540 / ...
// and the building save routine FUN_1401edc40. A changed field travels with
// the building's position and type category; every PC finds its building by
// them, writes the value and, where the window does so too, runs the game's
// follow-up function (pipeline mode / resource: FUN_1401b7af0 or
// FUN_1401b7c00; auto purchase: FUN_1401754d0).
//
// Storage settings ("limit amount", "store this item") are per storage slot:
// slot+0x18 is a vector of 16-byte entries {Resource*, float share,
// float enabled}; a changed slot travels whole.
//
// Not carried on purpose: the personal "notify me" checkboxes.
#pragma once

#include "tesmio_plugin.h"

#define SETTINGS_SCAN_INTERVAL_MS 250 // the field diff's scan
#define SETTINGS_CLICK_WINDOW_MS 1500
#define SETTINGS_MAX_SLOT_ENTRIES 32

// The command (LS_CMD_SETTING).
struct BuildingSettingMsg {
    int field;      // index into the field table, or SETTINGS_FIELD_SLOT
    int category;   // building type category (type+0x360)
    float pos[3];   // building position (+0x350)
    unsigned long long value;   // raw field bytes (fields up to 8 bytes)
    int slot;       // SETTINGS_FIELD_SLOT: storage slot index
    int count;      // SETTINGS_FIELD_SLOT: entries
    float data[SETTINGS_MAX_SLOT_ENTRIES * 2]; // share, enabled per entry
};
#define SETTINGS_FIELD_SLOT 1000

void BuildingSettings_Bind(const TsmHost* host);

// Per-frame (game thread, Present hook): tracks the clicks and runs the field
// diff when it is on.
void BuildingSettings_Tick();

// True if the local player clicked (or held the left button) within
// SETTINGS_CLICK_WINDOW_MS - a change seen now was most likely made by them.
bool BuildingSettings_ClickedRecently();

// Debug: log which bytes of a building a click changes ("FIELD_DIFF"), to
// find window settings that are not synced yet. Works without a session.
void BuildingSettings_SetFieldDiff(bool on);

// Lockstep (lockstep/commands, main thread). Baseline records every watched
// field, storage slot and - for construction sites - the window's pay and
// stop flags (+608 rubles, +609 dollars, +879 stopped); Detect finds what the
// interface or a tool changed since, puts the old value back on this PC and
// hands each change over as a command. ApplyCommand makes one on every PC: the field written with
// the window's own follow-up call, the pay flags through 160550(world,
// building, rubles, dollars), the stop flag through 160570(world, building,
// 1) or 161440(world, building). Citizen destination lists are not covered.
#define SETTINGS_FIELD_PAY 2000
#define SETTINGS_FIELD_STOP 2001
void BuildingSettings_LockstepBaseline();
void BuildingSettings_LockstepDetect();

// The words this sync carries itself on this building, with its follow-up
// steps (lockstep/building_switches leaves them to it): a building word at
// `offset`, and a storage slot's word (region 0 the slot's own bytes, 2 its
// +18 entries). A field watched only for other building categories is not
// this sync's on this building.
bool BuildingSettings_OwnsBuildingWord(const BYTE* building, unsigned offset);
bool BuildingSettings_OwnsSlotWord(const BYTE* building, unsigned slot, unsigned region, unsigned local);
bool BuildingSettings_ApplyCommand(const BuildingSettingMsg& w);
bool BuildingSettings_GetFieldDiff();
