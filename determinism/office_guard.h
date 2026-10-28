// SPDX-License-Identifier: GPL-3.0-only
// Guard the native linked-office vehicle lookup against stale object links.
#pragma once
#include "tesmio_plugin.h"
void OfficeGuard_Bind(const TsmHost* host);
bool OfficeGuard_Install();
// Main thread only: the native building pass in a common Early.
void OfficeGuard_BeginStep();
void OfficeGuard_EndStep();
