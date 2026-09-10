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
      project.ini              levels = <every stem>, and where the game begins
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

**BUILT (P3.5).** A journey now settles effects, regeneration and the stabilize
clock as well as supplies, IN SLICES of a simulated minute, through the SAME
`DungeonWorld::TickParty` the dungeon's own update calls — which is why the
party tick had to come out of `UpdateMonsters` after all. The extraction was a
MOVE, not a rewrite: the eval suites' numbers came out byte-identical, which is
what says the order inside it survived.

Two things fell out of doing it that were not in the plan:

**THE DANGER GATE BELONGED TO THE CALLER.** "A monster is within aggro" is a
fact about A LEVEL, and a travelling party is not in one — left inside the tick,
a stale dungeon's monsters were gating the recovery of a party days away.

**A DoT WAS BITING FOR THE WHOLE STEP.** `TickEffects` multiplied magnitude by
`dt` without asking how much of `dt` the effect was entitled to. At frame dt the
error is a rounding error and it went unnoticed for the life of the system; at
sixty-second slices a FOUR-SECOND bleed dealt sixty seconds of damage and killed
outright. Fixed where it was wrong rather than worked around here, and the
harness pins the exact number (a bleed of 6 for 4s costs 24 health, whatever
settles it).

**WALKING IS EXERTION**, and that one line is what makes travel dangerous rather
than restorative. Health regen is gated on the exertion signal the resources
model already has, and without it half an hour of road regenerated ~400 health —
enough to out-heal any DoT that was not lethal within the minute, so a poisoned
party arrived FULLER than it set out. You heal in CAMP, not on the road.

### Camp

**CAMP IS REST, reached from the world map** — not a second recovery model. It
turns the same state on and settles time until the same rules turn it off:
deprivation stops it, being fully recovered stops it, and the supplies it burns
are the ones the tick was always going to charge. It does NOT use rest's 60x
multiplier: that exists to make waiting bearable in real time inside a dungeon,
and out here world time is advanced directly, so an hour camped IS an hour.

`C` on the world map, or the `camp` dev command — which reports the REASON as
well as the hours, because "camped 0.0h" alone reads as a bug and is usually a
party too hungry to rest.

**A wipe on the road** ends the journey through the existing `onPartyWipe`
latch, and the slice loop stops rather than going on charging supplies to four
corpses.

The original note, kept because it is what was decided (Michael, 2026-09-09): *"DoT should still affect the
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

### In and out

**A DUNGEON HAS NO START OF ITS OWN** (Michael, 2026-09-09). It is a named
group of levels and nothing more. EVERY WAY IN CARRIES ITS OWN DESTINATION —
a world-map location says which level and cell it opens onto, and the game's
opening says the same in `project.ini`:

    start_dungeon = crypt      ; absent = the game begins on the world map
    start_level   = crypt1
    start_x       = 6
    start_z       = 9

There is therefore no `entry` field on a dungeon to drift out of step with the
doors that actually lead there, and a doorway that says nothing is a CHECKED
fault rather than a silent fallback.

**A LOCATION IS A DOORWAY, NOT A DUNGEON.** One dungeon may have several ways
in — a front gate and a back way that stands somewhere else entirely on the map
and comes out somewhere else inside it (Michael, 2026-09-09). So a location
names its dungeon rather than being one, and it names where it LANDS:

    location dungeon waystation      10 10
    location dungeon waystation_back  5 13 dungeon=waystation level=level2 entryx=9 entryz=4

`dungeon` absent means "the same as my id", so a single-entrance dungeon still
authors only its own name. `level` and the cell are not optional in the same
way: the checker names a doorway that omits them, because there is no longer
anything sensible for them to fall back TO. Naming one of `entryx`/`entryz`
without the other is refused outright — it would land on the level's start row
or column and read as "the entrance moved" instead of as the slip it is.

The level `.map` `P` glyph still exists and still means what it always did:
where the EDITOR drops you, and the arrival for anything that names no cell. It
is a level-authoring detail now, not a tier the world reaches for.

**An undiscovered location cannot be entered.** Discovery is a gate, not
decoration — a location the party has no idea exists should not be walkable
into, or finding it would mean nothing.

Coming back out is an **exit stair**: `stairs.cat` `exit = 1`, which makes a
stair step LEAVE rather than change level. **Its `dest` names the world
LOCATION it surfaces at**, not a level — reusing the record's existing field
rather than inventing a second one, because on an exit the destination simply
IS a location. An exit that names none surfaces wherever the party came in,
which is what a single-exit dungeon wants and needs no authoring.

That is what makes the back way work in both directions: go in by the back and
climb out of the front, and you emerge at the front gate. **Coming out of a
door discovers it**, which is how a back way is found from the inside.

`WorldState::atLocation` remains as the fallback — the location and not the
dungeon, for the same reason the two are separate everywhere else.

A NEW GAME opens on the world map when the project has one. A project with no
world still opens on a level exactly as before — that is what keeps the world
an optional tier rather than a requirement.

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

**Built (P5).** Travel rolls against the square ENTERED, and the chance is
`difficulty x hours x rate` — so terrain matters TWICE, which is the whole
reason it carries two numbers: an hour in the hills is more chances to be found
than half an hour on the road, and the road is safer per hour as well. The roll
happens after the step is complete, so an encounter is something that happens to
a party that has ARRIVED rather than one caught mid-stride.

The space is `generate::Run` with the area's difficulty for density and the
terrain's tags for the pool, rendered to `.map`/`.ent` TEXT by the same
`BuildLevelText` the editor's generator uses, and parsed by `DungeonMap::
FromText` / `DungeonEntities::FromText`. It carries the reserved stem
`~encounter` — no file has that name, and the leading `~` makes that structural
rather than a convention. It is installed as the ACTIVE level always: the stash
branch creates a level's slot by READING IT FROM DISK, which for a level with no
file is an abort, and an encounter has nowhere to be stashed to in any case.

**There is always something in it.** The generator's density is tuned for a
DUNGEON, where an empty room is breathing space between fights; asked for a
13-square encounter at low density it quite reasonably placed none at all. So
density has a floor, and if the roll still produced no one, a monster goes at
the far end. The road's safety is that an encounter is RARE — the roll already
said so — not that the one you get is empty.

The way out is an **exit stair** authored onto the arrival cell, so an encounter
is left exactly the way a dungeon is rather than by a second mechanism that
would need its own rules. Leaving returns the party to open ground, where it
stood: it came from no doorway, so `atLocation` is empty and the fallback is
already right.

Dev: `encounter [difficulty]`, `encounters [on|off|<rate>]` — which reports the
rate and the switch SEPARATELY, because a rate of zero and "switched off" are
different states and a reader has to be able to tell which they are looking at.

**Still parked:** what a mid-encounter save should DO. Saving is now REFUSED,
and that is not an answer — it is the guard that stops the absence of an answer
becoming a corrupt file, since a save naming `~encounter` would reload into a
level that no longer exists and cannot be rebuilt. Refusing is recoverable;
writing it is not. Determinism means a seed plus params WOULD reproduce the
space exactly, if that ever turns out to be the answer.

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

**The version starts again at 1** (Michael, 2026-09-09). A ladder of v1..v26
migration notes used to stand in `SaveGame.h`, each rung explaining how to read
the rung below — and none of it can be read any more, because the world tier
re-cut what a save IS. Keeping twenty-six comments about migrating files that
cannot exist would be a museum, not documentation; the notes are in the git
history if a format question ever needs archaeology. The v6 floor-snapshot
compat path went with them, field and both branches: dead code kept for saves
that can no longer exist is just weight.

`kMinReadableVersion` is 1 and anything else is refused, with a log line naming
both versions, rather than half-loaded. A refused save then vanishes from the
load list, since `ListSaves` keeps only what `ReadSave` returns: an entry that
cannot be loaded is worse than no entry, and the log is where the reason lives.

## The content, and the harness's ground

The demo project is now a small **starter dungeon** — `crypt`, two floors,
reached by a gate at 10,10 and a back way at 5,13 — plus one level that is not
content at all.

**`eval_arena` is the HARNESS's ground.** 28x24 and open, because that is what
the suites were written against: `arena.eval` carves its shapes centred on
14,12 and hardcodes those coordinates, and `smoke.eval` stands at 12,8 and
takes twenty seconds of whatever is nearby. An open room means every `tp` lands
and every carve has room, so a test never fails for a reason belonging to the
scenery.

It exists because a hand-authored SHOWCASE level used to serve that purpose,
which meant content could not be redecorated without moving measurements. The
suites' numbers came out identical across the swap, which is the evidence that
the substrate was all they ever needed. Harness levels are named `eval_*`,
belong to the `eval` dungeon (so the checker has a coherent world rather than a
permanent orphan warning), and `project.ini`'s `eval_level` names the one the
harness opens in — not "whichever is first", so the suites do not move when the
level list is reordered, and so more can join.

## What this does not cover

- **Towns and shops.** They need money and trade, none of which exist. A
  location kind is reserved for them and nothing else — a known to-do, out of
  scope here.
- **A world editor.** Undecided. If it happens it is the existing editor one
  tier up (paint terrain, place locations, reusing the palette, brushes and
  undo snapshot), and it is worth deciding only once the data has settled.

## Obligations on later phases

Two, recorded here because both are the kind of thing that passes silently:

1. **`reset` still means A LEVEL** — DISCHARGED in P4. The harness sets
   `m_harnessOpensInLevel` BEFORE calling `onStartNewGame`, so it states what
   it wants instead of inheriting the opening, and the two can now differ on
   purpose.

   It was first written the other way — start the game, then enter a dungeon —
   and that CRASHED. On a cold boot `onStartNewGame` queues the world, the
   portraits AND the HUD; entering a dungeon immediately afterwards calls
   `BeginLevelTransition`, which clears the queue and re-stages the world half
   alone, so the HUD was never built and the first frame dereferenced its
   widgets. P2 left exactly that lesson about exactly that function. It came
   back wearing different clothes.
2. **The alloc guard only arms in `Playing`.** A travel frame is unwatched
   until `SteadyStateFrame` learns about the new state, and a world map that
   allocates per frame is exactly what would go unnoticed. `uioverlap` must
   sweep the new screen too, like every other screen.
