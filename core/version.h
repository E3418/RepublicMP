// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP version and wire-protocol number.
//
// REPUBLICMP_VERSION: MAJOR.FEATURE.PATCH (see README.md's Versioning section).
// REPUBLICMP_PROTOCOL: bump whenever any network message layout changes.
//
// When a player joins, both sides exchange the version, the protocol and a
// CRC32 of their own RepublicMP.dll file (session/save_sync); any difference
// refuses the join, so two different builds never exchange messages.
#pragma once

#define REPUBLICMP_VERSION "0.5.1"
#define REPUBLICMP_PROTOCOL 22
