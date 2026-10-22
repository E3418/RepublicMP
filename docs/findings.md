# Game internals (reverse-engineering findings)

What is known about SOVIET64.exe 1.1.1.9 (stamp `0x6A3EB6AD`) and its engine
`C3DDLL64.dll` beyond what the code already names. Translated and condensed
from WRMP's `docs/findings.md` (by Sohn); RepublicMP's own findings live as
named `#define`s in the code (`game/world.h` first). Every RVA is for this
build; "site" is the instruction address that backs a finding, so it can be
found again.

Evidence levels:
- **observed**: confirmed by running the game or reading real data.
- **inferred**: from disassembly or analogy; test before writing to memory.
- **inherited**: documented and used by TesmioLoader's plugins, not
  re-checked.

A finding becomes a hook only through its owner (ARCHITECTURE.md, "Hooks and
their owners").

## World and entity lists

Already in `game/world.h`: the world object `exe+0x9D4F10`, the buildings
vector `world+0x11B08`, date `+0x590/+0x594/+0x59C`, money, building type
`+0x318`, position `+0x350`, construction progress `+0x604`. WRMP **observed**
the date base in game (day 327 of 1932 = 24 Nov); `*(exe+0x9941F0)+0x590`
reads garbage, so `0x9941F0` is another object (see Terrain).

Lists, all `std::vector<T*>` (begin, end, cap); **inferred** from the
script VM's handlers (dispatcher near `0x59F3xx`), counts **observed** to
match the UI in WRMP's F1 test:

| List | Where | Site |
|---|---|---|
| Buildings | `world+0x11B08` | `0x5B5CEB` |
| People | `world+0x126A8` (rva `0x9E75B8`) | `0x5BF559` |
| Vehicles | `world+0x12810` | `0x5BB471` |

- Ships `+0x128C8`, helicopters `+0x128E0` and airplanes `+0x128F8` are
  **subsets** of the vehicle vector; adding them counts twice.
- Removal keeps order (`memmove`, `0x3E22F5`): an index changes when another
  entity leaves. **An index is not a network id.**
- The people list has 300-500 more entries than the UI's population
  (**observed**, two saves); no one appears twice. `Person+0xC8` does not
  separate foreigners.
- The first building of the vector can be a special object (type 20 at
  (-19000, -20, -19000), **observed**); some entries have no type descriptor.

Other building fields (inherited unless noted):

| Field | Offset |
|---|---|
| 3D node, embedded (not a pointer) | `+0x320` |
| Bounding box | `+0x5D0` (`->+0x20` min, `->+0x28` max) |
| City | `+0x108` |
| Type number (`BUILDINGTYPE_*`) | `[+0x318]+0x360`, int32 (inferred) |
| Subtype | `[+0x318]+0x364` (inferred) |
| Construction paused by the player | byte `+0x879` (inferred; set `0x1605B3`, cleared `0x1614B3`) |
| Demolished | byte `+0xEA8` (inferred) |
| Visible / visibility distance | byte `+0x115A` / `+0x115C` (inferred) |

## Vehicles

Object of `0x1AD8` bytes, constructor `0x663720`; every wagon and private car
is its own entry (inferred).

| Field | Offset | Site |
|---|---|---|
| 3D node, embedded | `+0xD60` | `0x5BA5FA` |
| Type descriptor | `+0x1708` (pointer) | `0x5BA675` |
| Vehicle category | `type+0x294` | `0x5BA67C` |
| Script tag (wide string) | `+0x30A` | `0x5BA51C` |
| Mesh per LOD 0 / 1 / 2 | `+0x13F0` / `+0x1408` / `+0x1410` | `0x6726DF` |
| Colour variant material | `+0x16F0` (from `type+0x328+8*i`) | `0x671291` |
| Second section mesh / node | `+0x13F8` / `+0xED8` | `0x6737BA` |
| Drawn at all | byte `+0x13D8` (0 = skipped) | `0x67135B` |
| Carried by another object | `+0x398` (node may be stale) | `0x671368` |
| Mounted on a host | `+0x4A0` (local node `+0xE1C` x host matrix) | `0x6716D4` |
| Cargo | `+0x3B8` (`{Resource*, float}`) | - |

Inferred:
- The node `+0xD60` is only ever written with
  `CreateFromPositionRotationScale` (`0x6DE4A0` per frame, `0x6AD960` on
  placement): its rotation is always current.
- Categories (`type+0x294`): 0 not specified, 1 road, 2 road service, 3 rail
  wagon, 4 rail locomotive, 5 rail service, 6 ship, 7 cabin, 8 airplane, 9
  container, 10 helicopter. The track construction train and the Dampfkran
  are category 4.
- `byte +0x13E8` is written with `+0x13D8` (`0x6C07E0`): 1 at a route point, 0
  inside a building - probably "outside a building".
- Network identity of a model: (LOD 0 mesh file name, material file name).
  Mesh paths are always relative (`vehicles/...`, `dlc%d/...`,
  `workshop_subscribed/<id>/...`); the mesh cache key is an exact,
  case-sensitive `strcmp`.

## People

Object of `0x750` bytes (inherited).

| Field | Offset | Note |
|---|---|---|
| Placement node, embedded | `+0x5A0` | valid only while standing in a building (`+0x598` = `Building*`) |
| Status (11 floats) | `+0xD8` | |
| Tourist | `+0x71C` | |
| Script tag | `+0x69A` | wide string |
| Hidden | byte `+0x2C` | render skips it |
| Path building / segment / advance / length | `+0x38` / `[+0x50]` / `[+0x48]` / `[+0x4C]` | walking |
| Mesh record | `[+0x668]`: mesh `+0x0`, material `+0x8`, animation `+0x10` | |
| Animation frame | float `+0x65C` | |

Inferred:
- Canonical position: `0x824660(?, outVec3, person)`: the vehicle's node
  (`[+0x60]+0xD60`), else the path, else the building node `+0x320`.
- `0x12AA90(person, C3D_NODE* out, 0)` builds a walker's node (position and
  rotation) but returns 0 off-screen or beyond 450 m of the **local** camera.
  To compute it for any camera: `seg = [[p+0x38]+0xCA8] + [p+0x50]*0xF0`;
  points (vec3, step `0xC`) from `[seg+0xA0]` to `[seg+0xA8]`, accumulated
  distances at `[seg+0xB8]`; `s = p+0x48`; position = `P(s) + (p+0x9C, 0,
  p+0xA0)`; yaw from `P(s+0.4) - P(s)`; pitch and roll 0.
- The frame only advances near the local camera (`0x832E90`, list
  `world+0x126C0`): `frame += dt * [p+0xA4] * 30 * 5` walking, `* 1.5` standing;
  it wraps to **zero** at `nFrames-1`.

## Rendering

### How the game draws a model
The engine has no scene graph: the game sets a node and draws a mesh, both
through imports (inferred; types from the decorated names):

```
RenderingSetNode(middlepoint, node, bool, bool, bool, float)
C3D_MESH::Render(mesh, int, bool, bool, int part, int, C3DAPI_SHADERS*, bool,
                 C3DAPI_BUFFER*, uint, uint, C3DAPI_BUFFER* /*ptr*/, uint, uint, float)
```

- `part = -1` draws parts `0..[mesh+0x44]-1`; any other value draws only
  that part, and the engine checks `0 <= part < [mesh+0x44]` (signed).
- The middlepoint is the static `exe+0x9EACD0` (pass its address); its current
  effect is `[mp+0x38]`. Shadow passes use `RenderingSetNodeShadow`.
- The last float of `Render` is `[0x90B300]` = -1.0 at the game's sites.
- A mesh renders without a batch: with `byte [mp+0x89] == 0` it binds its own
  buffers and material (`UseThisMaterial(mesh+0x190, [mp+0x38], ...)`).
  `BeginMeshBatchRender` only opens a batch with `byte [mp+0x88]`, which the
  game keeps on. A batch has **one** vertex format (`mp+0x684B0`), so an
  extra mesh inside it needs the first mesh's format (`mesh+0x198`).

| Import | IAT slot | Sites |
|---|---|---|
| `C3D_MESH::Render` | `0x86CAA0` | 157 |
| `C3D_MIDDLEPOINT::RenderingSetNode` | `0x86C438` | 208 |
| `C3D_MIDDLEPOINT::RenderingSetNodeShadow` | `0x86D420` | 11 |
| `C3D_MIDDLEPOINT::CreateManagedMesh` | `0x86C4B0` | 467 |
| `C3D_MESH::LoadFromFile` | `0x86C5F0` | 464 |
| `C3D_MESH::LoadMaterial` | `0x86CAA8` | 401 |
| `C3D_MIDDLEPOINT::RenderingGetCamera` | `0x86C6E8` | 7 |
| `C3D_LIGHTING::CustomVectorGet` | `0x86C228` | - |

### C3D_NODE
A plain struct of `0xBC` bytes (inferred; the copy constructor DLL `0x25CE0`
copies exactly that, the default one does nothing, no destructor exported):
matrix `+0x00`, inverse `+0x40`, position `+0x80`, rotation `+0x8C`, scale
`+0x98`, orientation `+0xA4`, up `+0xB0`. A mod can keep a node in its own
memory (`alignas(16)`, `0xC0` bytes); nothing in the engine is allocated.

**Observed** outside the game, calling the DLL: the node matrix is row-vector
(rotation rows 0-2, translation row 3);
`C3DMatrixRotationYawPitchRoll` equals `D3DXMatrixRotationYawPitchRoll`;
`CreateFromMatrix(node, m, a, b, c, scale)` always sets `+0x80` and only
recomputes `+0x8C` with `c` true.

### C3D_MESH and C3D_MATERIAL
Inferred; the exported getters confirm `+0x190` and `+0x198`.
- `+0x44` part count; `+0x58` parts of `0xF8` bytes; `+0x50` sub-material
  names (64 bytes each).
- `+0x8D` byte "loaded" (set at the end of `LoadFromMemory`; `LoadFromFile`
  can return 0 even when it failed - check this byte).
- `+0x8E` file name, embedded `char[]` as the caller passed it, copied
  without a limit; read at most `0x100` bytes.
- `+0x190` material (null skips binding). Meshes are shared by path, and the
  game rewrites this field before each draw with the colour variant.
- `+0x198` vertex format. Animations: count `+0x1A8`, data `+0x1B0` (step
  `0xB0`, frames at `+0x9C`).
- `C3D_MATERIAL+0x29` file name. Loading a material without touching a
  mesh: `CreateManagedMaterial(mp, path)`, `C3D_MATERIAL::Create(mat, mp)`,
  `C3D_MATERIAL::Load(mat, path, 0, n)` - what the game does per vehicle on
  the main thread (`0x671213..0x67126B`).

### The camera
`RenderingGetCamera(mp)` returns `[mp+0x40]`; `ViewGetPosition` reads
`camera+0x80`, `ViewGetLookAt` `+0x98` (inferred). **Observed:** the look-at
point is not on the ground (Y -45..-162 up close); for the ground point at
the screen centre, cross the eye-target ray with the terrain.

### Render lists and the vehicle pass
Inferred unless noted. The render lists are built on the main thread and
drawn by the render thread:
- Items of `0xE8` bytes (vehicles): node `+0x00..+0xBB`, mesh `+0xC0`, part
  `+0xC8`, material for `mesh+0x190` at `+0xD8`. Items of `0xE0` (network
  objects): node, mesh `+0xC0`, colour mode `+0xCC` (-1 none), colour `+0xD0`.
- `0x5858F0` copies 30 main lists (`0x996FD8 + 0x30*i`) to their render twins
  (`+0x18`) and empties the main ones (**observed** in disassembly). Items
  appended to a render list at the start of the render frame are drawn in
  that frame and dropped by the next copy; `0xECF90` / `0xECEB0` are their
  `push_back`s.
- `0x47F380` draws the vehicle list `[[0x995268]+0xC0]` (filled in
  `0x6720E4`): per item `RenderingSetNode(mp, item, 1, 1, 0, 0)`,
  `mesh+0x190 = item+0xD8`, `Render(...)`. With `dl=1` (depth) it uses
  another list (`+0x210`) and the shadow node call.
- **Anchor for drawing extra models once per frame:** before looking at the
  list, the colour pass sets its shader and calls
  `C3D_LIGHTING::CustomVectorGet` at its only site `0x47F5A4` (return
  `0x47F5AA`), even with no vehicle on screen. An IAT hook filtered by that
  return address can draw its own models right after the original
  (passes, `SetNode`, `mesh+0x190`, `Render`). WRMP's puppets draw there.
- `0x47F150` is the propeller and rotor list (shader `"Screw"`), not the
  vehicle list.
- Effect objects are `C3DAPI_D3D11_SHADERS` (vtable DLL `0x187E78`): `+0x20`
  SetVertexPixelShaderPair, `+0x28` UploadChanges, `+0x30` Start (pass
  count), `+0x40` StartPass, `+0x50` GetHandle, `+0x58` SetFloat, `+0x68`
  SetVector, `+0x78` SetMatrix; `+0x38` and `+0x48` are empty on D3D11.
  `RenderingSetShaderPair(mp, s, name)` sets `[mp+0x38] = s` and the
  technique. Vehicles use `[0x9963F8]`, animated models `[0x996820]`.
- Animated models (pedestrians, `0x481610`): `SetShaderPair(mp, [0x996820],
  "BumpAnimWorker")`, lighting constants, default material,
  `ScaleFactorWorker`, then per item `SetNode`, `mesh+0x190`,
  `RenderAnimation(mesh, anim, frame, 0, 1, 1, NULL, 0, -1, 0, 0, 1, -1)`.
  `RenderAnimation` does not check the animation index or the base frame:
  clamp both before calling.
- Own vertex buffers: `C3D_MIDDLEPOINT::GetBuffer(mp)`; vtable
  (`0x1878D0`) `+0` Create(type, bytes, format), `+8` FillStart, `+0x10`
  FillEnd, `+0x18` Destroy (frees the D3D side, not the object - a `/MT`
  plugin must pool them), `+0x20` UseForRendering(slot, stride, ctx).

### Vehicle model details
Inferred, from the vehicle render `0x6712B0`:
- Order: skip (`+0x13D8 == 0`, carried by a plane) -> mounted -> LOD ->
  body (parts / replacement mesh `[type+0x7A98]` when `+0x13E8` / normal LOD
  mesh) -> extra parts -> rear section -> category additions (4, 8, 10) ->
  arm or rear by `+0x13E8`.
- Articulated rear (`$JOINT`): mesh `+0x13F8` on node `+0xED8`, tilted around
  the joint `+0x1544` by `[+0x1610]`; drawn whole (part -1); `+0x1550` is the
  tilting node index, not a part.
- Crane arm (category 4): working, the dynamic part `[type+0x7AA0]` is drawn
  on `+0xED8` (rewritten each frame by `0x669760`); idle, only the static
  part `[type+0x7A98]` on `+0xD60`.
- Propellers (category 8): hub `type+0x7A98`, blades `+0x7AA0`, blurred
  `+0x7AB0`, points `std::vector<vec3>` at `+0x7AD8`; roll advances by
  `pi * (v >= 0.042 ? 2v : 15v)` per drawn frame, `v = [veh+0x1778]`.
- Rotors (category 10): records of `0x458` at `type+0x9D10` (hub `+0x408`,
  blades `+0x410`, blurred `+0x418`, material `+0x420`, points `+0x428`).
- Visible cargo: boxes at `type+0x7C20`, mode `$RESOURCE_TRANSPORT_TYPE` at
  `type+0x8600`, capacity `+0x8604`; drawn by `0x2B08C0`.

## Buildings and construction

Inferred unless noted.
- **A building is an engine fragment.** `C3D_BUILDING*` =
  `[[exe+0x9941F0]+0xFF8]`. TYPE = `[cb+0xE718] + [td+0x410]*0x2330` (name at
  `TYPE+0`, model `+0x40`, LOD `+0x48`, `+0x58`); FRAGMENT = `[TYPE+0x2C0] +
  [Building+0x5F4]*0x118`. Type count = `([cb+0xE720]-[cb+0xE718])/0x2330`.
  Finished buildings are drawn by `C3D_BUILDING::Render` (DLL `0x36D60`), not
  by `C3D_MESH::Render`.
- **Do not create fragments for remote buildings:** they enter
  `Collision2DBBox` and `CollisionRay` (placement and paths).
- The building node `+0x320` is written once, on placement (`0x2DA588`).
- The building pass `0xAEE0` (only caller `0x4828C4`) runs 8 sub-passes (base,
  emissive, decals, each with a mirror pass); weather picks `BumpWinter*` or
  `BumpRain*` techniques.
- **Construction sites: `0x2DC470`** (called from the colour pass at
  `0x3DD999`, before the vehicles, and at `0xAD26`) draws buildings with
  `+0x604 != 1.0` or `byte +0x761`, without a batch:
  - each model piece (`type+0x840`, records of `0x21C`, node at `+0x200`) is
    cut with `PositionClip` (`BumpPositionClip` / `DepthPositionClip`) by the
    construction stage (`Building+0x610`, records of `0x21B8`: accumulated
    threshold `+0x0`, `SOVIET_CONSTRUCTION_*` type `+4`, target height, nodes)
    and the progress `+0x604`; height 0 = hidden, 1 = no cut, otherwise
    `minY + (maxY - minY)*h` from `C3D_MESH::GetNodeHeader`;
  - scaffolds generated by `0x167540` on a 6 m grid inside each piece
    (`world+0x11CF8[0..17]`: `buildings/scaff11..33.nmf` and underground),
    kept in `Building+0x640/+0x658/+0x670`;
  - dirt piles (`world+0x11DA0`, places at `b+0x718`) and gravel piles
    (`+0x11DA8`). Wrecks: `buildingwreck*` / `wreck_*.nmf`.
- `0x2DF4D0` / `0x2DF554` draw a ship under construction in a shipyard
  (vehicle type at `b+0x11C8`, progress `b+0xFC0 < 2`), not the building.

## Networks (roads, rails, pipes, power lines...)

Inferred unless noted.
- All linear infrastructure is the Road system (`SOVIET_ROADTYPE_*`).
  Networks at `[world+0x14038 + 0x50*type]`, count `world+0x14034`, fragments
  in a vector at `network+0x2B0`; network type at `[network+0x258]` (8 high
  voltage, 9 low voltage).
- Fragment: centre line `+0x8..+0x10`, hubs `+0x20/+0x28`, subtype
  `+0x120/+0x124`; finished flag byte `+0x141` (1 on placement with progress
  1.0 and on load). Geometry is **generated** (`0x54C260`, called by
  `0x552090` from construction, demolition, editing) from the local terrain;
  CPU vertex vectors (`0x30`-byte vertices) at `+0x218 + 0x18*k` map to VBs
  at `+0x1C8 + 8*k`; drawn straight to the device by `0x58ACE0`, without
  `C3D_MESH`.
- Poles, towers, lamps, catenary: three vectors of `0xD0`-byte elements
  (`+0x08` mesh, `+0x10` node) at `frag+0x2A8`, `+0x2C0`, `+0x2D8`; meshes per
  network at `network+0x808 + 8*subtype` (towers), `+0x8A8` (portals),
  `+0xA30..+0xA60` (lamps, `rail_pole*.nmf`).

## Terrain

Inferred, several sites each.
- The terrain is `[[exe+0x9941F0]+0xED8]` (`C3D_TERRAIN*`).
  `CollisionTerrainHeightOverPoint(terrain, &vec3)` (slot `0x86C058`, 937
  sites) returns the height in `xmm0`, read-only; out of the map returns 0.
- Source of truth: the texture `[terrain+0x210]` (R32F or R16F,
  `[+0x220] x [+0x224]` texels, 0..1); the save holds `heightmap.dds`
  2048x2048 R32F (**observed**). Derived: `u16` collision grid at
  `[terrain+0x840]` (`h*60000`), a min/max quadtree at `+0x408`, normals.
  World: size `+0x87C/+0x880`, `y = h01*[+0x89C] + [+0x894]`.
- Edited only through the `C3D_TERRAIN::Edit*` imports (owner:
  `world/terrain_hook`). `TextureAccessOpen` copies the whole texture to a
  staging copy and `Close` copies it all back (16 MB): batch edits.
- Water: the game imports no water height or level setter; it looks fixed
  per map.

## Clock and threads

Inferred unless noted.

| What | Where |
|---|---|
| Simulation timer, embedded `C3D_TIMER` | rva `0x9D4EE0` (inherited) |
| Effective FPS (game speed divisor) | float `timer+0x0`; `[t+4]` real FPS |
| Paused / blocked | byte `timer+0xC` / `+0xD` |
| Run state | int rva `0x9D54B4` (0 pauses) (inherited) |
| Fast / max speed flags | rva `0x9D54BC` (x0.05) / `0x9D54B2` (x0.01, else x0.35) |

`PowerTime(timer, x, 0, 0)` (DLL `0xFD670`) returns 0 while paused, else
`x * 1000 / [t+0]`: game seconds.

- The frame loop is `0x105A90` (main thread). The scene (`0x482790`) and the
  UI with mouse input (`0x317DB0`) run on a **render thread** created at
  `0x47D1F0` (events `RenderStart` `[[exe+0x995268]+0x60]`, `RenderFinished`
  `+0x68`).
- The main thread releases render frame N at the end of its frame
  (`0x47D320`) and waits for it in frame N+1 at `0x105EB8`, **after**
  `C3D_TIMER::Start`: a hook on `C3D_TIMER::Start` runs in parallel with the
  render and the UI.
- **Safe point to read the world:** right after
  `WaitForSingleObject(RenderFinished)` at `0x105EB8` (bytes
  `FF 15 5A 79 76 00`, infinite timeout): the render thread is done and
  blocked. It is mid-frame - the simulation step `0x30C790` (building
  dispatcher `0x139A70`) has run, the vehicle and people update `0x105F45`
  has not. It runs only with a world loaded (`byte [0x993C65]`); **observed**
  stable over minutes and across world changes in WRMP. Only one hook may own
  this site (it is matched by return address).
- With `byte [0x99528B]` on, the main thread draws alone through `0x9DC80`
  and the render thread does not run.
- **Observed** in WRMP: occasional single frames of ~1.7 s (not autosave).

## The engine's own GUI and the main menu

**Observed** from the DLL exports; the game's use **inferred**.
- `C3D_GUI` (exported, **not used by the game**): `Initialize(mp, input,
  skin)` (returns 1 when the skin file fails to open, 0 on success), layers,
  `AddButton/AddText/AddCheckBox/AddSlider/AddComboBox/AddTextBox...(name,
  [text], C3DVECTOR3 pos)`, state getters and setters, `Refresh(float)`,
  `Render()`, `Destroy()`. The object needs > `0x2D38` bytes (WRMP reserves 64
  KiB). A button's state is `element+0x10C`: 1 while hovered and the mouse is
  held; a click is the rising edge. Skin: `media_soviet/gui/default.ini`.
- The game's own UI: a global `C3D_PANEL2D` at `0x9BE060` and the
  `C3D_FONTMANAGER` at `0x996FB0` (flush: `EndDraw` -> `PrintAllTexts(fonts,
  "Basic")` -> `BeginDraw`).
- Static objects (pass the address): `C3D_MIDDLEPOINT` `0x9EACD0`,
  `C3D_INPUT` `0xA54B90` (mouse held / pressed bytes `+0x300` / `+0x301`).
  The game swallows a click by clearing `0xA54E91`.
- On every menu <-> game transition `0x105700` reloads the texture, mesh,
  material and font managers: a GUI of one's own must `Initialize` again
  when `[0x99528A]` goes 0 -> 1.
- **Menu frame** (main thread): `[0x993C65] == 0`, `[0x99528B] == 0`,
  `[0x99528A] != 0`; the menu is drawn by `0x261210(world)` at `0x1060EA`;
  drawing on top fits right after it (`0x1060EF`, expect `E8 21 B1 15 00`).
- **Root menu:** page int `world+0x5044` (1 = root, built by `0x28AF60`
  every frame, called at `0x261897`). Items: count `world+0x11B0`, wide texts
  at `world+0x11B4 + i*0x204` (30 slots). A click sets `world+0x5038 = 1` and
  `world+0x503C = index`, handled the next frame. **Adding an item:** after
  `0x28AF60`, check the click for our index from the previous frame, then
  write at index `[world+0x11B0]` and increment the count; an item at the
  last index never triggers a game item.

## Steam

From WRMP's ADR 0010 (2026-10-15; inspection of the files, the spike was
not done):
- The game imports from its `steam_api64.dll` only `SteamAPI_Init`,
  `SteamAPI_RunCallbacks` (the game pumps the callbacks), the callback
  registration and `SteamInternal_FindOrCreateUserInterface`, asking for
  `SteamFriends017` and `SteamUser020`: an old SDK.
- That DLL exports the old P2P API (`ISteamNetworking_SendP2PPacket`...) and
  lobbies (`ISteamMatchmaking_CreateLobby`, `InviteUserToLobby`), not
  `ISteamNetworkingMessages` / `ISteamNetworkingSockets`.
- The installed Steam client serves `SteamNetworkingMessages002` and
  `SteamNetworkingSockets` up to `010`. **Hypothesis:** a plugin gets the
  modern interface by name through `SteamInternal_FindOrCreateUserInterface`
  even with the old DLL.
- Limits: 512 KB per reliable message (modern), 1 MB (old; unreliable 1200
  bytes). Testing needs two machines with two Steam accounts owning the game.
- `C3DDLL64.dll` also exports a network layer (`C3D_NET_PEER`) the game does
  not import.

## Other

- On exit the game tries to write `.bin` files into
  `media_soviet/saved_last/` and fails with error 2 when the folder does not
  exist (**observed**; game behaviour, not a mod's).
- TesmioLoader's vectored exception handler runs before any plugin `__try`:
  a caught fault in a plugin still produces a `=== CRASH` report and counts
  toward a limit of 8 per process (**observed** in the loader source).
