// Settings sync: what players set in a building's window.
//
// Every SETTINGS_SCAN_INTERVAL_MS the buildings list is scanned for the
// fields in the table in settings_sync.cpp (all plain fields on the building
// object, all written directly by the window code - found through the window
// functions FUN_1406f8cc0 / FUN_140718fc0 / FUN_14071a540 / ... and the
// building save routine FUN_1401edc40). A field that changed is sent with the
// building's position and type category; the receiver finds its building the
// same way the demolition replay does, writes the value and, where the window
// does so too, runs the game's follow-up function (pipeline mode / resource:
// FUN_1401b7af0 or FUN_1401b7c00; auto purchase: FUN_1401754d0).
//
// Storage settings ("limit amount", "store this item") are per storage slot:
// slot+0x18 is a vector of 16-byte entries {Resource*, float share,
// float enabled}; a changed slot is sent whole.
//
// Not synced on purpose: the personal "notify me" checkboxes.
//
// A change is only sent if the local player clicked (or held the button)
// within SETTINGS_CLICK_WINDOW_MS - a field the simulation changes by itself
// is recorded but not sent. As a last guard, a field sent more than
// SETTINGS_FLOOD_LIMIT times within SETTINGS_FLOOD_WINDOW_MS on one building
// stops being watched there (a player clicking through a storage list sends
// a change every click, so the limit is generous).
#pragma once

#include "tesmio_plugin.h"

#define SETTINGS_SCAN_INTERVAL_MS 250
#define SETTINGS_CLICK_WINDOW_MS 1500
#define SETTINGS_FLOOD_LIMIT 40
#define SETTINGS_FLOOD_WINDOW_MS 10000
#define SETTINGS_MAX_SLOT_ENTRIES 32
#define SETTINGS_RETRY_MS 15000      // receiver: how long a setting waits for its building

// Wire form (part of BuildSyncMsg).
struct SettingWire {
    int field;      // index into the field table, or SETTINGS_FIELD_SLOT
    int category;   // building type category (type+0x360)
    float pos[3];   // building position (+0x350)
    unsigned long long value;   // raw field bytes (fields up to 8 bytes)
    int slot;       // SETTINGS_FIELD_SLOT: storage slot index
    int count;      // SETTINGS_FIELD_SLOT: entries
    float data[SETTINGS_MAX_SLOT_ENTRIES * 2]; // share, enabled per entry
};
#define SETTINGS_FIELD_SLOT 1000

void SettingsSync_Bind(const TsmHost* host);

// Per-frame (game thread, Present hook, after BuildSync_Tick): scans for local
// changes and applies received ones whose building exists.
void SettingsSync_Tick();

// A received setting (game thread, from BuildSync_Tick).
void SettingsSync_OnRemote(const SettingWire& w);
