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
  touches the heap or creates D3D12 resources. Each arena is a FIXED size.
  Where the use is bounded by construction (the renderer's constants, the
  particle batch) running out asserts; the UI's is the exception, because its
  use follows the content - the editor map draws a quad per square. Its arena
  (`SpriteBatch::kArenaBytes`, 8 MB) is sized for the map of the largest level
  the generator makes (`generate::kMaxSide`, 128) at fit zoom, which MapView.cpp
  static_asserts, and a frame past it - a level resized larger - DROPS the
  batch that does not fit and counts it (`TryAllocate`; the dev console's UI
  gauge beside SRV, and `sprites`) rather than aborting. EditorTest phase 19
  checks both halves (code-review C163).
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
  the last one lands (`loadstats` reprints it), and `assets::LoadGltf` reports
  allocations and bytes per model on its own log line. When first measured,
  the showcase level's load was 22 tasks, ~41k allocations and 2.0 GB
  requested in Release (~129k in Debug), 680 MB peak working set, and 80% of
  it was four rigged skeletons: 40 Mixamo clips x 99 animation channels, each
  channel then owning a `times` and a `values` buffer. That layout is gone - a
  clip now holds two POOLED arrays (`AnimationClipData::times` / `values`) and
  a channel is two ranges into them - which took a rigged model's Debug load
  from ~24k allocations to under 900 (skel_warrior 24,351 -> 875,
  2026-09-28).
  Two things found while measuring, both worth remembering:
  - **Debug allocation counts are not release allocation counts.** MSVC's
    iterator debugging allocates a proxy for every `std::vector` and
    `std::string` it constructs, in the MOVE constructor too. That constructor
    is still `noexcept` (the proxy is allocated inside a noexcept body), so a
    growing vector of vectors moves its elements in Debug exactly as in
    Release; the proxies are the gap, and intrinsic to the debug CRT. (This
    note used to say the move was not noexcept, so growth copied - both
    installed toolsets declare it noexcept.) Reserving the clip vectors
    (`LoadGltf`) still pays in Debug, since a regrow moves every element and
    every move costs a proxy, and a `static_assert` on
    `std::is_nothrow_move_constructible_v<AnimationClipData>` beside that
    reserve keeps growth a move. Time is inflated in Debug too, but by a
    different factor - compare like with like.
  - The dagger models load **twice**, once as `weapons.cat` items and once as
    `decorations.cat` props, because each catalog caches kinds separately. It
    is a few hundred allocations and a few MB, so it stays on the list rather
    than in the code, but a shared model cache across catalogs would close it.
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
it when the game is simply playing (no load, console, overlay, eval script or
deferred rebuild, and has been so for 120 frames), and a violating frame gets its call
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

Four boundaries worth stating, because they are policy and not oversight:

- **Event frames ARE steady frames.** This list used to say the opposite: a
  bump message allocated (`loc::Tr` returned a copy, `MessageLog` kept a string
  per line) and that was written up as "allocation proportional to events, not
  frames". It was a rationalisation of a defect, retired 2026-08-18
  (docs/message-allocation.md): printing a message now allocates nothing, so an
  allocation in a settled frame is a bug whatever caused it. The one policy
  left is that anything REPORTING from inside a guarded frame excuses itself.
- **A running eval script is a console session.** The runner executes one
  console line per frame, and a typed command only ever runs with the console
  open, which the guard never arms. A scripted line used to be held to the rule
  its typed twin is exempt from, and only once a batch had run 120 quiet lines -
  so `Eval.ps1` reported the runner's own parsing and printing as violations in
  a long batch and never for a suite run alone. `Game::EvalRunning` (true from
  load until the batch finishes, the verdict frames included) keeps those frames
  unarmed. The eval simulation is not checked incidentally in their place: a
  `step` runs thousands of ticks inside one frame, which is no steady-state
  frame either. Simulation event paths are checked where the frame really is
  steady - `AllocTest.ps1 -Wounded / -Melee / -Cast`.
- **A frame that LEAVES the guarded states is a transition.** The guard is armed
  at the top of `Update` on the state at that instant, so the frame Esc is
  pressed in starts as Playing and ends as Paused - having rebuilt the pause
  menu (a widget tree, plus `ListSaves` parsing every save for its Load entry),
  which then draws in the same frame's Render. That was reported as ~5000
  allocations on every Esc (2026-09-28). `Game::Update` now disarms any frame
  that ends outside `GuardedState()` (Playing, or the character sheet over a
  level), after every early return, so no transition site - Esc, a stair load, a
  party wipe - has to remember to. It is the overlay rule
  (`OverlayOpenedThisFrame`) one level up. The destination's frames were never
  armed, so this excuses exactly one frame per transition; `AllocTest.ps1
  -Pause` presses Esc inside the window and refuses a PASS unless the verdict
  counts a transition (`transitions=`). Opening the SHEET is not a transition
  out: it is a guarded state, and its opening frame stays checked.
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
