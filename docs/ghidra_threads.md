# Ghidra threads in SOVIET64.exe (1.1.1.9)

What the Ghidra database knows beyond `ghidra_labels.md` (the labels taken
from RepublicMP's own code), and how sure each name is. Addresses are RVAs
(image base 0x140000000). The engine DLL has its own page:
`ghidra_dll_threads.md`.

## How sure a name is

| Name | Meaning |
|---|---|
| `Net_SplitEdge` (no mark) | From RepublicMP's code, or plain in the decompile |
| `Site_InitForEdge?` | Inferred: strings, callers or the decompile point to it |
| `Net_NodeCheckOrphan??` | A guess, or a name made by rule (below) |

Comments: `RepublicMP: ... [file]` for the labels from the code,
`RE: ...` for what was read in the binary.

Names made by rule (all `??` unless said otherwise):

- `X_sub_RVA??` - only called from the thread of `X` (its one caller, or
  every caller in that thread). A thread's root keeps its name; deeper
  levels keep the root's prefix, so `Early_BuildingPass_sub_*` is the whole
  building pass however deep.
- `UI_<text>_RVA??`, `Render_<text>_RVA??`, `File_<text>_RVA??`,
  `Res_<text>_RVA??`, `Str_<text>_RVA??` - a function with strings: its
  kind (draws text / sets shaders / reads files / looks up resources /
  other) and the first telling string it uses.
- `StdVector_Grow_RVA?` / `StdString_Grow_RVA?` - throws `vector<T> too
  long` / `string too long`: a std container's reallocation.
- `Thunk_<target>_RVA?` - a few instructions that only call the target.
- `Units_Format_RVA?` - a small `UNITS_METRIC_IMPERIAL` formatter.

## The world is a static

`g_World` is the object at 0x9D4F10, so every `world+off` is the global
0x9D4F10 + off and is named as one: `g_World_*`, the network managers
`g_Net_*` (world+0xF7A0.. from `world/networks.cpp`), `g_LinesManager`
(world+0x12800), `g_JunctionManager` (+0x12808), `g_VehiclesBegin/End`
(+0x12810), `g_EarlyVehicleList` (+0x12828), `g_VehicleTypes` (+0x12840),
`g_World_SelectedTool` (+0xD428) and the rest of `worldglobals` below.
The world rules from the save header are world+0x5B0..+0x5E0
(`g_ConstructionRealism?` +0x5B8, `g_Realism` +0x5BC, `g_StartYear`
0x9D54A4).

Corrected on the way: 0x9E7710 was `g_AgentQueue` (it is the line manager,
held_frame reads its +0x60 queue), 0x9E7720 / 0x9E7728 were `g_SimVec_A/B`
(the vehicle vector), 0x9E7738 was `g_SimVector` (Early's vehicle list).

## The footpath node demolition (#622)

```
Tool_Pedestrian 3D68B0
  Net_DragTick 4FCAF0 (type 0x65)   picks the piece / hovered node
  Net_DragCommit 518A80
    Net_CommitDemolish? 5205A0
      node branch: for each edge on the hovered node (manager+0x378)
        > 6 points:  Net_SplitEdge 5446D0 (3 points from the node)
                       new piece's site: Building_Construct 25BAB0,
                       copies site +0xEA8 / +0x608 / +0x609,
                       Site_InitForEdge? 2D9D40,
                       Site_SplitProgress 3BD790 (parent progress x fraction)
                     then Net_RemoveEdge on the near piece
        < 7 points:  Net_RemoveEdge 5434C0
      Net_RemoveEdge(manager, edge, 0, -1, 0)
        g_ConstructionRealism? > 1 and edge +0x140 == 0 and
        (edge +0x141 || no site || site +0xEA8 || site +0x604 > 0)
          -> Net_EdgeStartDemolition 525EF0: the edge stays as a demolition
             site (a building), vehicles sent home
        else -> buildingfall3 particles, Building_Remove? 162CB0 (543e3b),
             Net_RemoveNode? 5423B0 for nodes left alone,
             Net_RegenGeometry? 552090 for types 0 / 0xd
```

What was ruled out:

- `g_ConstructionRealism?` (world+0x5B8) is a world rule: written only by
  the new game menu (`Menu_NewGameStart?` copies 9D5C50), the header import
  (`Save_ImportHeader?`, header +0x1ac) and LoadWorld (zeroed during the
  load, restored after). The same on every PC.
- A save and load keep edge +0x140 / +0x141 as they were
  (`Net_ImportEdge?` 570C00 reads both bytes from the file).
- The random draws on this path are the particles (their own sites) and
  draws inside a command's own stream.

What is left: the 4 extra buildings on arik's PC were the 4 demolitions
that split an edge (#622, #625, #627, #629). On arik those pieces went the
demolition-site way, so before #622 a parent edge's site differed there
(+0xEA8 or +0x604, or the edge's +0x141). Arik's log of that run is gone;
a log of those fields in `ApplyDemolishInner` before the commit would show
it on the next desync.

## Counts after this pass

12 868 functions: 4 280 still `FUN_`, 236 named `?`, 1 900 named `??`
(303 threads), the rest by RepublicMP's labels, the compiler (unwind and
catch funclets) and the imports. The largest threads:
`Early_BuildingPass` (147), `SaveGame` (35), `Vehicle_Update` (34),
`Net_DragTick` (30), `Load_Buildings??` (29), `LoadWorld` (28).
