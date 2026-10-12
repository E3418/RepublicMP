# RepublicMP

Multiplayer mod for *Workers & Resources: Soviet Republic*, built as a
[TesmioLoader](https://github.com/MaxLegend/TesmioLoader) plugin. Two players
share one city: the host's game is transferred to the joining player, and
everything either of them builds, changes or demolishes afterwards happens on
both machines.

**Status:** early alpha, tested with two players.

## Features

1. **Joining** - The joining player receives the host's current game (saved,
   zipped and sent automatically), loads it, and both sides resume together.
   A join screen shows each step and locks input on both sides until the
   player is in; it can be cancelled at any time, and a dropped connection
   ends the join on its own instead of leaving the screen up.

2. **Buildings** - Buildings land at the same position, rotation and mirror
   state, including buildings that create helper objects or lay their own
   access road or conveyor. Demolition of buildings is synced too.

3. **Construction mode** - Each placement is replayed with the sender's mode
   (pay with rubles, pay with dollars, or realistic construction), and
   switching a construction site to "finish with rubles / dollars" is synced.

4. **Infrastructure** - Roads (incl. one-way, bridges, tunnels), rails,
   footpaths, pipes, water and heating pipes, power lines, fences, conveyors,
   cableways, air roads and factory connections (incl. their direction) are
   rebuilt exactly as drawn and joined to existing nodes and building
   connections.

5. **Upgrades** - Re-typing an existing path (e.g. gravel road to panels) is
   synced.

6. **Path demolition** - The demolition tool removes the same pieces on both
   sides, for single clicks and for dragging with the button held.

7. **Terrain** - Every heightmap and terrain-mask edit (raise / lower,
   smooth, level, paint) is replayed on the other side. Roads and buildings
   wait for the terrain they were built on before they are applied.

8. **Game speed and clock** - The host owns the game speed; the client's
   speed changes are sent to the host as requests. Hyper / Plaid fast-forward
   is mirrored, and the host's clock is sent regularly so both cities stay on
   the same day and hour.

9. **Money** - The host owns the republic's rubles and dollars: its balances
   are sent every second and the player's game takes them over. A purchase
   made by the player is paid on the host's side, where the placement is
   replayed with the player's construction mode.

10. **Re-sync** - Both games simulate on their own, so vehicles, citizens and
    production drift apart. The host can send its world to the player again
    with **Re-sync now**, or automatically every 5-60 minutes. Both games
    pause for it like on the first join; the player reconnects by itself.

11. **Building settings** - What players set in a building's window travels
    to the other side: import/export mode and amount of power connections
    and pipelines, worker and visitor limits, priority, auto purchase,
    storage limits, and the gameplay checkboxes (the personal "notify me"
    ones stay local).

12. **Weather** - The host owns it: temperature (heating demand), wind (wind
    turbines), snowfall (road snow) and rain are sent every second. The game
    does not save the weather, so not even joining again aligned it.

13. **Drift check** - Every game hour both games compare citizens, vehicles,
    storage and production; the host window shows the difference and can
    re-sync automatically above a chosen limit.

Not synced yet: simulation state (vehicles, citizens, production) is only
brought back in line by a re-sync.

The Debug window has a **determinism probe**: it saves the game, loads and
simulates it twice under a fixed time step, call-site random numbers and
pinned math, and reports whether both runs stay identical - the groundwork
for simulating both games in lockstep.

## How it works

The mod hooks the game's own functions instead of simulating input:

- **Buildings** - the placement function is hooked; the receiver calls the
  same function with the sender's cursor, rotation, mirror state and
  construction mode, so the engine redoes snapping and auto-connections
  itself.
- **Infrastructure** - the shared commit of every drag tool is hooked. The
  finished route (both end nodes, the interior points exactly as the engine
  stores them, and any building connection ports) is sent and replayed
  through the same engine primitives on the other side, producing an
  identical segment.
- **Demolition and upgrades** - the piece of the segment the sender's engine
  cut is sent as the positions of its ends; the receiver repeats the tool's
  frame at the sender's cursor and cuts exactly that piece. Commits that do
  not expose the piece (on a hovered road end) are captured by comparing the
  segments before and after the commit.
- **Terrain** - the engine's terrain edit functions are hooked and replayed
  with the same parameters.
- **Save** - the game's own save and load actions are triggered directly.

Each feature uses its own TCP connection (ports 5556-5559) with TCP
keepalive, so a dead connection is noticed within about 15 seconds. Both
players must run the same build of the mod: the version, the protocol number
and a checksum of `RepublicMP.dll` are compared on join, and a mismatch
refuses it.

## Building

1. Clone [TesmioLoader](https://github.com/MaxLegend/TesmioLoader) and clone
   this repository into its root folder (`TesmioLoader\RepublicMP`). The
   project uses `TesmioLoader\src` for headers and writes the DLL to
   `TesmioLoader\build\plugins\`, where TesmioLoader loads plugins from.
2. Optional: copy `mp_config.h.example` to `mp_config.h` and set
   `REPUBLICMP_HOST_IP` to prefill the host address. The file is gitignored;
   without it the address is simply typed in the game.
3. Open `RepublicMP.slnx` in Visual Studio 2026 (toolset v145) and build
   **Release x64**, or from a developer prompt:

   ```
   MSBuild RepublicMP.vcxproj /p:Configuration=Release /p:Platform=x64
   ```

## Usage

1. Both players start the game with TesmioLoader and the same
   `RepublicMP.dll` installed.
2. The host loads a city and presses **Host** in the RepublicMP window.
3. The other player enters the host's IP (LAN or VPN) and presses **Connect**.
   Both games pause behind the join screen while the city is saved, sent and
   loaded.
4. Once both sides resume, all sync is active. **Disconnect** (client) or
   **Stop hosting** (host) ends the session.

Firewalls must allow TCP ports 5556, 5557, 5558 and 5559 on the host.

### Controls

| Key | Action |
| --- | --- |
| **Insert** | Show / hide the RepublicMP overlay |
| **F8** | Terrain sync log: off / per frame / per call |
| **F9** | Debug: spawn a test building next to the cursor |
| **F10** | Pause / unpause |

The **Debug** button in the main window opens manual reconnect controls and
the detailed sync log. A full log is written to `tesmioloader.log`; when
reporting a problem, attach the logs of both players.

## Changelog

### 0.4.1

Fixes and the construction-site controls.

- **Construction sites**: stopping and continuing construction, and
  cancelling "finish with rubles / dollars", now reach the other player
  (before, only switching the money finish on did). They apply at once even
  while the other player has a window open.
- **Moving-in restrictions**: all eight "who may move in" flags of a house
  are synced (before, only "nobody").
- **Roads**: a road message carries only the points actually used - a short
  path is ~200 bytes instead of ~48 KB. Paths can have up to 2048 points.
- Fixed: a crash on the other side when a footpath was replayed while that
  player's cursor was over a road.
- Drift check: per-subsystem floors (2 vehicles against 0 is no longer
  100%), vehicles count fully, the limit must be exceeded 3 game hours in a
  row and an automatic re-sync waits at least 10 minutes after the last one;
  summaries that arrive early are no longer dropped.
- Weather: only the host's rain decision is taken over; dawn, dusk and night
  follow the local clock (no more flickering light).
- The build mode in the log reads "rubles, auto purchase on 70% option 1,
  fill 20" instead of raw numbers.
- Debug window: "Log building fields changed by clicks (FIELD_DIFF)", a
  helper for finding window settings that are not synced yet.

Known issues:
- Stopping or paying for a road / rail construction site may not reach the
  other player (their site is not found at the same spot); a re-sync brings
  it over.
- While a player has a window open, buildings and roads from the other
  player wait until it is closed.
- Buying vehicles is not synced yet; a re-sync brings them over.

### 0.4.0

The world itself starts to be synced, not only what players build.

- **Weather sync** - the host owns temperature, wind, snowfall and rain; the
  player's game takes them over every second (#13).
- **Drift check** - every game hour both games compare citizens, vehicles,
  storage and production; the host window shows the drift and can re-sync
  automatically above a chosen limit (#14).
- **Determinism layer and probe** - groundwork for simulating both games in
  lockstep: fixed time step, call-site random numbers and pinned math,
  inactive in normal play. The Debug window's probe runs the same save twice
  and reports whether both runs stay identical (#12).
- Fixed: two build errors left by merging #13 and #14.

Known issues:
- With very few vehicles or citizens the drift percentage jumps (2 vehicles
  against 0 counts as 100%), so "Re-sync on drift" can trigger needlessly -
  keep it off or at 20% on a small city.
- Buying vehicles is not synced yet; a re-sync brings them over.
- A footpath drawn from a building port can fail on the other side; a
  re-sync brings it over.

### 0.3.9

- **Money sync** - the host's balances apply to both players (#8).
- **Re-sync** - the host can send its world to the player again, by hand or
  every 5-60 minutes; the player reconnects by itself (#9).
- **Building settings** - what players set in a building's window is synced
  (#11).
- **Factory connections** are synced, including their direction (#10).
- Roads and other paths can have up to 2048 points (was 512).
- Fixed: a remote building was not placed when the receiving player had no
  construction tool selected (#6, issue #3), and some buildings (grain silo,
  cement plant, ...) still failed in debug builds.
- Fixed: crash at startup with the Steam overlay (#5, issue #2).
- Fixed: a debug-build assertion when several buildings were placed quickly
  and a terrain batch ended with a height edit.
- The received save is refused if it contains unsafe file names (#4).
- When the live sync channels fail to connect, the error names the ports to
  forward (#7).

Thanks to veryinsanee for the pull requests and TucKnock for the detailed
bug reports.

## Versioning

`MAJOR.FEATURE.PATCH` - `MAJOR` for a stable milestone, `FEATURE` when a new
capability is added, `PATCH` for fixes within a feature.

`REPUBLICMP_PROTOCOL` in `version.h` is raised whenever a network message
changes.

## License

[MIT](LICENSE)
