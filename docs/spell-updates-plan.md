# Spell updates - plan

Branch `spell-updates`. The notes, organized, and the open questions are in
docs/spell-updates-notes.md. This plan assumes the RECOMMENDED answer to each
open question (marked "Q<n>"); an answer that differs changes only the phase
that names it.

## Phase 1 - Rune vocabulary (no behaviour change)

- Append `Ingwaz` and `Hagalaz` to `SpellSymbol` (Spells.h; append-only, so the
  known-symbols mask and saves are untouched). Ids `multiple` / `explode`, both
  non-school, gold like the other forms.
- FUTHARK NAMES (note 9): a new key family `rune.<id>` = "Kenaz", "Tiwaz", ...
  for the rune's NAME; `item.rune_<id>` reads "Kenaz rune" and its `.desc`
  carries the meaning ("...the rune of fire"). `symbol.<id>` STAYS the element
  word, because the weapon details row and the memorize line use it as
  "fire", not as a rune. Ids never change - they are skill ids, damage-type
  schools and saved item ids.
- Assets: RuneBaker's spec table 7 -> 9 (strokes for ᛜ and ᚺ), BuildRuneGlow's
  list, the three UI images per rune, the tablet texture sets.
  `ItemIconBank::kRuneSlots` 8 -> 12.
- `rune_ingwaz` / `rune_hagalaz` items in all three items.cat copies; lang x5.
- The Magic dock's rune grid gains its third row (fixed panel height: check
  the spell-name line, run `uioverlap`); the hard-coded `RunePhase(i+7)` offset
  derives from the count.
- Recipe ORDER (Q4 ANSWERED): school, then form, then ONE modifier - enforced
  in `SymbolAvailable` so the grid only offers a legal next rune (a modifier
  only after a form; none after another modifier). Sight + a modifier fizzles.

## Phase 2 - What a spell can reach

- `CastContext` gains the casting HAND (today dropped in MagicSystem::Cast;
  the spellbook's both-hands case means "no hand": a hand spell then uses the
  first free / matching hand) and the ROSTER (a span), appended at the end.
- `CastServices` gains the world hooks the new spells need, each a lambda wired
  in DungeonWorld like the existing three: the fixture ahead (sconce on the
  facing wall of the party's square, brazier in the square ahead), light /
  douse / flare it, put an item in a hand, swap a held item's id, push the
  monster or projectile ahead, detonate at a cell with an EXCLUDED cell.
- Spells stay walled off: they ask the services, never DungeonWorld.

## Phase 3 - Fire as live, saved state

- Runtime lit/unlit for a wall torch or brazier: flip the live `Fire` (its
  effect via `FireEffect::Ignite`/`Clear`, no allocation) AND the record.
  Fixes the likely bug where a smashed sconce keeps burning (DouseFixture
  touches only the record) - check it first, it is one test.
- Save the lit state as a DIFF from the authored map (the niche `open` pattern),
  never by writing the runtime value into the static record - today a douse
  leaks into `savemap` and the editor's stash.
- FLARE: a decaying `flareT` per fire, scaling light intensity / radius and the
  flame's spawn rate (capacity reserved at load so a flare is not clipped).
- TRANSIENT TURBIDITY: a small fixed array of dust "puffs" (cell, strength,
  decay) in the scene constant buffer, added in `DustDensity()` - the Sight
  peephole's route. The baked grid is rebuilt only when a fire's lit state
  CHANGES (its turbidity ring comes or goes), not per frame.

## Phase 4 - Torch and water items (Q1, Q2)

- `torch` (lit) and `torch_unlit`, holdable, swapped in place. Model: the
  bought torch pack if it imports, else a script-built one.
- Q1 ANSWERED: THE HELD TORCH IS THE LIGHT. The unconditional eye light
  (DungeonWorld.cpp ~696) goes; a lit torch in any member's hand puts a light
  at that member's side of the eye (it keeps shadow slot 0 by being nearest).
  No lit torch = dark, to the floor the follow-ups set. The HUD torchlight
  colour dropdown becomes the torch's colour or goes. A new game's party needs
  a lit torch in the starter kit, or the first room is black. Every eval/
  AllocTest that renders assumes today's light - a starter torch keeps them
  honest.
- DARKNESS (answered): no torch = the level's `atmosphere ambient=` and nothing
  else; ambient 0 is pitch black. Fixed fires and spell light still show.
- BURN-DOWN (answered, reversed): a lit torch IN A HAND burns down over time;
  stowing it in a pack or putting it on the floor PUTS IT OUT (it keeps what is
  left). This needs PER-ITEM state, which items lack (a slot is an id): the
  phase decides between a small numeric field on ItemSlot / HeldItem /
  floor items (saved beside the id - the honest route, and the one food,
  charges and fill levels would reuse) and stage ids (`torch_75` ...). A
  spent torch becomes a stub or vanishes - decided in the phase.
- SUPPLY (answered): the starter kit holds one lit torch; `torch_unlit` is
  placed in levels; a click on a wall torch (from its square, facing its wall -
  the lever's rule) TAKES it into the leader's free hand and the sconce goes
  dark and EMPTY. A sconce therefore has three states - lit, unlit, empty (no
  torch mesh) - all saved as a diff. The inverse (a held torch clicked onto an
  empty sconce mounts it) falls out of the same code; confirm at play-test.
  Puff of Flame / Splash on an EMPTY sconce do nothing.
- Water (Q2 ANSWERED, as recommended; no flask): `waterskin_empty` / `waterskin_half` / `waterskin` (full).
  Drinking steps it down a level instead of deleting it (ConsumeItem returns
  the container). Splash steps an empty or half one up one level.

### Phase 4 - as built (2026-10-01)

- PER-ITEM CHARGE (answered): ItemSlot / HeldItem / floor Item carry a
  `charge` (kNoCharge = none / untouched) through every swap, pick, drop, throw
  (ProjectileSpec::cargoCharge) and save (`id#charge` tokens; a drop's 6th
  field), so a half-burnt torch is half-burnt wherever it goes.
- TORCH TIERS (answered): crude (pack #2) 10 min, common (#1) 15, fine (#5) 25;
  each an unlit + a lit item (`lit_as` / `unlit_as`), `burn_time`, and
  `spent_as = torch_stub` (answered: a stub, not nothing). Models imported by
  FetchModels with the new ConvertMesh `--bind-textures`.
- The LIGHT is every lit torch held (hands + cursor), at its member's side of
  the eye, dimming over its last tenth (DungeonWorld_Light.cpp). Sera starts
  with a lit common torch in her free right hand.
- Out: stowed in a pack (per-frame pass) or anything placed on the floor
  (PlaceDrop). A torch on the CURSOR still burns and lights.
- WALL TORCHES: a click ON one from its square takes it (lit if it burned) into
  the leader's free hand, else onto the cursor; the bare bracket stays
  (`empty_model = wall_torch_bracket`, tools/BuildWallTorchBracket.py, fitted
  to wall_torch.gltf exactly). A torch clicked onto an empty bracket mounts,
  lit or not. Saved (`fire ... empty`).
- WATERSKIN: full / half / empty, 25 water a drink, `drink_as` steps it down.
- Known, for the balance pass / play-test: resting runs the world at 60x, so a
  held torch burns 60x as fast while resting; a mounted torch loses its charge
  (taking it back gives a full one); the stub wears the crude torch's model.

## Phase 5 - The four hand spells (notes 1-5)

Each loses BoltSpell and gets its own `Cast()`. Thresholds are spells.cat
knobs, so the balance pass owns them.
- Kenaz / Puff of Flame: other-hand `torch_unlit` -> `torch`; else the wall
  torch ahead lights; else the brazier ahead lights if power >=
  `brazier_power`; else a puff and a "nothing catches" line.
- Berkano / Pebble: a `pebble` (small, light, throwable - a smaller cousin of
  `rock`) into the casting hand if empty, else the other, else it drops at the
  caster's feet (Q3 ANSWERED, as written).
- Ansuz / Puff of Wind: flares the torch / brazier ahead; at power >=
  `push_power` (Q6 ANSWERED: every threshold reads cast power) pushes the monster ahead back a square, or turns back a
  projectile in the square ahead (a new ProjectileSystem method - nothing can
  redirect a projectile today). Monster arrows are the generic ranged bolts.
- Laguz / Splash: fills a held container one step; else douses the wall torch
  ahead; else the brazier ahead at power >= `brazier_power`; a douse puts a
  turbidity puff on that square.
- Monster use: a tier-1 spell has no MonsterBolt; `skel_mage` moves to Fire
  Bolt in Phase 6 (Q9 ANSWERED). Higher-level mages volley (Ingwaz) and higher
  still explode (Hagalaz) - Phase 7.

### Phase 5 - as built (2026-10-01)

- The four are HandSpells (Spell/HandSpell.h: OtherHands / LandingHands - a cast
  from no hand looks in both, right first). Mana 2 each; power is only what the
  thresholds read: Flame / Splash `brazier_power` 14 / 12, Gust `push_power` 8
  (+ `push` 1 square). At school level 0 none reaches them; level 30 does.
- Two new services, so a spell never learns what an item kind is: `lightItem`
  (an item's `lit_as`) and `fillItem` (`fill_as`: empty -> half -> full
  waterskin), both DungeonWorld::RenameHeldItem.
- Pebble conjures `conjures = pebble` (a new throwable, 0.1 kg; the rock's model
  for now). The skeleton mage casts fireburst until Phase 6 renames it.
- Not built: a visible puff in the hand - every spell says what it did in the
  message log, and the world shows it (a torch lights, a brazier flares).
- Harness moved off Kenaz-as-a-bolt: the evals and AllocTest -Cast spell Kenaz
  Tiwaz (with fire skill 5, so it cannot fumble); -Impact fires Water Bolts.

## Phase 6 - Tier 2 bolts (note 6)

- The four Project spells become single-target bolts: no blast fields, one
  strike. Display names Fire Bolt / Water Bolt / Earth Bolt / Air Bolt; ids
  `firebolt` / `waterbolt` / `earthbolt` / `airbolt` (Q8 ANSWERED - a renamed id drops
  from an old save's known list, the usual dev-cycle cost).
- "Too powerful" (note 1) is answered for tier 1 by Phase 5; the bolts keep
  today's numbers as the first cut for the balance pass, minus the blast.

## Phase 7 - Tier 3 modifiers (notes 7, 8)

GENERIC, not one class per combination: a modifier is a transform the form
spell applies to its own cast, so 2 modifiers x 2 forms x 4 schools needs no
16 new classes. `Spell` gains `CastModified(ctx, modifier)` with a default
that fizzles; BoltSpell and WardSpell override it. spells.cat may author
per-spell numbers under the modified recipe (`[firebolt+explode]`-style
blocks or `explode_*` fields - decided in the phase).
- Ingwaz on a bolt: N = f(power) bolts (Q5 ANSWERED): staggered a beat apart,
  all in the CASTER'S lane, each with a little side-to-side jitter that stays
  inside the lane (so the lane hit test still holds). Each is its own strike.
  Launching after the cast frame needs a small pending-launch queue in the
  world (fixed size, no allocation).
- Hagalaz on a bolt: the bolt detonates on impact; radius and damage are
  rewritten per cast from power (today blast damage ignores power).
- Ingwaz on a ward: the ward goes on every standing member.
- Hagalaz on a ward: a blast of the school's element centred on the caster's
  square, which is EXCLUDED (a new Detonate parameter - the propagation already
  treats an impassable centre as a phantom source).
- MONSTER CASTERS (Q9 ANSWERED): monsters.cat `spell` may name a modifier
  (`spell = firebolt`, `spell = firebolt ingwaz`, `spell = firebolt hagalaz`),
  read through the same modifier path as the party's casts. The mage ladder is
  authored as monster kinds (e.g. skel_mage / an adept / a magus - names
  and numbers at authoring time), not a per-instance level, since monsters
  have none today.
- Fumble (Q7 ANSWERED): unchanged - ~70% for a 3-rune spell at level 0 is
  intended; the balance pass owns it.
- Where the runes are found (Q10 ANSWERED): placed tablets in a level (deeper,
  guarded), not the starter kit. The harness learns them with `learn`.

### Phases 6 + 7 - as built together (2026-10-01)

They had to land at once: Fire Burst was the only blast, and the blast suites,
the pipeline check and AllocTest -Impact detonate one.
- BOLTS: fireburst -> `firebolt` (one target, no blast, still ignites),
  slingshot -> `earthbolt`, push -> `airbolt`; waterbolt kept. Classes renamed.
- MODIFIERS are ONE class, Spell/ModifiedSpell.h, made by AllSpells.cpp for
  every Project and Protect spell x {Ingwaz, Hagalaz}: sixteen whole spells
  (`<bolt>_volley`, `<bolt>_burst`, `<ward>_party`, `<ward>_burst`), so
  learning, the book, the hand menus and saves needed nothing new. Each has its
  own spells.cat entry (power, mana, knobs). Volleys go through a fixed
  pending-bolt queue (cast service `spawnBoltAfter`, DungeonWorld_Ahead.cpp).
  Answered: a ward burst is the burst INSTEAD of the ward; a volley's bolts are
  each weaker (`share` 0.6).
- Fire Burst's blast tuning moved whole to `firebolt_burst` (the blast suites,
  pipeline, worldpersist and AllocTest -Impact now detonate that; the fire flask
  bursts as it). TRAP met on the way: `blast_force` counts SQUARES, not a
  radius - a ward burst at force 3 filled three of the four squares round the
  caster. Ward bursts are force 6-8.
- MAGE LADDER: skel_mage casts firebolt; new skel_mage_adept (firebolt_volley)
  and skel_magus (firebolt_burst); monsters cast a volley through
  Spell::MonsterVolley + the same queue.

## Phase 8 - Harness and checks

- AllocTest -Cast / -Impact cast `fire` / `flame` / `fireburst` as bolts and
  blasts: move them to Fire Bolt and Fire Bolt + Hagalaz, and ADD a hand-spell
  case (light / douse / flare / fill / pebble inside a guarded window - the
  item swaps must not allocate).
- Evals: `pipeline` (a bolt in flight), `blast-geometry` / `worldpersist` /
  `pipeline` (`blast fireburst`), `resources` / `resettest` (`cast 3 fire`)
  move to the new recipes. A new `spells` suite: each hand spell's outcomes,
  each modifier, the thresholds, a douse surviving save/load.
- `/check-ingame` (rune grid third row), `/check` quick tier, `/check-alloc`.

## Phase 9 - Docs

docs/spells.md and "docs/magic system.md" (tiers, runes, Futhark names),
CLAUDE.md's MAGIC bullet, the tier-1 `spell.<id>.desc` lines (they say
"flung ... for {} damage").
