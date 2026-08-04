# Dungeon — Architecture

## Overview

Dungeon is a grid-based dungeon crawler (in the tradition of Dungeon Master and
Legend of Grimrock) built in C++23 on DirectX 12. Fallible loaders return
`std::expected<T, std::string>` so failures carry their reason to the caller. The codebase is split into
strictly layered static-library modules. Each module owns one responsibility,
and dependencies flow in one direction only — a module may depend on modules
*below* it in the diagram, never sideways or upward.

```
                ┌────────────────────────────┐
                │           Main             │  composition root (exe)
                └─────────────┬──────────────┘
                ┌─────────────▼──────────────┐
                │           Game             │  dungeon-crawler rules & scenes
                └─┬─────┬───────┬─────┬──────┘
        ┌─────────▼─┐ ┌─▼─────┐ │ ┌───▼───────┐
        │    UI     │ │ Audio │ │ │ Animation │
        └─────┬─────┘ └───┬───┘ │ └───┬───────┘
        ┌─────▼───────────│─────▼─────│──────┐
        │             Graphics        │      │  D3D12 renderer
        └─────┬───────────│───────────│──────┘
        ┌─────▼─────┐ ┌───▼───────────▼──────┐
        │ Platform  │ │        Assets        │  CPU-side data loading
        └─────┬─────┘ └───────────┬──────────┘
        ┌─────▼───────────────────▼──────────┐
        │               Core                 │  log, math, time, events
        └────────────────────────────────────┘
```

## Modules

| Module    | Responsibility | May depend on |
|-----------|----------------|---------------|
| Core      | Logging, assertions, math (DirectXMath wrappers), timing, event dispatch, localization (Loc: key=value language tables from assets/lang, loc::Tr / loc::Format) | — |
| Platform  | Win32 window, message pump, keyboard/mouse input | Core |
| Assets    | Loading CPU-side data: images (stb_image), glTF 2.0 / OBJ models (cgltf), WAV (dr_wav) | Core |
| Animation | Skeletons, animation clips, pose sampling, skinning palettes | Core, Assets |
| Graphics  | D3D12 device/swapchain, meshes, textures, shaders, forward renderer with dynamic lights, 2D sprite/text batch | Core, Platform, Assets |
| UI        | Retained-mode control library: Label, TextOutput, Button, Slider, DropDown | Core, Platform, Graphics |
| Audio     | XAudio2 engine, sound-effect playback | Core, Assets |
| Game      | Dungeon map, party movement, lighting setup, HUD, game loop logic | everything above |
| Main      | `wWinMain`, owns the App object, wires modules together | Game |

## Rules

1. **No upward or sideways includes.** Graphics never includes UI; Assets never
   includes Graphics. Data flows up through plain structs (e.g. `Assets::MeshData`
   is consumed by `Graphics::Mesh`).
2. **Assets is CPU-only.** It produces format-independent structs and never
   touches D3D12. The Graphics module uploads that data to the GPU.
3. **Game contains all gameplay.** Engine modules know nothing about dungeons,
   parties, or items.
4. **Main is glue only.** No logic beyond construction, the frame loop, and
   shutdown ordering.

## Frame flow

```
Platform::Window::PumpMessages
  → Game::Update(dt)         (input → party movement → animation → UI state)
  → GraphicsDevice::BeginFrame
  → Game::RenderShadowMaps   (cube distance maps for the lights nearest the
                              camera; resolution falls off with distance)
  → Game::RenderScene        (3D pass: dungeon, props; torch lights with
                              shadows + per-cell dust scattering)
  → UI::Context::Render      (2D pass: HUD, message log, controls)
  → GraphicsDevice::EndFrame (present)
```

## Memory strategy

Steady-state frames perform no heap allocation — and that is now *checked*
rather than taken on trust (see "Checking the rule" below). The patterns, by
subsystem:

- **GPU transient data — linear arena.** `gfx::UploadAllocator` is a per-frame
  bump allocator over a persistently mapped upload buffer (one per frame in
  flight, reset at frame start). All per-draw constant buffers, skinning
  palettes, and the UI's dynamic vertex stream come from it; nothing per-draw
  touches the heap or creates D3D12 resources.
- **Audio — object pool.** `audio::AudioEngine` keeps a pool of XAudio2 source
  voices reused by sample format (capped at 32). Playback references the
  caller's PCM memory directly instead of copying it, so `Play` allocates
  nothing once the pool is warm. Sounds passed to `Play` must outlive
  playback; the game's sounds live for the app's lifetime, and `~Game`
  calls `StopAll()` so app shutdown never leaves a voice reading freed
  sample memory (the engine outlives Game).
- **Async AI — buffer pools, flat grids.** The per-frame `ai::Snapshot` the
  main thread publishes to the AI workers comes from a pool reused when
  `use_count()==1` (no worker still holds the buffer); its blocked/occupancy
  sets are flat `mapW*mapH` grids rather than node-based containers, so the
  per-publish clear-and-refill allocates nothing. The workers' `ai::Plan`
  batches (and their path vectors) are pooled per IQ bucket the same way.
  Anything that hand-builds a `Snapshot` (e.g. `tools/ThreadStress`) must
  size the flat grids itself.
- **Shader-visible descriptors — free list.** The CBV/SRV heap
  (`kSrvHeapCapacity` = 1024 slots) bounds the *live* texture count, not the
  total ever created: `gfx::Texture` returns its slot on destruction
  (`GraphicsDevice::FreeSrv`), and `AllocateSrv` reuses freed slots before
  bumping the high-water mark, so texture-churn paths (font atlas rebakes,
  level transitions, quality swaps, turbidity rebuilds) recycle instead of
  leak. A recycled slot's old descriptor can still be referenced by
  in-flight frames, so whoever overwrites it must drain the GPU first
  (`Texture::Upload` drains via `ExecuteImmediate`; `Texture::RenderTarget`
  calls `WaitIdle` before its descriptor write).
  It is still a hard CEILING, and reaching it is an abort rather than a
  degradation — so the occupancy is now visible instead of silent:
  `SrvLive()`/`SrvHighWater()` feed an `SRV 275 / 1024 (peak 275)` gauge in the
  dev console's perf panel, crossing 75% and 90% logs a warning, and the
  exhaustion assert quotes the peak so the message reads as "something is
  leaking" rather than "the limit is 1024". Measured: the showcase level sits
  at 275 slots, and two full quality swaps (every texture reloaded twice)
  leave live *and* peak unchanged at 275 — the recycling holds exactly.
  Removing the ceiling by GROWING the heap is deliberately not done: it needs
  index-only `SrvHandle`s first (the absolute CPU/GPU pointers handed out today
  would dangle when the heap is reallocated), and at 27% occupancy the
  measurement says that work has not earned itself yet.
- **Per-frame containers — retained capacity.** Containers rebuilt every frame
  (light list, sprite batch vertices, animator pose/palette buffers) are
  long-lived members that are cleared, never destroyed, and reserved up front,
  which makes them de-facto pools of their elements.
- **Strings.** HUD label text is reformatted only when the underlying value
  changes, never per frame.
- **Load-time data** (mesh/image/clip vectors, D3D resource creation, the
  one-shot `ExecuteImmediate` upload path) deliberately uses plain ownership —
  it runs once at startup, where clarity beats allocator ceremony. C-API
  boundaries (cgltf, `FILE*`, shell COM) ride RAII wrappers so even an
  exception mid-parse can't leak. Plain does not mean unmeasured: `LoadQueue`
  times and counts every staged task and dumps a table to `dungeon.log` when
  the last one lands (`loadstats` reprints it). The showcase level's load is
  22 tasks, ~223k allocations, 2.1 GB requested, 706 MB peak working set — and
  **88% of those allocations are one task** (monsters + items + buttons), which
  is where to look first if load time ever becomes the complaint.
- **In-flight frame safety.** With `kFrameCount` = 3, up to two prior frames'
  GPU work may still reference a resource; every destroy-or-replace path
  (quality swap, level load, chunk edit rebuild, undo restore, font atlas
  swap, editor preview-mesh reset) calls `WaitIdle` first, and all run from
  `Update`, before the frame's command list opens.

### Checking the rule

`Core/AllocTrack` replaces the global `::operator new`/`delete` family and
counts allocations into a per-thread, constant-initialized slot — no lock, no
allocation, nothing to re-enter. On in Debug; `-DDN_TRACK_ALLOCS=ON` puts it in
a Release build for a measurement run at real speed. It sees our containers (one
statically linked exe, so `std` allocations route through it) and deliberately
not raw `malloc`/`HeapAlloc` or anything a DLL allocates inside itself (D3D12,
DXGI, XAudio2, PDH) — driver allocations are not ours to remove.

Around that, a frame guard: `Main` brackets the whole frame, `Game::Update` arms
it when the game is simply playing (no load, console, overlay or deferred
rebuild, and has been so for 120 frames), and a violating frame gets its call
stacks symbolized through DbgHelp into `dungeon.log`, each unique stack once per
session. `alloctest [seconds]` measures a window of armed frames and prints one
machine-readable verdict line; `tools\AllocTest.ps1` drives the whole run and
exits non-zero on failure. `allocguard` shows the running stats and per-thread
totals; `allocguard strict on` turns a violation into an assert (off by default
— an abort in a debug build leaves a CRT dialog and a process that looks alive).

Three per-frame allocations turned up the first time it ran: a `const
std::string&` bound to a ternary whose other arm was `""` (so it bound to a
*copy*) in `ui::DropDown::DrawSelf`, a per-sample buffer in
`PerfMonitor::SampleGpu`, and `DungeonWorld::PickClip` returning `std::string`
by value. After those, 21,338 armed frames with the party idle allocate nothing,
and the AI workers total 8–50 allocations for a whole session.

Two boundaries worth stating, because they are policy and not oversight:

- **Event frames are not steady frames.** A bump message walks `loc::Tr` and
  `MessageLog::AddLine`, both of which build strings. That is allocation
  proportional to *events*, not to frames, and the rule is about the latter. It
  is measured and reported but not asserted on — and deliberately not wrapped in
  an `alloc::Excused` scope, since that would equally hide the bug where
  something starts logging every frame.
- **A test that cannot fail proves nothing.** `allocpoke` allocates every frame
  on purpose and `AllocTest.ps1 -SelfTest` inverts the expected verdict, so the
  harness must catch a real violation to pass.

Not done: the counter covers the main thread's frame and each worker's totals,
but a worker TICK is not individually guarded, and nothing runs this in CI (the
test drives a real window).

## Asset pipeline

All binary assets (PNG textures with normal/height companions, WAV sounds,
glTF block/prop/monster models) live under `assets/` and are produced offline
by `tools/AssetBaker`. The game never generates assets at runtime; the engine
loads them through the Assets module (cgltf / stb_image / dr_wav). The
renderer applies bump + parallax mapping from the `_n` maps' normal (RGB) and
height (A) channels using a derivative-based tangent frame, so meshes carry no
tangent attributes.

## Build

`build.cmd [debug|release]` — uses the Visual Studio 2026 bundled CMake + Ninja.
Outputs land in `build/<config>/bin/Dungeon.exe`. Assets are referenced
relative to the executable via a copied `assets/` directory.
`gen-vs.cmd` produces `build/vs/Dungeon.slnx` for Visual Studio work.
