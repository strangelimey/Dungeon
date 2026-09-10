World editor — the plan
=======================

Michael, 2026-09-09: *"We need a way to edit the 'world' properties. Then we
need to be able to create/edit/delete dungeons in that world. Then we need to
pick a dungeon to edit. This latter will be the current editor experience."*

This is P7, which was deferred a day and then asked for with a shape. Written
against what the editor already does, not against a memory of it.

The shape
---------

**The editor should mirror the data.** The world tier gave the project three
levels of nesting; the editor still shows one, a flat list of every level stem
in a dropdown. What he is describing is the same hierarchy, walked downward:

    the WORLD          its terrain, its locations, where the game starts
      a DUNGEON        its levels, its name — created, edited, deleted here
        a LEVEL        the editor exactly as it is today

The last line is the important one: **the level editor does not change.** Every
brush, dialog, inspector and undo step keeps working, and what is added is the
two tiers above it plus a way down.

### Most of this already exists

Three mechanisms carry most of the weight, and the plan is largely a matter of
pointing them at the new tiers:

- **A non-placeable catalog category.** `effects` is already a palette category
  with `placeable = false`: clicking a row opens the TYPE EDITOR instead of
  arming a brush, and it offers no "+ New..." because an effect needs a class.
  `dungeons`, `terrain` and `quests` are the same shape — except they DO want
  "+ New...", because a dungeon is pure data.
- **`TypeEditorDialog` renders from a SCHEMA.** `CatalogSchema.h` is a FieldSpec
  table per catalog; sections become tabs, kinds become widgets, and "?"
  explains them. Exposing a dungeon's `levels` and `tags` is a table, not a
  dialog. Rename and delete come with it, including the reference sweep.
- **`LevelSettingsDialog` is the per-level settings modal**, opened from a
  toolbar button for the level being viewed. The world's own properties want
  exactly that dialog one tier up.

So the genuinely new work is: a WRITER for the world, an EDIT MODE on the world
screen, and a NAVIGATION change from a flat level list to dungeon-then-level.

Phases
------

**W1 — the world can be written.** DONE (2026-09-09). `WorldMap::Serialize`
returns TEXT and touches no file, so it can be diffed and round-trip-checked
without a disk; `Game::SaveWorld` writes it and READS IT BACK, warning if the
two differ — because the failure mode is the nastiest kind, an in-memory world
that stays correct all session with the damage only showing on next launch.

`saveworld` writes the world ALONE, and it exists because using `savemap` to
test the world writer rewrote every level too and stripped the authoring notes
off eval_arena. A command that does one thing can be used to check that one
thing.

The original entry: `world/world.map` has no writer: it is
hand-authored, and every phase below is pointless until an edit can survive the
session. A writer in the shape of the `.map` writer (records, then grid), and
`savemap` learns to include it. `project.ini` already round-trips.

DO THIS FIRST even though it is the least visible: editing without saving is a
demo, and a demo is what gets built when persistence is left till last.

**W2 — dungeons, terrain and quests become catalog categories.** One
`kCategoryInfo` row and one `CatalogSchema` table each, following `effects`.
That buys create, edit, rename and delete through machinery that already exists
and is already checked.

THE ONE PIECE THAT IS NOT FREE is the reference sweep. `SweepTypeRefs` walks
levels and the cross-catalog fields; it knows nothing about world LOCATIONS,
which now name a dungeon (`dungeon=`), a level (`level=`) and — for an exit
stair — a location. Renaming or deleting a dungeon has to reach them, or the
sweep's promise ("a delete REFUSES while anything still references the type")
quietly stops being true at the tier that matters most.

**W3 — the world screen gets an edit mode.** `WorldMapView` already draws the
world; this gives it brushes, on the MapView/MapEditor pattern: a terrain paint
(the palette's Terrain category arms it), location place/move/delete, and area
rectangles. Undo through the existing snapshot idiom.

It belongs on `WorldMapView` rather than as a third `MapView` mode. MapView is
built around a `DungeonMap` — its docks, variant grids, stair pairing, fog and
level browsing are all level concepts — and P3 kept the two apart deliberately.
Bending it to a grid whose cells are terrain kinds would cost more than the
drawing code it would save.

**W4 — the world's own properties.** A `WorldSettingsDialog`, the LevelSettings
one tier up, holding what belongs to the world rather than to any square: the
`start` cell, and the manifest's game-opening fields (`start_dungeon`,
`start_level`, `start_x`, `start_z`) plus `eval_level`.

That dialog is where "where does a new game begin" becomes editable rather than
a hand-edited ini — and it is the natural home for the answer to a question the
world tier raised and never surfaced in a UI.

**W5 — pick a dungeon, then a level.** The toolbar's level dropdown becomes
two-tier: dungeons, each expanding to its levels. The `[+]` new-level button
creates the level INSIDE the dungeon being viewed rather than loose in the
manifest, which is the moment the flat `levels` list stops being the thing an
author thinks in.

`project.levels` still holds every stem — it is the editor's universe and the
checker's — but it stops being the thing the UI presents.

What this does not change
-------------------------

The LEVEL editor. Brushes, inspectors, the type editor, undo, level browsing,
the check button, generate, balance — all of it keeps working on the level you
have picked. If any of that has to change to accommodate the tiers above it,
that is a sign the seam is in the wrong place.

Open questions
--------------

1. **Deleting a dungeon: what happens to its levels?** ANSWERED (Michael,
   2026-09-09): **delete the levels too, after a confirmation dialog.**

   Two consequences to build in rather than discover. First, this is the one
   editor action with NO UNDO — the snapshot history is in memory and the files
   are not — so the confirmation has to say what it is about to destroy by
   NAME and by count, not ask "are you sure?". Second, the REFERENCE SWEEP runs
   first and can refuse: a level of this dungeon may be the far side of a stair
   from a level in another one, and deleting it would leave that stair pointing
   at nothing. Refusing with the reason beats deleting and reporting the
   wreckage afterwards.
2. **Is the world screen reachable in play, or editor-only?** ANSWERED
   (Michael, 2026-09-09): **both.** So `WorldMapView` takes the same shape
   `MapView` did — one view, two MODES — which is the answer the dungeon map
   already arrived at and is worth copying rather than re-deriving.

   Three things follow. FOG is the mode's main difference, exactly as it is
   below ground: Player mode draws undiscovered ground as unknown, Editor mode
   draws the whole world and every location whether the party knows of it or
   not. The WAY IN mirrors it too — the `editor` console command, and Esc backs
   out. And unlike the dungeon map there is no "while it is open the world
   keeps simulating" problem to solve: the world map is an app state that
   simulates nothing, so an edit mode on it is a mode of that state and not an
   overlay over a running game.
3. **Areas: painted, or a list of rectangles?** ANSWERED (Michael,
   2026-09-09): **a list.** So no fifth brush, and no implied "which area owns
   this cell" — a small table of rows (id, x, z, w, h, difficulty) that edits
   the records as they already are.

   Worth keeping honest in the UI: areas OVERLAP legally and the LAST match
   wins, which is a fact about file order. So the table must show them IN ORDER
   and let that order be changed, or the one rule the format has becomes
   invisible in the only place anyone would edit it.
4. **Does the world need its own undo stack**, or does it join the editor's
   existing snapshot history? ANSWERED (Michael, 2026-09-09): **join the
   editor's.** One history, one Ctrl+Z, and a step that spans tiers undoes as
   one thing — which matters because some edits genuinely do span them (adding
   a level to a dungeon touches the manifest, the catalog and the files).

   The cost is that every snapshot grows by the world's records, and the
   existing history already copies every level's editable state per step. That
   is worth watching but not worth pre-solving: the world is a few dozen
   records against maps of hundreds of cells.

All four answered. Nothing above is blocking W1, which is the writer.
