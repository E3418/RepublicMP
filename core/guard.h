// SPDX-License-Identifier: GPL-3.0-only
// A C++ exception that leaves this plugin's code ends the game: on a thread
// of its own nothing catches it, and in a hook it unwinds into the game's
// frames, so the CRT stops the process at once (a fast fail - no crash
// report, nothing in the log; a file name the code page could not hold did
// that at a join's DLC check). Guard_Call runs a thread's body or a piece of
// per-frame work and turns such an exception into a log line instead; the
// game keeps running. Each call site logs its first few only.
//
// Only for code whose skipped rest leaves nothing half done (a whole thread,
// one command, the plugin's own per-frame work). A module that includes this
// must be bound (TsmBind) - the log goes through its host pointer.
#pragma once

#include "tesmio_plugin.h"
#include <exception>

#define GUARD_LOG_LIMIT 5

template <class Fn>
bool Guard_Call(const char* where, Fn&& fn)
{
    static volatile LONG s_logged = 0; // one counter per call site
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        if (InterlockedIncrement(&s_logged) <= GUARD_LOG_LIMIT)
            Logf("republicmp  GUARD: %s stopped by an exception - %s", where, e.what());
    } catch (...) {
        if (InterlockedIncrement(&s_logged) <= GUARD_LOG_LIMIT)
            Logf("republicmp  GUARD: %s stopped by an unknown exception", where);
    }
    return false;
}

// A critical section held for a scope: an exception passing through cannot
// leave it held (the next thread to enter would wait forever).
struct LockScope {
    CRITICAL_SECTION& cs;
    explicit LockScope(CRITICAL_SECTION& c) : cs(c) { EnterCriticalSection(&cs); }
    ~LockScope() { LeaveCriticalSection(&cs); }
    LockScope(const LockScope&) = delete;
    LockScope& operator=(const LockScope&) = delete;
};
