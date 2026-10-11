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
   cableways and air roads are rebuilt exactly as drawn and joined to existing
   nodes and building connections.

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

9. **Building settings** - What players set in a building's window travels
   to the other side: import/export mode and amount of power connections and
   pipelines, worker and visitor limits, priority, auto purchase, storage
   limits, and the gameplay checkboxes (the personal "notify me" ones stay
   local).

Not synced yet: factory connections, money (each side pays from its own
pool), and simulation state (vehicles, citizens, production) - both games
simulate on their own.

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

## Versioning

`MAJOR.FEATURE.PATCH` - `MAJOR` for a stable milestone, `FEATURE` when a new
capability is added, `PATCH` for fixes within a feature.

`REPUBLICMP_PROTOCOL` in `version.h` is raised whenever a network message
changes.

## License

[MIT](LICENSE)
