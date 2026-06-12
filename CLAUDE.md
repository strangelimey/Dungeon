# Dungeon — project context for Claude

Old-school grid dungeon crawler (Grimrock / Dungeon Master style), C++23 +
DirectX 12, owned by Michael (GitHub: strangelimey/Dungeon, private repo).
Built collaboratively with Claude across sessions; this file is the handoff.

## Build & run

- `build.cmd [debug|release]` — VS 2026 Community's bundled CMake + Ninja
  (plain `cmake` is NOT on PATH; the script sets up vcvars64). The
  "'vswhere.exe' is not recognized" warning it prints is harmless.
- Output: `build\<config>\bin\Dungeon.exe`. Assets copy next to the exe on
  link (post-build); after changing assets without code, sync manually:
  `robocopy assets build\debug\bin\assets /MIR` (MIR also removes stale).
- `gen-vs.cmd` → `build\vs\Dungeon.slnx` (VS 2026 emits .slnx, not .sln).
- Debug builds open a console for logs; DN_ASSERT failures abort() — in
  debug that means a CRT dialog and the process LOOKS alive but is stuck.

## Architecture (docs/ARCHITECTURE.md has the full version)

Nine strictly layered static libs, one-way deps:
Core → Platform/Assets → Animation/Graphics → UI/Audio → Game → Main(exe).
Key conventions (memorize, they bite):
- DirectXMath ROW-vector convention: v' = v*M, translation in row 4
  (_41.._43); matrices uploaded raw; HLSL always uses mul(matrix, vector).
  glTF column-major memcpy is CORRECT under this pairing (same bytes).
- Left-handed, +Y up, camera forward = (sin yaw, 0, cos yaw). Facing index
  +1 is on-screen LEFT (see Party.cpp comment — controls were once reversed
  because of this).
- All indentation is TABS (see .editorconfig). Comments use file banners +
  section dividers; keep that style.
- Per-frame GPU transients come from UploadAllocator arenas (one per frame
  in flight, kFrameCount=3); steady-state frames allocate nothing on the
  heap (docs/ARCHITECTURE.md "Memory strategy").
- Constants that must match HLSL: kMaxPointLights=16, kMaxSkinJoints=128,
  root signature layout in Renderer.h header comment.

## Renderer features (assets/shaders/scene.hlsl)

Forward pass: Blinn-Phong with per-material specular (MaterialParams),
normal + steep-parallax mapping (derivative cotangent frame, height in
normal-map alpha), per-cell volumetric dust (turbidity grid texture t2,
raymarched extinction + in-scattering), point-light cube shadows with
distance-graded slots (4 slots, 512/256/256/128, slot 0 = nearest light,
PCF; carried torch always wins slot 0; dust march samples the same cubes →
god rays), fire light positions wander so shadows flicker. Shaders compile
at launch with an on-disk cache (shadercache/, hash-invalidated) — edit
.hlsl and relaunch, no rebuild.

## Asset pipeline (everything loads from assets/, nothing generated at runtime)

- `AssetBaker <assets>` — regenerates all procedural assets (block models
  incl. worn tiers, monsters, sconce/brazier, pillar, sounds, title art)
  and ends with a mip bake.
- `AssetBaker import <folder> <assets> <name> [--flip-green]` — packs a
  downloaded PBR set (auto-detects maps by filename; flips GL normals;
  AO→albedo; height→normal alpha; bakes BC7 DDS).
- `AssetBaker mips <assets>` — rebakes derived .dds (BC7 mode-6 encoder in
  tools/AssetBaker/Bc7Encoder.cpp; use the RELEASE baker, encode is slow).
- `AssetBaker models <assets>` — rebakes only the .gltf models (fast). Worn
  blocks sample the installed texture height maps, so rerun after
  FetchTextures.ps1 or a texture import.
- Textures: PNG = source, .dds = derived BC7 mip chains (gitignored).
  Scanned sets (7 Poly Haven CC0 materials × 1k/2k/4k) are NOT in git:
  raw downloads live in OneDrive\DungeonAssets\<res>\<material>\ and
  `tools\FetchTextures.ps1 [-Resolutions 1k,2k,4k]` imports them. A full
  pre-history-rewrite git bundle also lives there.
- Maps are two files per level, split static vs dynamic for the future
  save system (saves will only ever store the dynamic side):
  - assets/maps/level1.map — STATIC layer (DungeonMap): ASCII grid, ';'
    comments, glyphs '#' rock '.' floor 'D' dusty 'T' sconce 'F' brazier
    (blocks movement) 'P' start. Lines starting lowercase are decoration
    records (grid glyphs are never lowercase).
  - assets/maps/level1.ent — DYNAMIC layer (DungeonEntities): monsters,
    items, buttons; one record per line, `<kind> <type> <x> <z> [facing]
    [key=value ...]` (Entity.h). Monster type → model: <type>.gltf.
    Records validate against the map at load (bounds, walkability,
    buttons face a wall). Edit + relaunch, no rebuild.
- Worn block meshes are baked PER SURFACE TEXTURE at 3 tiers
  (worn_<texture>_<low|med|high>.gltf), displaced by that texture's scanned
  height map (normal-map alpha) so geometric relief matches the painted
  bricks/slabs; DungeonMeshBuilder stamps the mesh matching each cell's
  texture variant. wall_stone's Poly Haven displacement export is flat
  (detected at import), so it uses procedural wear — its 0.5x0.31 block
  grid happens to fit that texture's large blocks anyway.

## Quality system

Settings page (landing page) is tabbed Game/Video/Audio via ui::TabControl:
quality dropdown on Video (Low/Medium/High/Ultra: mesh tier low/med/high/high
+ textures 1k/1k/2k/4k), master-volume slider on Audio, Game empty so far.
Quality persists to settings.ini next to exe (quality=0..3); hot-swaps in
place (WaitIdle + rebuild). Ultra falls back per-material to 2k with a
warning if 4k not installed.

## Game state machine

Loading (staged tasks, one per frame, progress screen) → Menu (baked title
art title_bg, MenuList: Continue/Start New Game/Load/Save/Settings — only
Start New Game and Settings work) → Playing. Esc quits the app. Monsters
block movement (no combat yet); fires are sconces at 'T' (wall-mounted,
light at flame) and braziers at 'F', each with FireEffect particles
(flame/spark/smoke via gfx::ParticleBatch premultiplied billboards) and
fire-driven turbidity rings around them.

## Workflow conventions used so far

- Verify changes by launching the exe and driving it with PostMessage
  keystrokes + CopyFromScreen screenshots into docs/ (see git history for
  the PowerShell pattern). Menu nav: Down/Enter; allow ~10s+ load on
  High/Ultra cold cache before sending keys.
- Commit per feature with detailed messages; push to origin/main. Long
  commit messages via a temp file + `git commit -F` (PowerShell mangles
  embedded quotes).
- NEVER rewrite UTF-8 files via PowerShell Get-Content/Set-Content — it
  mojibakes em-dashes (happened twice). Use the Write/Edit tools.
- User prefs: concise replies, no emojis; permission prompts disabled.

## Known gaps / natural next steps

- Continue/Load/Save menu entries are inert (save system not started).
- No combat: monsters are static blockers that announce + face the party.
- Monster models are box-rigs; authored glTF would drop in via LoadModel
  (JOINTS_0 remap already handled).
- BC7 encoder is mode-6 only (slight banding possible on smooth gradients).
- HUD layout is fixed to the startup window size; no resize reflow.
- The clean (non-worn) block set is baked but unused — intended for newer
  dungeon areas, needs per-region block-set selection in DungeonMeshBuilder.
- Roughness maps from imports are ignored (specular is per-material).
