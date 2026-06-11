# Dungeon — Architecture

## Overview

Dungeon is a grid-based dungeon crawler (in the tradition of Dungeon Master and
Legend of Grimrock) built in C++20 on DirectX 12. The codebase is split into
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
| Core      | Logging, assertions, math (DirectXMath wrappers), timing, event dispatch | — |
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
  → Renderer::BeginFrame
  → Game::Render             (3D pass: dungeon, props; lights from torches)
  → UI::Context::Render      (2D pass: HUD, message log, controls)
  → Renderer::EndFrame       (present)
```

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
