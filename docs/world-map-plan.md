World map — the plan
====================

Against `docs/world-map-notes.md` (the dump, its organized themes, and his eight
answers). Written 2026-09-09, before any code. Where a decision is PROPOSED
rather than dictated by the notes it says so; where the notes deliberately
parked something it stays parked.

The shape
---------

The whole feature is ONE idea applied a tier up: **the world is a level**. It
has an authored static layer that ships with the project and a dynamic layer
that exists only in the save as a diff (his answer 1 — "like a level, there's
content and then a diff for the save"). Everything below follows from taking
that literally, because every mechanism a level needs already exists and is
worth reusing rather than paralleling: a grid, a palette resolved through
catalogs, fog/discovery as a revealed-cell set, records for placed things, a
diff-based save, a top-down view with baked icons.

    project (a GAME)
      world            static: terrain grid + locations + areas   -> save diff
        dungeon        a named group of levels                    -> save diff
          level        today's .map/.ent                          -> save diff

The dungeon tier is the only genuinely new container. Today `project.ini` has a
FLAT `levels = stem stem stem` and a dungeon is merely implied by which stems'
stairs point at each other.

### Files

- `catalog/dungeons.cat` — the dungeon tier. `[crypt]` with `name =`,
  `levels = crypt1 crypt2`, `entry = crypt1`, plus whatever the world map needs
  to show it. A catalog because that is what this project uses for "a category
  of authored things", and it gets the type editor, the reference sweep and the
  rename/delete machinery free.
- `catalog/terrain.cat` — the world map's surface palette, the exact analogue of
  walls/floors/ceilings: per terrain kind an icon/colour, a TRAVEL COST, a
  DIFFICULTY, and encounter `tags`. This is what makes "an encounter generated
  from the level/difficulty of that area" data rather than code.
- `world/world.map` — the authored world: a `palette terrain ...` record, an
  ASCII grid, and records for what stands on it — `location <dungeon|town> <id>
  <x> <z>`, and an `area` record where a region wants to override the terrain's
  difficulty. Same dialect as a level `.map`, same parser primitives.
- `catalog/quests.cat` — quest definitions (id, name, stages). Thin on purpose;
  see Phase 6.

`project.levels` STAYS as the flat list of every level. It is the editor's
universe (the level dropdown, the stair `dest=` sweep, `SweepTypeRefs`), and
keeping it means the dungeon tier is ADDITIVE — nothing that works today breaks
on the day dungeons appear. A dungeon names its levels; the flat list still
knows them all.

### The world map is a grid

His answer 3 was "probably, not sure yet". PROPOSED: a grid, for three reasons —
movement, line of sight and projectiles in this engine are 4-cardinal by rule;
fog of war is already a per-cell revealed set, which is exactly what "dungeons
are discovered as you explore" wants; and the whole `.map` dialect, the top-down
renderer and the icon bake are grid-shaped already. The escape stays cheap:
everything outside `WorldMap` asks it questions (what is at this position, what
does travel here cost, what is discovered) rather than indexing its grid, so a
later switch to free positions is one class.

### Where the party is

A new `AppState::WorldMap`. Not an overlay like MapView: MapView is a view of
where the party is, and this is where the party IS (his answer 2 — "in the
world, at coordinates x,y"). The precedent for a full-screen 2D screen with no
3D scene is already in `Game::Render`'s `editorMap` flag, which skips the shadow
and scene passes entirely.

**Two cost models, and no extraction** (revised 2026-09-09 on his instruction).
An earlier draft of this plan had the party tick coming OUT of
`DungeonWorld::Update`, on the grounds that supplies, effects and regen all tick
in there and travel costs time. That is not the design. In a DUNGEON the tick
stays continuous, exactly as now; on the WORLD MAP a journey has a DURATION —
the party commits to a move, the world works out how long it takes, and the
costs are SETTLED for that span rather than accrued frame by frame. Nothing
ticks on the world map, so nothing needs lifting out of the dungeon to keep
ticking there.

So travel is NOT another multiplier on `Game::Update`'s
`wdt = dt * m_timeScale * m_world.RestTimeScale()` seam. Rest runs the dungeon's
clock fast; a journey settles a bill.

What must not drift is the ARITHMETIC — two cost models are two chances to
disagree about what an hour of walking costs. Most of that is already safe: the
drain maths lives in the PURE `resource::` TU (`DrainPerSec(rules, practice)`,
including only `Core/Types.h` and `Curve.h`), and `TickSupplies` is merely its
per-frame caller, so a journey can ask the same function for a large span.
Lifting the REST of the cost loop out of `DungeonWorld` is deferred — his call,
"that can wait" — and is cheaper than it looks for the same reason.

Phases
------

Each lands on its own, builds clean and is checkable. Order is dependency order.

**P0 — the design doc.** DONE (2026-09-09). `docs/world-map.md` in the house style, pinning the
file layouts above and the vocabulary (world / dungeon / level / area /
location). No code. Cheap, and every later phase is checked against it.

**P1 — the world data layer.** DONE (2026-09-09). `WorldMap` (grid, terrain palette, locations,
areas), `dungeons.cat`, `terrain.cat`, `Project` loading them. No view, no
travel. Authored by hand for now. Checked by a dev command (`world`) that prints
the grid, the locations and each area's difficulty, and by the existing
validation pass learning the new records (a location naming no dungeon, a
dungeon naming a missing level, a terrain not in the palette).

**P2 — the save split.** DONE (2026-09-09). `SaveData` grows a WORLD section — party world x,y,
world time, discovered locations, quest state — and the per-level states become
per-dungeon-per-level. Round-tripped like every other save field. His answer 8
("we won't need to keep existing content") frees this from the compat ladder,
and he CONFIRMED the consequence (2026-09-09): the version bump REFUSES older
saves outright, with a clear message, rather than half-loading them — "early
saves are WIP only". So the read path gains a floor rather than another rung,
and the version comment block records where the ladder was cut.

**P3 — the view and travel.** DONE (2026-09-09). `AppState::WorldMap`, a `WorldMapView` built on
what MapView already does (pan/zoom, cell render, baked model icons, fog), and
movement as a JOURNEY: terrain travel costs give the move a duration, and the
costs are settled over that span through the same pure `resource::` arithmetic
the dungeon tick uses. Discovery marks locations seen as the party moves. No
party-tick extraction (see above) — which makes this phase markedly smaller
than the first draft had it. Left OPEN by the design doc: what a journey does
with the things that have a state machine inside them rather than a rate — a
DoT that would kill someone partway, a downed member's stabilize clock.

**P4 — entering and leaving.** A location on the world map opens its dungeon at
its entry level; leaving the dungeon returns the party to the world map at that
location. This replaces "a new game starts on a level" as the game's opening,
which is the moment several other things notice (see Risks).

**P5 — random encounters.** Travel rolls against the area; an encounter builds a
throwaway space from that area's difficulty and tags. Almost all of it exists:
`generate::Run` is pure and deterministic and emits ORDINARY content with no
"generated" flag, `Game_Generate`'s `BuildLevelText` already renders a generated
level to `.map`/`.ent` TEXT, and catalog `tags` is already the pool-matching
axis. The ONE missing seam is that `DungeonMap` and `DungeonEntities` construct
from a PATH only, so this phase adds a from-text construction path and the
encounter never touches disk — the same "writes no files" property
`DungeonWorld_Arena` already relies on. Saving mid-encounter stays PARKED (his
answer 4); note only that determinism means a seed plus params would reproduce
the space exactly, if that is ever the answer chosen.

**P3.5 — effects while travelling, and camp.** NEW, from Michael's answer of
2026-09-09 (docs/world-map.md "Time, and what a journey costs"): DoTs must bite
on the road and travel may kill. That needs the PARTY-TICK EXTRACTION this plan
dropped at P3 — the same work, for the opposite reason — settled in SLICES so a
DoT kills at the hour it would, plus a CAMP button as the recovery counterweight.
Ordered before P6 but after P4/P5 unless he says otherwise; P4 must in any case
cope with a party that wipes while travelling.

**P6 — quests.** `quests.cat` for definitions, global state in the save, and the
two content hooks the dump actually names: an item that sets a flag when found,
and a found map/clue that reveals a location. Deliberately thin — no journal UI
was asked for.

**P7 — the world editor.** OPEN (his answer 6). If it happens it is the editor's
existing shape one tier up: a mode that paints terrain and places locations,
reusing the palette, brushes, undo snapshot and remote-level machinery. Worth
deciding only once P1–P4 have settled what the data actually is.

OUT OF SCOPE, recorded so it does not creep in: towns and shops, which need
money and trade (his answer 7 — a "to do").

Risks and traps
---------------

1. **The two cost models can drift.** This replaces the party-tick extraction as
   the plan's main hazard, and it is the subtler of the two: a journey that
   computes drain its own way will disagree with the dungeon about what an hour
   costs, and nothing will report it. Route both through the pure `resource::`
   functions and give the journey path its own check. If the deferred cost-loop
   extraction ever happens, note that the ordering inside `DungeonWorld::Update`
   is load-bearing and commented as such (supplies BEFORE effects, so an emptied
   meter bites on the same frame) — move it whole and keep the order.
2. **The eval harness assumes a loaded level — and keeps doing so.** Ten suites
   run against a world the harness `reset`s to "where a new game would leave
   it", and P4 changes where a new game leaves it. DECIDED (2026-09-09):
   `reset` still means A LEVEL for now. So P4 must break the identity `reset`
   currently leans on — it goes through `m_ui.onStartNewGame` — and give the
   harness its own explicit "put the party in a level" entry instead, rather
   than letting the two drift apart silently. A suite that quietly started
   measuring the world map would still pass while measuring nothing.
3. **The alloc guard only arms in `Playing`.** A new state means travel frames
   are unwatched unless `SteadyStateFrame` learns about them. A world map that
   allocates per frame is exactly the kind of thing that would go unnoticed.
4. **`uioverlap` must sweep the new screen**, like every other screen — that
   sweep found four defects nobody had reported the last time it ran wide.
5. **Authoring dirties the git tree.** `paths::Asset` IS the source tree in a
   dev build, so any world editing writes straight into the working copy. Known
   and normal here, but it means P7 has a review surface, not an export step.
6. **File size.** `DungeonWorld` is already split by category and the world tier
   should not land inside it — a `World`/`WorldMap` pair of its own from the
   start, and P3's view as its own file rather than a mode bolted into MapView.
