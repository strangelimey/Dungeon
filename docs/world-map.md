# The world map

**Status:** PLAN (Michael, 2026-09-09). Nothing built yet. This is P0 of
`docs/world-map-plan.md`; the raw brain dump it answers to is
`docs/world-map-notes.md`.

An overworld above the dungeons. The party travels it, discovers dungeons on
it, is ambushed while crossing it, and carries quest state that outlives any
one dungeon.

## Why

Everything the game holds today is one level deep. A save names a level stem
and carries per-level dynamic diffs; the project manifest is a FLAT list of
level stems; a "dungeon" exists only as an accident of which stems' stairs
happen to point at each other. There is no place to put a fact about the
*world* — a quest flag, a discovered location, where the party is when it is
not in a dungeon at all.

The cost is not tidiness. It is that **a quest item cannot mean anything past
the level it was found in**, a dungeon cannot be found (only listed), and
there is nowhere for a random encounter to come from, because nothing knows
what part of the world the party is crossing.

## The shape

### The world is a level

The one idea, and Michael's own words for it: *"like a level, there's content
and then a diff for the save"*. Taken literally it settles most of the design,
because every mechanism a level needs is already built and can be used a tier
up rather than paralleled:

| A level has | The world gets |
| --- | --- |
| a `.map` grid + records | a world grid + records, same dialect |
| a surface palette resolved through catalogs | a terrain palette, same shape |
| fog of war (a revealed-cell set) | discovery (the same set, one tier up) |
| placed things as records | locations as records |
| dynamic state as a save diff | world state as a save diff |
| a top-down view with baked icons | the world map view |

    project (a GAME)
      world            terrain grid + locations + areas     -> save diff
        dungeon        a named group of levels              -> save diff
          level        today's .map/.ent                    -> save diff

The **dungeon** tier is the only genuinely new container.

### Vocabulary

Fixed here so the code, the catalogs and every later doc agree:

- **World** — the whole overworld: one grid, one authored file, one save
  section. There is exactly one per project.
- **Terrain** — what a world cell IS (forest, moor, road). A catalog kind,
  carrying travel cost, difficulty and encounter tags. The world's palette.
- **Area** — a named region of the world that overrides its terrain's
  difficulty. Optional; terrain alone is enough for a plain world.
- **Location** — a thing standing on a world cell that the party can enter or
  interact with: a dungeon entrance, later a town. Discovered or not.
- **Dungeon** — a named group of levels with an entry level. Reached through
  a location.
- **Level** — unchanged: today's `.map` + `.ent` pair.
- **Encounter** — a throwaway space built on the fly when travel is
  interrupted. Never saved, never named, never in the project.

Note that **a dungeon is not a place**: the location is the place, the dungeon
is the content behind it. That split is what lets a found map reveal a location
without the dungeon itself changing, and what would later let two locations
share one dungeon.

### Files

    assets/projects/<name>/
      project.ini              unchanged; levels = <every stem>
      world/world.map          the authored world
      catalog/terrain.cat      terrain kinds (the world's palette)
      catalog/dungeons.cat     dungeons: their levels and entry
      catalog/quests.cat       quest definitions

`project.levels` **stays** the flat list of every level in the game. It is the
editor's universe — the level dropdown, the stair `dest=` sweep,
`SweepTypeRefs` — so the dungeon tier is purely ADDITIVE: a dungeon names its
levels, and the flat list still knows them all. Nothing that works today
breaks on the day dungeons appear.

`world/world.map` speaks the level dialect (`;` comments, whitespace-tokenised
records, lowercase records above an uppercase grid), because the parser
primitives already exist and a second dialect would be a second thing to learn:

    ; the overworld.
    palette terrain moor forest road
    ;
    area lowlands difficulty=0.2
    location dungeon crypt 14 9
    location dungeon barrow 3 21
    ;
    MMMFFF...

A location record names a **dungeon id**, not a level stem — that is the tier
boundary made syntactic.

### Where the party is

A new `AppState::WorldMap`. Not an overlay like `MapView`: the map overlay is
a *view of where the party is*, and this is *where the party IS* — at world
coordinates, with no level loaded. `Game::Render` already has the precedent
for a full-screen 2D screen that skips the shadow and scene passes (the
`editorMap` flag).

**This is what costs the most work**, and it is worth stating plainly because
it is invisible from the feature description: supplies, effects, regen and the
exhaustion latch all tick inside `DungeonWorld::Update`, and travel costs time
— so a travelling party would silently stop eating, stop burning and stop
healing. The party tick has to come OUT of the dungeon and become something
both callers run. It is the honest fix rather than keeping a level loaded to
nurse a timer, and it is cheap only because `DungeonWorld` already holds the
roster by POINTER rather than owning it.

Time itself needs nothing new. `Game::Update` scales it in exactly one place —
`wdt = dt * m_timeScale * m_world.RestTimeScale()` — and REST already proved
the pattern: multiply time at a single seam and every rate, timer and cooldown
moves together, so no second set of travel rates can drift out of step with
the dungeon's.

### The grid

The world is a grid. Michael's answer was "probably, not sure yet", and the
reasons to commit are: movement, line of sight and projectiles in this engine
are 4-cardinal by rule; discovery wants exactly the revealed-cell set fog of
war already is; and the `.map` dialect, the top-down renderer and the icon
bake are all grid-shaped.

The escape stays cheap deliberately. Everything outside `WorldMap` asks it
QUESTIONS — what is at this position, what does travel here cost, is this
discovered — rather than indexing its cells, so a later switch to free
positions is one class rather than a sweep.

### Discovery

A location is either discovered or not, and that is dynamic state: it lives in
the save, never in `world.map`. Two ways in, both the same write:

- **Exploring** — the party's travel reveals what it passes, the same way
  walking a dungeon reveals cells.
- **A map or a clue** — a found item names a location and reveals it outright.
  This is why locations have ids.

### Encounters

Travel may be interrupted. The encounter space is built from the area's
difficulty and its terrain's tags, is entered like a level, and is **thrown
away** — it is never written to the project, never named, never saved.

Most of this exists. `generate::Run` is pure and deterministic (same seed and
params, same level, every time) and emits ORDINARY content with no "generated"
flag — which is exactly what makes every existing tool work on the result.
`BuildLevelText` already renders a generated level to `.map`/`.ent` TEXT, and
catalog `tags` is already the pool-matching axis, resolved by the caller so
the generator stays pure.

**The one missing seam:** `DungeonMap` and `DungeonEntities` construct from a
PATH only. A level that must never touch disk needs from-text construction —
which is the same "writes no files" property `DungeonWorld_Arena` already
depends on, and the reason that file exists.

**Parked:** what happens if the player saves mid-encounter. Michael's answer
was that it needs handling differently, and he chose not to decide. Recorded
here only so it is not accidentally designed around: determinism means a seed
plus params WOULD reproduce the space exactly, if that ever turns out to be
the answer.

### Quests

Quest **definitions** are a catalog; quest **state** is global save state.
Thin on purpose — the dump asks for flags a found item can set, and for
quests "tracked globally with their own state". No journal UI was asked for,
and the discovery-by-clue hook is the same write discovery already uses.

### The save

One file still, with a WORLD section above the per-dungeon, per-level states:

    world      party x,y; world time; discovered locations; quest state
    dungeons   per dungeon, per level: the diffs that exist today

Old saves are **refused** at the version bump, with a clear message, rather
than half-loaded — "early saves are WIP only" (Michael, 2026-09-09). So the
read path gains a FLOOR rather than another compat rung; the version comment
block records where the ladder was cut and why.

## What this does not cover

- **Towns and shops.** They need money and trade, none of which exist. A
  location kind is reserved for them and nothing else — a known to-do, out of
  scope here.
- **A world editor.** Undecided. If it happens it is the existing editor one
  tier up (paint terrain, place locations, reusing the palette, brushes and
  undo snapshot), and it is worth deciding only once the data has settled.

## Obligations on later phases

Two, recorded here because both are the kind of thing that passes silently:

1. **`reset` still means A LEVEL.** The eval harness resets to "where a new
   game would leave it", via `m_ui.onStartNewGame`. The day a new game opens
   on the world map, ten suites follow it there and keep passing while
   measuring nothing. The phase that moves the opening must give the harness
   its own explicit entry.
2. **The alloc guard only arms in `Playing`.** A travel frame is unwatched
   until `SteadyStateFrame` learns about the new state, and a world map that
   allocates per frame is exactly what would go unnoticed. `uioverlap` must
   sweep the new screen too, like every other screen.
