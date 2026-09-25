# Level building - generate-on-create for play testing

Branch `level-building`, opened 2026-09-24. P1-P5 are built (each phase's
"LANDED" note below says what, and what it found); the checks are
`tools/LevelBuildTest.py`, six phases, all measured from the level files.

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

### P1 - one New Level flow
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
  into rock. This predates P1. **Fixed on `level-tuning` (below)**, and the
  checker did NOT report it then; it does now (`arrivalblocked`).

### P2 - branches as a count (restructure the tree)
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

### P3 - complexity: four separate knobs
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

### P4 - difficulty that means strength
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

**P4 LANDED (2026-09-24).**
- `Game/Threat.h`: threat = sqrt(offence × toughness) against a fixed
  reference party. It uses the combat model's own opposed d100: the stance
  splits accuracy into press and guard, armor is flat soak, and the resists
  averaged are the physical ones. It is pure and has one home. `threat
  [tag...]` prints the ranking with its parts. Current order: coward 0, swarm
  3.9, mage 6.4 … mummy 13.8, warrior 14.3, berserker 15.2. KNOWN GAP: a
  caster's SPELL is not counted, only its melee, so casters rank low.
  **Closed on `level-tuning` (below).**
- The caller (the catalog seam) scores the pool; the generator stays pure.
  Rooms remember their PARENT, so each has a depth, and progress = depth /
  deepest. A room draws near the rank `difficulty + ramp × (progress − 0.5)`,
  with ±1 of jitter, and its density leans the same way. So `ramp` (new, 0..1)
  makes both the kinds and the numbers rise toward the far end.
- SAFE START: the start room gets nothing, and no monster stands within 3
  steps of the arrival square. P2 saw one beside the stair.
- `boss` (new, a checkbox, the first Bool knob): the pool's strongest kind goes
  in the exit room, and never in the start room.
- Loot follows depth too, and every side branch's end room gets a find with
  chance `reward`, which gives a branch its reason to be walked.
- The report gained a fourth line (threat range, boss). `generate dialog tab
  <n>` shows a tab, so the sweep audits the Population tab as well.
- Checks: `LevelBuildTest.py` phase 4 joins each level's monsters to the
  game's own printed `threat` table (never a copy of the formula). Control
  pairs, MANY SEEDS POOLED: difficulty 0.2→0.8 over 5 seeds a side needs more
  monsters and a mean 3+ higher (measured 5.8 → 14.0). Ramp 0→1 over 8 seeds
  a side needs the far-half-minus-near-half gap to grow by 2.5+ (measured
  +0.1 → +4.1). Boss off→on: the strongest kind goes from absent to exactly
  one, in a dead-end room. Safe start on all 28 levels, and not vacuously: a
  total count of monsters checked is required.
- THE STATISTICS LESSON. The first ramp check ("far half stronger, and more so
  than without the ramp", on ONE seed) passed a mutant that ignored the ramp.
  On three seeds with a 2.5 margin it caught that mutant, but a fully RANDOM
  pick still cleared the bar on noise (+2.59), because a random pick is far
  noisier than the real one. A check about a tendency needs a margin AND a
  sample size set against the noisiest plausible mutant, not the nearest one.
  Both mutants (random pick; ramp ignored) now fail on exactly the check
  meant for them.

### P4b - theme, rooms, presets
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

### P5 - the play-test loop and proving it
- "Generate & play": create the level and start the party at its start cell in
  one click.
- A generator REPORT after every run, in the dialog and the log: asked vs got
  for rooms, branches, path length, locks, monsters (count and threat range).
  A knob you cannot measure is a knob you cannot tune.
- Checks (the house method: break what is measured and see the number move):
  the same params give the same level; a seed sweep gives 0 checker errors; each
  knob moves its own number in the report.

**P4b LANDED (2026-09-25).**
- `roommin`/`roommax`; a Theme tab (a theme tag and "walls and floors like
  <level>", both "(as before)" by default) on a new TEXT knob kind, `Choice`;
  presets in the project's `catalog/genpresets.cat` (seedless recipes, the demo
  ships four), so the catalog round-trip is 25 of 25. `LevelBuildTest.py`
  phase 5; the palette check first makes crypt2 DISTINCT, since every demo
  level shares one palette.

**P5 LANDED (2026-09-25).**
- `Game::PlayLevel(stem)`: closes the generator and the editor and puts the
  party on the level at its start - a level transition, or, for the level the
  party is already on (a reroll of the active one), a walk back to the start.
  Create mode gains "Generate and play"; regenerate mode "Play this level";
  the console `generate play [stem]`.
- A BUG IT FOUND, older than this branch: crypt1 has a sconce glyph in open
  floor, which loads with a default facing, but `savemap` wrote it back as an
  explicit `fixture sconce 4 4 north` - a record the loader ASSERTS faces a
  wall. One save of crypt1 made the demo world fatal to load. The writer now
  omits a facing that names no wall, so the record takes the glyph's own path.
  (The floating sconce itself is an authoring question for Michael.)
- UNATTENDED CRASHES: `-headless` now calls `crash::SetUnattended()`, so a
  fatal error records, dumps and EXITS instead of parking a modal "Debug
  Error" dialog on the desktop until a test's timeout. Michael saw two of those
  and took them for crashes. Checked on purpose: `crashpoke assert` headless
  exits by itself in 12 s with code 3, the FATAL line and a minidump.
- `LevelBuildTest.py` phase 6: the party lands on the level's start AS THE FILE
  HAS IT, on that level (mapinfo's size, start and walkable count against the
  file), still in play; an unknown level is refused; the writer round-trips
  the floating sconce and a second run reloads the saved world; playing the
  level the party is on brings it back to the start. MUTATION: writing the
  facing always fails both writer checks. The test takes phase numbers now
  (`LevelBuildTest.py 6`) and reports a game crash instead of dying on it.

## Follow-ups: branch `level-tuning` (2026-09-25)

The four known issues the handoff listed, fixed before Michael's first
play-test. His calls: density is its own knob; a caster's spell counts, with a
range bonus on top; the Generate button gets a die.

- **Density is its own knob** (`density`, Population tab, 0..2, 1 = one
  monster per 25 floor squares). `difficulty` now picks strength ONLY. It used
  to set the number too (0.04 x difficulty per square), so an easy level was an
  empty one: a 0.2 level came out with no monsters at all. Default 0.5, the old
  number at the old default difficulty. The shipped presets carry
  `density` = their old difficulty, except `first_floors` (0.25 -> 0.5): gentle
  should mean weak, not deserted. A world-map ambush keeps its floor on the
  NUMBER only (`density = max(difficulty, 0.25)`), so safe ground now meets the
  weak end of the pool; the floor used to lift the strength with it.
- **Threat counts what a monster actually does** (`Game/Threat.h`). A kind has
  up to two attacks: melee and, for a skirmisher or caster, its SHOT - a
  caster's spell bolt, anyone else's plain bolt. Offence is the better one, and
  a shot gets `kRangedEdge` = 1.5 on top (a first cut; `threat` prints the shot
  before the edge so a different value reads straight off). The attacks are
  resolved by `DungeonWorld::ThreatProfile` from the catalog entry alone (no
  model load), mirroring MonsterAttack / MonsterRangedAttack: `powers` applied,
  the spell's own bolt via `Spell::MonsterBolt`, and on-hit DoTs. **A DoT counts
  as a RATE x UPTIME, not its total per hit**, because every effect refreshes
  rather than stacks: scored as a total, the blob's 20-second poison put it at
  the top of the whole ranking (20.4). Measured, old -> new: mage 6.39 -> 13.74,
  archer 7.08 -> 9.06, lurker 13.93 -> 15.74 (now the top, by its bleed), blob
  10.89 -> 12.72, centipede 9.85 -> 11.57 and giant spider 11.75 -> 13.01 (their
  poison); a kind with no shot, no `powers` and no on-hit effect is unchanged.
- **A reroll keeps every way in.** `Game::ArrivalsOn(stem)` lists the squares
  the game's opening (project `start_x/z`) and world-map doorways (`entryx/z`)
  land on, and `RegenerateViewedLevel` keeps them open like stairs. When no
  stair leads up, the first arrival becomes the ENTRY (the layout's root and the
  start), since that is where a player walking in begins. The checker gained
  `map.check.arrivalblocked`: a doorway opening onto rock. It had no such rule,
  so a bad reroll passed it.
- **The Generate icon is a die**, drawn onto the pack's blank disc like the
  level, save and globe glyphs.
- Checks: `LevelBuildTest.py` phase 4 now demands difficulty 0.2 -> 0.8 be 3+
  stronger and about as many (within a quarter; not exact, since each new floor
  is built round a different stair square), every easy level non-empty, density
  0.2 -> 0.8 2.5x+ as many and no stronger, exactly the ranged kinds (by the
  catalog FILE's archetype) scored with a shot, and a caster's shot 3x+ its
  melee. Phase 7 (new): three rerolls each of crypt2 and crypt1 into small maps,
  with the opening moved to a square no stair uses and a doorway added onto
  another; the checker stays clean, the doorway square is floor joined to the
  level, and crypt1 starts on its opening. MUTATIONS, each failing the check
  meant for it: density read from difficulty (both control pairs), the spell
  scored as a plain bolt (shot 1.6x melee), the ranged edge dropped, the
  arrivals ignored (four checks). Phase 7's first version passed the arrivals
  mutant, because both demo doorways land on exit stairs a reroll already kept;
  the added doorway has no stair under it.
- Noticed, not changed: every monster in monsters.cat has `defense = 0.1`, but
  defense is in d100 POINTS (the evasion roll adds it as-is), so all of them
  effectively guard with nothing. It reads like a leftover fraction.
  Balance-pass material.

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
