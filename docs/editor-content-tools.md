# Editor content tools — the plan

The editor stops being a side feature here. From this branch on it is the
primary workspace: content is authored in it, and every minute it costs is a
minute not spent on the game. So its ergonomics ARE the feature.

Source: ten lines dictated 2026-08-11 (session notes). This document organizes
them into five themes, says what already exists, and proposes an order.

---

## The premise

> As I work on adding the game content, I'm going to spend more and more time in
> the editor. So, we need to add a lot of features to it so it doesn't get in
> the way of the main content creation. (1)

> One of the most frustrating things in the Elder Scrolls Creation Engine is how
> bloody fiddly it is. Each piece of an environment has to be placed
> pixel-perfect with no snapping or surface placement rules. I want it to be easy
> to build content, not frustrating. (2)

The standard to hold everything below against: **the editor should never ask for
precision the content does not actually need.** Where a rule can be derived, the
editor derives it; where it cannot, it shows the answer before committing it.

---

## Theme A — Placement that cannot be fiddly (lines 2, 3)

> Snapping and surface rules should be automatic, not manual. (3)

**What exists.** More than it looks like, but all of it hardcoded per kind:
sconces resolve their mount wall at load (`DungeonMap::WallSconce`); the door
brush auto-detects the doorway axis so there is no facing UI; wall-mounted
decorations edge-pick a wall face (`MapEditor::BrushIsWallMounted`, the
`WallFace` argument threaded through `ApplyBrush`/`EraseAt`); `MountOnWall` is
the one shared geometry helper. Each of these was written for its own kind. A
NEW type gets none of it without new C++.

**The observation that shapes the fix.** This game's world is a discrete grid, so
the Creation-Engine failure mode — nudging a mesh a pixel at a time — cannot
happen here by construction. The fiddliness that CAN happen lives in the three
places where a cell is not the unit: sub-cell quadrant slots, wall faces, and
facing. That is the whole surface area to fix.

**What to build.** A declarative placement contract on the catalog schema, so
placement behaviour is a data field like everything else in this project:

- `surface` — floor / wall / ceiling / doorway / corner / free. Says what the
  type may attach to. A wall prop is refused on open floor instead of silently
  landing wrong.
- `snap` — cell centre / quarter slot / wall face / wall mid-height.
- `facing` — face-the-room / face-the-solid-wall / face-the-entrance / free.
- Hover **ghost**: show the resolved placement *before* the click. The editor is
  a top-down map, not a 3D view, so the ghost is the type's baked map icon drawn
  faint at the exact quadrant slot or wall edge it will occupy, with its facing
  arrow — which is precisely the information a cell-centred icon hides today.

The invariant that makes the ghost worth building: **one resolver, called twice.**
Hover and commit must run the same function, or the preview becomes a second
implementation that can disagree with the real thing — and a preview that lies is
worse than none.

The payoff is N+M rather than N×M: one contract serves every type, and adding a
type is a catalog row, never a code change. That is the same argument that
retired baked wall columns in favour of composed decorations.

---

## Theme B — Generate rough, refine by hand (lines 4, 5, 6, 7)

> I also want tools to help with the broad, rough creation which can then be
> refined iteratively by the user. So, for instance, we need a way to create a
> new level that is simple. We'd pick rough size, complexity (how many branches,
> etc., in the main pathways), difficulty, reward level, etc. (4)

> Other level details would be tile set (stone, brick, marble, outdoors, etc.).
> The editor would then auto-generate a level from these details. The user could
> keep tweaking knobs and hitting "regenerate" at will until something roughly
> the right shape is created. (5)

> Another setting would be monster types. You might have a dungeon that is
> populated by undead, or animals, or other creature sets. (6)

> Loot and treasure placement should follow the same generation rules. (7)

**What exists.** `Game::CreateNewLevel` (Game_Editor.cpp:145) already writes a
level as **text** — a palette header copied from the active level, a 16x16 grid
with a 3x3 room, an empty `.ent` — appends the manifest, and jumps the view onto
it. The generator is an expansion of this function, not a new subsystem.

**The rule that keeps it cheap.** The generator emits ORDINARY `.map`/`.ent`
records. There is no "generated level" type, no marker, no second code path —
which means every tool already built (brushes, inspectors, undo, validation,
level browsing, save) works on the output on day one, and a generated level is
diffable in git like a hand-built one.

**Knobs**, from the lines: size, complexity/branching, difficulty, reward level,
tile set, monster set, loot density. Plus a **seed** — so a result you liked is
reproducible, and so "regenerate" is a knob rather than a dice roll you cannot
get back.

**Loot follows the same rules (7)** — treasure placement reads difficulty and
reward level off the same parameter block, so a hard dungeon is a rich one
without that being authored twice.

**Regenerate is destructive, behind undo** (decided). A reroll replaces the whole
level and Ctrl+Z brings the old one back; nothing pretends to merge. This fits
the existing machinery exactly — the editor's undo step is already "copies of
every level's editor-visible state", so a regenerate is one snapshot like any
other edit.

One trap that follows directly: **undo history clears on level transitions.** So
a regenerate must rewrite the level being viewed, in place, without a level
swap — otherwise the reroll drops the very history the undo promise depends on.
Generating into a fresh level and jumping to it would quietly void it.

---

## Theme C — Theme as a first-class key (lines 5, 6, 10)

> Once a tile/monster theme has been picked there needs to be a way to filter the
> palette by this key without disallowing other things to be brought into the
> dungeon. (10)

**What exists.** Catalog entries carry a `category` field (drives palette
sub-accordions), and the palette has a case-insensitive substring filter box.
Neither is a theme: `category` is what a thing IS (weapon, key), not what world
it belongs to.

**What to build.** A `tags` field on catalog entries — `undead`, `animal`,
`stone`, `brick`, `marble`, `outdoor`. Multi-valued deliberately: a mossy stone
set is both `stone` and `outdoor`, and a single field would force a false choice.

Tags then do double duty, which is why this theme is the spine:

- **Input to the generator** (5, 6): "undead dungeon, marble tile set" is a tag
  query over the catalogs, so new content joins a theme by being tagged, with no
  generator change.
- **A soft palette lens** (10): the level's theme **ranks and groups** the
  palette — on-theme first, everything else still present below a divider. Never
  a hard exclusion. Line 10 is explicit that off-theme things must stay
  reachable, and that is the right call: the one-off that breaks a theme is
  usually the memorable thing in a dungeon.

Distinct from the existing text filter, which stays exactly as it is.

---

## Theme D — Validation (line 9)

> There will be validation rules for the levels/dungeons. Things like ensuring
> there is an entrance/exit, making sure each stair up has a corresponding stair
> down, and vice-versa. There must also be a way to check that every locked door
> has the correct key BEFORE the door. (9)

**What exists.** Only per-record sanity at load: out-of-bounds entities are
skipped with a warning (DungeonEntities.cpp:35), buttons must face a wall,
stair placement auto-authors its pair, and type deletion refuses while anything
still references it. Nothing checks whether a level is *playable*.

**The insight.** These read as three checks but the third subsumes the others.
"Every locked door has its key before it" is a **lock-and-key reachability
fixpoint**:

1. Flood from the entrance, treating locked doors as walls.
2. Collect the keys in the reached region.
3. Unlock every door those keys open; repeat until nothing new is reached.
4. Solvable iff the exit — and everything required — is in the final region.

Run that and you get entrance/exit existence and stair pairing for free, because
an unpaired stair is simply a region nothing reaches. So build the fixpoint, and
report the cheap checks as its by-products.

Stair pairing still deserves its own explicit check for a different reason: the
pair is auto-authored on placement, so a broken pair means drift — a hand-edited
record, a rename, a cross-level delete — and naming that cause in the message is
worth more than "unreachable".

**Where it runs.** On demand from the toolbar, and automatically after every
generate. Which is the real point: **D is what makes B trustworthy.** A
generator that can emit a dungeon whose key is behind its own locked door is
worse than no generator, because it costs you the playthrough to find out.

---

## Theme E — The world map (line 8)

> The game will feature a world map with many different dungeons to discover and
> explore. These will range from side-quest like dungeons with only loot and exp
> etc., to main story-line dungeons that will contain quest items, information,
> etc. (8)

This is the largest line and mostly a GAME feature, not an editor one — map
screen, discovery, travel, quest state. It should be its own thread.

But it has one consequence worth taking NOW, for free. A world map implies a
container above `level`: a **dungeon/site**, several levels deep, carrying its
own difficulty, reward level, theme and monster set — which is exactly the
parameter block Theme B needs anyway. So define the container when B needs it,
put B's knobs on it rather than on the individual level, and the world map later
finds its nodes already authored instead of needing a migration.

Recommendation: **defer E**, adopt only the container.

---

## Proposed order

Dependencies first, then daily payoff:

1. **C — tags** (smallest). Unblocks both the generator's inputs and the palette
   lens. Pure data plus one schema field.
2. **A — placement contract**. Independent of everything else and the change felt
   most often; every placement, every day.
3. **D — validation**. Independent, self-contained, and must exist before B can
   be believed.
4. **B — the generator**. The big multiplier, built on C's tags and checked by
   D's fixpoint.
5. **E — world map**. Its own thread. Adopt only the site container here.

A and C are also the two that pay off before anything else lands, which matters
if the branch runs long.

---

## Decisions (2026-08-11)

1. **Regenerate is destructive, behind undo.** No merge, no pinning. See Theme B
   for the level-transition trap this creates.
2. **This branch carries C, A, D and B.** E (the world map) stays deferred; only
   its site container is adopted, and only where B needs it.
3. **Multi-valued `tags`, not a single `theme` field.** A mossy stone set is both
   `stone` and `outdoor`. Recorded because it is a schema decision that is
   painful to reverse once content is tagged.

---

## Phases

Each phase is independently useful and independently mergeable, in the order the
dependencies demand. The branch is long, so the early phases are deliberately the
ones that pay off on their own.

### Phase 1 — `tags` (Theme C)

- `tags` FieldSpec on every placeable catalog's schema table; space-separated,
  free-form. `CatalogTags()` helper beside `CatalogGet`/`CatalogBool`.
- Tag the existing content: 28 wall / 20 floor / 6 ceiling surfaces, 34
  decorations, 14 monsters. Authoring work in `.cat` files, no code.
- Level-side theme (a tag set) on the Level settings dialog, persisted as a `.map`
  record beside `atmosphere`.
- Palette lens: on-theme rows first, a divider, everything else below —
  ranking only, never exclusion. The existing text filter is untouched.

### Phase 2 — placement contract (Theme A)

- `surface` / `snap` / `facing` FieldSpecs; one `ResolvePlacement(type, cell,
  wallFace)` returning a resolved pose or a refusal.
- Hover ghost in `MapView`, drawn from that same resolver.
- **Migrate the three existing hardcoded rules onto it** — sconce mount, doorway
  auto-orient, wall-decoration edge-pick — so there is one path and not two. This
  is the part that makes the phase worth doing; adding a contract beside the
  hardcoded rules would leave the codebase worse than it started.

### Phase 3 — validation (Theme D)

- The lock-and-key reachability fixpoint, over a whole site rather than one
  level, since a key may legitimately live a floor away.
- Explicit stair-pairing check reported separately, with drift named as the
  cause.
- A results panel listing each problem, click-to-jump to the offending cell.
- Toolbar button, and an automatic run after every generate.

### Phase 4 — the generator (Theme B)

- The site container, carrying difficulty / reward / theme / monster set.
- Rooms and corridors from size + branching; start, exit and stairs; monsters by
  difficulty from tag-matched sets; loot by reward level.
- **Build the lock/key ordering by construction, not by retry.** Place each key
  in the region that is already reachable when its door goes down, and the
  fixpoint passes by design. A generate-check-reject loop is slow, and worse, it
  can fail to converge on exactly the tight parameters that most need it — so
  Phase 3's validator stays a *check*, never a filter the generator leans on.
- Emit ordinary `.map`/`.ent` text through the `CreateNewLevel` path.
- Knobs dialog with seed and Regenerate; one undo step, in place, no level swap.

### Deferred

Theme E, the world map itself — its own thread, on its own branch.
