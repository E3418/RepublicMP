// Determinism layer and determinism probe.
//
// Goal: two machines that load the same save and run the same number of
// simulation steps end in the same state, so that later only player actions
// have to travel (lockstep). What makes the stock game non-deterministic, and
// what this module does about each (all of it only while active):
//
// Time - every step is scaled by the frame time: C3D_TIMER::Stop stores
//   QPCfrequency / elapsed ticks (FPS) at timer+0/+4, and PowerTime(x) =
//   x * 1000 / FPS. The game timer is exe+0x9D4EE0. The exe's import of
//   C3D_TIMER::Stop is redirected; after the real Stop the game timer's FPS
//   is overwritten with DETERMINISM_FIXED_FPS, so every step has the same
//   length (the speed factor the main loop multiplies in is synced anyway).
//
// Random numbers - C3DRandom_Int/Float/Double are rand() wrappers, seeded
//   from timeGetTime()/_time64(). The exe's imports of rand, srand and
//   C3DRandom_* are redirected to a counter-based generator keyed by CALL
//   SITE (return address as an exe RVA) and a per-thread, per-site counter:
//   value = mix(seed, site, n-th call of that site on this thread). Code that
//   depends on the camera or on sounds therefore has its own sequences and
//   can never shift the simulation's - no need to classify the ~180 users.
//   The counters restart (new epoch) whenever a save is loaded under the
//   probe, so both runs start from the same generator state.
//
// Math - the exe's and the engine's imports of sinf, powf, atan2f, ... come
//   from the system UCRT, whose results may differ between Windows builds
//   and CPUs (FMA3 code paths). They are redirected to this DLL's statically
//   linked CRT with FMA3 disabled - the same code on every machine.
//
// Threads and local input inside the step - also only while active:
//   - the "Filling" worker (FUN_1402593d0) normally runs in parallel with
//     the rest of the step and both modify vehicles and citizens: the main
//     thread now waits for it right after starting it (SetEvent import);
//   - the off-road pathfinder thread (FUN_1405d0ae0 via _beginthread in
//     FUN_1406e1930) delivers its route whenever it finishes: it now runs
//     synchronously inside the call;
//   - FUN_140139a70 / FUN_140699420 read a held key (and the real FPS, which
//     the fixed timer covers): GetKeyDown answers "not pressed" there.
//
// Probe (offline, one machine): saves the current game as "mp_probe", loads
// it, runs N steps recording a state hash (hooks/state_hash) every K steps,
// loads it again, runs again, and compares. Identical sequences = the
// simulation is deterministic under this layer; otherwise the first
// differing step and subsystem point at the next source to fix.
#pragma once

#include "tesmio_plugin.h"

#define DETERMINISM_FIXED_FPS 30.0f

void Determinism_Bind(const TsmHost* host);

// Installs the import redirections and the step/load hooks (inactive until
// Determinism_SetActive). Call once from TsmPluginStart.
bool Determinism_Install();

// Turns the fixed timer and the call-site generator on/off. Turning it on
// (or calling Determinism_Restart) starts a new generator epoch.
void Determinism_SetActive(bool active, unsigned long long seed);
bool Determinism_IsActive();
void Determinism_Restart();

// Simulation steps since the layer was last activated (main thread).
unsigned long long Determinism_StepCount();

// Called after every simulation step (main thread), whether or not the layer
// is active - for modules that sample or set state at a fixed point of the
// step (net/sync_check, hooks/weather). Up to DETERMINISM_MAX_OBSERVERS.
#define DETERMINISM_MAX_OBSERVERS 4
typedef void (*StepObserverFn)(void);
void Determinism_AddStepObserver(StepObserverFn fn);

// Probe control (UI). steps: steps per run, interval: steps between hashes.
bool Probe_Start(int steps, int interval);
bool Probe_IsRunning();
// Current state / last result, for the Debug window.
const char* Probe_Status();
