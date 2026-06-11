# Dungeon

An old-school grid-based dungeon crawler (Dungeon Master / Legend of Grimrock
style) with modern rendering — C++20, DirectX 12, dynamic lighting, skeletal
animation, and XAudio2 sound.

## Building

Requires Visual Studio 2026 (or 2022) with the C++ workload and Windows SDK.

```
build.cmd            # debug build
build.cmd release    # optimized build
```

Run `build\debug\bin\Dungeon.exe`.

## Controls

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
| `assets/` | shaders, models, textures, sounds, fonts |
