# Editor type authoring — creating and editing content TYPES in the editor

Branch `editor-updates`. Goal: make the editor's **type** half as complete as its
**instance** half. Right-clicking a placed object already opens a proper
per-instance dialog (`InstanceInspector` + 6 concrete subclasses). Right-clicking
a palette ROW should likewise open a per-TYPE editor for every category, and the
palette's `+ New` should create a type that is immediately usable — including
picking or importing its texture set and tuning its material.

## Where it stands

- **Type dialogs**: only Monsters (`MonsterConfigDialog`) and Walls/Floors/
  Ceilings (`WallStyleDialog` — just `wear` + `columns`). Decorations, Fixtures,
  Doors, Stairs, Items, Buttons, WallFeatures right-click into nothing
  (`Game_Wiring.cpp` `onConfigure`).
- **Creation**: `AssetDialog` → `Game::StartBakeStep` (`AssetBaker import` /
  `import-model`, then `wornblock` for surfaces) → `Game::FinishBake` appends one
  hardcoded-shape catalog entry.
- **The dead end**: surface palette rows come from the *level's* `palette` record
  (`MapEditor::CategoryItems`), not the catalog. `walls.cat` has 9 entries;
  `start.map` lists 4. A newly created wall type never appears in the palette, so
  it can't be painted. `DungeonMap` exposes the three palettes as const getters
  with **no mutator**.
- **Surfaces ignore material factors**: `DrawSurface` feeds albedo/normal/ORM +
  a per-variant `heightScale` only. `metallic`/`roughness`/`color` are read for
  *props* (`DungeonWorld_Load.cpp` `ApplyPropMaterial` path), never for surfaces,
  and `FinishBake` discards them on the texture-set branch.

## The decision: schema-driven, not seven bespoke dialogs

One `TypeEditorDialog` renders its rows from a per-catalog **field schema** table
— the `kBalanceFields` / `kCategoryInfo` / `kThemeFields` idiom the project
already uses everywhere. Catalogs are free-form key/value with the read fields
documented at the top of each `.cat`, so the schema is just that documentation
made executable. Category-specific UI (monster animation tabs, surface
wear/columns) slots in as a **custom section** hook rather than a separate
dialog. Adding a field to a category becomes one table row; adding a whole
category becomes a table.

Type edits stay OUTSIDE the editor's undo stack (`EditorSnapshot` copies levels,
not catalogs) — same as the Balance dialog and monster config today. Saving a
type is explicit; that's the model, not a gap.

---

> **Status.** Phases 1–5 landed — see the notes at the end of each. Phase 6 is
> unstarted.

## Phase 1 — Palette membership (makes everything else usable)  ← DONE

Without this, nothing created in Phases 2–3 can be painted. Do it first.

**Data model.** `variant <wall|floor|ceiling> <x> <z> <index>` records store the
palette **INDEX**, so:

- **Appending is safe.** Reordering or removing silently repaints cells.
- Phase 1 is **append-only**. Removal lands in Phase 5 behind an index remap.

**Code.**

1. `DungeonMap`: `bool AddToPalette(SurfaceSel, const std::string& id)` (no-op +
   false if already present). The `SaveLevel` writer already emits the palette
   from the map, so persistence is free.
2. `DungeonWorld`: `AddPaletteEntry(SurfaceSel, id)` for the active level →
   `ReloadDungeonBlocks(/*textureResChanged*/ true)` (re-resolves palettes,
   reloads worn meshes + textures, rebuilds chunks, invalidates shadow cubes).
   Plus `AddPaletteEntryRemote(stem, ...)` mutating the `m_levelMaps` stash, next
   to `EditVariantRemote` — the editor edits browsed levels too.
3. `MapEditor`: the three surface categories gain an **`+ Add from catalog…`**
   row beside `+ New…`, opening a chooser listing the catalog entries NOT in the
   viewed level's palette (display name + albedo swatch, reusing
   `SurfaceAlbedoForId`'s loader for the thumbnail). Picking one appends it.
4. `+ New` (Phase 3) ends by appending the created type to the viewed level's
   palette — that is what makes creation useful.
5. Bracket the append with `BeginUndoStep`/`CommitUndoStep`. Verify
   `RestoreEditorState` re-runs `ResolveSurfacePalettes` (it rebakes geometry via
   the quality-swap path; confirm the palette re-resolve is on that path, add it
   if not) so undoing a palette add reloads the right variant arrays.

**Done when**: a wall type present in `walls.cat` but absent from `start.map` can
be added to the level from the palette, painted immediately, survives `savemap`,
and Ctrl+Z removes it again.

---

## Phase 2 — The schema + `TypeEditorDialog`  ← DONE

**New file `Game/CatalogSchema.h/.cpp`.**

```
enum class FieldKind { Bool, Float, Int, Text, Id, Enum, Color, TextureSet, Model, LocKey };
struct FieldSpec {
    const char* key;         // catalog field name
    FieldKind   kind;
    const char* labelKey;    // loc key for the row label
    const char* helpKey;     // loc key for the "?" explanation (BalanceDialog pattern)
    float lo = 0, hi = 1;    // Float/Int range
    const char* options = ""; // Enum: space-separated tokens
    const char* def = "";     // default, also the NEW-entry template value
    bool rebakes = false;     // saving this field triggers the worn rebake
};
std::span<const FieldSpec> SchemaFor(std::string_view catalogKey);
std::span<const FieldSpec> SectionsFor(...); // grouping: Identity / Appearance / Behaviour
```

One table per catalog, transcribed from each `.cat`'s header comment. `def`
doubles as the **new-entry template**, killing the current one-shape-fits-all
`FinishBake` (stairs `solid = 0` + `up`/`pair`/`hole`, doors `authored = 0`,
items `name = item.<id>` + `weight`/`holdable`, monsters hp/damage/iq).

**New `Game/TypeEditorDialog.h/.cpp`.** Modal in the house style
(`LevelSettingsDialog` chrome + `AssetDialog`'s busy overlay). Builds rows from
the schema; footer Save / Delete / Close; Esc closes. Writes through one shared
`Game::WriteTypeFields(catalogKey, id, fields)` that starts from the existing
entry so **unknown fields survive the round-trip** (the `WriteWallStyle` /
`WriteMonsterAnim` rule). Fields flagged `rebakes` route to `StartRestyleBake`.

**Custom sections.** A `virtual BuildContent()`-style hook, exactly as
`InstanceInspector` does it: Monsters embed the existing animation/behaviour tabs
(keep `MonsterConfigDialog`'s content, host it here), Surfaces get the
wear/columns knobs — **`WallStyleDialog` is then retired into a section**.

**Wiring.** `MapEditor::onConfigure` fires for every category (drop the
Monsters-only guard); `Game_Wiring` opens `TypeEditorDialog` for the category's
catalog key.

**Done when**: right-clicking any palette row in any category opens an editor
showing that category's real fields, Save round-trips the `.cat` losslessly, and
`wear` still rebakes + hot-swaps as it does today.

**As built.** All of the above, plus three things the plan didn't foresee:

- Field **labels come from the key itself** (`PrettyFieldName`), and `help` is
  English in the table. A catalog field name is project vocabulary like an ini
  key or an asset name, which CLAUDE.md keeps out of Loc — and per-field loc
  keys would have meant ~180 new strings across five languages. Only the dialog
  chrome and section names are translated.
- Sliders **snap to a per-field `step`**, because the first save wrote
  `hp = 68.9157` where the catalog wants `hp = 69`.
- **Catalog comments now survive a write.** `ParseBlocks` dropped them, so every
  catalog write (this dialog, monster config, asset create) silently deleted the
  header that documents that category's fields. They ride as
  `serialize::Block::lead` → `CatalogEntry::lead`, and `Catalog::Save` yields its
  generic header to the file's own.

`WallStyleDialog` is deleted — `wear`/`columns` are schema rows. Monster
animation/behaviour stays in `MonsterConfigDialog` (it rewrites those rows
authoritatively), reached by the type editor's "Animation..." button; the schema
omits exactly the fields it owns, so there is one writer per field.

---

## Phase 3 — Rewrite `+ New` on top of the schema  ← DONE

**Source modes** (a three-way radio at the top of the dialog):

- **Import new** — the current folder/model browse.
- **Use installed** — pick an already-imported texture set or model from the
  pool. Needs a `paths::Asset("textures")` scan collapsing `<name>_<res>.dds` to
  base names (mirror `loc::ScanLanguages`). This is what makes "a second wall
  type off cobblestone with different wear" possible; today it's impossible.
- **Duplicate existing** — copy another type's fields under a new id.

**Id validation** (`.map`/`.ent` records are whitespace-tokenised, so a space
corrupts a level): filter input to `[a-z0-9_-]` like the door Name field, reject
empty, and check `Catalog::Contains` — today `Catalog::Add` **replaces by id**,
silently overwriting a type used by every level.

**Import controls**: `--flip-green` toggle (GL vs DX normals), a pre-bake report
of which maps `DiscoverMaps` matched by filename (and a loud warning when no
height map was found — parallax bakes flat), plus the `category` field so the new
entry lands in a palette sub-accordion.

**Feedback**: today a failed bake only logs and unfreezes the dialog. Surface the
AssetBaker exit code and the tail of its output in the dialog, and post a
`world.onMessage` line on success. (`dungeon.log` next to the exe stays the full
record.)

**Preview**: `AssetDialog::Browse` returns early for texture sets, so the preview
pane is empty for exactly the categories the sliders matter for. Render a
unit cube / wall panel with the picked set through the existing `ModelPreview`
rig so roughness and height are tuned by eye.

**Done when**: creating a wall type from an installed set, with a chosen
roughness, takes one dialog, no file browsing, no restart — and the type is in
the palette and paintable when the dialog closes.

**As built.** All of the above. Notes:

- `DiscoverPbrMaps` moved **out of AssetBaker into the Assets lib**, so the
  dialog's report and the baker's import are the same code rather than two
  copies of a substring table.
- An installed texture set adopted as a *surface* still needs its worn block
  mesh, so that one case enters the bake flow at its second step. Model
  categories and Duplicate write the catalog and are done — no subprocess.
- **A second comment bug fell out of testing.** Phase 2 attached comments to the
  block they introduced, but `monsters.cat` annotates a *field* mid-entry
  (`; A single-minded brute…` above `threat_threshold`). Those floated to the top
  of the NEXT entry on save — describing the wrong monster. Comments now attach
  to fields as well as blocks (`serialize::Field::lead`), and a save is
  byte-stable apart from the intended change.
- Not done: Duplicate of a model shows no preview (the dialog would have to
  resolve the source entry's `model` field). Cheap to add later.

---

## Phase 4 — Surface material factors (what "set roughness" actually needs)  ← DONE

Engine-side, small, and independent of the UI phases.

1. `Surface` gains a per-variant factor array parallel to `heightScale`
   (metallic / roughness / tint, `-1` = "use the map").
2. `ResolveSurfacePalettes` fills it from the catalog entry, exactly as it
   already folds `height_scale × wear`.
3. `DrawSurface`'s `ApplyPbr` call passes the variant's factors instead of the
   `{}, 0.0f` placeholders.
4. Factors need **no rebake** — only `wear`/`height_scale` do — so the type
   editor can apply them live: re-resolve, reassign the arrays, done.

**Done when**: dragging Roughness in a wall type's editor changes the scene the
same frame, and the value persists to `walls.cat`.

**As built.** As planned, with the refresh hung off two choke points rather than
sprinkled: `BuildDungeonMeshes` calls `ApplySurfaceFactors` (so every load,
quality swap and undo restore is covered), and `RefreshSurfaceMaterials`
re-resolves + re-pushes for a live catalog edit. Saving a surface type now
applies `height_scale` live too — that was silently waiting for a level reload
before.

Known wart: an absent factor draws as 0.00 on its slider, indistinguishable from
an explicit 0. Only TOUCHED fields are written so nothing is corrupted, but the
form can't currently say "map-driven" or set an explicit 0 from the absent state.
A "use the map" checkbox per factor would fix both.

---

## Phase 5 — Lifecycle: duplicate / rename / delete  ← DONE

The risky ops; they need a shared **reference sweep** (the `RenameLevel` stair
`dest=` sweep is the precedent):

- level `palette` records and `variant` indices,
- `.ent` records by type (decoration/monster/item/door/button/stair),
- cross-catalog references: `pair =` (stairs), `key =` (doors), the door frame,
  `project.ini` default fixture ids.

**Rename** = sweep + rewrite. **Delete** = refuse when referenced (list the
referring levels), else remove; removing from a palette must **remap every
`variant` record's index** on that level. **Duplicate** is trivial and belongs in
Phase 3's source modes.

**As built.** The sweep is three layers, each owning what it knows:
`DungeonEntities::SweepTypeRefs` (dynamic records), `DungeonMap::SweepTypeRefs`
(the static families: palettes, decorations, fixtures, wall features, stairs),
and `DungeonWorld::SweepTypeRefs` walking every level in the project — including
levels not in memory, parsed on demand through the same lazy stash the map
overlay browses with. `Game::SweepCatalogRefs` covers the long tail outside the
levels (stairs `pair`, doors `key`, the project's default fixture ids).

Details worth remembering:

- A rename **re-spawns the live objects** from the retyped records
  (`RespawnFromRecords`, extracted from the undo restore, which had exactly this
  code) — live monsters and props hold their type by name and resolve kinds
  through a cache keyed by it.
- A rename **clears the undo history**: every held snapshot names the type by its
  old id, so restoring one after a rename would resurrect dangling references.
- `Catalog::Rename` renames **in place**. The first attempt did remove + re-add,
  which moved the entry to the end of the file — and since Phase 2 attaches lead
  comments to the entry they introduce, that dragged the file's header comment
  down with it.
- The **variant remap is not needed**: delete refuses while the type is in any
  palette, so the only way to remove a palette member is still the "not built"
  remove-from-palette op. Recorded here so the next person doesn't assume it.

Not done: no rename/delete for a type referenced ONLY by a save file — saves
store dynamic diffs keyed by record id, not type, so this is believed safe, but
it hasn't been tested against an old save.

---

## Phase 6 — Reproducibility of created assets

`SyncProjectToSource` copies only `projects/<name>`. A type created in the editor
therefore reaches git as a catalog entry whose texture/model exist **only** in
`build\<cfg>\bin\assets` — and both `assets/textures` and `assets/models` are
gitignored, so a clean clone can't rebuild the level.

Options (needs a decision — see below):

- Record **provenance** on the entry (`import_source`, `import_flip_green`, …)
  plus a project-level `imports.cat` manifest that `FetchTextures.ps1` /
  `FetchModels.ps1` can replay from the OneDrive archive. Keeps the repo lean,
  matches the existing `$propSets` / `$modelSets` model.
- Or have sync copy the imported **source PNG/GLTF** into the source tree's
  `assets/` (still gitignored, but at least the source tree is authoritative).

Also: the next build's `copy_directory` overwrites bin catalogs from source, so a
created type is lost if `synctosource` wasn't run. Worth a dirty-state warning on
editor close.

---

## Cross-cutting gotchas

- **Palette index = variant index.** Append-only until Phase 5's remap.
- **`Catalog::Add` replaces by id** — every create path needs a collision check.
- **`.cat` has no trailing-comment stripping** — never write `value ; note`.
- **Ids are whitespace-tokenised** in `.map`/`.ent`.
- **UIContext `Clear()` invalidates cached widget pointers** — any callback that
  rebuilds its own page must defer a frame (`m_pendingLanguage` pattern).
- **Drain before swapping GPU resources** (`WaitIdle`) when reloading textures or
  preview meshes — the SRV free-list rule.
- **New UI strings need `.lang` keys ×5**, `en.lang` first.
- Editor writes go to the **bin** copy (`paths::Asset`), never the git tree.

## Open decisions

1. **Phase 6 strategy** — provenance manifest vs copying imported sources into
   the source tree. Manifest is my recommendation; it keeps the repo lean and
   matches how textures/models are already sourced.
2. **Monster config**: fold its tabs into `TypeEditorDialog` as a custom section
   (one dialog, consistent), or keep `MonsterConfigDialog` standalone and have
   the type editor link to it? Folding is more work now, less later.
3. **Phase order 4 vs 2–3** — Phase 4 is independent and small; it could land
   first so the material sliders in the new dialog have something to drive.
