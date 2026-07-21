# Wall details — editor-authored wall geometry

Branch `editor-wall-updates`. Goal: let the editor specify per-wall geometry
detail — worn vs flat, columns vs no columns, and (later) alcoves / niches /
see-through openings — instead of every wall of a texture looking identical and
baked-in.

## Background: how a wall is built today

Wall geometry is **not** per-solid-cell cubes. `DungeonMeshBuilder::StampCell`
walks every *walkable* cell and, for each of its 4 edges that borders solid
rock, stamps one **wall panel** (authored facing +Z, rotated to face the room)
into a combined per-variant vertex buffer. A solid cell contributes no geometry
of its own; a shared wall gets two panels (one per adjacent room). All panels
merge into per-variant, per-chunk meshes (`kChunkCells = 4`) so the renderer
frustum- / sphere-culls regions, not objects.

Each panel is a **displaced height-map grid**, baked offline by AssetBaker into
`worn_<texture>_<tier>.gltf` (one mesh per surface texture, three tessellation
tiers low/med/high). `BuildWornWallBlock` emits a `(kNx+1)·(kNy+1)` grid
displaced by the texture's scanned height map, plus a fixed 32-vert set of edge
"pillars" (`AddWallPillars` — the border strips + convex-corner seals). Both the
worn displacement (grid) and the pillars (columns) are **hardcoded**:
`AddWallPillars` is always called; the wear field always comes from the height
map. Those are exactly the two knobs Phase 1 makes editable.

The worn mesh is keyed by **texture set**, and a level's palette maps catalog
ids → textures (`ResolveSurfacePalettes` → `m_wallSets`), then
`LoadDungeonBlocks` loads `worn_<texture>_<MeshSuffix()>.gltf` per variant.

## The two axes

The requested features split into two mechanisms:

- **Style** (worn/flat, columns/no-columns, later tessellation): a property of a
  wall *kind*. Lives as catalog fields on the `walls.cat` entry, baked into that
  texture's worn mesh set. Fits the existing bake-on-save seam (the asset dialog
  already persists moved fields and re-bakes a set). **Phase 1.**
- **Features** (alcove/niche, window/passage): per wall *segment*. A per-edge
  record like a wall-mounted decoration. **Phases 2–3.**

## Phase 1 — wall style as catalog params (per type)  ← THIS BRANCH

### Data model

New optional fields on a `walls.cat` entry (defaults preserve today's look):

| field     | type  | default | meaning                                            |
|-----------|-------|---------|----------------------------------------------------|
| `wear`    | float | `1.0`   | displacement multiplier; `0` = flat panel          |
| `columns` | bool  | `1`     | the edge pillars / border strips                   |

Hand-authorable, and set by the editor's Wall Style dialog. Because the worn
mesh is per-texture, **style is effectively per-texture-set** — two catalog
entries sharing one `texture` share a baked mesh (in practice entries are 1:1
with textures). Per-id styled meshes (`worn_<id>_*` with a per-texture fallback)
are a later option if two styles on one texture are ever needed.

**Wear also drives parallax depth.** The mesh displacement is only half the
relief — the shader independently fakes per-pixel relief via normal + steep
parallax mapping from the texture's height map, scaled by the surface's
`height_scale`. So `wear` is folded into the parallax depth too: the effective
depth is `height_scale × wear` (0 at `wear = 0`), and a flat wall type reads
genuinely flat instead of geometrically-flat-but-still-bumpy. This required
making parallax depth **per texture variant** (`Surface::heightScale` is now a
`std::vector<float>` parallel to the albedo array; `ResolveSurfacePalettes`
fills `height_scale × wear` per palette entry; `DrawSurface` indexes it by the
chunk's variant). Columns has no parallax effect.

### Bake pipeline (AssetBaker)

- `BuildWornWallBlock(kNx, kNy, wear, columns)` — gate `AddWallPillars` on
  `columns`.
- `BakeWornTiers(kind, texture, relief, seed, dirs…, wearScale, columns)` —
  scale `relief` by `wearScale`; when `wearScale == 0` bake walls as a flat 1×1
  panel (no displacement grid) so a flat wall is ~4 verts + optional pillars, not
  a dense flat grid. Floors/ceilings scale wear too (columns N/A).
- `BakeWornBlocks(kind, name, assetsDir, wearScale = 1, columns = true)` — thread
  the style through; defaults keep the current full-bake output.
- CLI: `AssetBaker wornblock <kind> <name> <assets> [--wear <f>] [--columns 0|1]`.
- The full bake (`AssetBaker <assets>`, `BakeModels`) is project-agnostic and
  bakes **default** worn blocks (wear=1, columns=1). Catalog styles are the
  source of truth and are (re)applied by the editor's per-set rebake — same
  lifecycle as imported texture sets (a full bake / fresh clone regenerates
  defaults; the editor re-applies).

### Editor UI

- Right-clicking a **Walls** palette row fires `onConfigure(Walls, id)` (the same
  hook Monsters use), which opens a small **WallStyleDialog** (modelled on
  `LevelSettingsDialog`): a `wear` slider (0 = flat … 1 = worn) + a `columns`
  checkbox + Save / Close.
- Geometry detail is baked, so there is **no live preview** — **Save** commits:
  1. `WriteWallStyle(id, wear, columns)` writes the catalog fields + `Project::Save`
     (synchronous, the `WriteMonsterAnim` pattern).
  2. Kicks a `wornblock` rebake of that texture with `--wear/--columns`
     (reusing the async `m_bake` subprocess flow; `m_restyleBake` distinguishes
     the completion from an asset-create).
  3. On success, `DungeonWorld::ReloadDungeonBlocks()` (the `ApplyQuality(false)`
     reload core: WaitIdle → clear chunks → `LoadDungeonBlocks` →
     `BuildDungeonMeshes` → invalidate shadow cubes) swaps the mesh in live.

### Touched files

- `tools/AssetBaker/ModelBaker.{h,cpp}` — bake params + flat path.
- `tools/AssetBaker/Main.cpp` — `wornblock` CLI flags.
- `src/Game/WallStyleDialog.{h,cpp}` — new dialog (+ `CMakeLists.txt`).
- `src/Game/MapEditor.cpp` — `OnRightClick` fires `onConfigure` for Walls rows.
- `src/Game/Game.{h,cpp}` — dialog member, wiring, `WriteWallStyle`, restyle bake.
- `src/Game/DungeonWorld.{h,*}` — public `ReloadDungeonBlocks()`.
- `assets/projects/dungeon-demo/catalog/walls.cat` — document the new fields.

### Out of scope for Phase 1

- Per-segment styling (any single wall face styled without a new type).
- Per-type tessellation override (tessellation stays quality-driven; all three
  tiers are always baked and the game picks by quality).
- Floors/ceilings UI (the bake honours `wear` for them; only walls get a dialog).

## Phase 2 — recessed niche (per segment)  ← BUILT

A recessed pocket carved into one wall of a walkable cell. NOTE: the original
"discrete mesh in front of the panel, no stamping change" plan does NOT give a
real recess — the opaque base panel spans the whole cell face and occludes any
pocket behind it. The niche is instead an **alternate wall panel**: the mesh
builder stamps the `wall_niche` mesh (a flat frame around a ~55 cm pocket, +Z
authored like every wall block) in place of the plain worn panel on that edge,
into the SAME variant bucket — so it keeps the wall's texture and rides the
per-chunk rebuild. This pulls Phase 3's structural machinery forward.

- Storage lives on `DungeonMap` (like `WallSconce`): `struct WallNiche {x,z,wall,
  type}`, `m_niches`, `AddNiche` (first free solid wall — the sconce mount rule),
  `RemoveNicheAt`, `HasNiche(x,z,dx,dz)` (the mesh builder's per-edge query),
  pruned by `PruneFixturesForCell` when its wall is opened/buried.
- Mesh builder: `BuildDungeonGeometry/Region/StampCell` take a `const MeshData*
  niche`; the edge loop stamps it instead of `wallBlocks[variant]` when
  `map.HasNiche`. `DungeonWorld::m_nicheMesh` (wall_niche.gltf) is passed through;
  add/erase call `RebuildChunksAround`.
- `.map` record `niche <x> <z> <dir>`; parsed tolerantly (dropped if not facing
  solid). Editor: a "Wall Features" palette category (wallfeatures.cat `[niche]`),
  placement on a wall edge, middle-click erase; live + remote (stash) seams.
- The mesh: `BuildWallNiche` in ModelBaker (frame border quads + pocket back +
  four reveals + shared edge pillars), baked to `wall_niche.gltf`.

Remaining niche work (follow-ons):
- **Multiple niche types** (e.g. an arched niche with a keystone): make the mesh
  per catalog `model` (type→mesh map + a resolver in the builder; the record
  carries the type). Placement/save already carry `type`.
- **Item in a niche**: a niche-relative mount for a placed item/decoration.
- **Secret/revealed niche**: a named, hidden-by-default niche wired to a button
  (the door pattern — `ToggleNichesNamed`, a dynamic revealed flag in the save).

## Phase 3 — real opening (window / passage)

Structural. A per-edge feature grid (a `.map` record like `variant`), checked in
`StampCell`: an opening edge stamps a framed-aperture panel instead of the plain
worn panel. The cell stays solid (blocks movement like a barred window) but LoS +
projectiles get a per-edge "transparent" exception — consistent with the
orthogonal-grid rule (sight/shots pass along that one cardinal). A full passage
is doorway-adjacent (doors already fill a doorway cell and toggle blocking).
Chunk-local rebuild handles the live edit; the feature grid round-trips through
the `.map` writer.
