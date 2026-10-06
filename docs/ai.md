# Monster AI

Design notes for how monsters perceive, decide, and act. This is the living
reference for the dungeon's AI — both what the **code does today** and where we
want to take it so behaviour becomes *content* (authored in the editor) rather
than C++ baked into one function.

Sections marked **Current implementation** record what the code actually does
now; sections marked **Target design** are the proposals it was built from (and
what of them is still open). Keep this doc in sync with the work as it lands
(like `docs/movement.md` and `docs/magic system.md`).

The starting point for this doc (2026-07-01) was that `DungeonWorld::UpdateMonsters`
(and the brain it drives, `src/Game/MonsterAI.{h,cpp}`) was **one behaviour,
hard-coded**: every monster ran the identical decision logic and only the
*numbers* differed per type. Layers 1 and 2 of the target design have since been
built - six archetypes chosen as data, four intent modes, per-instance sleep /
leash / patrol / behaviour overrides, and an editor dialog for each layer - so
"Current implementation" below describes that. "The gap" and "Target design" are
kept as the plan they were; "Suggested phasing" records what landed, and what
did not (call-for-help and the group leader, hearing, an authorable cone).

---

## Current implementation

### Where it lives

- **`src/Game/MonsterAI.{h,cpp}`** (`namespace dungeon::ai`) — the brain, walled
  off from the rest of the game exactly like `MagicSystem`. It knows nothing
  about `DungeonWorld`, the `Monster` struct, the map, the party, the HUD, or
  audio. It reaches the world through one read-only seam (`ai::IWorldView`) and
  flat value structs (`Agent`, `Snapshot`, `Intent`, `Plan`).
- **`DungeonWorld::UpdateMonsters`** (`src/Game/DungeonWorld.cpp`) — the host
  side: it builds the snapshot, consumes the brain's plans (`ConsumeAIPlans`),
  and **executes** them every frame on the main thread (movement glide, facing,
  slot/formation, attacks, animation, cooldowns). The executors that need no
  path, the ranged attack and the melee pick live in `DungeonWorld_Combat.cpp`
  (`UpdateKiter`, `UpdateFleer`, `UpdatePatroller`, `UpdateReturner`,
  `MonsterRangedAttack`, `PickMeleeVictim`, `MonsterAttack`).
- **`assets/projects/<project>/catalog/monsters.cat`** — the per-type data
  (stats + the behaviour fields), parsed in `DungeonWorld_Load.cpp`
  (`MonsterKindFor`); the `.ent` record's per-instance overrides are read in
  `LoadMonsters` beside it.
- **The editor**: `MonsterConfigDialog` (a type's Behavior and Animation tabs,
  writing `archetype` / `keeprange` / `fleebelow` / `spell` to monsters.cat) and
  `EntityInspector` (a placed monster's overrides and patrol route, writing the
  `.ent` record).

### THINK vs ACT (and why it's threaded)

The single most important structural idea, and the one any new design must
respect: **thinking is split from acting, and thinking runs off the main
thread.**

- **THINK** (cheap, infrequent, IQ-gated): `Brain::Think` decides a monster's
  *standing orders* - an `ai::Intent` - and, for Engage, `Brain::FindPath`
  computes a full chase **path** (4-connected BFS). Both are *pure*: they only read an immutable
  `ai::Snapshot` through `IWorldView` and write outputs, which is what makes them
  safe to run on worker threads.
- **ACT** (every frame, at the monster's own cadence): the main thread *executes*
  those standing orders — pops the next path cell (re-validating it against
  **live** occupancy), commits the step, glides the visual, turns to face, and
  swings when in range.

So a dim monster still moves and swings at full speed; only its *change of mind*
lags. `ai::AsyncDirector` owns one worker thread **per IQ bucket** (4 buckets),
each waking on a prime-millisecond cadence (251/499/997/1999 ms ≈ 4/2/1/0.5 Hz —
coprime so they don't resonate). The main thread `Publish()`es a pooled snapshot
once per frame and `TakePlans()` to adopt the freshest batch. Plans are keyed by
a **stable `runtimeId`** so a plan whose monster died/moved buckets/was erased
simply finds no match and is dropped. LOCKSTEP (`AsyncDirector::SetLockstep`,
used by the eval harness and while the party rests) pauses the workers and has
the host run each bucket inline (`ComputeInline`) on SIM time, so a fast-forwarded
world still thinks at the bucket rates. (Full threading rationale is in
`CLAUDE.md` → "Threading & async monster AI".)

**This pipeline is infrastructure, not behaviour.** It should survive any
redesign unchanged — what changes is *what `Think` decides* and *what the host
knows how to execute*.

### The decision logic

`Brain::Think` decides in this order, from the `ai::Agent` the snapshot carries:

1. **Leash** (per instance, `.ent` `leash` / `leashfrom`): a monster more than
   `leash` cells (Chebyshev) from its anchor - `leashfrom`, else its spawn -
   goes Idle, so the host walks it home. It applies even to an aware monster.
2. **Range**: the party must be within `aggro` cells (Chebyshev). A DORMANT
   monster - the `lurker` archetype, or a placement with `asleep=1`, until it is
   aware - is triggered only within 2 cells (`kAmbushTrigger`), and once sprung
   pursues at full `aggro`.
3. **Perceived** = already `aware` (sticky - it chases round corners) **or** a
   clear ORTHOGONAL line to the party (`IWorldView::HasLineOfSight`: a shared row
   or column, no wall between) **and** either no front (`faces = 0`, or the
   `swarm` archetype) or the party inside its sight cone: **±60°**, a `sentry`'s
   **±90°**. So the party can sneak up behind a facing monster, or stay behind a
   wall, and it stays oblivious.
4. **Mode**: perceived and wounded below `fleebelow` (an hp fraction, 0 = never)
   -> `Flee`, whatever the archetype; else `skirmisher` / `caster` -> `Kite`;
   else `Engage`. The goal is the cell the host's formation pass assigned.

Awareness latches when the host consumes a non-Idle plan (`ConsumeAIPlans`) and
when the monster is hit (`ProvokeMonster`, which sets Engage at once); only a new
game or a reload clears it. **A hit wakes only the monster struck** - see "Not
built" below.

**Intent vocabulary** - four modes (`ai::Intent::Mode`). Only Engage carries a
path; the other three are host executors stepping greedily (`GreedyStep`, one
orthogonal cell at a time) from LIVE positions:
- `Idle` - hold, or walk a per-instance **patrol** route (`UpdatePatroller`:
  toward the next `patrol=` waypoint, wrapping), or, leashed and displaced, walk
  home (`UpdateReturner`).
- `Engage` - follow `Brain::FindPath`'s BFS route to the assigned attack cell,
  then melee.
- `Kite` - `UpdateKiter`: hold `keeprange` from the party, preferring a cell on
  its row or column within `aggro`, and shoot (`MonsterRangedAttack`) when the
  line is clear, in range and off cooldown - a plain bolt for a skirmisher, the
  named `spell` for a caster (`MagicSystem::FindSpell` -> `Spell::MonsterBolt`,
  so a volley or a burst too: the skel_mage / skel_mage_adept / skel_magus
  ladder). The shot flies down the shared axis, its lane slid onto the threat
  target's quadrant.
- `Flee` - `UpdateFleer`: step to the free neighbour farthest from the party; no
  attack; holds when cornered.

**Archetypes** (`ai::Archetype`, monsters.cat `archetype`, overridable per
placement) - six, each a bundle of the rules above rather than code of its own:

| archetype    | perception                         | mode it takes | executor   |
|--------------|------------------------------------|---------------|------------|
| `brute`      | ±60° cone (the default)            | Engage        | chase + melee |
| `skirmisher` | ±60° cone                          | Kite          | hold range, plain bolt |
| `caster`     | ±60° cone                          | Kite          | hold range, its `spell` |
| `swarm`      | no blind spot (line still needed)  | Engage        | chase + melee |
| `lurker`     | dormant until 2 cells, then relentless | Engage    | chase + melee |
| `sentry`     | ±90° cone                          | Engage        | chase + melee; pairs with a patrol + leash |

Things that stop a monster ACTING (it still animates, burns and can be hit): a
flare's `dazzle` effect, the eval harness's `freeze`, and a spawn still rising
(`Monster::spawnAnim`, a kit skeleton's spawn clip).

**Formation** (`AssignFormation`, host side, main thread): aware monsters are
spread around the party's walkable **orthogonal neighbours** ("attack cells") so
a group *surrounds* rather than conga-lines. Hysteresis (a monster already on a
free side holds it) keeps a formed ring stable. Sub-cell sizes pack multiple
monsters per cell via the slot grid (see `docs/movement.md`).

**Movement** (`UpdateMonsters`): follow the cached BFS path one cell per
`moveInterval`, claiming a free slot in the destination, gliding `visualPos` with
smoothstep. In-cell repositioning slides a monster toward the slot nearest the
party (front rank). Facing eases toward direction-of-travel, or toward the party
when stationary and aware.

**Attack** (`MonsterAttack`): when a monster stands on its assigned attack cell
within its REACH - **orthogonally adjacent** (never from a diagonal), or, with
`reach = 2`, from its queue post down a clear shared row or column - it swings
every `attackcd`. WHO it swings at is `PickMeleeVictim`: the per-file blocking
rule first (a standing near member shields the one behind; a fallen one opens
the file; a reach-2 pike skewers past), then the member holding its THREAT lock
once their threat crosses the threshold (`AddThreat` / `UpdateThreatLock`,
balance.cat plus monsters.cat `threat_*`), else a uniform-random reachable
member - so "random" holds only until somebody earns a grudge. The blow is an
`fx::DamageEvent::Blow` through `fx::Deal` (docs/effects.md), its accuracy
scaled by the kind's `offense` stance.

**Animation** (`DriveMonsterAnim`): clip state machine, priority `die > attack >
walk > idle`, cross-faded; degrades gracefully when a rig lacks a clip.

### What is data-driven

- **Per type**, `monsters.cat` (parsed in `MonsterKindFor`): the stats (`hp`,
  `damage`, `dmgtype`, `accuracy`, `defense`, `offense`, `armor`, `resists`,
  `powers`, `reach`, `attackcd`, `movecd`, the `on_hit` / `on_crit` /
  `on_fumble` procs and the `fumble` tables), the behaviour (`archetype`,
  `keeprange`, `fleebelow`, `spell`, `aggro`, `iq`, `faces`, `threat_scale` /
  `_threshold` / `_switch` / `_decay`) and the body (`size`, `flammable`, the
  model and animation rows).
- **Per placement**, the `.ent` record: `asleep`, `leash`, `leashfrom`,
  `patrol` (`x,z;x,z;...`), and overrides of `archetype` / `keeprange` /
  `fleebelow` / `spell` that fall back to the type's (`Monster::Archetype()` and
  its siblings), so a type edit still reaches an un-overridden placement.

### Not built

- **Call for help** (decision 5): a provoked monster does not wake its group -
  `ProvokeMonster` sets only the monster struck aware. A "group" today is only
  the monsters sharing a cell (`ReconcileGroups`, rebuilt every frame), used for
  slot placement; there is no leader / follower either.
- **Hearing / noise** (`hearing` in the Layer 1 sketch): nothing wakes a monster
  but range + line + cone, a hit, or its own trigger. (The light stones' monster
  tracks record their MAKER, so party noise and scent can join them later.)
- **An authorable cone or sight range** (`sightcone` / `sightrange`): the cone
  is ±60° / ±90° in `Brain::Think`, the range is `aggro`.
- **`Patrol` and `Cast` as intents**: patrol is an Idle behaviour of the host,
  and a monster's cast is the Kite executor's shot - a bolt-shaped spell only
  (`Spell::MonsterBolt`); a caster whose `spell` has no thrown form (a ward, a
  sight, a light) falls back to its plain shot.
- **Pathing for a patrol or a walk home**: both are greedy, so a wall between
  two waypoints can stall a sentry - lay waypoints densely.
- **Layer 3** (a behaviour graph): deferred, as decided.

---

## The gap

*As it stood on 2026-07-01, kept as the record of what the design set out to
close. Items 1, 2 (but call-for-help and regrouping), 4 (but the group leader)
and 5 (ranged and cast; AoE through a caster's burst) are closed - see "Current
implementation" and its "Not built" list. Item 3 is closed only for
line-of-sight and leashing.*

To define AI "in the editor and implement it in the world," we need a **data
model for behaviour**, not just stats. Concretely, the limitations then:

1. **One strategy for all.** Engage-and-melee is the only thing a monster can do.
2. **No richer intents.** No kiting/ranged, casting, fleeing at low HP, patrol
   routes, guarding a spot, calling for help, fleeing then regrouping.
3. **Perception is one fixed shape.** Cone angle, hearing/noise, line-of-sight
   through walls, leashing distance — all constants, none authorable.
4. **No per-instance behaviour.** The `.ent` record places a monster with a
   position/facing but can't say "this skeleton patrols A→B→C" or "starts asleep"
   or "is the group leader."
5. **Attacks are hard-coded melee.** No data path for ranged projectiles, spells
   (even though `MagicSystem` exists), AoE, or multi-attack profiles.

---

## Target design

The guiding constraint is the project's standing decision (memory:
*content-stays-data-driven*): **typed catalog fields + C++ behaviour primitives,
NOT a scripting language.** We defer general scripting (Lua) until behaviours
must combine freely in ways a curated set can't express. So the target is a
*curated, composable* model the editor can present as dropdowns + typed fields,
and the world executes with C++ strategies.

Three layers, smallest-commitment first. We can ship layer 1 alone and grow.

### Layer 1 — Behaviour archetype + typed parameters (recommended first step)

Add an **`archetype`** field to `monsters.cat` selecting one of a small set of
C++-implemented behaviour strategies, plus typed parameters that tune it. Each
archetype is a named bundle of perception + movement + attack rules.

Proposed starter archetypes:

| archetype    | perception          | movement                        | attack             |
|--------------|---------------------|---------------------------------|--------------------|
| `brute`      | cone + sticky       | surround + close to melee       | melee (today)      |
| `skirmisher` | cone + sticky       | keep at range N, kite if crowded| ranged projectile  |
| `caster`     | cone + sticky       | hold at range, reposition LoS   | cast a spell id    |
| `lurker`     | short cone, ambush  | stationary until provoked, then surround | melee     |
| `sentry`     | wide cone           | patrol a route, return on leash | melee/ranged       |
| `swarm`      | omnidirectional     | surround, no kiting             | weak melee         |

New typed fields (all optional, defaulted so existing `.cat` files keep working):

```
archetype   = brute | skirmisher | caster | lurker | sentry | swarm
sightcone   = degrees (default 120; 360 ≡ omnidirectional, replaces `faces=false`)
sightrange  = cells (rename of/alias for `aggro`)
hearing     = cells of noise radius that wakes it (default 0 = deaf)
leash       = cells from spawn before it disengages and returns (0 = none)
keeprange   = cells the skirmisher/caster tries to hold (ranged kiting)
fleebelow   = HP fraction at which it flees (0 = never)
attack      = melee | ranged | cast
projectile  = <item/projectile id>      ; for attack=ranged
spell       = <spell id from spells.cat> ; for attack=cast
```

**World side:** widen the intent vocabulary. `ai::Intent::Mode` grows beyond
`Idle/Engage` to something like `Idle, Engage, Kite, Flee, Patrol, Cast`.
`Brain::Think` picks the mode from the archetype + snapshot inputs (it stays
pure); `UpdateMonsters` gains an executor per mode (chase exists; add
keep-distance, flee-away, patrol-waypoint, fire-projectile/cast). Carry the
behaviour descriptor on `ai::Agent` so the brain reads it from the snapshot
(today `Agent` already carries `aggroRange`, `iq`, `directional`, `facingYaw` —
add the archetype enum + its few params).

This is the **80/20**: a fixed menu of behaviours, each a small C++ strategy,
fully selectable as data. It aligns with how the rest of the game is built
(typed catalog fields, e.g. items' `category`, decorations' `alpha_test`).

### Layer 2 — Per-instance behaviour on the `.ent` record

Some behaviour is per-*placement*, not per-*type*: a patrol route, an initial
state, a group leader. Extend the dynamic entity record (`<kind> <type> <x> <z>
[facing] [key=value...]`, see `Entity.h`) with AI overrides:

```
monster skeleton 5 7 south  patrol=5,7;9,7;9,11  asleep=1  leashfrom=5,7
```

These override/augment the catalog archetype for that one monster. The editor
already places monsters; this is additional typed fields on the placement.

### Layer 3 — Composable behaviour graph (only if Layer 1 proves too rigid)

If we hit combinations the archetype menu can't express, the next step is a
**data-authored behaviour tree / state machine** (nodes + transitions in a
`.bt`-style file or catalog block, with a node-graph UI in the editor). This is
the point at which the *content-stays-data-driven* memo says to reconsider
scripting. **Explicitly deferred** — do not build until Layer 1 + 2 are in use
and demonstrably insufficient.

### How the editor surfaces it

The editor is already catalog-driven (see `CLAUDE.md` → "Project & catalogs" and
the `MapView`/`MapEditor` sections). The Monsters palette category lists
`monsters.cat` ids; the "+ New…" row opens the asset-creation dialog. So:

- **Layer 1** needs the asset dialog to expose the new typed fields — an
  `archetype` dropdown and its dependent params (a few sliders/dropdowns,
  shown/hidden by archetype, like the Video tab's dependent display lists). No
  new editor *concept*, just more fields on a form that already writes
  `monsters.cat`.
- **Layer 2** needs the monster placement (brush-placed instances) to carry
  per-instance overrides — a small inspector for the selected entity that writes
  the `key=value` pairs into the `.ent` record. The Select tool is the natural
  home; an entity inspector panel is a generally-useful editor addition.
- **Layer 3** would need a node-graph editor — a much larger surface, hence
  deferred.

### What does *not* change

- The async THINK/ACT pipeline and the thread manager — behaviour redesign is
  about *what* `Think` decides and *what* the host executes, not *how* the work
  is scheduled. `Snapshot`/`Plan`/`runtimeId` keying, bucket cadences, the
  snapshot pool, and the dev-console THREADS panel all stand.
- Combat resolution (`Combat.cpp`), the slot/formation system
  (`docs/movement.md`), animation (`DriveMonsterAnim`), and save round-trip
  (`CaptureState`/`ApplyState`) — though new per-instance/awake state may need
  to join the save side (memory: *save-load-new-dynamic-state*).

---

## Decisions (settled 2026-07-01)

The open questions have been answered; these constraints now drive the build.

1. **First archetype: `skirmisher`.** It ships first to prove the pipeline —
   deliberately the hardest one, because it exercises *both* a new movement
   executor (keep-distance / kite) *and* a new attack mode (ranged projectile).
   If skirmisher works end-to-end, the rest of the table is mostly filling in
   strategies. `brute` (= today) is the zero-regression baseline it sits beside.
2. **One shared "moving-item" engine for ALL projectiles.** Do NOT give monsters
   a separate projectile path. Instead, extract projectile flight out of
   `MagicSystem` into a standalone engine (working name `ProjectileSystem`) that
   both a spell cast and a monster ranged attack spawn into. A projectile is a
   generic *moving item* whose **properties** (speed, range, size/visual, gravity/
   arc later, and a **target side** — who it may strike) determine how it moves
   and what it hits. `MagicSystem` keeps only recipe/mana/cast resolution and
   *emits a projectile spec*; the world spawns it into the shared engine.
   - The current `resolveHit` hook only tests monsters (`ResolveSpellHit`). The
     shared engine must be **faction-aware**: a party spell resolves against
     monsters; a monster ranged attack resolves against the party (mirror of
     `MonsterAttack` — strike a random standing member). Carry a `TargetSide`
     (Party | Monsters) on each moving item.
   - Rationale: reuse over duplication, and it sets up thrown items / traps /
     future emitters riding the same engine. Accepted cost: the two systems
     couple through one shared primitive (which is the point).
3. **Patrol routes = waypoint lists on the `.ent` record** (Layer 2), not a named
   route asset. Simplest thing that works; a route asset can come later if reuse
   demands it.
4. **True line-of-sight** (walls block sight), not the cheap range+cone test
   alone — but **ORTHOGONAL-only**: the dungeon is a 4-directional grid, so sight
   (and shooting) runs only straight down a shared row or column, never diagonally.
   This matches the party's cardinal spell bolts and orthogonal melee (see the
   *orthogonal-grid-rule* memory). Exposed on `ai::IWorldView::HasLineOfSight`
   (true iff `x0==x1 || z0==z1` with every cell between walkable; endpoints never
   block). `Brain::Think` uses it for perception AND the skirmisher "line up on the
   axis to shoot." Reads only the immutable snapshot, so it stays pure/thread-safe.
5. **Group behaviour rides the archetype** ("both"): leader/follower AND
   "call for help" (a provoked monster propagates awareness to its group) are
   archetype fields/flags, not a separate concern. Groups are already derived
   every frame in `ReconcileGroups`, so the executor keys off that.
   **NOT BUILT** - neither half: `ProvokeMonster` wakes only the monster struck,
   and a `ReconcileGroups` group is only the monsters sharing a cell.
6. **Editor inspector: Layer 1 first (type-only), Layer 2 later.** Start by
   surfacing the new typed fields in the asset-creation dialog (type-level); the
   per-instance entity inspector comes with the Layer 2 `.ent` overrides.

---

## Suggested phasing

Reflecting the decisions above, P1 is split so the risky refactor lands behind a
regression guard before any new behaviour rides on it.

1. **P1a — Shared moving-item engine. [DONE]** Extract projectile flight out of
   `MagicSystem` into a standalone, faction-aware `ProjectileSystem` (moving item
   = pos/dir/speed/range/visual + `TargetSide` + `AttackProfile`). Reroute spell
   casts through it with **zero behaviour change** (same bolts, same hits) — the
   regression guard. `MagicSystem` shrinks to recipe/mana/cast → projectile spec.
2. **P1b — Perception LoS. [DONE]** Added `HasLineOfSight` on `ai::IWorldView`
   (`SnapshotView`, ORTHOGONAL-only — clear iff on a shared row/column; pure).
   `Brain::Think` now takes the view and LoS-gates FRESH detection (walls block
   sight; sticky `aware` still chases around corners); movement/attack unchanged.
3. **P1c — Skirmisher end-to-end. [DONE]** Add the `archetype` enum + skirmisher params
   to `Agent`/`MonsterKind`/`monsters.cat`; widen `ai::Intent::Mode` (add `Kite`);
   `Brain::Think` picks Engage vs Kite from archetype + LoS + range; host gains a
   keep-distance executor and a ranged-attack executor that spawns a monster
   projectile (`TargetSide::Party`) into the shared engine. `brute` stays the
   untouched baseline. Proves the whole data path.
4. **P2** — Fill out the archetype table (flee/patrol/cast/lurker/sentry/swarm
   executors + group leader/call-for-help); surface the fields in the
   asset-creation dialog. **In progress:** `caster` DONE (kites like a skirmisher
   but its bolt is a named `spells.cat` spell fired through the shared engine —
   `SpellBook::Find`/`MagicSystem::FindSpell` + a `spell` catalog field).
   Flee-at-low-HP DONE (`Intent::Mode::Flee` + `fleebelow` HP-fraction field + a
   `UpdateFleer` run-away executor; the brain decides it from `hpFrac` in the
   snapshot). `swarm` DONE (Engage + bundled omnidirectional perception) and
   `lurker` DONE (ambush: short wake trigger, then full-aggro relentless pursuit) —
   both reuse the brute executor, no new code path. Five archetypes now data-
   selectable (brute/skirmisher/caster/swarm/lurker) + the fleebelow modifier.
   Editor authoring DONE: the monster-config dialog is now TABBED (ui::TabControl,
   like the settings page) — a Behavior tab (archetype dropdown + dependent
   keep-range/flee-below/spell controls) beside the Animation tab; edits apply live
   and Save writes the archetype/keeprange/fleebelow/spell rows to monsters.cat.
   Added a ui::Checkbox widget for the animation rows. P2 complete except `sentry`
   (deferred - needs P3 patrol routes; it landed in P3b) and the group leader /
   call-for-help, which were never built (decision 5).
5. **P3** — Per-instance `.ent` overrides (Layer 2). The `.ent` record already
   parses arbitrary `key=value` params (Entity.params/Param), so this is about
   CONSUMING them at spawn + an editor inspector to author them. Decisions (2026-
   07-01): tabbed-dialog-style inspector (like the monster-config dialog); patrol
   routes authored by CLICKING grid cells; ship in two steps.
   - **P3a** DONE — per-instance `asleep` (dormant until provoked/close, a per-
     placement lurker) + `leash`/`leashfrom` (disengage + walk home when pulled too
     far), consumed in LoadMonsters, carried on Monster + into the AI snapshot,
     written back by the .ent writer. Plus the EntityInspector (Select-click a
     placed monster → tabbed AI panel): edits its overrides live, Save writes the
     level .ent. Behaviour is overridable PER INSTANCE too (archetype + keeprange/
     fleebelow/spell) as std::optional overrides that fall back to the type default
     (so a type edit still flows to un-overridden placements); the AI reads through
     Monster::Archetype()/KeepRange()/FleeBelow()/Spell() accessors.
   - **P3b** DONE — patrol as a HOST-side executor (UpdatePatroller: greedy step to
     the next waypoint, advance/wrap on arrival; no AI-layer change) driven by a
     per-instance route; the `sentry` archetype (wide ±90° cone + patrol/leash). The
     editor gained a SELECTED-SQUARE concept (MapEditor selection = cell + creature
     runtimeId): a Select-click selects (high-contrast outline) and draws the
     selected creature's route as high-contrast ARROWS; a second click opens the
     inspector. Routes are authored by grid-click (inspector Patrol tab → "Edit
     route on map" → click cells; Backspace undo, Enter done); the .ent writer
     round-trips `patrol=`. **P1–P3 complete.**
6. **P4** — (Deferred/optional) behaviour-graph authoring (Layer 3) only if
   needed.
