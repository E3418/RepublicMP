# Ghidra threads in C3DDLL64.dll

The engine DLL of the same game build (image base 0x180000000; addresses
below are RVAs). Names follow the marks of `ghidra_threads.md`: no mark =
plain in the binary, `?` = inferred, `??` = a guess or a name made by rule.
Comments: `RE: ...`.

The DLL exports about 2 080 of its functions with C++ names, so most of
the engine (`C3D_TERRAIN`, `C3D_BUILDING`, `C3D_FOREST`, `C3D_SOUNDMANAGER`,
...) was named before this pass. What was left were the functions without
an export: library code and the engine's own helpers.

## Where the code is

| RVA | What | Named as |
|---|---|---|
| 001000-002000 | the globals' dynamic initializers | `StaticInit_RVA?` |
| 002000-110000 | the engine (exports), with its helpers | `<Class>_<Method>_sub_RVA??` |
| 110000-118000 | DirectXTK (WIC, screen grab), DDS conversion, TrackIR, WAV | `DirectXTK_*?`, `TrackIR_*?`, `Wav_Load?` |
| 118000-146DB0 | Tokamak physics (no exports, Tokamak's error strings) | `Tokamak_*?`, `Tokamak_sub_RVA??` |
| 1479E0-166EEC | Effects11 (FX11), with RTTI | `FX11_<Class>_<Method>?`, `FX11_*_vfN?` |
| 166EEC-16B000 | MSVC CRT / STL startup | `Crt_*??`, `StaticDtor_RVA??` |

## Names by rule

- `<Class>_<Method>_sub_RVA??` - only called from that export's thread
  (one caller, or every caller in it). `_ctor`, `_dtor`, `_assign` and
  `_op` stand for the constructor, destructor, `operator=` and other
  operators.
- `<Class>_sub_RVA??` - called only by methods of one class.
- `Tokamak_sub_RVA??` - a helper in the Tokamak region; its comment keeps
  the thread it was reached from (mostly `C3D_PHYSICLAYER`).
- `<Class>_vfN?` - slot N of the class's vftable (from RTTI). FX11's typed
  slots get the interface method names: Scalar and Matrix are anchored by
  the `ID3DX11Effect...::Get*` error strings (`?`), Vector follows the
  header's order (`??`). A slot shared by several classes is
  `FX11_shared_vfN_RVA??`.
- `StdVector_Grow_RVA?`, `StdList_Grow_RVA?`, `StdTree_Grow_RVA?`,
  `StdString_Grow_RVA?`, `StdAllocate_RVA?` - std containers; `Std_sub_*??`
  are their helpers.
- `Thunk_<target>_RVA?` - only calls the target.

## Random numbers and clocks

Every call of `rand`, `srand`, `timeGetTime` and `_time64` has an EOL
comment. `rand` is called 125 times from 33 functions, all of them
exports. The game imports 17 of them:

| Function | Draws | What for |
|---|---|---|
| `C3DRandom_Int/Float/Double` | 1 each | the game's own wrappers; the exe's imports of them are patched, nothing inside the DLL calls them |
| `C3D_FOREST::Import`, `EditorGenerateCursor`, `EditorGeneratePosition`, `EditorAddCursorTreesSoviet` | 1-4 | trees: places, kinds and turns, at load and in the tree tools |
| `C3D_BUILDING::Import` | 1 | a 0..1 phase per model part at load |
| `C3D_BUILDING::EditorSetCursorSingle` | 1 | a 0..1 value in the placement cursor's record |
| `C3D_BUILDING::FallDownRefresh` | 3 | falling fragments: particles, and a 1 in 3 chance of the hit effect |
| `C3D_PARTICLEFFECT::SpawnParticles`, `C3D_CLOUDS::GenerateClouds` | 18, 12 | particles, clouds |
| `C3D_SOUNDMANAGER::PlaySound2D/3D`, `GetSound2D/3DForLoop`, `C3D_SOUND3D::Play` | 1-4 | which sample plays |

The other 16 are reached only inside the DLL or not at all:
`C3D_PATH::DeleteSomethignFromType` (thins a path type's elements at
random) has no caller, and `C3D_BUILDING::FallDownStart`,
`PhysicDetachFragmentPart` and `AddSmokePoint` hang under
`ProcessDamageOnFragment`.

Since 0.7.6 every one of these sites draws from its own stream in a session
(`determinism.cpp`, `H_engineRand`, site `0x40000000 | RVA`), so the
particles and sounds each PC makes for its own camera no longer move the
trees' numbers. Nothing found here decides the simulation.

Clocks: `C3DRandom_Start` seeds with `timeGetTime` (a no-op in a session),
`C3D_NET_PEER` (the engine's own Steam P2P peer) times its packets with
it, and `C3D_PHYSICLAYER::threadUpdate` steps the
physics on its own thread with `QueryPerformanceCounter`.

## Counts after this pass

4 038 functions: 271 still `FUN_`, 415 named `?`, 895 named `??`
(93 threads), the rest by the exports, the compiler and the imports. By
area: FX11 541, std 275, the engine's helpers 208, Tokamak 184, CRT 62,
Steam 9. The `FUN_` left are mostly helpers called from more than one
class, and a few reached only through pointers.
