# Level building — generate-on-create for play testing

Branch `level-building`, opened 2026-09-24. Draft plan. Nothing is built yet.

## The notes (Michael, 2026-09-24)

- Multiple worlds now work, so it's time to start PLAY TESTING what we've built.
- To do that, the level editor needs more features.
- When auto-creating a new level, the user should set several options first:
  complexity, branches (how many branches a path has), difficulty, "etc."

## What already exists (this is more than it might look)

The editor-enhancements branch built a generator (`Game/Generate.h`, pure and
deterministic). It already has a knobs dialog (`GenerateDialog`) with **width,
height, rooms, branching, locks, difficulty, reward and seed**. Its output is
ordinary `.map`/`.ent` content, and its locks are placed by construction so the
checker passes. The gap is how you reach it, and what the knobs actually do:

| Asked for | What is there today |
|---|---|
| Set options when **creating** a level | The toolbar's [+] writes a blank 16x16 box. The dialog only **regenerates the level being viewed**. Generating a NEW level exists only as the `generate` console command. |
| **Branches** as a count | `branching` is a 0..1 probability that reshapes one tree (a chain at 0, a fan at 1). You cannot ask for "3 branches" and get 3. |
| **Complexity** | No such knob. Only the room count gets near it. |
| **Difficulty** | Monster DENSITY only. It picks uniformly from the themed pool, so it never chooses a harder monster, and nothing gets harder deeper in. |
| Worlds / dungeons | `GenerateLevel` names levels `levelN`, never joins them to a dungeon, and stair-links them to the project's LAST level, not the previous floor of the same dungeon. It predates W5. `CreateNewLevel` gets all three right. |

Also: the fields are raw numeric text boxes, the knobs reset every time the dialog
opens, and nothing reports whether the result matches what was asked for.

## Plan

### P1 — one New Level flow
- [+] opens the generator dialog in CREATE mode, aimed at the viewed dungeon.
  "Empty" stays as a choice (today's box), so nothing is lost.
- Merge `GenerateLevel` into the `CreateNewLevel` path: dungeon-named stem
  (crypt3), added to the dungeon's `levels`, and stair-linked to the PREVIOUS
  FLOOR OF THAT DUNGEON. One writer, so creating and generating cannot drift.
- Regenerate stays as it is (in place, one undo step). It becomes the same dialog
  in REGENERATE mode.
- Controls: sliders and dropdowns instead of text fields. The last-used knobs are
  remembered (settings.ini), so the loop of tweaking and generating again keeps
  its state.

**P1 LANDED (2026-09-24).**
- `Game/GenerateKnobs.h` is the knob TABLE (key, label, tab, kind, range,
  accessors). The dialog's tabs and rows, the settings.ini line (`gen_knobs=`)
  and the `generate` console command's `knob:value` arguments all go through it.
- `GenerateDialog` has two modes, CREATE and REGENERATE. A generated create
  flips into regenerate on the new level. The create form opens with where the
  level lands ("Joins The Crypt below crypt2"). The title stays short because
  `uioverlap` caught a long title running 18px past its slot.
- `CreateNewLevel(dungeon, params*)` is the ONE writer of new levels.
  `GenerateLevel` is gone.
- **Stairs, both directions.** The first test run showed that searching two
  finished, unrelated layouts for a square both have free fails: all three new
  floors came back unreachable. Now the stair square is chosen FIRST
  (`DungeonWorld::FarthestStairCell`: the square farthest from every existing
  way in on the floor above), and the new floor is BUILT AROUND it
  (`Params::entryX/Z` becomes room 0 and the start). A REGENERATE keeps EVERY
  stair the level has: the one up is the entry, and the rest go to
  `Params::keepOpen`, carved and joined to the layout. The records are carried
  across verbatim, and the map grows to hold them. Michael's rule (2026-09-24):
  generating or regenerating must keep any stairs added from the level above
  or below.
- `AddStairAt` finds the next floor in the level's DUNGEON order, not the
  project's flat list. The flat list interleaves dungeons, so a stair down from
  crypt2 led to `eval_arena`.
- A regenerate takes its palette from the level itself, not the active level.
  Rerolling a browsed floor used to quietly re-skin it.
- Checks: `tools/LevelBuildTest.py` (scratch world; the checker clean, every
  stair paired, a reroll into a too-small map grows). Mutation: with the reroll's
  stairs dropped, 5 of 7 checks FAIL. `InGameTest.ps1` sweeps both dialog modes.
  Dev: `generate [dungeon|again] [knob:value...] | dialog [new|off]`,
  `levels view <stem>`.
- NOT covered yet: a world-map doorway or the game's opening that lands on a
  regenerated level. Those are not stairs, so a reroll can turn their square
  into rock (the checker reports it). This predates P1.

### P2 — branches as a count (restructure the tree)
- Build a SPINE first: start → exit, `path length` rooms long. Then grow
  `branches` side branches off it, each `branch depth` rooms deep (a range).
  "How many branches a path has" becomes a number you get exactly (or the
  report says why not, e.g. no room on a small map).
- It stays a TREE, which the lock construction depends on. Locks go on the
  spine first (they gate progress), optionally on branches (they gate treasure).

**P2 LANDED (2026-09-24).**
- Knobs `path`, `branches`, `branchmin`, `branchmax` replaced `rooms` and
  `branching`. An old settings line naming those just has them ignored.
- The shape is GROWN, not scattered. The root room goes first (around the
  entry, when there is one). The main path is hung off it room by room,
  growing away from the start, and its last room is the exit. Then each branch
  sprouts from a path room other than the exit. Branches are spread along the
  path: branch b prefers the anchor at its fair share of the way along. Each
  child room hangs off its parent by a STRAIGHT corridor, and is kept only if
  neither the corridor nor the room comes within a square of anything carved
  except the parent (`Grower::Clear`). So the layout is a true tree: rooms
  never fuse and corridors never graze, and the counts are exact.
- Locks go on the main path's corridors first, so they gate progress and the
  key is usually down a branch.
- `generate::Report` gives asked vs BUILT for path, branches (and each one's
  length), locks, monsters and loot. The console prints it in full. The dialog
  shows two localized lines under the tabs, kept per level: reopening on the
  same level shows it again. One line ran 162px past the dialog, so it became
  two; the panel is taller now too.
- A map too small stops short and SAYS so: 30/12 asked on 16x16 builds what fits.
- Checks: `LevelBuildTest.py` phase 2 MEASURES each level from its .map file,
  independent of the generator. Room squares are floor in a 2x2 block
  (corridors are 1 wide). Branches = dead ends − the exit − the start (if the
  start is a dead end). It demands the file match the knobs AND the report. The
  control pair differs only in `branches` (0 → 4 measured 0 → 4). MUTATIONS: a
  report claiming every asked-for branch fails the shortfall level; switching
  off the clearance rule fails every shape check (rooms fuse).
- Noticed for P4: a monster can stand right beside the arrival stair. The
  difficulty ramp (entrance easier than exit) is the place to fix it.

**P1 REGRESSIONS found by WorldTest (fixed in P2/P3).**
- `AddStairAt` following ONLY the dungeon's order took away the stair brush's
  ability to author a stair BETWEEN dungeons, off one's last floor. That is
  legitimate content: the W10 delete rule "a stair from outside leading in"
  exists for it. The dungeon's order wins now, and the flat list is the
  fallback when the dungeon has no floor that way.
- P1 also moved the EMPTY level's box to line up with the floor above, and
  auto-stairs it. WorldTest's rename scenario builds on the room being at
  7..9, so a hand-written stair landed in rock and loading hit a fatal assert
  (a CRT dialog on screen). **The empty level is the old blank canvas again**:
  fixed box, no stair. Only a GENERATED level is built around the stair square
  and linked. The checker still reports an empty floor as unreachable until
  you place a stair, as it always has.

### P3 — complexity: four separate knobs
Each is its own setting, 0..1, and each shows up as its own line in the report:
- **Loops**: extra corridors that close cycles. Cycles break "everything beyond
  this door", so a loop may only join two rooms in the SAME lock region. Carve
  the loops after the locks are placed, and only between cells with the same
  flood label. The construction proof stays intact.
- **Winding**: corridors take several elbows (a random walk biased toward the
  target) instead of one L-bend.
- **Irregular rooms**: the chance that a room is an L, a cross, or a hall with
  pillars instead of a rectangle. Pillars must not create doorway-shaped cells
  that the lock pass would mistake for doors.
- **Dead ends**: stub corridors off rooms and corridors that lead nowhere. They
  never hold a key.

**P3 LANDED (2026-09-24).** A Complexity tab with four knobs:
- `loops` (count). After the locks, lock REGIONS are read off the doors (flood
  from the start with every door shut, then from each square not yet reached).
  A loop is a straight corridor between two rooms of the SAME region, facing
  each other across a gap, and it passes the same clearance rule as the tree. It
  can never be a way round a lock. This is proven by a check the project
  checker CANNOT make: every door, shut on its own, must still strand floor.
  With the region test removed (MUTATION), one door is bypassed and that check
  fails, while `validate` stays clean. The checker verifies keys come before
  their doors; it does not verify that a door still guards anything.
- `winding` (fraction). A corridor jogs: a staircase of forward runs and
  sideways steps, always turning the same way. So it never doubles back, never
  puts two of its own squares side by side, and never forms a 2x2 block. The
  first run is at least 2 when jogging, because a sideways step one square out
  from a room runs along its wall and widens it into a notch.
- `irregular` (fraction). A room can be an L, a cross, or a pillared hall, and
  every shape stays two squares thick (`Room::Has`). Pillars sit TWO squares in
  from the walls: one square in left a one-wide strip that read as a corridor
  running round inside the room, which the measurement caught.
- `deadends` (count). Stub corridors off rooms, with the same clearance. No
  lock goes on one and no key is hidden down one.
- Locks now only go on the TREE's corridors. The gap between two pillars is
  doorway-shaped too, and a door there shuts nothing off.
- The report gained a third line (loops, dead ends, winding corridors, odd
  rooms). The panel is wider (0.50) because the line ran 11px over at 0.44.
- Checks: `LevelBuildTest.py` phase 3 measures each complexity count from the
  .map file: loops = links − (rooms − 1), dead ends = corridors touching one
  room, winding = links that are not one row or column, irregular = rooms that
  do not fill their bounding box. One base level has every knob at 0, and four
  variants each raise ONE knob; each variant's own count must rise and no
  other count may move. The uioverlap audit ran with a real report in ALL FIVE
  languages. Each game was LAUNCHED in its language: the first attempt switched
  with the console mid-game, and after the first switch every in-game command
  was refused, so four "clean" audits looked at a stale dialog. That bug
  predates this branch and is spun off as its own task.

### P4 — difficulty that means strength
- A THREAT score per monster, DERIVED from its catalog stats (hp, damage,
  offense, defense, armor), so there's no field to maintain. It is computed at
  the catalog seam (`Game_Generate.cpp`) and handed to the generator as
  (id, threat) pairs, so the generator stays pure.
- Difficulty picks the BAND of monsters as well as the density, and RAMPS along
  the spine: the entrance is easier than the exit. Loot scales with depth the
  same way.
- **Boss at the exit** (toggle): the strongest monster in the band stands in
  the exit room.
- Dev command `threat` lists the scores, so the ranking can be checked before
  anything is tuned against it.

### P4b — theme, rooms, presets
- **Room size range**: min/max room width and height. Today it is fixed at 3-7.
- **Theme / tileset**: pick the theme tags and the wall/floor/ceiling palette in
  the dialog. The default is still the active level's, as today.
- **Saved presets**: a named set of knobs ("small easy crypt"). They are stored
  in the PROJECT (`catalog/genpresets.cat`), not settings.ini, because a world is
  a project and its presets are authoring content that should travel with it.
  Save / load / delete from the dialog.
- The dialog now has about 20 knobs, so it gets TABS (Shape / Complexity /
  Population / Theme). The knobs are one FieldSpec-style TABLE (the
  kBalanceFields idiom), and that one table drives the widgets, the preset
  round-trip and the remembered-last-knobs round-trip. Adding a knob stays a
  one-row change.

### P5 — the play-test loop and proving it
- "Generate & play": create the level and start the party at its start cell in
  one click.
- A generator REPORT after every run, in the dialog and the log: asked vs got
  for rooms, branches, path length, locks, monsters (count and threat range).
  A knob you cannot measure is a knob you cannot tune.
- Checks (the house method: break what is measured and see the number move):
  the same params give the same level; a seed sweep gives 0 checker errors; each
  knob moves its own number in the report.

## Decisions (Michael, 2026-09-24)
1. **Complexity** covers loops, winding, irregular rooms and dead ends, with **a
   separate setting for each**.
2. **Difficulty** uses a **derived threat** score from existing stats (no
   authored tier). It picks the band and the density, and ramps toward the exit.
3. **One create = one level**, added to the viewed dungeon and linked to the floor above.
4. Extra knobs: **room size range, theme/tileset, boss at the exit, saved presets**.

## Order
P1 (the create flow, and the table-driven tabbed dialog it needs) → P2 (spine +
branch count) → P3 (the four complexity knobs, loops last because of locks) →
P4 (threat, ramp, boss) → P4b (room sizes, theme, presets) → P5 (play-test
loop, report, checks). The report grows WITH each phase rather than arriving at
the end, so every new knob is measurable the day it lands.
