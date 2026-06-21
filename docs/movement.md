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

Regardless of size class, when a group is down to its **last member**, that
monster can move to a **front-centre** position so it can attack **both**
front-rank party members.

Implications to work through:
- Front-centre is a special slot straddling the cell midline (between the two
  front quarters), not one of the fixed size-grid slots — so a solo monster
  isn't stuck attacking only one party member.
- This is a repositioning case (item 7) that unlocks once group size hits 1;
  combat resolution must let that centred attacker reach both front members.
- Define for each size: a Small/Tiny solo monster still slides to the shared
  front-centre rather than sitting in a corner slot.

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

Current focus: **Phase 1**.

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

Not yet started: Phase 2 (item drops to quartile slots) and beyond.
