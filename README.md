# Dungeon

An old-school grid-based dungeon crawler (Dungeon Master / Legend of Grimrock
style) with modern rendering — C++23, DirectX 12, dynamic lighting, skeletal
animation, and XAudio2 sound.

## Building

Requires Visual Studio 2026 (or 2022) with the C++ workload and Windows SDK.

```
build.cmd            # debug build
build.cmd release    # optimized build
```

Run `build\debug\bin\Dungeon.exe`.

### Visual Studio

```
gen-vs.cmd           # generates build\vs\Dungeon.slnx
```

Open `build\vs\Dungeon.slnx` in Visual Studio — the Dungeon executable is the
startup project, so F5 builds and runs the game. Regenerate after adding or
removing source files (or just build; the solution refreshes itself through
ZERO_CHECK).

## Controls

Landing page: mouse hover or arrow keys / W/S to select; click or Enter/Space
to activate. "Start New Game" begins a run (the other entries are
placeholders for now).

In game:

- **W / S** — step forward / back
- **A / D** — strafe left / right
- **Q / E** — turn left / right
- **Mouse** — interact with the HUD

## Layout

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the module diagram and
dependency rules.

| Path | Contents |
|------|----------|
| `src/Core` … `src/Main` | engine + game modules (one static lib each) |
| `external/` | vendored single-header libraries (cgltf, stb, dr_wav) |
| `assets/` | shaders, glTF models, PNG textures (+`_n` normal/height maps), WAV sounds |
| `tools/AssetBaker` | offline generator for everything under `assets/` |

## Asset pipeline

The game loads only files from `assets/` — nothing is generated at runtime.
Textures come in albedo + `_n` pairs (`_n` holds the tangent-space normal in
RGB and a height field in alpha, used for bump and parallax mapping). Dungeon
cells are instanced from `wall_block.gltf` / `floor_block.gltf` /
`ceiling_block.gltf`; monsters (`skeleton`, `mummy`, `blob`) are skinned glTF
models with an `idle` clip. To regenerate everything:

```
build\debug\bin\AssetBaker.exe assets
```

Hand-authored replacements work too — drop in any glTF 2.0 file with the same
name and clip names.
