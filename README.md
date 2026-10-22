# RepublicMP

Multiplayer mod for *Workers & Resources: Soviet Republic*, built as a
[TesmioLoader](https://github.com/MaxLegend/TesmioLoader) plugin. Up to four
players share one city (**co-op**): the host's game is transferred to the
joining players, and from there every game runs the same simulation tick by
tick (**lockstep**).

**Status:** alpha, tested with up to four players. Current version: **0.7.0**
([changelog](CHANGELOG.md)).

## Support and community

Questions, bug reports and test partners are welcome on
[Discord](https://discord.gg/BRTAYNKAYu). If you want to support the work,
you can do it on [Buy Me a Coffee](https://buymeacoffee.com/3418).

## Lockstep

A determinism layer makes the game simulate the same way on every PC: a fixed
time step, random numbers per call site, pinned math, zeroed allocations, and
the camera and the windows kept out of the simulation - for the simulation
every building is off the camera on every PC, while each PC still draws what
its own camera sees. The host admits the ticks (at most 10 ahead of the
player) and orders the players' commands. The lockstep panel shows the tick,
the ping, how the ticks flow and whether the games match.

Carried as commands:

- **Construction** - building placement (with terrain levelling), every drag
  tool (roads, rails, footpaths, pipes, power lines, fences, conveyors,
  cableways, factory connections, decoration paths), upgrades, planned
  construction, the group tools, building and network demolition with the
  bulldozer.
- **Terrain and trees** - the terrain brushes (raise, lower, levelling, tree
  demolition, topsoil restoration) and the single-tree tools.
- **Building windows** - settings and storage slots (auto purchase amounts),
  the bottom menu's world settings, pay with rubles / dollars and stop on
  construction sites, factory connection directions, adding and removing
  fields of a farm, office assignments and links, station and distribution
  links added with the link tools, the cargo filters of links, specific
  sources (the add-source tool and the source windows), research.
- **Roads and junctions** - road signs, junction rules (right-hand, main road,
  traffic lights) from the junction window and the decoration tool, rail
  signals.
- **Game rules** - changes on the settings page apply on both PCs.
- **Names** - renaming buildings, vehicles, lines, cities and crossroads.
- **Vehicles** - purchases (new and used vehicles, licences, factory
  blueprints), the vehicle window's buttons (send to depot, release, stop /
  start, clear line, next skin), vehicle settings changed in its window
  (checkboxes, limits, per-stop cargo settings, cargo limits), sending to a
  depot with the pick tool, composing trains in the depot, selling vehicles.
- **Lines and routes** - creating and deleting lines, the line window's
  Accept, adding stops, putting vehicles on a line, launching vehicles.

Not carried yet (tools are put away when selected, or the action acts on one
PC only): removing decoration paths, worker relocation, buoys, Christmas
lights, merge tools, "Go there", goods purchases in shops, the office
window's order popup. Saving in a session saves on every PC at once. The construction animation
(scaffolding, ground, workers on a building site) is not drawn; the building
still finishes on time. While the game is paused, what comes into view
appears only after the pause.

## How it works

The mod hooks the game's own functions instead of simulating input. An
action is taken where the game decides it (a placement click, a drag
commit, a Buy press, a pick tool's choice, a window's routine call), and
every PC makes it again with the game's own code at the same tick boundary.
Objects travel as positions or as their place in the world's lists, never as
pointers. [ARCHITECTURE.md](ARCHITECTURE.md) describes the code: its
layers, how a command travels and which module owns which hook.

Both players must run the same build of the mod: the version, the protocol
number and a checksum of `RepublicMP.dll` are compared on join, and a
mismatch refuses it.

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
2. In the RepublicMP window both pick **Co-op**. The host loads a city, sets
   the session's size and presses **Host**.
3. The other players enter the host's IP (LAN or VPN) or SteamID64 (shown
   in the host's window, with a Copy button) and press **Connect**. Steam
   friends can instead use **Join Game** in the friends list, or accept the
   host's invite, while their game is running.
   Every game pauses behind the join screen while the city is saved, sent and
   loaded.
4. Once all are in, the session runs, paused until a player resumes. **Disconnect** (client) or
   **Stop hosting** (host) ends it.

Both games need the same DLCs: with **Check DLCs** ticked on both PCs (the
default) a join with different DLCs is refused and names the difference;
either player can untick it to play a map that needs no DLC content.

A connection by IP needs TCP ports 5556 and 5560 open on the host; a
connection by SteamID64 goes through Valve's relay and needs no open port.

### Controls

| Key | Action |
| --- | --- |
| **Insert** | Show / hide the RepublicMP overlay |
| **~** | Ping the place under the mouse for every player (another key can be set in the window) |

The **Debug** button in the main window opens the lockstep state, the
determinism probe, the money trace (on by default: on a desync each log gets
the balances and construction sites of the ticks before it) and the detailed
log. A full log is written to
`tesmioloader.log`; when reporting a problem, attach the logs of every
player.

## Versioning

`MAJOR.FEATURE.PATCH` - `MAJOR` for a stable milestone, `FEATURE` when a new
capability is added, `PATCH` for fixes within a feature.

`REPUBLICMP_PROTOCOL` in `version.h` is raised whenever a network message
changes.

## Credits

The lockstep work builds on SoylentGreed's research and code (RepublicLAN);
parts taken from it are marked "(by SoylentGreed)". Thanks to veryinsanee
and TucKnock for pull requests and detailed bug reports.

Steam connections use the headers of Valve's
[GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)
(BSD-3-Clause, `third_party/gns`).

## License

[GPL-3.0-only](LICENSE) for everything after the tag `v0.4.3-mit`. Versions up
to that tag were published under the MIT License and stay under it.
TesmioLoader, which loads this plugin, is GPL-3.0 as well.
