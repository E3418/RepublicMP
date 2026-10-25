# Changelog

## Unreleased

Protocol 51 - every player needs this build.

- **Held vehicle rendering guard** - follow +4a0 links throughout the parent
  and child closure used by the native render builders, including objects
  outside the root render list. Pointer changes to or from null now refuse
  the held pass without restoring a potentially freed pointer. This closes
  guard gaps; the remaining live vehicle and resource desyncs need retesting.

- **Commands during session setup** - retain queued commands until the
  lockstep connection is ready. The host waits until roster acceptance ends;
  a player waits for its own connection. Previously the host could apply
  commands at tick zero and broadcast them to no player, while a waiting
  player could discard its requests. Tick admission uses the same readiness
  gate. Offline probe recording and replay keep their existing behavior.

- **Shared terrain brush collisions** - ordered brush samples now run the
  eleven network obstacle queries in 3a5930 against the full network with
  the local shortlist suppressed. Local camera lists could accept a different
  number of cells, changing the terrain and its worker-price charge.
- **Construction updates on held frames** - the Late site's 159070 update
  is skipped outside common simulation ticks and ordered commands. Native
  code cleared productivity before checking pause, so extra local frames
  changed the value used by the next tick's worker-target update.
- **Construction recipe report identity** - compare local snapshots by object
  identity instead of a temporarily zero native building index. Logs include
  the building list ordinal. A save no longer compares unrelated recipes or
  consumes the report budget on false changes.

- **Linked-office lookup crash guard** - before the shared simulation checks
  vehicles assigned through a building's linked offices, validate live object
  membership and the lists the native lookup will read. Invalid references
  refuse that assignment and produce a bounded OFFICE_GUARD error instead of
  reaching the unsafe read at 0x19F660. This contains the observed crash; the
  writer that corrupted the office list has not yet been identified.

- **Construction recipe diagnostics** - the optional storage resource trace
  also records new or changed material requirements after ordered commands,
  including network sites. CONSTRUCTION_RECIPE lines retain resource names
  and exact required float bits, so a tiny money desync can be traced back to
  the command that produced a different recipe. Off by default, 512-line cap.
- **Storage desync diagnostics** - building records describe up to 64 slots.
  The optional Debug storage resource trace retains individual resource names,
  amounts and float bits at the compared tick, and reports stock mutations left
  by the UI after its command guards. It is off by default and does not change
  simulation hashes or the protocol. UI mutation reports are limited to 32.
- **Window capture after loading** - learned interface noise is cleared when
  the world is loaded, so an earlier world's passive UI writes cannot suppress
  a real setting in the new world. This is separate from the railway-office
  stock desync, whose cause has not yet been established.

- **Construction rendering and worker targets** - the site drawer rebuilt
  worker target vectors on each PC's render frames; Late copied them and
  re-targeted construction workers, including on held frames. The drawer now
  uses private vectors and restores the original headers, while construction
  animation stays enabled. Untyped sites and finished buildings on its list
  are guarded too. A nested camera scope no longer bypasses the guard.
  Found while investigating building/citizen desyncs; awaiting an in-game
  comparison of both players' logs.

- **Republics, first part** - the HOST tab now hosts Republics (2 players);
  a player who joins gets the host's mode. The map is cut along one of its
  diagonals, moved so both halves get the same playable area whatever the
  map's border, and drawn as a red line on the map and the minimap. Each
  player builds, lays and demolishes buildings and networks on their own
  half only; a click on the other half is refused on both PCs with a notice,
  the host's too while it waits alone for the player. A 50 m band along the
  line is open to both, so roads and rails can join the halves. Money is
  still shared, and terrain tools and windows are not limited yet. A command
  now carries the player who made it.
- **Desync reports name the objects** (#55) - at a desync each PC writes to
  its log one `DESYNC_RECORD` line per object of the lists that differ
  (buildings, vehicles or citizens) at the tick that differed, so the two
  logs show which object split. Kept for the last hashed ticks in every
  session, as the money trace is.
  After a desync the host goes on comparing and logs when the games match
  again, which tells a difference of one moment from games that split.
- **Fix: building settings and freed buildings** (#55) - the window settings
  capture read, and wrote back into, buildings demolished since its last
  record; it now only reads buildings still listed. A setting command whose
  building is not found no longer gives up on one unreadable entry, and logs
  the nearest building of that kind.
- **Fix: the bulldozer on networks** - a piece the bulldozer removed on one
  PC was often left on the other, planned (white) pieces most of all: the
  other PC looked for it under the cursor and by position, and silently did
  nothing when it found none. The piece now also travels as its place in the
  network's segment list with a check (by SoylentGreed), and is armed
  directly where the cursor picks another piece or none; a hovered node
  travels the same way. A demolition that is not made logs why.
- **Fix: the offices assign tool's selection at a session's start** - the
  host sent the selection it saw before the session's load as a change, and
  made it alone. The start is now taken after every world load.
- **Logs for road and pipe desyncs** - a segment command logs how many
  buildings (construction sites) it made on this PC, and a segment the game
  refuses on one PC logs the buildings in its way.
- **Join tool** - the tool that makes the two pieces of a road, rail or pipe
  at a node one piece works in a session (by SoylentGreed); it was put away
  before. The node the clicking player picked is joined on every PC.
- **Fix: the bulldozer on a network node** - a removal through a hovered node
  found the node by its exact position; a node whose height differed in its
  last bits was missed, and the PC then cut whatever piece its own cursor
  picked. The node is now matched by its place in the network and its x and
  z, and when the sender's node is not found nothing is cut.
- **Logs for construction sites made on one PC** - a segment command logs
  the pieces at its ends and whether they are planned: a new piece joined to
  a planned one starts that one's construction.
- **Load onto flatbed** - the vehicle window's "Load onto flatbed of another
  vehicle" works in a session (by SoylentGreed); the tool was put away
  before. The click is made on every PC.
- **Desync records show the storage** - a building's desync record now lists
  each storage slot's entries and amount, so two logs show which slot split
  and by how much.

## 0.7.2

Protocol 46 - every player needs this build.

- **City tool** - making a new city with the city tool works in a session:
  the city is made with the clicking player's name for it on every PC, and
  the buildings around it join it. Before, the tool did nothing in a session.

## 0.7.1

The MULTIPLAYER page becomes the players' UI, in the game's language. Protocol
45 is unchanged; the DLL checksum differs, so 0.7.0 and 0.7.1 do not join
each other - every player needs this build.

- **The MULTIPLAYER page is the players' UI** - it now also opens over a
  city, from a Multiplayer section added to the game's Escape menu, and by
  itself when a session starts. While a session runs it shows it: the
  state, the players with their ping, who sees the game, the SteamID64, the
  players' cursors and Stop hosting / Disconnect. Over a city with no
  session it offers Host this city. The page is modal: the game gets no
  click or key while it is up. The RepublicMP window is now a debug tool,
  hidden until Insert.
- **Fix** - the session's player list no longer crashes the game when it is
  asked for before the first player has connected.
- **The MULTIPLAYER page in the game's language** - the page and the menu
  item follow the language set in the game (Portuguese (Brazil), Spanish,
  French, German, Italian, Polish, Czech, Russian; the others stay English).
  Russian draws the rows in the game's own Cyrillic menu font, and a letter
  a font lacks comes from the game's Arial. The session's messages are
  still English.
- **MULTIPLAYER page** - the item sits after Load game in the main menu,
  away from Quit; the HOST tab sets the session's size (2-4 players, the
  same setting as the RepublicMP window's); every row reacts only over its
  text. When a session starts another way (Steam's Join Game, the RepublicMP
  window) the page closes and the RepublicMP window shows the join, and why
  it failed; long errors wrap inside the panel.

## 0.7.0

Co-op only, Steam connections, and the code reorganised for a second game
mode. Protocol 45 - every player needs this build.

- **Citizens' destinations** - the destinations panel of homes, bus stops and
  the other buildings that send people somewhere (workers, students,
  passengers, tourists) works in a session: picking a building, "any
  building", Remove and the share buttons are made on every PC (by
  SoylentGreed). Picking was refused before and did nothing in a session.
  "Remove all connections" is still not carried.
- **Game list** - the MULTIPLAYER page has HOST and JOIN tabs. A hosted
  session is listed through a Steam lobby, for Steam friends (the default)
  or for everyone; JOIN lists the friends' games and the public ones of the
  same protocol, with mode, players and an estimated ping, and joins one with
  a double click or JOIN. Players connect to the host over Steam as before;
  nobody joins the lobby itself.
- **MULTIPLAYER in the main menu** - the game's own main menu gets a
  MULTIPLAYER item, drawn like the others. It opens a page in the game's own
  panel, fonts and colours: pick the game mode, **Host new game** or **Host
  saved game** through the game's own pages (hosting starts by itself once
  the city has loaded), or join by SteamID64 or IP; the player's SteamID64
  is shown with a Copy button.
- **Steam connections** - a player can type the host's SteamID64 instead of
  an IP and connect through Valve's relay: no IP to share, no port to open.
  Steam friends can also use **Join Game** or an invite while someone hosts
  (with the game already started through TesmioLoader). The host shows its
  SteamID64 with a Copy button and takes TCP and Steam players at once; LAN
  and VPN by IP work as before.
- **Send rate** - over Steam the world transfer raises its send rate while
  the link stays clean and halves it when the ping climbs or packets drop
  (up to `[steam] max_rate_kib` in `RepublicMP.profile.ini`, 16384 by
  default); lockstep messages go out without delay.
- **Replay mode removed** - the old mode where each game simulated on its own
  and actions were repeated on the other side (with ports 5557-5559, the
  drift check and the timed re-sync) is gone; co-op runs in lockstep only.
  Only TCP ports 5556 and 5560 are used.
- **Construction sites** - a building site shows its construction stages in
  a session again (cut model pieces, scaffolding, dirt and gravel piles). The
  site drawer skipped every building the simulation keeps off the camera; it
  now draws with this PC's own view, which the simulation never sees. The
  Debug window can turn it off and log the sites it draws.
- **Start page** - the RepublicMP window opens on a choice of game: Co-op,
  or Republics (each player their own republic on one map - in development).
- **Code layout** - `world/` holds the actions made from data (buildings,
  networks, terrain) and the hooks that capture the player's actions;
  `modes/` is where a game mode plugs in its page, frame and overlay;
  `api/republicmp_api.h` lists what a mode may use; ARCHITECTURE.md
  describes the layers, how a command travels and the owner of every hook.

## 0.6.2

Construction offices, copy tools, cursors and pings. Protocol 43 - every
player needs this build (0.6.1 and 0.6.2 do not join each other).

- **Construction offices** - the office pick tools work in a session again:
  adding construction or demolition to an office, copying one office's
  constructions to another (offices sharing their sites work as one
  company), the area office tool, the office window's "add construction",
  starting a reconstruction and continuing a construction from the building
  windows (by SoylentGreed).
- **Offices assign tool** - the tool's window is shared: the selected
  offices, the mode and the quick selection slots are the same on every PC,
  so any player can assign constructions with the same group of offices.
- **Copy tools** - copying waste and transfer settings, distribution links
  and specific sources, and the distribution office's lists work in a
  session again (by SoylentGreed).
- **Weights and fire stations** - copying a building's weights and sending
  the fire station's vehicles to a burning building with the pick tool are
  made on every PC (by SoylentGreed).
- **Cursors and pings** - the other players' cursors are shown on the map
  with their names, and pressing ~ pings the place under your mouse for
  everyone (an arrow at the screen's edge when it is out of view). The
  RepublicMP window can hide the cursors and set another ping key or mouse
  button.
- **Settings remembered** - besides the name and the session's size,
  RepublicMP.profile.ini keeps the DLC check, the cursors and ping key and
  the Debug window's logs (money trace, field diff, terrain log).

## 0.6.1

Early Start fix and building together. Protocol 40 - every player needs
this build (0.6.0 and 0.6.1 do not join each other).

- **Early Start cities** - a fresh city in an early era (Early Start DLC)
  no longer gets the era's start again at the session's load on one PC:
  that PC had a third of the money and of every price, so purchases (the
  vehicles that showed up on one PC only) and everything paid split the
  games from the first minute.
- **Building together** - a road, rail or pipe you are drawing is no longer
  dropped when another player builds or demolishes something: it pauses for
  their command and goes on from its start while you still hold the button.

## 0.6.0

Up to four players in lockstep. Protocol 40 - every player needs this build.

- **Multiplayer for 3-4 players** - the host sets the session's size and
  everyone picks a name in the menu (both are remembered for the next
  start, in RepublicMP.profile.ini next to the DLL). The world is sent to every player at
  once, the game starts once all are connected, a player who joins during
  the game brings everyone through a re-sync, and the host stops taking
  players when the session is full.
- **Money desync** - the small money difference that came back now and then
  is fixed: the game's periodic update of resource prices kept a timer from
  before the session, so the PC that had played its city before hosting
  changed the prices at another moment and every construction site paid
  for materials a little differently there.
- **Lockstep by default** - Lockstep is the default session mode.
- **Saving** - saving in a lockstep session saves on every PC at the same
  moment (each keeps its own copy).
- **Research** - starting or suspending research in a research building's
  window happens on every PC (by SoylentGreed).
- **Station and distribution links** - links added with the building
  window's link tools are made on every PC; before, the link existed on the
  clicking PC only, its load/unload settings were refused elsewhere and the
  vehicles split the games (by SoylentGreed).
- **Building pick tools** - the copy tools (weights, sources, waste,
  transfer, distribution, construction), adding construction or demolition
  to an office, the distribution office lists, the fire station's dispatch
  and the area office tool acted on one PC only; they are put away in a
  lockstep session for now.
- **Debug** - the money trace is on by default; a desync report says which
  players differ and which match, and the per-site part covers the same
  ticks as the balances, with each site's materials; each tick's line has
  a hash of the resource prices, and a desync lists the prices.

## 0.5.5

More of the game shared in lockstep. Protocol 36 - both players need this
build.

- **Immigrants** - the invite buttons of a residential building's window
  (third world, Soviet bloc, experts) bring the same citizens on both PCs;
  before, they came on the clicking PC only and the games split (by
  SoylentGreed).
- **Construction funding** - the bottom menu's "build with rubles / dollars"
  switches can be switched back from either PC; once the session had left
  the starting value, neither player could click it back.
- **Debug** - the money trace also logs what each construction site paid in
  the ticks before a desync.

## 0.5.4

Desync fixes for building and terraforming in lockstep. Protocol 35 - both
players need this build.

- **Forklift paths** - factory connections attach to buildings again in
  lockstep; the buildings' connection points were missing.
- **Placement** - a building's access road or path is laid the same way on
  both PCs, whatever the cameras' angles and whatever each player's mouse
  points at (bus stops, substations and garages next to roads split the
  games); the building tool no longer sees what one camera sees while a
  placement is made.
- **Construction sites** - a site under one player's camera no longer runs
  differently on that PC; sites under construction no longer flicker while
  more of the same building is placed.
- **Terraforming** - a paid terrain stroke's entry in the construction
  ledger is shared, so the construction sites pay the same on both PCs
  after terraforming.
- **Fixes** - opening a building window in a session no longer crashes the
  game.
- **Debug** - a money trace (off by default): on a desync each PC logs its
  balances and construction sites of the ticks before it; a placement's log
  line lists the objects it made.

## 0.5.3

Lockstep that no longer depends on the camera, a reworked window and a DLC
check. Protocol 28 - both players need this build.

- **Camera-free simulation** - for the simulation every building is off the
  camera on every PC, so moving the cameras apart no longer splits the games
  (tested for 54,000 ticks with the cameras far apart); every citizen is
  updated every tick, so citizens walk smoothly; each PC draws the citizens,
  stops included, and building doors near its own camera (by SoylentGreed).
- **Fewer desyncs** - the game's K debug key no longer changes the
  simulation on one PC; city and crossroad renames are shared (by
  SoylentGreed).
- **Hyper and Plaid** - Ctrl+Numpad1 / Ctrl+Numpad2 (or the fast button held)
  work in lockstep; the host's, the player's keys are requests.
- **DLC check** - a join with different DLCs is refused while both players
  keep "Check DLCs" ticked; either can untick it to play anyway (#30).
- **Window** - text wraps; the mode is chosen as Lockstep or Replay; a
  lockstep panel shows the simulation, the ping, the tick flow and a desync
  on both PCs, with a re-sync a set time after a desync (1 minute by
  default); replay-only indicators stay in replay mode.
- **Fixes** - a session whose load refused the patches no longer leaves the
  host paused; the F8 / F9 / F10 dev hotkeys and the heartbeat log are gone
  (the terrain log is in the Debug window).

## 0.5.2

More of the game shared in lockstep. Protocol 23 - both players need this
build.

- **Roads and junctions** - road signs, junction rules (right-hand, main
  road, traffic lights) from the junction window and the decoration tool,
  and rail signals (by SoylentGreed).
- **Game rules** - changes on the settings page apply on both PCs; a
  critical rule no longer saves and reloads one game only.
- **Vehicles** - selling, per-stop cargo settings, composing trains in the
  depot (couple, uncouple, move wagons) (by SoylentGreed).
- **Buildings** - cargo filters of links and specific sources (the add-source
  tool, the source windows) (by SoylentGreed).
- **Fixes** - the vehicle window's "Carried" list shows the cargo again;
  traffic lights and building doors no longer blink; the camera no longer
  flies off.

## 0.5.1

More of the game shared in lockstep. Protocol 22 - both players need this
build.

- **Terrain brushes** - raise, lower, the three levelling brushes, tree
  demolition and topsoil restoration run as the game's own tool on both PCs
  with the clicking PC's inputs (cursor, brush, frame time, level height),
  so vegetation, topsoil and costs come out the same (by SoylentGreed).
- **Trees** - the single-tree tools plant the same trees on both PCs (#27,
  by SoylentGreed).
- **Decoration paths** - the second-road paths and pavements (airplane,
  basic road, l2-l4) are drag commands like roads (by SoylentGreed).
- **Window settings** - what a click changes in a building's window, its
  storage slots (auto purchase amounts, "store this item") or the bottom
  menu's world settings is put back and carried; the bottom menu's
  every-frame rewrite no longer splits the world settings.
- **Farms and offices** - adding a field to a farm, and unassigning fields
  and buildings from farms and offices, removing links (by SoylentGreed).

Still put away in lockstep: removing decoration paths, worker relocation,
buoys, Christmas lights, junction and merge tools, "Go there".

## 0.5.0

Lockstep: both games run the same simulation tick by tick. This is a rewrite
of how the two games stay together - vehicles, citizens and production no
longer drift apart. Protocol 21 - both players need this build. Replay mode
stays available.

- **Lockstep sessions** - **Host in lockstep** makes the next session run in
  lockstep; the joining player follows the host. The host saves, both PCs
  load the same save in parallel, and from there the host admits the ticks
  (at most 10 ahead of the player) and orders the commands. Pause and speed
  are the host's. A state hash is compared every 60 ticks; the first
  difference is shown in the RepublicMP window and can trigger a re-sync. A
  keepalive ends the session after 20 s of silence.
- **Determinism layer** - fixed time step, random numbers per call site,
  pinned math, zeroed allocations; the camera, the windows and the tools are
  kept out of the simulation (private view and search lists for the tools,
  window cost displays, per-frame window work). The Debug window's probe runs
  a save twice, with held frames, speed changes and recorded building, and
  compares every tick (by SoylentGreed).
- **Construction as commands** - placement (taken at the game's own decision,
  with levelling and access roads), every drag tool, upgrades, planned
  construction, group tools, terrain edits, building demolition, and the
  bulldozer: one command per piece, removal drags split into several
  commands, pieces removed through a hovered node carried with that node.
  Edges a command creates go back into the network's visible-edge lists, so
  the pieces a cut leaves can be picked at once.
- **Purchases** - vehicles (new and used), licences and factory blueprints
  bought in the purchase windows (by SoylentGreed).
- **Lines and routes** - adding stops, putting vehicles on a line, the depot
  pick tool, creating and deleting lines, the line window's Accept (by
  SoylentGreed).
- **Windows** - the vehicle and line windows' buttons as commands; vehicle
  settings changed in a window are put back and carried; the vehicle
  window's per-frame route rebuild no longer runs in a session; building
  window settings, storage slots and construction pay / stop; the farm field
  tool (by SoylentGreed).
- **Logs** - every command logs the money it changed, so a money difference
  can be traced to the command.

Known issues:
- The construction animation (scaffolding, ground, workers) is not drawn in
  lockstep.
- Not carried yet: trees, worker relocation, pavements, buoys, Christmas
  lights, junction and merge tools, "Go there", goods purchases and the auto
  purchase window, research, world settings.
- Some house window settings may still differ between the PCs.

## 0.4.3 (pre-release)

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

## 0.4.2

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

## 0.4.1

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

## 0.4.0

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

## 0.3.9

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
