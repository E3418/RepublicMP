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

### Unreleased

- **Native building update restored for command sessions**: building
  visibility and the visible-building list again come from the game's own
  Early pass, as in the replay multiplayer. Removed the whole-render wrapper
  and unsuccessful visibility diagnostic. The existing Prepare/Late preview
  scope stays for camera movement and new buildings on held frames, where
  Early does not run. Ordered placement still uses the native click frame;
  tick admission and command ordering are retained. The isolated simulation
  probe keeps its off-screen policy (by SoylentGreed). This rollback needs an
  in-game building/connection test and does not establish determinism between
  different cameras in command sessions. Previous probe results do not cover
  this restored building path.

Lockstep sessions (experimental): **Lockstep** next to Host makes the next
session run both games in lockstep. The host saves, the player receives
and loads the save, then the host loads the same save too, both with the
determinism layer on, and from there both run the same ticks: the host
admits them (at most 10 ticks ahead of the player) and the player runs a
tick only once the host granted it. Building, drag tools (roads, rails,
pipes, power lines and the rest, upgrades, network demolition) and
building demolition are commands the host orders and both games make
after the same tick. Pause and speed are the host's; the player's keys
and buttons are requests. While either player has a construction tool
open, no tick runs. Every 60 ticks the player's state summary is compared
with the host's, and the first difference is shown in the RepublicMP
window. The lockstep connection uses TCP port 5560 on the host.

Also carried as commands: building window settings and storage slots, the
construction window's pay with rubles / dollars and stop, starting planned
construction, the group construction tools, factory connection directions,
and terrain edits. Changes made outside the hooks (the interface, or a tool
inside a held frame) are found by comparing with a record taken at the end
of the frame and before the Late body, put back on the PC that made them
and sent as commands; money such an action paid travels as a balance
change. While a command is made, this PC's keys read up and its own drag is
cancelled. Saving inside a lockstep session is refused (a save ages every
vehicle on that PC), F9's test building is off, and the player waits in the
Esc menu. Tools whose actions are not carried yet (vehicle and line picks,
worker relocation, pavements, buoys, Christmas lights, trees, junction and
merge tools, Go there) are put away when selected. Not carried yet:
vehicle purchases, lines and routes, research, citizen destinations, world
settings and the remaining window actions - they act on one PC only.
Re-sync is not available in a lockstep session.

Lockstep, step 3 (first part, awaiting the in-game probe): player actions
become commands. While the determinism layer runs, the building tool, the
commit of every drag tool (roads, rails, pipes, power lines and the rest,
their upgrades, and network demolition with the demolition tool) and
building demolition do not act on the frame they are made. They become
commands, made at the start of the next frame, before its tick, through the
replays of the replay multiplayer - on the clicking PC as well. A building
is placed by the building tool's own click frame, which lays its access
roads; when that builds nothing, by the replay's call, without them. Ticks
wait while a tool is selected (tool hold, by SoylentGreed).

The Late body also works off two queues by frame instead of by tick: the
buildings to re-check after a network or building change near them, and
the building links list. Held frames ran them too, so after a construction
command the PC with more held frames finished first; they now run on step
frames only. Probe traces no longer count vehicle camera distances and the
sound levels derived from them.

- **Build during run 1** in the Debug window (off by default): the player
  builds while probe run 1 runs; the commands are recorded with the tick they
  were made after, and run 2 makes them again after the same ticks with no
  input. Both runs must stay identical.

Lockstep, step 2: the active determinism layer admits whole ticks - the
probe with held frames and speed changes matches for 10,800 ticks (run 2
held 16,096 frames, run 1 none). Held frames draw entities and guarded
vehicles without running Early (vehicles that cannot be guarded are hidden
for that frame); Prepare stays paused, and UI speed writes cannot change the
tick already in flight. The fast speed uses the game's own timer factor. Late
restores the tick timer; weather, road speed recovery and citizen
initialization run only on step frames (by SoylentGreed).
The current replay multiplayer and ordinary play keep their existing paths.

- **Fuzz held frames and speed** in Debug is off by default. Run 1 has no
  inserted holds; run 2 inserts 0-3 held frames between ticks. Both change
  speed 1/2 every 300 ticks. Space/F10 pauses admission without changing this
  schedule. Select 10,800 ticks to test the full run, including alt-tab.
- Samples and the trace are taken after Late, so the last tick is complete.
  Fuzz traces use a temporary file deleted on close and cover all requested
  ticks instead of stopping at the memory budget. An incomplete trace or a
  presentation guard failure cannot produce an IDENTICAL result. The world
  block and the timer are logged but do not decide it: they also hold the
  view, input and interface clocks that run on every rendered frame.
- The speed buttons show the requested speed again (the pause button looked
  pressed while the layer ran).


Lockstep, step 1: the same save now simulates exactly the same twice on one
PC - the determinism probe matches for 10,800 steps (citizens, vehicles,
buildings, money, random numbers).

- **Uninitialised memory**: the game leaves some fields of new objects unset
  and reads them later (a citizen's activities, which feed its needs). They
  held whatever an earlier game left in memory, so a city loaded after
  playing another differed from a fresh start - and two PCs never agreed.
  Every allocation of the game now comes zeroed, as in a fresh process. Always
  active.
- **Simulation patches** for the determinism layer: the interface's searches
  no longer change which target a vehicle picks next, the camera object no
  longer moves on its own while the game runs, and each frame waits for the
  render thread before the simulation reads the game timer (by
  SoylentGreed).
- Determinism probe: a step-by-step trace of every vehicle, building and
  citizen that shows the first word that differs and when; an optional
  hardware write watch shows which game code wrote it.

### 0.4.3 (pre-release)

Planned construction, the group tools and power lines now come out the same
on both PCs; hosting needs a loaded city. Protocol 18 - both players need
this build.

- **Starting planned roads and rails** ("start construction" in the
  construction menu, the construction window or the network tool panel)
  now turns the same plans into construction sites on the other PC too;
  before, they stayed white plans there (#20). The new sites take the
  sender's construction mode (rubles, dollars or realistic, auto purchase),
  not the receiver's. Protocol 18.
- **Group construction tools** (the drag box that starts plans for rubles,
  dollars or resources, and the one that suspends construction) now act on
  the other PC too, with the same box (by SoylentGreed).
- **Power lines joined to an existing line end**: on the other PC the new
  line's end was lifted by the pole height (20 m for high voltage) and became
  a separate node above the old one, so the line carried no power there and
  the sender's demolition later missed that piece. The replay now joins the
  same node (#22, #23, by TucKnock).
- **Demolishing the middle of a power line** could move the line's own end
  node (e.g. at a switch's port) onto the cut on the other PC, so the next
  demolitions removed the wrong spans and a couple stayed there. The cut now
  takes a fresh node, as the game's own tool does, and a power line is cut
  exactly where the sender cut it - not one span further (#23).
- **Hosting needs a loaded city**: Host is unavailable in the main menu.
  Leaving the world (back to the main menu or loading another save) ends the
  session - stops hosting or disconnects - instead of leaving the other
  player connected to a game that is gone.
- **Open windows** no longer hold the other player's buildings and roads:
  a replay runs with the menus closed for its call and puts them back
  after (#21, by TucKnock).

### 0.4.2

The other player's camera no longer decides how a building or a road comes
out.

- **Access roads**: a building's automatic access road is laid with the same
  collision checks and the same nearby roads on both PCs, instead of what
  each player's camera sees. Before, it could come out as a road on one PC
  and a footpath on the other, or take another route, so the sender's later
  demolition missed it and new buildings were refused there (by
  SoylentGreed).
- **Demolishing roads** works wherever the other player is looking (the pick
  and the piece lookup no longer use the camera's list of roads). A piece
  the game has not settled yet after a neighbouring demolition is tried
  again a moment later, and a piece already gone counts as done. A
  building's access road laid by the other PC's own game is now demolished
  too, and a piece whose building is still being demolished there waits
  for it.
- **Footpaths joined to a road** get their pedestrian crossing on the other
  PC too: where the path splits the road (or meets its node) now travels
  with the path. Protocol 17.
- **Alt-tab**: a game in the background (low frame rate) no longer builds
  differently from the other player's; construction sites a camera sees no
  longer get faster worker updates; a new citizen's value is set by the
  simulation only, never first by a window or overlay on one PC. All of
  these are active only while connected (and in the determinism probe) (by
  SoylentGreed).
- **Construction sites**: stop / continue / money finish is sent only from
  that building's own window, never the game's own clean-up of a finished
  site (#17) or a new site drawn with the road tool.
- **Placement**: the replayed building is found by identity and checked
  against the sender's position before it gets the sender's rotation (#16);
  an access road is no longer built twice, or for a building that could not
  be placed.
- **Joining**: both games stay paused after a successful join or re-sync
  until either player resumes (#18).
- **Citizens' destinations**: where a home's or bus stop's workers,
  students, passengers and tourists go (up to 9 buildings per list) is
  synced, including removing them (#15, by TucKnock).

Known issues:
- Realistic construction can still drift apart (e.g. a road at different
  percentages) - each PC simulates its own workers and vehicles.
- Buying vehicles and a shop's or pub's goods purchases are not synced yet.
- While a player has a window open, buildings and roads from the other
  player wait until it is closed.

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

[GPL-3.0-only](LICENSE) for everything after the tag `v0.4.3-mit`. Versions up
to that tag were published under the MIT License and stay under it.
TesmioLoader, which loads this plugin, is GPL-3.0 as well.
