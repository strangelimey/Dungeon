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

**W1 — the world can be written.** `world/world.map` has no writer: it is
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

1. **Deleting a dungeon: what happens to its levels?** Delete the `.map`/`.ent`
   files too, or leave the stems in the manifest as orphans (which the checker
   already warns about)? Deleting content is the one editor action with no undo
   at the file level.
2. **Is the world screen reachable in play, or editor-only?** Today
   `AppState::WorldMap` is where the party travels. An edit mode on it needs a
   way in that does not collide with playing — the dungeon map solved this with
   two modes of one view and a console command.
3. **Areas: painted, or a list of rectangles?** They are rectangles in the file.
   Painting them means a fifth brush and an implied "which area owns this cell";
   a list means a small table UI and no new brush. The file format does not
   care.
4. **Does the world need its own undo stack**, or does it join the editor's
   existing snapshot history? The existing one snapshots every level's editable
   state; adding the world to it is cheap but makes each step bigger.
