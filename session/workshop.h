// SPDX-License-Identifier: GPL-3.0-only
// Workshop sync: at a join the host and the player exchange their Steam
// Workshop items (id, the installed version's time, size and name). When the
// player lacks some of the host's, or has them older, the join stops and a
// screen (ui/workshop_panel) lets the player choose what to download; the
// chosen items are all asked for at once and the player joins again - the game reads the Workshop items at
// each world load, so the shared world's load takes them in. The player may
// also join without them. Items the player has and the host has not, and the host's
// items Steam cannot download (private, removed, not from Steam), are only
// warned about.
//
// After the shared world's load the lockstep hello carries the content hash
// (the vehicle types and the build menu, names in order): the same items in
// another load order give other type numbers, which commands use, so a
// difference there is the real test.
//
// Each player chooses for themselves (SETTINGS: Sync Workshop, remembered,
// on by default): a host with it off offers nothing, a player with it off
// downloads nothing and is only told what differs; neither refuses a join.
#pragma once

#include "tesmio_plugin.h"
#include <vector>
#include <string>

void Workshop_Bind(const TsmHost* host);
bool Workshop_GetSync();
void Workshop_SetSync(bool on);

#pragma pack(push, 1)
struct WorkshopItem {
    unsigned long long id;
    unsigned updated;        // the installed version's time stamp (0: not installed)
    unsigned pad;
    unsigned long long size; // on disk, bytes
    char name[64];           // $ITEM_NAME of its workshopconfig.ini ("" unknown)
};
#pragma pack(pop)

// This PC's subscribed items (false: no Steam).
bool Workshop_List(std::vector<WorkshopItem>& out);

// What the player lacks, has older, or has on top of the host's list.
struct WorkshopDiff {
    std::vector<WorkshopItem> missing, older;
    std::vector<unsigned long long> extra;
};
WorkshopDiff Workshop_Compare(const std::vector<WorkshopItem>& host, const std::vector<WorkshopItem>& mine);
// "123, 456, ... (+N)" for the log and the page.
std::string Workshop_Ids(const std::vector<unsigned long long>& ids, size_t shown = 8);
std::vector<unsigned long long> Workshop_IdsOf(const std::vector<WorkshopItem>& items);

// The join's choice screen (ui/workshop_panel).
enum WorkshopPhase { WORKSHOP_CLOSED, WORKSHOP_CHOOSE, WORKSHOP_DOWNLOADING, WORKSHOP_DONE };
enum WorkshopItemState { WORKSHOP_OFFERED, WORKSHOP_RUNNING, WORKSHOP_INSTALLED, WORKSHOP_FAILED };
struct WorkshopOffer {
    WorkshopItem item;
    bool selected;
    int state;                          // WorkshopItemState
    unsigned long long done, total;     // bytes
};
// Opens the screen: `need` to choose from, `extra` the player's items the host has not.
void Workshop_Offer(const std::vector<WorkshopItem>& need, size_t extra);
int Workshop_Phase();
void Workshop_Snapshot(std::vector<WorkshopOffer>& out, size_t* extra);
void Workshop_Select(unsigned long long id, bool on);
void Workshop_SelectAll(bool on);
void Workshop_DownloadSelected();
void Workshop_Close();
// Items the player chose not to download (unticked, or "Join without
// them"): not offered again in this game run.
void Workshop_DeclineOffered();
bool Workshop_Declined(unsigned long long id);

// The content the game loaded: vehicle types and build menu rows, names in order.
struct WorkshopContent {
    unsigned vehicles, vehicleHash, buildings, buildingHash;
};
WorkshopContent Workshop_Content();
