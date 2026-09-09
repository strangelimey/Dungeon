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
    start 6 6
    area lowlands 2 5 20 11 difficulty=0.12
    location dungeon waystation 10 10
    ;
    ~~~AAAAAAAAA^^^^AAAAAAAA
    ~~~^^^^^^MMMM^^^^AAAAAAA
    ...

A location record names a **dungeon id**, not a level stem — that is the tier
boundary made syntactic.

**There is no terrain palette record**, and that is the one place the world
deliberately does NOT copy a level (settled while building P1). A level's
`variant` records store the palette INDEX, which makes the palette append-only
forever: insert an entry and every cell above it silently repaints. That trap
only exists because a level needs its own ordered subset of a shared catalog.
The world does not — there is exactly one per project — so a terrain kind
declares its own `glyph` in terrain.cat and the grid is read through that.
Glyphs carry no ordering, so terrain can be renamed or reordered freely.

An **area** is a rectangle: `area <id> <x> <z> <w> <h> [difficulty=]`. Areas are
tested in file order and the LAST match wins, so a broad region can be authored
first and exceptions carved out of it afterwards. `start <x> <z>` is where a new
game puts the party.

### Where the party is

A new `AppState::WorldMap`. Not an overlay like `MapView`: the map overlay is
a *view of where the party is*, and this is *where the party IS* — at world
coordinates, with no level loaded. `Game::Render` already has the precedent
for a full-screen 2D screen that skips the shadow and scene passes (the
`editorMap` flag).

`WorldMapView` is its own class rather than a third `MapView` mode. What the
two share is the LOOK — MapColors' ink and the same fit-to-view pan/zoom feel
— and that is a convention worth copying rather than a base class worth
extracting from two objects with almost no state in common. It DRAWS and it
PICKS; it does not travel. A keypress becomes a direction and `Game` turns
that into a journey, which is what lets the harness travel with no window.

Two consequences of the state existing at all, both found by adding it:

- **Pause and the sheet can now be opened from two places.** `m_resumeState`
  records which, because an unconditional "resume means Playing" would quietly
  teleport a travelling party into whatever level was last loaded.
- **The eval harness's end-state guard had to learn the state.** It exists to
  catch a run that FELL OUT of the game (a wipe returns to the title screen,
  where every dev command keeps answering normally). A travelling party has
  not fallen out, so `WorldMap` counts as in play — widened deliberately, at
  the one place that decides it.

**Not covered by the allocation guard**, and that is the existing precedent
rather than an oversight: `SteadyStateFrame` already excludes the map overlay,
and the world map is the same kind of screen — formatted captions, redrawn on
demand, no simulation behind it. Worth revisiting when the world map becomes
the primary way the party spends time, which is P4's doing, not P3's.

### Time, and what a journey costs

**Two cost models, deliberately** (Michael, 2026-09-09). In a DUNGEON time runs
continuously and everything ticks off it, exactly as now. On the WORLD MAP a
journey has a DURATION: the party commits to a move, the world works out how
long that takes, and the costs are settled for that span rather than accrued
frame by frame. So travel is **not** another multiplier on `Game::Update`'s
time seam — it does not run the dungeon's clock fast, it settles a bill.

That keeps the party tick where it is. An earlier draft of the plan called for
lifting it out of `DungeonWorld::Update` so a travelling party would keep
eating; that is NOT needed, because nothing ticks on the world map — a journey
is RESOLVED, not simulated.

**What must not drift is the arithmetic.** Two cost models are two chances to
disagree about what an hour of walking costs, and rest already taught the
lesson: rates stay coherent because exactly one place owns them. Most of that
is already true here — the drain maths lives in the PURE `resource::` TU
(`DrainPerSec(rules, practice)`, which includes only `Core/Types.h` and
`Curve.h`), and `TickSupplies` is merely its per-frame caller. A journey can
ask the same function for a large span and get an answer that CANNOT disagree
with the dungeon's. Lifting the rest of the cost loop out of `DungeonWorld` is
deferred — his call — and is cheaper than it looks for the same reason.

**DECIDED, NOT YET BUILT** (Michael, 2026-09-09): *"DoT should still affect the
party members. Travelling on the world map COULD easily kill affected members."*
So a journey must tick effects — and regeneration and the stabilize clock with
them, since they are the same kind of state and a journey that burns you but
never heals you is not a model of anything.

Today it settles SUPPLIES and world time only, and says so at the settlement
site rather than pretending otherwise. A poisoned party still travels for free
until the work below lands.

**WHAT IT COSTS, and why it is not a two-line change:** the party's effect tick
lives INSIDE `DungeonWorld::UpdateMonsters`, interleaved with the monster loop.
Calling it would run the AI, so honouring the decision needs the PARTY-TICK
EXTRACTION this plan originally called for and then dropped — dropped on the
grounds that nothing ticks on the world map, which this decision reverses. The
same work, for a different reason. Two properties must survive it: the ORDER
inside that block is load-bearing and commented as such (supplies before
effects, so an emptied meter bites on the same frame), and every health write
in it is bracketed by the damage ledger, which is what makes the one-pipeline
rule checkable.

It must also be settled in SLICES rather than one lump, or the ordering
question the parked design note raised comes back: a DoT that would kill
someone three hours into a six-hour march has to kill them there, not at the
end, and an unconscious member has to come round at the hour it happens.

**A DEATH DURING TRAVEL IS A NEW PATH**, and P4 has to cope with it: a party
that wipes on the world map is not in a level to wipe out of.

### Camp — flagged, not designed

A **camp** button on the world map: rest, take potions, deal with what the road
has done to you (Michael, 2026-09-09 — flagged and deliberately not designed
here). It is the counterweight to the decision above; travel that can kill needs
somewhere to recover that is not "walk to a dungeon and hope". Rest already
exists as a STATE that multiplies time at one seam, so camp is likely that same
state reached from here rather than a second recovery model — but that is a
guess, and the design belongs to whoever picks it up.

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

### What a step costs

**The cost of a step is the cost of the square you ENTER**, not an average of
the two. It is the rule a player can read straight off the map — the caption
quotes exactly that number for the square under the cursor — and a rule you can
see before you act is worth more than a smoother one you cannot.

A step then advances world time by that many hours and settles the same span's
supply cost through `resource::DrainPerSec`, the same pure function the
per-frame dungeon tick calls. Measured: three road squares (0.5h each) plus
grass (1.0h) plus moor (1.4h) is 3.90h, and the party's water falls at exactly
the rate the dungeon would have charged it.

Walking into water REFUSES rather than clamping, and a multi-step travel
command reports what it did rather than what it was asked — a run that silently
came up short is how a test measures the wrong journey.

**Discovery falls out of seeing the ground.** A step reveals its cell and the
eight around it (the same reach `DungeonWorld::MarkSeen` uses underground), and
a location standing on any revealed cell becomes discovered. The other way in —
a map or a clue naming a location outright — writes the same list without
touching `seen`, which is exactly why the two are separate fields.

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

One file still (v26), with a GLOBAL section above the per-level states:

    world 0 3 12 0.000        on-world-map flag, cell, elapsed hours
    worldseen 6,6 3,12        revealed world cells - the world's fog
    discovered waystation     location ids the party knows
    flag <key> <value>        global/quest state (P6 gives the keys meaning)
    level showcase            ...then the per-level blocks, unchanged

The global lines are written BEFORE the level blocks because a `level <stem>`
header captures every following line: anything global has to be stated while
no level block is open.

**The per-level states stay keyed by STEM.** The plan called for them to become
per-dungeon-per-level and building it said not to: a stem already names a level
uniquely, and the checker refuses a level claimed by two dungeons, so which
dungeon a state belongs to is DERIVABLE. Storing it as well would be a second
source of truth that can disagree with the first. The global/local split the
design asks for is about WHAT IS SAVED WHERE, not about nesting.

`WorldState` (WorldMap.h) is ONE type used as both the runtime truth and the
save record. A parallel pair would need conversion at two points, and
conversion code between two structs with the same fields is exactly where a
field gets added to one side and forgotten on the other.

Old saves are **refused** at the version bump, with a log line naming both
versions, rather than half-loaded — "early saves are WIP only" (Michael,
2026-09-09). The read path gained a FLOOR (`kMinReadableVersion`) rather than
another compat rung. A refused save then vanishes from the load list, since
`ListSaves` keeps only what `ReadSave` returns: an entry that cannot be loaded
is worse than no entry, and the log is where the reason lives.

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
