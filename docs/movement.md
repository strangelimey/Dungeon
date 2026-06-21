# Movement

Design notes for the dungeon's party movement. This is the living reference for
how grid-locked stepping, turning, free-look, and their feel fit together. Start
here.

This document is kept in sync with the `movement` branch and (once it lands) its
project-memory entry — the two carry the same design, implementation status, and
remaining work. Update both together. Sections describing the **target** design
are marked as such; "Current implementation" records what the **code** actually
does today.

## Current implementation

Grid-locked, Grimrock / Dungeon Master style. The party occupies exactly one
cell and faces one of four compass directions (0 = N/−Z, 1 = E/+X, 2 = S/+Z,
3 = W/−X). All movement lives in `src/Game/Party.{h,cpp}`.

### Discrete actions

`MoveAction` — `Forward, Back, StrafeLeft, StrafeRight, TurnLeft, TurnRight`.
The bound keys (`MoveKeys`, default QWEASD) map onto these in
`Party::HandleInput`; the HUD's six arrow buttons feed the same actions straight
into `Party::Act`. One action at a time.

- **Logical vs visual split.** The logical cell/facing snaps **instantly**; the
  visual position/yaw then interpolates over ~0.3 s (eased + head bob), which is
  what gives the genre its weight.
- **Easing.** Tweens run through the shared `Core/Easing.h` `EaseLerp`. Base
  curve is `EaseInOut` for both move and turn (overridable via
  `SetMoveEasing`/`SetTurnEasing`).
- **Input buffer.** A single-slot buffer holds an action requested mid-tween
  (newest overwrites) and replays it the instant the grid frees. A same-kind
  continuation flattens the tween's tail to a linear exit so chained
  steps/turns glide continuously instead of stuttering at each cell
  (`RefreshChainEasing`, `m_activeMove/TurnEasing`, `startLinear`).
- **Pace.** `SetSpeed` (slowest party member) scales both step duration and turn
  speed.

### Blocked moves (bump)

A blocked step never changes the logical position; the **visual** position
lunges ~30 % toward the blocked cell at the normal step rate, then bounces
rapidly back (`m_bumping`, two phases). `onBumpImpact` fires at the lunge peak so
the game can jar the party (small damage + portrait splat + grunt). A
`m_blockCooldown` throttles repeated bump feedback when a key is held.

### Free-look (right-mouse)

Holding RMB and dragging adds a transient yaw/pitch **offset** on top of the
grid facing (`AddLook` → `m_lookYaw/Pitch`; renderer reads
`EyeYaw()`/`EyePitch()`, while `Yaw()`/`Facing()` stay the grid pose for the
HUD/compass).

- Past ±`kLookSnap` (45°) the ordinal facing snaps one quarter and the inverse
  folds back into the offset, so the view glides on continuously while the grid
  facing turns under it — look (and walk) around corners.
- Releasing **parks** the view, then after a hold eases back to orthogonal
  (slow-in/fast-out). A move/turn triggers a faster straighten that overtakes an
  in-flight hands-off return (`StartReturn`).
- Tunable on Settings → Controls "Mouse Look" (`LookSettings`: sensitivity,
  return hold/time, move-straighten time, two easing curves). Part of the save
  (`SetLookState`/`LookYaw`/`LookPitch`).

### Collision / occupancy

The Party knows the map walls but **not** monsters/props; the Game injects an
`isOccupied(x,z)` callback that blocks a step. `noclip` (dev console) bypasses
both walls and occupancy.

## Open questions / planning

### 1. Monster sizes (sub-cell occupancy)

Monsters have a **size class** that determines how many fit in a cell and where
they stand within it. The cell is conceptually subdivided into a grid of slots
and a monster occupies one or more slots, positioned at the slot centre.

| Size   | Footprint        | Per cell | Slot layout / placement                          |
|--------|------------------|----------|--------------------------------------------------|
| Huge   | 2×2 cells        | —        | Spans 4 squares → **cannot enter corridors**     |
| Large  | 1 full cell      | 1        | Centre of the cell                               |
| Medium | ½×½ cell (human) | 4        | Cell split into quarters; one in the middle of each quarter (2×2) |
| Small  | ⅓×⅓ cell         | 9        | 3×3 grid of slots                                |
| Tiny   | ¼×¼ cell         | 16       | 4×4 grid of slots                                |

Implications to work through:
- Occupancy is no longer one-monster-per-cell — needs sub-cell slot tracking,
  and the party/AI/`isOccupied` collision checks must understand multi-slot and
  multi-cell footprints.
- Huge monsters need 2×2 clearance to move; corridors (1-wide) exclude them.
- Pathfinding (`Brain::FindPath` BFS) must account for footprint size when
  testing walkability/occupancy.
- Rendering/positioning: place each monster at its slot centre rather than the
  cell centre.

### 2. Intra-cell movement & tactical repositioning

Monsters can move **between slots within and across cells** independently of the
party's discrete grid step — fluid positioning on top of the slot grid. Example:
4 skeletons engaging the party from the front cell; the rear 2 peel off, move
around, and take adjacent cells to **surround** the party.

Implications to work through:
- Monsters need a notion of a *formation* / desired slot relative to the party,
  not just "the cell in front."
- AI intent grows beyond "engage toward cell": pick a slot (front rank vs flank
  vs rear), path to it through the sub-cell slot graph.
- Movement is slot-to-slot (possibly diagonal/continuous between slot centres),
  visually tweened like the party step but on the finer grid.
- Surround behaviour: when a flank/rear cell is free, rear-rank monsters
  reroute to it; attack resolution then comes from multiple sides.

### 3. Rank discipline & promotion

If a group **cannot** move around the party (no free flank/rear cell — e.g. a
corridor), it stays massed in formation. But when a **front-rank monster dies**,
a **rear-rank monster steps forward** to fill the vacated slot.

Implications to work through:
- The formation has ranks (front engaging, rear waiting); slots are prioritised
  so the front fills first.
- On a front-rank death, a waiting rear monster is promoted into the open front
  slot (move + retarget to attack).
- This is the fallback when surround (item 2) is impossible — the two behaviours
  share the same slot-assignment logic, just constrained by available cells.

### 4. Reach: front vs rear rank

Only the **front rank** can make melee attacks. **Rear-rank** combatants —
monsters *and* player characters — can only attack with **ranged**, **magic**, or
**reach (polearm)** weapons. Reach weapons let a rear-rank attacker strike past
the front rank into melee.

Implications to work through:
- Attacks gain a *reach* category: melee (front rank only), polearm (front or
  one rank back), ranged/magic (any rank).
- Combat resolution must check the attacker's rank against the weapon's reach
  before allowing the swing — applies symmetrically to the party (back two
  members) and monster formations.
- Ties into the existing per-hand attack system (`Combat.cpp`) and the party
  layout (front/back member positions).

### 5. Item drops snap to quartile slots

When the player drops a held item — cursor holds an item and clicks the **lower
third** of the view (existing `DungeonWorld::DropItemAt` / `TryPickItem` flow) —
the item lands in the **nearest quartile slot** (the Medium 2×2 sub-cell grid),
not the cell centre.

Implications to work through:
- Picking the target cell, then the nearest of its 4 quartile slots from the
  click point (ray hit → cell → slot).
- Floor items get a sub-cell slot position (multiple items can sit in distinct
  quarters of one cell); rendering and pick-up read that slot.
- Reuses the same slot grid as monster occupancy (item 1), Medium tier.
- Floor items always use the **Medium 4-quarter** rule: up to 4 items per cell,
  one per quarter, each at its quarter centre — regardless of any monster sizing.

### 6. Monster groups & per-monster slot state

It follows from sizing (item 1) that monsters can be **grouped** in a cell — up
to 16 per cell depending on size (Large 1 / Medium 4 / Small 9 / Tiny 16). Each
monster carries a **slot position** that travels with it as the group moves.

- A group occupies one (or more) cells; each member is pinned to a slot.
- The slot position is **saved/persisted per monster** (round-trips through
  `CaptureState`/`ApplyState` + the save layer, like all dynamic state).
- When a group **splits** (e.g. rear rank peels off to surround, item 2), the
  departing monsters form a **new group** with their own cell + slot
  assignments; the original group reflows into the vacated slots.

Implications to work through:
- A group is a logical grouping over the existing per-monster entities (keyed by
  the stable `runtimeId`), not a new owning container — monsters can leave/join.
- Group membership + slot must reconcile every tick as monsters move, die
  (item 3 promotion), split, or merge.
- The async AI snapshot/plan path must carry slot/group info so workers can plan
  formations without touching live world state.

### 7. Free repositioning within a partially-full group

When a group is **not** at capacity for its size class, its members can
**reposition freely** among the open slots of the cell's grid (2×2, 3×3, 4×4 per
size). Empty slots let monsters shuffle — close ranks, spread to face threats,
shift to a better slot — rather than being statically pinned.

Implications to work through:
- Slot assignment is dynamic, not fixed at spawn: a member can vacate its slot
  and take another open one within the same group/cell.
- Repositioning is driven by the AI (e.g. move toward the rank facing the party,
  or open a path for a splitting member) and tweened slot-to-slot (item 2).
- Need contention handling so two monsters don't claim the same open slot in one
  tick (the slot grid is the authority; claims reconcile on the main thread).

### 8. Lone monster takes front-centre

When a group is down to its **last member**, a **Medium-or-smaller** monster
moves to a **front-centre** position so it can attack **both** front-rank party
members. **Larger than Medium stays centred:** a Large monster already fills the
whole cell (its slot 0 *is* the cell centre) and a Huge fills its 2×2 block, so
they don't slide — only sub-cell sizes (Medium/Small/Tiny), which normally sit in
a corner quarter, gain anything from re-centring.

Implications to work through:
- Front-centre is a special position straddling the cell midline (between the two
  front quarters), not one of the fixed size-grid slots — so a solo sub-cell
  monster isn't stuck attacking only one party member. Precisely: **centred on
  the cross-axis** (the centre-line between the grid's columns) and at the
  **centre of the front ROW** of the size's slot grid, on the side facing the
  party (snapped to the dominant cardinal axis — never a diagonal). For a Medium
  2×2 that is centred left/right and at the centre of the near front/back half.
- Gated on size: only Medium/Small/Tiny slide; Large/Huge are left at their
  centred slot. A Small/Tiny solo monster slides to the shared front-centre
  rather than sitting in a corner slot.
- This is a repositioning case (item 7) that unlocks once group size hits 1;
  combat resolution must let that centred attacker reach both front members.

### 9. Per-monster facing (travel direction + provocation)

Each monster has its OWN facing — never a group-wide value. A monster turns to
face its **direction of travel** as it moves (slot-to-slot or cell-to-cell), and
an **idle monster keeps its resting facing** (it may be facing away from the
party). Facing the party is driven by *awareness/engagement*, not mere proximity:
the player can sneak up on a group of skeletons all facing away, **attack one**,
and only **that** skeleton independently turns to face the party — its neighbours
stay as they were until they too become aware.

Today (baseline): a monster snaps its yaw straight at the party every frame while
`intent == Engage` (if `kind->facesTarget`), and engagement is purely distance
(`Brain::Think` aggro range). So monsters never face their travel direction, and
proximity alone makes a whole group face the party — no sneaking.

Implications to work through:
- **Face travel direction:** on each committed step, set a target yaw from the
  move vector (`atan2(dx,dz)`) and ease the visual yaw toward it (a turn tween
  like the party's), instead of snapping at the party.
- **Face the party when engaged & adjacent:** once a monster is attacking, it
  faces the party (the current behaviour), but reached via the same smooth turn.
- **Provoke on hit (per-monster):** a struck monster becomes engaged/aware and
  turns to face the party, independent of its neighbours — wire it where party
  damage lands on a monster (`Combat`/`DungeonWorld`), setting that monster's
  intent/aggro so the async brain keeps it engaged.
- **Awareness gate (the sneak mechanic):** engagement does NOT trigger on
  proximity alone. `Brain::Think` gates engage on a perception test — the party
  must be within `aggroRange` AND inside the monster's **frontal sight cone**
  (~120°, i.e. ±60° of its facing). A per-monster sticky **`aware`** flag, once
  set, keeps the monster engaged even if the party slips behind it (no instant
  un-noticing). `aware` is set when the brain first engages via the cone, or
  immediately on a hit (provoke). It is **persisted** in the save (monster diff),
  so a reloaded alerted monster stays alerted.
- Facing is already per-monster (`Monster.yaw`); keep it that way — no shared
  group yaw. `faces=false` monsters (the blob) never rotate (radially symmetric)
  and their perception is omnidirectional (no cone — a lump has no front).

## Build plan / phasing

The eight items above are interdependent; nearly all of them rest on one
keystone — a **sub-cell slot model** (item 1). We build foundation-first, take a
cheap self-contained slice to validate it, then layer the group/AI behaviours.

| Phase | Items | Scope | Why here |
|-------|-------|-------|----------|
| **1. Slot & size foundation** | 1 | `SizeClass` catalog field; slot-coordinate scheme + per-size footprint; per-cell occupancy that understands multi-slot and multi-cell (Huge); monsters render at slot centres; collision + BFS pathfinding become footprint-aware. | Keystone — nothing else has anything to stand on without it. |
| **2. Item drops to quartile slots** | 5 | Held-item drop lands in the nearest Medium quarter; floor items carry a sub-cell slot (≤4/cell); render + pick read it. | Small, AI-free, immediately testable — proves the slot geometry end-to-end before the AI work. |
| **3. Groups + per-monster slot state** | 6 | Logical group over `runtimeId` entities; per-monster slot persisted via `CaptureState`/`ApplyState`; split → new group + reflow. | The grouping layer the tactics build on. |
| **4. Slot assignment & repositioning** | 7, 8 | Free shuffle among open slots in a partial group; lone monster slides to front-centre. | Dynamic slot assignment + the solo special case. |
| **5. Formation tactics** | 2, 3 | Surround when flank/rear cells are free; rank promotion on front-rank death. | The headline combat behaviours, needing groups + assignment first. |
| **6. Reach & combat** | 4 | Front-only melee; polearm/ranged/magic from the rear; symmetric for party and monsters. | Layers onto established ranks; touches `Combat.cpp`. |

Phases 1–4 are **done**; item 9 (facing) landed between 1 and 2. Current focus
will be **Phase 5** next.

## Phase 1 design — slot & size foundation

Goal: give every monster a **size class** and a **sub-cell slot**, replace the
one-monster-per-cell occupancy assumption with slot/footprint-aware occupancy,
and make positioning, collision, and pathfinding understand it. No new tactics
yet — just the model everything else builds on.

**Simplifying assumption:** a cell's occupants are **homogeneous in size** (a
group of like monsters). Mixing different size classes in one cell is out of
scope — slot grids for different sizes don't nest (Medium ½, Small ⅓, Tiny ¼
don't align), and groups are like-with-like anyway.

### Slot geometry (new `src/Game/SlotGrid.h`)

`enum class SizeClass { Huge, Large, Medium, Small, Tiny }`. Each size maps to a
square sub-grid of the 2.4 m cell:

| Size   | Cell footprint | Sub-grid dim | Slots/cell | Slot centre offset from cell centre |
|--------|----------------|--------------|------------|-------------------------------------|
| Huge   | **2×2 cells**  | n/a          | 1          | centre of the 2×2 block             |
| Large  | 1 cell         | 1            | 1          | 0 (== `CellCenter`)                 |
| Medium | 1 cell         | 2            | 4          | ±0.25·`kCellSize` on each axis      |
| Small  | 1 cell         | 3            | 9          | (col,row)/3 grid                     |
| Tiny   | 1 cell         | 4            | 16         | (col,row)/4 grid                     |

Helpers (pure, header-only): `SlotDim(size)`, `SlotsPerCell(size)`,
`SlotCenter(cellX, cellZ, size, slot)` → world `Vec3` where
`slot = row*dim + col`, offset `((col+0.5)/dim − 0.5)·kCellSize` per axis. Slot
indices are plain **world-space** grid positions; *rank* (front/flank/rear
relative to the party) is derived later (Phase 4/5), not baked into the index.

### Data model changes

- `MonsterKind` gains `SizeClass size`, parsed from a `monsters.cat` `size=`
  field (`huge|large|medium|small|tiny`, **default `large`** → preserves today's
  behaviour for every existing monster).
- `Monster` gains `int slot` (its slot within its cell; for Huge, `x,z` is the
  anchor/NW cell of the 2×2 block). `visualPos`/`moveFrom` glide to
  `SlotCenter(...)` instead of `CellCenter(...)`.

### Occupancy (slot/footprint-aware)

Today occupancy is binary per cell (`CellFreeForMonster`, `Party::isOccupied`,
the AI snapshot `blocked` set). Phase 1 makes it capacity-aware:
- A cell holds up to `SlotsPerCell(size)` monsters in distinct slots.
- `CellFreeForMonster` answers "is there a free slot for a monster of this size
  here," and the step picks the slot.
- **Huge** occupies a 2×2 cell block: all four cells must be walkable and clear
  to enter → 1-wide corridors naturally exclude it.

### Pathfinding

`Brain::FindPath` BFS becomes footprint-aware: a step is valid only if the
monster's footprint fits (Huge needs 2×2 clearance; others fit any walkable
cell with a free slot). The `ai::Snapshot` carries per-agent size + per-cell
capacity (primitives only — keeps the AI layer walled off, no game headers).

### Save

Phase 1 persists nothing new — slots are assigned deterministically at spawn
(fill order) so behaviour is stable. Per-monster slot persistence lands with
groups in **Phase 3** (item 6).

### Sub-steps (compile/verify each)

- **1a** ✅ — `SlotGrid.h`: `SizeClass` + `SlotDim`/`SlotsPerCell`/`SlotCenter`.
  No behavioural change.
- **1b** ✅ — `MonsterKind.size` catalog field + `Monster.slot`;
  positioning/render use `SlotCenter` (Large slot 0 == `CellCenter`, so no
  visible change yet).
- **1c** ✅ — slot-aware occupancy: several same-size monsters share a cell.
  Host `DungeonWorld::FreeSlotInCell` (bitmask slot pick, lowest-index-first,
  foreign size = cell off-limits) drives the live step + `CellFreeForMonster`;
  the AI snapshot carries a per-cell `CellOcc{count,capacity}` map + per-`Agent`
  `capacity`, and `SnapshotView::CellFreeForMonster(x,z,self,capacity)` answers
  capacity for the BFS. Slots assigned at spawn/reset/restore (fill order).
  Party-vs-monster (`isOccupied`) unchanged: any monster blocks the cell.
- **1d** ✅ — Huge 2×2 footprint + corridor exclusion + BFS footprint clearance.
  `FootprintCells(size)` (2 for Huge); host `FreeSlotInCell` + AI snapshot mark/
  check every footprint cell, and the BFS `FootprintFree` helper requires the
  whole f×f block clear while **self-excluding** the agent's own footprint (a
  Huge's 2×2 overlaps between adjacent anchors, else it could never step). A
  1-wide corridor fails the clearance, so a Huge can't enter it.

> _Verification note: this worktree has no baked textures yet, so the exe won't
> run until they're provisioned (FetchTextures.ps1 / robocopy). Sub-steps
> compile-check as we go; first run-test once 1b/1c produce something visible._

## Phase 3 design — groups + persisted slot

Phase 3 turns the sub-cell `slot` into real saved state and introduces the
**group** as a first-class identity. It is the DATA model only — group BEHAVIOUR
(repositioning, split→new group, reflow, rank promotion) is Phases 4–5.

### Persisted slot

`Monster.slot` becomes saved state (deferred from Phase 1, which re-derives it).
The monster save record carries the slot (reusing `SaveData::EntityState::slot`,
already added for items); `ApplyState` restores the saved slot instead of
re-deriving via `FreeSlotInCell`. `LoadMonsters` still derives the initial slot
for the `.ent` baseline by fill order. A baseline monster only gets a save diff
once it has drifted (moved/announced/aware/hurt); since a chase step changes both
`x,z` AND `slot`, the diff naturally captures a moved monster's slot.

### Group identity

`Monster.groupId` (u32, stable per session, 0 = unassigned), from a
`m_nextGroupId` counter. Assigned at spawn by **co-location**: monsters sharing a
spawn cell join one group; a lone monster is a singleton. Editor-placed and
save-restored monsters get the same treatment. Accessor: `MonsterGroupMembers` /
`MonsterGroupCount` for the upcoming formation code.

`groupId` is **not persisted** — it is re-derived from spawn co-location on load,
which is deterministic and yields the same grouping structure (the id *numbers*
may differ but they are opaque). Once Phase 5 lets groups span cells and split,
group identity will diverge from spawn co-location and will need persisting then;
Phase 3 deliberately stops short of that.

### Scope guard

No repositioning, splitting, merging, or reflow in Phase 3 — those are the next
phases. This phase is verifiable by save/reload: a monster's sub-cell slot (and
thus its exact stance within a cell) survives a round-trip instead of snapping to
a re-derived slot.

## Phase 4 design — in-cell repositioning

The first group BEHAVIOUR: monsters shift WITHIN their cell (no new cell step) to
better positions, gliding smoothly. Two drivers (items 7 + 8), both funnelled
through one new mechanism — a per-frame **in-cell settle**: when a monster is not
mid cell-step, its `visualPos` eases toward a **desired anchor** instead of being
pinned to its slot centre. (Cell-to-cell stepping is unchanged.)

### Desired anchor

`DesiredAnchor(monster)` returns the world point the monster wants within its
cell:
- **Lone + Medium-or-smaller (item 8):** the **front-centre** — centred on the
  cross-axis (centre-line between columns) and at the centre of the front ROW of
  the size's grid, offset `(dim−1)/(2·dim)·kCellSize` toward the party along the
  **dominant cardinal axis** (Medium 0.25, Small 0.333, Tiny 0.375 of a cell; not
  a diagonal). Recomputed each frame so it tracks the party. Large and Huge are
  **excluded** (already centred) — they keep their slot anchor.
- **Otherwise:** `SlotCenter(x,z,size,slot)` — the monster's quarter (unchanged).

`visualPos` eases toward the anchor (exponential, like the facing turn). When a
cell-step glide is in flight (`moving`), the settle is skipped (the glide owns
the position); after it lands, the settle takes over.

### Front-slot repositioning (item 7)

A **grouped** (≥2 alive), engaged, settled monster picks the FREE slot in its
cell nearest the party (the front rank) and, if it is closer than its current
slot, **claims** it (`slot = newSlot`). The settle then glides it there. Slot
claims are processed sequentially on the main thread, so two monsters never grab
the same slot in one tick (the live occupancy scan excludes already-claimed
slots). This makes a group "close ranks / face the threat" — rear members slide
into open front quarters as they free up.

### Helpers / state

- `IsSubCellSize(size)` (SlotGrid.h): Medium/Small/Tiny (the sliding sizes).
- `AliveInGroup(groupId)`: live member count (gates lone front-centre and the
  grouped reslot).
- No new persisted state: anchor is derived; `slot` (already saved, Phase 3)
  still records the claimed quarter. Front-centre is purely visual/positional and
  re-derives from the live party each frame.

### Scope guard

No splitting / merging / reflow / rank-promotion yet (Phase 5). Phase 4 only
moves monsters within the cell they already occupy: lone sub-cell monsters
re-centre toward the party, grouped members fill front slots.

## Current implementation status

**Phase 1 is complete (1a–1d)** on the `movement` branch: compiles clean (debug)
and behaviourally verified in-game. Monsters carry a `SizeClass` (default Large)
and a sub-cell `slot`; positioning, live occupancy (`FreeSlotInCell`), and the
async-AI BFS (per-cell `CellOcc` + per-`Agent` `capacity`/`footprint`, with
footprint self-exclusion) are slot- and footprint-aware. Several same-size
monsters share a cell at distinct slots; Huge spans a 2×2-cell block and is
excluded from 1-wide corridors.

Verified with temporary test content (now reverted): four Medium skeletons
stacked on one cell fanned into quarter-slots and crowded the party
four-to-a-cell (docs/phase1_04_playing.png); a Huge blob occupied/attacked from
its 2×2 anchor with the party blocked from its footprint. NOTE: a Huge's render
is **not** scaled by size yet — Phase 1 changes footprint/position/occupancy, not
model scale (visual scaling is a later rendering task). No shipping `monsters.cat`
entry sets `size=` yet, so default content is unchanged at runtime; the field is
documented and ready to assign when content is authored (later phases).

**Item 9 (per-monster facing) is also complete + verified** (done after Phase 1,
before Phase 2). Monsters ease their visual yaw toward a target: their travel
direction while moving, the party while aware, else their resting facing.
Engagement now goes through perception in `Brain::Think` — within `aggroRange`
AND (already `aware`, OR omnidirectional, OR the party inside a ±60° frontal
sight cone). `aware` is sticky, latched when the brain first engages or via
`ProvokeMonster` on a hit (melee/spell), and is persisted in the save (monster
diff gained an `aware` token; `ResetForNewGame` clears it). Verified in-game:
skeletons facing away stayed idle while the party approached from behind
(docs/phase1_19_sneak_rear.png), then noticed, turned, and engaged when
approached from the front (docs/phase1_20_front_notice.png).

**Phase 2 (item drops to quartile slots) is complete + verified.** `Item` gained
a `slot`; floor items render, glow, and hit-test via
`SlotCenter(x,z,Medium,slot)`. A dropped tablet snaps to the quarter nearest the
cursor's floor-hit point via `DungeonWorld::FreeItemSlotNear` (nearest FREE
quarter; falls back to the nearest if all four are taken). Baseline `.ent` items
fan across quarters by fill order at load; the dropped-item slot is persisted
(the `drop` save line gained a slot token, back-compatible). Verified in-game:
four runes stacked on one cell rendered as a 2×2 cluster of distinct tablets
(docs/phase2_06_before_pick.png); pick/drop interactions register and re-slot.

**Phase 3 (monster groups + per-monster saved slot) is complete + verified.**
`Monster.groupId` is assigned at spawn by cell co-location (lone monster =
singleton; re-derived on load, not saved); `GroupForSpawnCell` mints/joins ids
and `GroupsReport` backs a new dev console `groups` command. `Monster.slot` is
now persisted — written into the monster save record (`ent`/`monster` lines gain
a slot token, back-compatible) and restored in `ApplyState` instead of
re-derived. No group behaviour yet (reposition/split/reflow/promotion are Phases
4–5). Verified in-game: `groups` listed four co-located skeletons as one group at
slots 0–3 (docs/phase3_01_groups.png); after waking + a save/reload they restored
without crashing, the save file carrying the slot token
(docs/phase3_02_after_load.png).

**Phase 4 (in-cell repositioning) is complete + verified.** A settled monster
(no cell-step in flight) eases its `visualPos` toward a `DesiredAnchor` each
frame: a lone, aware **Medium-or-smaller** monster slides to **front-centre** —
cross-axis centred and at the centre of the front row of its grid, offset
`(dim−1)/(2·dim)·kCellSize` toward the party along the dominant cardinal axis,
tracking it; everything else holds its slot centre (Large/Huge already centred,
excluded via `IsSubCellSize`). Grouped (≥2), aware, adjacent monsters also reslot to the FREE
slot nearest the party (front rank, item 7), claimed sequentially so no two grab
the same slot. `AliveInGroup` gates both. No new saved state (anchor is derived;
slot already persists). Verified in-game: a lone Medium skeleton centred toward
the party from both the south and north approaches
(docs/phase4_01_lone_front.png, docs/phase4_02_lone_trackS.png).

Not yet started: Phase 5 (formation tactics — surround + rank promotion) and
Phase 6 (reach/combat).
