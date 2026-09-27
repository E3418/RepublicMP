# RepublicMP

Multiplayer mod for *Workers & Resources: Soviet Republic*, built as a
[TesmioLoader](https://github.com/MaxLegend/TesmioLoader) plugin. Two players
share one city: the host's game is transferred to the joining player, and
everything either of them builds afterwards appears on both machines.

**Status:** early alpha, tested with two players.

## Features

1.**Save sync** - The joining player receives the host's current game (saved, zipped and sent automatically), loads it, and both sides resume together. 
2.**Building sync** - Buildings land at the same position, rotation and mirror state, including buildings that create helper objects or lay their own access road or conveyor. 
3. **Infrastructure sync** - Roads (incl. one-way, bridges, tunnels), rails, footpaths, pipes, water and heating pipes, power lines, fences, conveyors, cableways and air roads are rebuilt exactly as drawn and joined to existing nodes and building connections. 
4. **Upgrades** - Re-typing an existing road (e.g. gravel to panels) is synced.

Not synced yet: demolition, factory connections, economy and simulation
state.

## How it works

The mod hooks the game's own functions instead of simulating input:

- **Buildings** - the placement function is hooked; the receiver calls the
  same function with the sender's cursor, rotation and mirror state, so the
  engine redoes snapping and auto-connections itself.
- **Infrastructure** - the shared commit of every drag tool is hooked. The
  finished route (both end nodes, the interior points exactly as the engine
  stores them, and any building connection ports) is sent and replayed
  through the same engine primitives on the other side, producing an
  identical segment.
- **Save** - the game's own save and load actions are triggered directly.

Each feature uses its own TCP connection (ports 5556-5558). Both players must
run the same build of the mod.

## Building

1. Clone [TesmioLoader](https://github.com/MaxLegend/TesmioLoader) and clone
   this repository into its root folder (`TesmioLoader\RepublicMP`). The
   project uses `TesmioLoader\src` for headers and writes the DLL to
   `TesmioLoader\build\plugins\`, where TesmioLoader loads plugins from.
2. Optional: copy `mp_config.h.example` to `mp_config.h` and set
   `REPUBLICMP_HOST_IP` to prefill the host address. The file is gitignored;
   without it the address is simply typed in the game.
3. Open `RepublicMP.slnx` in Visual Studio 2026 (toolset v145) and build
   **Release x64**.

## Usage

1. Both players start the game with TesmioLoader and the plugin installed.
2. The host loads a city and presses **Host** in the RepublicMP window.
3. The other player enters the host's IP (LAN or VPN) and presses **Connect**.
   The game pauses on both sides while the city is transferred and loaded.
4. Once both sides resume, building and infrastructure sync are active.

Firewalls must allow TCP ports 5556, 5557 and 5558 on the host.

### Controls

Based keyboard shortcuts:

 **Insert**  Show / hide the RepublicMP overlay 
 **F9**  Debug: spawn a test building next to the cursor 
 **F10**  Pause / unpause 

The **Debug** button in the main window opens manual reconnect controls and
the detailed sync log. A full log is written to `tesmioloader.log`.

## Versioning

`MAJOR.FEATURE.PATCH` - `MAJOR` for a stable milestone, `FEATURE` when a new
capability is added, `PATCH` for fixes within a feature.

## License

[MIT](LICENSE)
