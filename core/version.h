// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP by E3418 and Sohn (juniorsohn); all authors in README.md.
// RepublicMP version and wire-protocol number.
//
// REPUBLICMP_VERSION: MAJOR.FEATURE.PATCH (see README.md's Versioning section).
// REPUBLICMP_PROTOCOL: bump whenever any network message layout changes.
//
// When a player joins, both sides exchange the version, the protocol and a
// CRC32 of their own RepublicMP.dll file (session/save_sync); any difference
// refuses the join, so two different builds never exchange messages.
#pragma once

// The authors (README.md's Authors section). Shown in the menu and the log,
// so a build passed around without the repository still says whose it is.
#define REPUBLICMP_AUTHORS "E3418 and Sohn (juniorsohn)"

#define REPUBLICMP_VERSION "0.7.6"
#define REPUBLICMP_PROTOCOL 70
