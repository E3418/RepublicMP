# RepublicMP architecture

RepublicMP is one TesmioLoader plugin (`RepublicMP.dll`, C++, MSVC) for
Workers & Resources: Soviet Republic 1.1.1.9 (`SOVIET64.exe`, PE stamp
`0x6A3EB6AD`). It hooks the game, reads its memory and calls its own
functions; there is no scripting API. Every address in the code is for that
build.

The DLL offers two kinds of game on the RepublicMP window's start page:

- **Co-op** (built): one republic, 2-4 players. Every PC simulates the same
  city in deterministic lockstep; player actions are commands made at the
  same tick on every PC.
- **Republics** (planned, `modes/republics/`): each player runs a republic of
  their own on one map; the neighbour is drawn as render-only copies.

## Layers

Lower layers never include higher ones.

```
ui/            the ImGui windows (RepublicMP window, Debug window, sync screen)
modes/         game modes other than co-op plug in here (page, frame, overlay)
session/       joining players: hello, versions, DLCs, the save sent to all, re-sync
lockstep/      co-op: commands, the lockstep connection, the window/tool commands
world/         actions on the game world made from data (buildings, networks,
               terrain) and the hooks that capture the local player's actions
determinism/   making the simulation identical on every PC: fixed ticks, RNG,
               math, timers, patches; state hashes; the determinism probe
game/          the game itself: world fields, camera, save/load, input, Present
transport/     links: blocking TCP, or Steam P2P through Valve's relay (steam_net)
save/          zip of a save folder
core/          plugin entry points, version, the player's profile (ini)
api/           republicmp_api.h - what a mode may use (one include)
```

## A frame

- **Present hook** (`game/present_hook`, the frame tick): `SessionPatches`,
  `SimPatches`, `InputBlock`, `TerrainHook`, `SaveSync`, `BuildingSettings`,
  `LocalBuilds` ticks, the mode's frame (`Modes_Frame`), then the ImGui
  overlay (`game/imgui_hook`: windows, other players' cursors, the mode's
  overlay).
- **The game's simulation step** (`determinism/`): in a session the host
  grants ticks; at each tick boundary `Determinism_BeforePrepare` makes the
  commands received for it (`lockstep/commands`) on the main thread, then
  the tick runs. Tools and windows act in a private scope between ticks, so
  the local player's frames never change the shared world on one PC only.

## A co-op command, end to end

1. A hook sees a local action: the building tool's placing press, a drag
   tool's commit, a demolition, a terrain edit, a window button, a purchase...
   (`world/local_builds`, `world/demolish_hook`, `world/terrain_hook`,
   `lockstep/*`).
2. Instead of letting the game do it, the hook fills the action's message
   (its inputs: cursor, type, the clicking PC's build mode...) and calls
   `Commands_Submit(kind, &msg, sizeof(msg))`.
3. `lockstep/lockstep_net` sends it to the host; the host orders every
   player's commands and sends each with the tick it belongs after.
4. Every PC, the clicking one included, makes it at that tick boundary
   through the kind's module (`Buildings_ApplyPlacementCommand`,
   `Networks_ApplyCommandSegment`, `GameCalls_ApplyCommand`...), with the
   game's own functions.
5. Every 60 ticks each player sends a state hash (`determinism/state_hash`);
   the host compares, reports the first difference and re-syncs after the set
   delay (`session/save_sync`).

### Adding a command

- A new kind in `LockstepCommandKind` (`lockstep/commands.h`, appended only)
  and its message struct (plain data, fixed size).
- The capture: where the game makes the action, `if (Commands_TakeLocal())`
  fill the message, `Commands_Submit`, and skip the game's own call.
- The apply: `Xxx_ApplyCommand(const void* data, size_t size)`, called from
  the switch in `lockstep/commands.cpp`; check the size, find objects by
  identity (`lockstep/world_refs`), never by pointer.
- Bump `REPUBLICMP_PROTOCOL` (`core/version.h`) when a message layout
  changes.

### A window button

Most buttons are one routine call: add a site to `kSites` in
`lockstep/game_calls.cpp` (appended only; the schema string says how each
argument travels). The call is relayed to a command and made on every PC.

## Hooks and their owners

Every hook on the game has one owner. A mode that needs to know about one of
these calls adds a callback in the owner - never a second hook on the same
function.

| Game function | Kind | Owner |
|---|---|---|
| `0x2BADE0` building placement | inline | `world/local_builds` |
| `0x518A80` drag tool commit | inline | `world/local_builds` |
| `0x168730` building demolition | inline | `world/demolish_hook` |
| `0x4FC040` drag start | inline | `world/networks` |
| `0x5258E0`, `0x525C30` start planned construction | inline | `world/networks` |
| `0x21C110` group construction tools | inline | `world/networks` |
| `0x569D70` placement collision query | inline | `world/placement_collision` |
| `C3D_TERRAIN::Edit*`, heightmap/mask open/close | IAT | `world/terrain_hook` |
| `C3D_INPUT` mouse/key reads | IAT | `game/input_block` |
| `0x30C790` simulation step, `0x430FC0` world load | inline | `determinism/determinism` |
| `rand`, `srand`, `C3DRandom_*`, `C3D_TIMER`, `GetKeyDown`, `_beginthread`, `SetEvent`, `malloc`, CRT math | IAT | `determinism/determinism` |
| `0x30D1A0` Late body, call-site relays, tool stubs | inline / call sites | `determinism/sim_patches` |
| window button call sites (`kSites`) | call sites | `lockstep/game_calls` |
| `IDXGISwapChain::Present` | vtable | `game/present_hook` |
| the game window's `WndProc` | subclass | `game/imgui_hook` |

Hooks check the bytes they replace (`expect`) and refuse to install on a
different build.

## A mode under modes/

- Its folder: `modes/<name>/`. It includes `api/republicmp_api.h` only.
- It plugs in with a `RmpMode` (`modes/mode.h`): the start page's card, its
  page in the RepublicMP window, a frame callback and an overlay. Its
  `Xxx_Bind(host)` is called from `core/main.cpp` and calls
  `Modes_SetRepublics`.
- Settings of its own go to `RepublicMP.profile.ini` under a section of its
  own (`Profile_GetInt` / `Profile_SetInt`).
- New files go into `RepublicMP.vcxproj` and `.filters`.

## Rules

- Every `.cpp` that reads the game calls `TsmBind(host)` in its Bind
  (`g_exeBase` and `Logf` are per translation unit).
- `__try` around every read of the game's memory; inside a hook that runs
  within the game's own call: no allocation, logging or I/O.
- Offsets and RVAs are named `#define`s with what they are and where they
  were found. No numbers of one's own city (coordinates, object indices).
  What is known about the game beyond the code, with its evidence, is in
  `docs/findings.md`.
- Diagnostics are off by default (the Debug window turns them on; the money
  trace is the exception).
- Files are CRLF. Code, comments and the changelog are in English; no
  internal tags. Work taken from another project is credited in the code,
  e.g. "(by SoylentGreed)".
- Build: `MSBuild RepublicMP.vcxproj /p:Configuration=Debug /p:Platform=x64`
  with the game closed; the DLL goes to `..\build\plugins\RepublicMP.dll`.
