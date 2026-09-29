# Editor updates - the plan

Source: Michael's brain dump of 2026-09-29, after trying to edit the crypt
levels ("awkward and clunky"), plus his answers to the organize step. The raw
notes and every answer are in `docs/editor-updates-notes.md`; this document is
checked against them.

## Status: BUILT (2026-09-29)

Every phase below is built and committed, each checked by
`tools\EditorTest.py` (11 phases, every check mutation-tested) on top of the
quick tier:

| Phase | What | Commits |
|---|---|---|
| P0 | foundations: file split, one resolver, strokes, edit counter, read-only gather, chunk batching | 8be13df..4cfe4d8 |
| P1 | area fill + fill level, tool strip, icons | cdb0503, 05aae73 |
| P2 | live validation | 72181c3 |
| P3 | surface combinations, editing them, `typerefs` stash fix | ad9f0ff, f3d4432, a49c805, 44ed1cd |
| P4 | template, three ways to make a world, New world dialog | e94dec0, 11c2144 |
| P5 | the wizard | 2d150cf |

Found along the way and fixed: pressing Check (and counting a type's uses)
made the next save rewrite every level; a drag whose first square was
unchanged lost its undo step; a new world could only be left with a dev
command; a failed world create left a half-built folder the list offered; a
small wizard floor could come out with no monsters. Not this branch: WorldTest
phase 20 and LevelBuildTest's open-floor sconce read crypt1 content Michael's
uncommitted crypt1 edit changed, and `uioverlap` cannot see text overrunning
its widget (spun off as its own task).

Five themes:

- A. **Area fill.** Paint a connected room or corridor in one action; the fill
  stops where narrow meets open. Also a whole-level scope.
- B. **Combinations.** A world-wide named floor + wall + ceiling set such as
  "marble hall". Each surface is a MIX that keeps the random variation. Cells
  STORE the reference, so editing the combination repaints every room using it.
- C. **Tool strip.** Pickable tools (paint, rectangle, flood, area,
  eyedropper) in a strip beside the palette. Shift/Ctrl/Alt stay as shortcuts.
- D. **Live validation.** Re-check when a stroke ends; red box = error, amber
  = warning, hover tooltip says why. A broken stair pair is boxed at both ends,
  and findings with no cell are a count badge on Check.
- E. **New world.** A button on both toolbars opens a dialog with four
  choices: blank (from a TEMPLATE project), copy this world, copy one level,
  or a wizard (name, theme, size, difficulty, generated starting dungeon).

---

## What the code survey found first

Three findings change the order of work. None of them is visible from the
feature list.

1. **Pressing Check today makes the next save rewrite every level.**
   `DungeonWorld::Validate` gathers non-active levels through
   `EnsureMapStash` / `EnsureEntStash` (DungeonWorld_Validate.cpp:176), which
   stashes them for editing. From then on `SaveAllLevels` rewrites every
   level's `.map` and `.ent`, including untouched ones, and every undo snapshot
   copies every stash. Live validation would trigger this in every session
   after the first edit. The gather must become read-only BEFORE D is built.
2. **Drag painting has an undo hole, and there is no stroke-end hook.**
   `ApplyBrush` begins AND commits its undo step on the press
   (MapEditor.cpp:615/768). If the pressed cell already had that texture, the
   step is dropped and the rest of the drag cannot be undone. Nothing observes
   the left-button RELEASE for painting, which is exactly the event D needs.
3. **"Which texture does this cell show" is written out three times:** the
   mesh builder's `pick` (DungeonMeshBuilder.cpp:94), the map overlay's
   `fillVariant` (MapView.cpp:951) and the editor's `ResolvedVariant`
   (MapEditor.cpp:920). Combinations add a third cell state, and adding it
   three times is how the scene and the map would come to disagree.

Also relevant:
- `DungeonWorld_Editing.cpp` is 2307 lines, already past the ~2000 split line,
  and most of the new variant code would land there. `MapView.cpp` (1686) will
  cross it with the tool strip.
- Switching worlds no longer RELAUNCHES (world-on-demand made it in-process).
  Several comments and `docs/world-editor-plan.md` still say it does.
- CLAUDE.md's "+ Catalog..." row is gone; a persisted "Catalogue" checkbox
  replaced it. Fix the doc as part of this branch.
- A W7 starter world has no exit stair, so its party can only leave the keep
  with the dev `leave` command. E fixes that for every world it creates.

---

## P0 - Foundations (no visible feature; everything after leans on it)

1. **Split `DungeonWorld_Editing.cpp`** by concern before adding to it:
   level save/serialize, undo snapshots, remote/stash editing, and the
   rest.
2. **One surface resolver.** A single function answers "which palette index
   does surface S of cell (x,z) show", used by the mesh builder, the map
   overlay and the editor's flood/eyedropper. No behaviour change. The check is
   that every level bakes byte-identical geometry before and after.
3. **Stroke lifecycle.** Split painting into press / drag / release in
   `MapView::Update`: begin the undo step on the press, commit it on the
   RELEASE (only if anything in the stroke changed). This fixes the drag-undo
   hole and gives D its "stroke ended" event.
4. **An editor edit counter** on `DungeonWorld`, bumped by
   `CommitUndoStep(true)`, undo/redo, level install/create/rename/delete, and
   the paths that change checker inputs WITHOUT an undo step: inspector
   `onApply` (door key/name, button target, stair destination), `WriteTypeFields`
   (a key item's category, a stair's traverse/exit), and the world tier.
5. **Read-only validation gather.** Non-active levels are parsed into a
   per-level cache (an edit stash still wins over the file), never into the
   edit stashes. Then check that pressing Check followed by `savemap` rewrites
   only the levels that were actually touched.
6. **Batched chunk rebuilds.** A multi-cell fill collects dirty chunks and
   rebuilds each once, with one `WaitIdle`, instead of N x (WaitIdle + up to 5
   chunks). Rect and flood get faster now; area and level fill need it.

## P1 - Tool strip + area fill (themes C and A)

**The strip.** A vertical column between the left dock and the grid (carved
out of `GridArea`, so pan/zoom/picking follow for free). It stays visible
when the palette dock collapses. It has one list of tools, walked by layout,
hover, click and render (the `ToolbarButtons` pattern), with tooltips placed
BESIDE the strip rather than under it.

| Tool | Gesture | Shortcut kept |
|---|---|---|
| Paint | click / drag (today's behaviour) | - |
| Rectangle | press-drag-release with a live outline preview | Shift+click from the last anchor |
| Flood | click; same cell type + same resolved texture | Ctrl+click |
| Area | click a room or corridor | (new) |
| Eyedropper | click; arms the cell's texture (or its combination, P3) | Alt+click |

Plus one action button, **Fill level**: applies the armed brush to every cell
of that surface kind on the viewed level, as one undo step.

- Holding a modifier temporarily selects its tool (the Photoshop idiom), so the
  existing muscle memory still works.
- The active tool persists in settings.ini beside the dock flags.
- No single-letter tool keys, because the party keeps walking on the keyboard
  while the editor is open.
- Harness: an `editor tool <name>` console command.

**Area fill - the rule.** A walkable cell is OPEN if it sits in any 2x2
block of walkable cells, otherwise NARROW. The fill spreads (4-connected)
through walkable cells of the same class as the clicked one:
- a room fills to its doorways;
- a 1-wide corridor fills as one run, bends and T-junctions included, and
  stops where it opens into a room;
- a doorway cell is narrow, so it is its own boundary.

The existing `DoorwayFacing` predicate is NOT enough: it calls a corridor's
bend "open" and would break every corridor at its first corner.

What it paints depends on the armed brush:
- floor or ceiling: the area's cells;
- wall: the solid cells 4-adjacent to the area.

With a combination (P3) armed, all three at once.

## P2 - Live validation (theme D)

- **When it runs:** Once per frame, if the edit counter moved and no mouse
  button is held (so a drag re-checks once, on release). Results are cached
  until the next change.
- **Boxes:** A red or amber ring on each finding's cell for the VIEWED level,
  drawn after entities and under the hover/selection rings. If a cell has both,
  red wins.
- **Both ends of a stair:** `Issue` gains `level2/x2/z2`. The stair checks fill
  it, so a mismatched or offset pair is boxed on both levels.
- **`itemslost`:** Today it boxes one arbitrary item. It becomes one finding per
  lost item, and ordering within a severity becomes deterministic.
- **Tooltip:** Hovering a boxed cell lists every finding on it, word-wrapped.
  - It is placed by the HandSlot rule (below the cell, above if it would run
    off, clamped) in the toolbar tooltip's look.
  - `WrapLines` moves out of CharacterSheet into a shared UI helper.
  - Text comes from `loc::FormatLine`.
  - It is suppressed while any modal is open, since hover goes stale then.
- **Badge:** A count on the Check disc of findings with no cell (level- or
  world-wide), red if any is an error. Clicking Check still opens the full list.
- **Performance:** The checker is pure and small (well under a millisecond on
  dungeon-demo). Log the run time once, so a large world that makes it slow
  shows up.

## P3 - Combinations (theme B)

**Where they live.** A new project catalog, `combos.cat`:

```
[marble_hall]
display = Marble Hall
floor   = marble_floor marble_floor_worn
wall    = marble_wall
ceiling = marble_ceiling
```

- Each list is a MIX, and an empty list leaves that surface alone.
- The type editor needs a new schema field kind: a LIST of catalog references,
  edited through the asset picker.
- A **Combinations** palette category holds them:
  - clicking a row arms it as a brush;
  - "+ New..." creates one, seeded from the eyedropped cell;
  - right-click opens the type editor.

**How a cell refers to one.** The existing variant grids gain a third state:
- -1 = default hash
- >= 0 = one palette index
- <= -2 = a combination

On disk it is a new record naming the combination by ID, never by index:
`surfacemix <wall|floor|ceiling> <x> <z> <combo id>`. The keyword must not
start with `variant`/`palette`/`stair`, which are dispatched by prefix.

**How it resolves.** The P0 resolver takes the combination's member list and
hashes within it, with the same salt as today. So:
- a combination cell varies exactly as a default cell does, but only across
  its members;
- the scene and the map agree by construction.

**The palette rule still holds.** Only palette entries have their textures and
worn meshes loaded, so a member must be in the level's palette.
- Painting a combination appends any missing members, append-only as today.
- Editing a combination later sweeps every level that uses it, appending new
  members to each palette, then reloads the active level's surfaces.
- Rename sweeps references (the type-rename machinery). Delete refuses while
  referenced, and says which levels.

**The eyedropper** arms the combination when the cell holds one.

**Undo.** Painting is undoable. Editing the combination's definition is a type
edit and, like every type edit, not undoable.

Older builds cannot read `surfacemix`. That is acceptable: levels are data and
the branch moves forward.

## P4 - Template project + new-world dialog: blank and copy (theme E)

**The template** lives at `assets/templates/default/`, OUTSIDE `projects/`, so
no world list offers it and nothing can switch into or delete it by accident.
It is a curated STARTER KIT, not a bare minimum:

- Everything the fixed roster and the systems need to run:
  - rune items and the roster's armour ids;
  - spells, effects, damage types, balance, attacks;
  - a down stair (traverse) and an exit stair;
  - a door, a lever, sconce and brazier fixtures;
  - a passable terrain.
- A handful of wall/floor/ceiling types and a few monsters and weapons, all
  already installed in the pool.

A check (a WorldTest phase) demands that a blank world made from it loads,
starts a game and validates clean. The template cannot rot unnoticed.

**One creation path.** The Worlds dialog's name-and-Create row is replaced by a
**New world...** button. That button, and a new disc on BOTH editor toolbars,
open the same **NewWorldDialog**: a name field plus four choices.

- **Blank:** the template's catalogs, plus the starter keep. The keep now
  gets an exit stair to its world location, so the party can leave it.
- **Copy this world:**
  - Copies the catalogs, levels, dungeons, overworld, quests and opening.
  - UNSAVED edits go into the COPY, not the source. The level writers get a
    target-root parameter, so creating a world never silently saves the one
    you are in.
  - Saves do not come across (they carry `world=`).
- **Copy one level:**
  - The current world's catalogs, plus that level as the only floor of a
    one-level dungeon.
  - Stairs to other levels are removed, and one exit stair goes to the new
    world location.
  - Quest hooks are stripped.
- **Wizard:** P5.

On success the dialog offers **Switch now** (in-process).

**Build in a temporary folder and rename at the end.** Today `CreateWorld`
writes project.ini FIRST, so a failed level or world write leaves a
half-built folder that the world list still offers.

Harness: `worlds new <name> [blank|copy|level <stem>]`, extending WorldTest
phases 14/15 and the InGameTest dialog sweeps.

## P5 - The new-world wizard

The wizard asks for:
- a name;
- a theme (the tags the catalogs carry);
- a size;
- a difficulty;
- a seed (re-rollable).

It then writes a world from the template and GENERATES its starting dungeon
with the existing generator:
- `generate::Run` is pure;
- `FillPools` is changed to take a `const Project&` instead of reading the
  running world;
- `BuildLevelText` takes palette id lists instead of a donor map;
- an exit stair is placed at the level's entry, as `StartEncounter` already
  does.

No running world is needed, so the wizard works from the title screen too.

---

## Decisions (Michael, 2026-09-29)

1. **Shared wall textures - ACCEPTED for this branch.** A wall block has ONE
   texture for all four faces, so painting a corridor's walls also retextures
   the room on the other side of any 1-thick wall. Per-face wall textures
   (a format change plus a mesh-builder change) are a later thread.
2. **2-wide corridors count as rooms - OK.** Under the 2x2 rule they fill
   together with the rooms they join.
3. **The wizard generates ONE floor.** Add more with [+] afterwards.
4. **The wizard uses the TEMPLATE's catalogs.** No current-world option.
5. **Tool icons come from a committed SCRIPT** that draws each glyph onto an
   existing disc (a `tools/Build*` script, so a revision is a re-run).
   Until the icons land, the strip falls back to text labels.

## Order and checks

P0 -> P1 -> P2 -> P3 -> P4 -> P5.
- D is second because it is independent and immediately useful while testing
  everything after it.
- Combinations come after the strip because they are a brush on it.
- The world dialog is last because nothing else depends on it.

Every phase ends with:
- a debug and release build;
- the quick check tier;
- a `uioverlap` sweep of anything it drew;
- for rules (area classes, the resolver, the read-only gather, the stair
  far-end boxes), a MUTATION check: break it and see the test fail.

Then you drive it: this branch's whole point is how the editor FEELS.
