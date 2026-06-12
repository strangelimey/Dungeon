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
to activate. "Start New Game" begins a run; "Settings" selects the quality
tier (Low/Medium/High/Ultra — mesh tessellation plus texture resolution:
1K, 1K, 2K, 4K — persisted to settings.ini, applied without restart). The
Ultra 4K texture sets are fetchable content: run
`tools\FetchUltraTextures.ps1` once to install them (the game falls back to
2K otherwise). Continue/Load/Save are placeholders for now.

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
| `assets/` | shaders, maps, glTF models, PNG textures (+`_n` normal/height maps), WAV sounds |
| `tools/AssetBaker` | offline generator/importer for everything under `assets/` |

## Asset pipeline

The game loads only files from `assets/` — nothing is generated at runtime.
Levels are plain ASCII text under `assets/maps/` (see `level1.map` for the
glyph legend) — edit the file and relaunch, no rebuild needed.

Textures come in albedo + `_n` pairs (`_n` holds the tangent-space normal in
RGB and a height field in alpha, used for bump and parallax mapping). Dungeon
cells are instanced from block models in two sets: the `*_block_worn.gltf`
set (tessellated, displaced geometry — crumbling bricks, sunken slabs) used
by the current dungeon at three complexity tiers, and the clean
`*_block.gltf` set reserved for newer, well-kept areas. Monsters
(`skeleton`, `mummy`, `blob`) are skinned glTF models with an `idle` clip.
To regenerate everything procedural:

```
build\debug\bin\AssetBaker.exe assets
```

After changing any texture, regenerate the derived .dds mip chains with
`AssetBaker.exe mips assets` (the import mode does this automatically for its
own outputs) — every level is BC7 block-compressed (quarter the VRAM and
bandwidth of RGBA8) and the game loads the pre-mipped DDS when present,
falling back to PNG with runtime mip filtering otherwise. Use the release
build of AssetBaker for mip baking; the BC7 encoder is compute-heavy.

Hand-authored replacements work too — drop in any glTF 2.0 file with the same
name and clip names.

### Importing downloaded PBR texture sets

Real scanned materials (Poly Haven, ambientCG, Megascans/Fab, ...) can be
packed into the engine format with the import mode:

```
AssetBaker.exe import <downloaded-folder> assets <output-name> [--flip-green]
```

It finds the albedo/normal/height/AO maps by filename convention, multiplies
AO into the albedo, flips OpenGL-style normals to the DirectX convention
(automatic when the filename says GL, forced with --flip-green), and packs
height into the normal map's alpha for parallax. Use an existing material
name (e.g. `wall_brick_2k`) as the output to replace a material, or a new
name and add it to a texture set in Game::LoadSurfaces. The shipped material
sources are listed in `assets/textures/SOURCES.txt`.
