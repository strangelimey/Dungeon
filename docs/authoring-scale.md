# Authoring scale — how to build assets for this dungeon

**One rule: everything you build is measured in SQUARES, not metres.**
A dungeon square is 1.0 wide, 1.0 deep and 1.0 tall — it is a cube. A column
you build 1.0 tall touches the ceiling exactly. A barrel 0.36 tall comes up to
about mid-thigh.

How many *metres* a square happens to be is one number in the engine,
`kUnit` in `src/Game/DungeonMap.h` (currently 2.5 m). Nothing on disk knows it.
Change it and rebuild, and the whole world — walls, props, monsters, items,
fires, doors, particles — rescales together with **no asset rebake**. That is
the entire point of this convention: it stops every model you author from
silently baking today's metre size into its vertices.

To convert a real-world size into units: **divide by 2.5**.
A 1.7 m skeleton is 0.68. A 20 cm lever handle is 0.08.

---

## The reference card

Keep these beside you while modelling. All values in units.

| What | Value |
| --- | --- |
| Cell footprint | 1.0 × 1.0 |
| Floor to ceiling | 1.0 |
| Party eye height | 0.62 |
| Wall block extents | x, z ∈ [−0.5, +0.5], y ∈ [0, 1] |
| Wall pillar protrusion (per side) | 0.034 |
| Doorway clear opening | 0.68 wide × 0.84 tall |
| Archway opening | 0.50 wide, springline at 0.62 |
| Niche pocket (plain) | floor 0.30, top 0.72, depth 0.22 |
| Niche pocket (arched) | floor 0.20, springline 0.50, radius 0.21 |
| Lever / button pivot height | 0.46 |
| Sconce flame origin | y 0.68, 0.064 out from the wall |
| Brazier flame origin | y 0.296 |
| Stair step | rise 0.068, run 0.096 |
| A humanoid monster | ~0.68 tall |
| A dagger | ~0.20 long |
| A floor rune tablet | ~0.07 |

---

## Orientation and origin

The engine is **left-handed, +Y up**. Blender's glTF exporter handles the
handedness; you only have to get the facing and the origin right.

- **+Z is the front.** Every prop is authored facing +Z, which is the default
  `Direction::South`. Placement rotates it by the record's facing.
- **Floor props** (column, barrel, fountain, plinth): origin at the centre of
  the cell floor. `min y = 0` — the model rests ON the floor plane, it does not
  straddle it. Centre it in X and Z.
- **Wall props** (arch panel, sconce, banner, lever): the back face sits at
  **z = 0** and the model reaches into the room along **+Z**. The engine pushes
  the origin out to the wall face and turns it to look into the room
  (`DungeonWorld::MountOnWall`), so the mesh itself carries its hanging height —
  a sconce is authored high up at y ≈ 0.7, not at y = 0.
- **Cell-spanning props** (an arch that must meet the flanking walls) run the
  full ±0.5 in X and the full 0..1 in Y. Do not leave a margin, or you will see
  a seam.

---

## Blender setup

1. **Scene Properties → Units → Unit Scale = 1.0**, Unit System = Metric,
   Length = Metres. Then treat **1 Blender metre = 1 dungeon square**. Do not
   change the unit scale — the glTF exporter writes Blender units straight
   through, and 1.0 in equals 1.0 out.
2. **Viewport Overlays → Guides → Scale = 1.0, Subdivisions = 10.** The default
   grid square is now one dungeon square and each subdivision is 0.1 — so you
   can eyeball tenths of a cell without measuring.
3. Add a **reference cube**: 1×1×1, sitting on the origin with its base at
   z = 0 (Blender Z-up), scaled so it spans −0.5..+0.5 horizontally. Everything
   you build lives inside or against that cube. Keep it on its own collection
   and exclude it from export.
4. Model with the **front of the object facing −Y in Blender** — Blender's −Y
   becomes the engine's +Z after the exporter's axis conversion. If a prop comes
   in backwards, that is the fix (or pass `--yaw 180` at import).

### Export

File → Export → glTF 2.0 (.glb/.gltf):

- **Format**: glTF Separate or Embedded (`.gltf`) for static props.
- **Include**: Selected Objects (leave the reference cube out).
- **Transform**: +Y Up — on (the default).
- **Data → Mesh**: Apply Modifiers on. Normals on. UVs on.
- **Data → Material**: Export materials — the engine ignores the glTF material
  for single-material props (it binds a PBR set by name), so this only matters
  for multi-material models.

Apply all transforms in Blender first (`Ctrl+A → All Transforms`) so the object
has no leftover object-level scale.

---

## Getting it into the game

Because you have already authored in unit space, use `--raw` — it trusts your
placement exactly and does no re-fitting, grounding or centring:

```bash
build/debug/bin/AssetBaker.exe import-model path/to/plinth.glb assets plinth --raw
```

If the folder beside the model holds PBR maps (albedo / normal / roughness /
AO / metallic), they are imported as the texture set `plinth_2k` automatically.
To share an already-imported set instead, add `--texture-set <name>`.

Without `--raw` the importer NORMALIZES for you — useful for a bought model
whose scale is arbitrary. `--height` and `--lift` are then **in units**:

```bash
AssetBaker.exe import-model bought.glb assets statue --height 0.8
```

Then wire a catalog entry, e.g. in
`assets/projects/dungeon-demo/catalog/decorations.cat`:

```
[plinth]
display = Plinth
category = architecture
model = plinth
texture = plinth_2k
solid = 1
```

and it appears in the editor's Decorations palette, ready to place.

### If it comes out the wrong size

Every prop family reads a `scale` field — a uniform trim applied on top of the
authored size, so you can nudge without re-exporting from Blender:

```
scale = 0.9
```

It works in `decorations.cat`, `fixtures.cat`, `items.cat`, `doors.cat` and
`stairs.cat`; monsters use the older `modelscale` for the same thing. Treat it
as a correction, not a substitute for authoring at the right size.

---

## For engine work: where the scale is applied

Models on disk are unit-space; the multiplication by `kUnit` happens at the
handful of places a mesh becomes world geometry. If you add a new one, it must
go through `UnitScale()` (`src/Game/DungeonMap.h`) or the new content will be
2.5× too small:

| Site | File |
| --- | --- |
| Wall / floor / ceiling block stamping | `DungeonMeshBuilder.cpp` `StampCell` |
| Decorations (standing and wall-mounted) | `DungeonWorld_Load.cpp` `LoadDecorations`, `DungeonWorld_Editing.cpp` |
| Stair / pit props | `DungeonWorld_Load.cpp` `PlaceStairProp` |
| Sconces / braziers + flame origins | `DungeonWorld_Load.cpp` `BuildFires` |
| Doors, buttons, monsters, floor + niche items | `DungeonWorld_Render.cpp` |
| Fire particles | `FireEffect.cpp` (lengths in units × `kUnit`) |

On the tool side, `tools/AssetBaker/ModelBaker.cpp` authors the block family
directly in units (`kCellHalf = 0.5`, `kWallH = 1.0`) and converts its
metre-proportioned props and creatures at one boundary — `ScaleMeshToUnits` /
`ScaleModelToUnits`, called from `FinishProp` and the few builders that assemble
their own model. `U(metres)` and `M(units)` convert between the two in-place.
