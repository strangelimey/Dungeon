# Magic System

Design notes for the dungeon's magic system. This is the living reference for
how schools, runes, and spells fit together. Start here. The SPELL LIST — what
each spell does and how it grows — lives in `docs/spells.md`.

This document is kept in sync with the `spell-system-plan` entry in Claude's
project memory — the two carry the same design, implementation status, and
remaining work. Update both together. The sections above the line ("Schools",
"Spell construction", "Opening the spell panel") describe the **target** design;
"Current implementation status" records what the **code** actually does today and
where it diverges from that target.

## Schools of magic

There are **four** schools of magic (for now). Each school has a single **base
rune**, an associated **color**, and an associated **stat**.

| School | Base rune | Color | Associated stat |
|--------|-----------|-------|-----------------|
| Earth  | Earth rune | Brown | Stamina |
| Air    | Air rune   | White | Agility / Speed |
| Fire   | Fire rune  | Red   | Strength |
| Water  | Water rune | Blue  | Health |

### Notes

- The base rune is the foundational symbol for its school — the entry point
  for any spell drawn from that school.
- The color identifies the school visually (rune art, UI accents, projectile
  tint, etc.).
- The associated stat ties a school to a character attribute: Earth/Stamina,
  Air/Agility (Speed), Fire/Strength, Water/Health.
- "For now" — the count of four schools is expected to grow; the structure
  (base rune + color + stat per school) should generalize to additional
  schools later.

## Spell construction

A spell is built by selecting runes in sequence.

### First rune — picks the school

The **first rune selected determines the school of magic** for the spell.
Because of this, only the **four base runes** (Earth, Air, Fire, Water) are
available for selection when starting a new spell — one per school.

### Second tier and beyond

After the first rune is selected, the **2nd-tier runes** appear as the next
available choices.

**Decision (2026-07-06): tier-2 runes are SHARED FORM runes**, not per-school
sets — the Dungeon Master grammar (element + form + class). A spell reads as a
short sentence: **one school rune** (mandatory, first — picks the school and
the spell's colour), **an optional form rune** (shapes what the school does),
and later **a possible third-tier rune** refining it further. One form rune
yields up to four spells (one per school), each flavoured by its school in the
authored recipe rather than by bespoke runes — so vocabulary stays small (the
`knownSymbols` mask holds 32) while the recipe space multiplies, and the
player who learns a form with one school is invited to try it with the others.
Not every school+form combination must be authored; an unauthored combination
simply fizzles (failed experiments are part of discovery). A school may still
gain a signature specialty rune later — the enum just appends; shared-first is
not a one-way door.

The form runes (glyphs are Elder Futhark, like the schools — Fire=Kenaz,
Water=Laguz, Air=Ansuz, Earth=Berkano):

| Form | Glyph | Meaning | Status |
| --- | --- | --- | --- |
| **Project** | Tiwaz (the up arrow) | "throw it ahead" — the directed/thrown form: a single-target bolt | BUILT — Fire / Earth / Water / Air Bolt (docs/spells.md) |
| **Protect** | Algiz (the warding stave) | "guard the caster" — a ward whose behaviour the school picks: earth hardens, air deflects, water absorbs, fire retaliates | BUILT — all four shields (docs/spells.md) |
| **Sight** | Dagaz (the day-rune) | "see through the wall ahead" — a round peephole bored through the block directly in front, the school picking what it reveals: fire lights, air sees deep, earth remembers, water scrys | BUILT — the four `<school>,sight` spells (docs/spells.md) |
| **Light** | Sowilo (the sun) | "make light" - a light round the party for a while, the school picking what it does beyond its colour: fire kindles and scorches, water clears the haze, soothes and quenches, air reaches furthest, crackles and warns, earth is SET DOWN as a stone that maps what it shows and shows the tracks monsters left | BUILT - the four `<school>,light` spells (lighting-updates, docs/spells.md) |

Form runes carry no school: their tablets/UI ink use a neutral **arcane gold**
(`ElementColor(Project)`), and a cast spell always tints by its SCHOOL — the
first rune colours the whole spell.

### Third tier - the modifiers (spell-updates, 2026-10)

A third rune may follow a FORM and changes what it does - ONE of them, never on
Sight, never on a bare school rune:

| Modifier | Glyph | On a bolt (Project) | On a ward (Protect) | On a light (Light) |
| --- | --- | --- | --- | --- |
| **Multiple** | Ingwaz | a VOLLEY: more bolts as power grows, each weaker, down the caster's own lane | the ward on the WHOLE PARTY | ONE BIGGER LIGHT: cast at twice the power, lasting what the plain power buys |
| **Explode** | Hagalaz | the bolt BURSTS on impact; radius and damage grow with power | a burst of the element round the caster, sparing the caster's square - and no ward | a DAZZLING FLARE: no lasting light; the monsters within 3 steps do nothing for a few seconds, and the school's light acts once |

The grammar is enforced in one place, `Spells.h`: `TierOf` names each rune's
tier (School / Form / Modifier), `SymbolMayFollow` says what may come next (the
spellbook's buttons ask it), and `WellFormedRecipe` refuses the rest at load and
at cast. `kMaxRecipe` is 3.

### Rune names

Every rune is SHOWN by its Elder Futhark name (Michael, 2026-10-01): "the names
of the runes shouldn't be 'fire', etc. It should be the name of the Futhark rune
with 'fire' mentioned in the description." The spellbook, the tablet items
(`item.rune_<id>` = "Kenaz rune"), the memorize line and every other place read
`RuneNameKey` -> `rune.<id>`. The ids stay the meanings.

| Id | Name | Id | Name | Id | Name |
| --- | --- | --- | --- | --- | --- |
| fire | Kenaz | project | Tiwaz | multiple | Ingwaz |
| earth | Berkano | protect | Algiz | explode | Hagalaz |
| air | Ansuz | sight | Dagaz | | |
| water | Laguz | light | Sowilo | | |

The glyphs are drawn by `tools/BuildRuneIcons.py` (the UI icons) and
`tools/BuildRuneGlow.py` (the glowing halo), and carved on the tablets by
AssetBaker's RuneBaker; all ten sit in the Magic dock's grid, one row per tier.
Sowilo (`SpellSymbol::Light`) was APPENDED after Hagalaz, so it is bit 9 of
`knownSymbols` and every older save reads unchanged.

## Opening the spell panel

The spell-construction panel (where runes are selected to build a spell) needs
a way to open that does **not** depend on owning any magic items, so a brand-new
caster with empty hands can still cast.

**Decision: a dedicated Magic sigil in the HUD's reserved Magic area.**
The DM-style control panel already reserves a "Magic area" below the per-member
hand slots. A small per-member rune/sigil button lives there and opens the
spell-construction panel for that caster. This is **item-independent** — it
works from day one with empty hands.

Later, as characters progress and acquire a **focus item** (wand, spell book,
etc.) held in a hand, right-clicking that item becomes an *additional* shortcut
to the **same** panel. One panel, multiple doors; the item door simply unlocks
later. (A possible future rule: empty-handed casting requires a free hand, while
a focus item lifts that restriction — noted but not decided.)

### Hand-click semantics (to keep casting off the hands)

Casting is its own verb and must not fight the hand-slot gestures. Hand
behaviour since the use-menu model landed (branch `magic-system`,
`GameUI::OnHandLeftClick` / `OnHandRightClick`):

- **Left-click, holding a HOLDABLE item on the cursor** → place it in the hand,
  swapping any occupant onto the cursor (nothing destroyed). Items without the
  catalog `holdable` flag are refused with a log line — on the control bar AND
  the sheet's hand doll cells.
- **Left-click, empty cursor, control-bar hand** → execute the hand's DEFAULT
  USE: the member's remembered per-item-type pick, else the item's first
  defaultable `command`; an empty hand throws the unarmed punch. (Picking an
  item OUT of a hand is the character sheet's job — its hand cells keep the
  pick/swap semantics.)
- **Right-click, control-bar hand** → the item's USE menu (catalog `command`
  list: stab/slash/eat/memorize/...). Selecting an entry records it as that
  member's default for THAT HAND and the item type (defaults are per member
  AND per hand — Michael, 2026-07-07 — so left-click on the left hand can be
  one spell and on the right another) and — per the Settings → Controls
  "Hands" checkbox — performs it. Menu-only commands (memorize) always
  perform and never become defaults.
- **Left-click on a hand with NO default yet** (bare hand, or an item with no
  defaultable command — rune, key) → the same use menu opens, so the first
  click picks what future clicks will do. For those hands the menu is
  TWO-LEVEL: **Combat** → Punch / Kick, and **Magic** → the Spellbook plus the
  spells the member has LEARNED — a spell is learned the first time the member
  successfully CASTS it (built in the spellbook; saved per character), so the
  quick-cast list is earned, not implied by vocabulary. The quick-cast list is
  THAT HAND's most-recently-cast spells (the MRU is per member AND per hand,
  like the defaults; a cast credits the hand it was fired from, a spellbook
  cast the hand whose menu opened the book). Higher-tier spells
  demand higher school skill and can FAIL to cast (the skill roll in
  docs/skills.md — mana spent, nothing learned). A spell pick stores as `cast:<id>` in the same
  default map ("unarmed" key for a bare hand) and left-click then casts it —
  **the first casting door is live**: memorize a rune, arm the spell from the
  hand menu, click to cast (DungeonWorld::CastSpellById, the usual vocab/mana
  gates). A held wand/spellbook later becomes the richer second door — its use
  menu listing its own spells rides this exact mechanism.
- **The SPELLBOOK** (Magic » Spellbook, the submenu's first entry; the Magic
  group appears once the member knows ANY symbol) opens the `SpellbookPanel`
  in the HUD's Magic area — **where the player BUILDS a spell**: the member's
  known symbols as rune buttons, a six-slot sequence spelled out by clicking
  them (click a filled slot to take it back), a live "= <spell>" label when
  the sequence matches a recipe the member has already LEARNED (an unlearned
  match stays anonymous — the book never confirms a discovery before the
  first successful cast does; experimentation is the point), and Cast (fires
  DungeonWorld::CastSpell — exact match casts, a miss fizzles) / Clear. This panel is the seed the
  school-first construction (first rune picks the school → tier-2) will grow
  into; today it exposes the flat exact-sequence model directly.

## Current implementation status

What the code actually does today, and where it diverges from the target design
above. (Phase labels P1–P6 track the build-out order.)

### Built — P1–P4, merged to main (`ed733c1`)

- **Symbols + per-character vocabulary (P1).** `SpellSymbol` enum
  {Fire, Earth, Air, Water}; `Character` carries a `knownSymbols` bitmask
  (`Knows`/`Learn`) plus an `intelligence` attribute (5th sheet row) that drives
  mana regen. Vocabulary is per character. Round-trips through save.
- **Rune-tablet items + pickup (P2).** Runes are carved-stone **tablets**
  (`rune_tablet.gltf` + per-element carved textures from `tools/AssetBaker/RuneBaker`;
  Elder Futhark Fire=Kenaz / Water=Laguz / Air=Ansuz / Earth=Berkano). Flow:
  click the in-world tablet to pick up → it rides the cursor → left-click a
  portrait (→ backpack), a hand slot (→ swap/place), or the world (→ drop);
  right-click a hand holding a rune → context menu **Memorize** → `Character::Learn`
  (tablet consumed). Per-character `Inventory` (8-slot backpack + 2 hand slots)
  replaced the old shared satchel. Save v3 carries hands+backpack per char and a
  per-level floor-items snapshot.
- **Recipes + cast + mana (P3).** `spells.cat` holds 5 tier-1 recipes
  (flame / rock / gust / splash / firebolt) with fields
  `symbols`/`effect`/`element`/`power`/`mana`/`speed`/`range`. `SpellBook`
  (`Spells.h/.cpp`) builds the table and does **exact-sequence `Match`**. Cast
  checks mana, deducts it, dispatches the typed effect. Mana regenerates
  per-frame as a function of `intelligence` (`ManaRegenPerSec`).
- **Projectiles (P4).** A cast spawns a travelling bolt at the party eye that
  flies the faced direction cell-by-cell, impacts the first live monster
  (`ResolveAttack` + particle burst + log) or fizzles on a wall / at max range.
  Bolts + impact sparks render as additive billboards. Transient — **not** saved.

### Built - the three tiers (spell-updates, 2026-10; docs/spell-updates-plan.md)

- **Runes (P1).** Nine `SpellSymbol`s across three tiers, shown by their
  Futhark names; the grammar in `Spells.h` (see "Third tier" above). Ingwaz and
  Hagalaz tablets (`rune_multiple`, `rune_explode`) ride in both casters'
  starting packs.
- **What a spell can reach (P2).** `CastContext` gained the casting `hand` and
  the `party`; `CastServices` grew from "spawn a bolt, say a line" into the
  world hooks the new spells need - `fireAhead` / `setFireAhead` /
  `flareAhead`, `lightItem` / `fillItem` (rename a held item by its `lit_as` /
  `fill_as`, so a spell never learns what an item kind is), `dropAtFeet`,
  `shoveAhead`, `repelAhead`, `blastAroundParty`, `spawnBoltAfter`. Each is
  driven bare from the console by `castsvc`.
- **Fires and torches (P3, P4).** Fires are live, saved state; the held torch is
  the party's light and burns down; items carry a charge. docs/torches-and-fire.md.
- **The hand spells (P5).** Flame, Rock, Gust and Splash are `HandSpell`s, no
  longer bolts (docs/spells.md, Tier 1).
- **Bolts and modifiers (P6, P7).** The four Project spells are single-target
  bolts (`firebolt`, `earthbolt`, `waterbolt`, `airbolt`); `ModifiedSpell` makes
  the sixteen tier-3 spells; monster casters may name any of them, and a volley
  launches through a fixed pending-bolt queue.
- **Checked (P8).** `tools\SpellTest.py` judges `spells.eval` (33 checks,
  self-tested by cutting every cast); `AllocTest.ps1 -Hand`.

### Built - the lights (lighting-updates Phase 6, 2026-10; docs/lighting-updates-plan.md)

- **The rune (6a).** Sowilo, a fourth FORM rune; `SpellIdList` grew from 32 ids
  to 64 (it held exactly 32 for exactly 32 spells, so the next spell would never
  have been learned) and `SpellBook::Build` warns if the registry outgrows it.
  MERGE NOTE: the tablet textures are gitignored, so after a merge run
  `AssetBaker runes assets` then `AssetBaker mips assets rune_`.
- **The light form (6b).** `Spell/LightSpell` + Firelight / Tidelight /
  Skylight / Stonelight. A light is ONE effect kind, `light` (effects.cat;
  `Effect/LightEffect`), landing on the CASTER with the school on the instance
  (so schools stack, a recast replaces its own), magnitude = the cast power.
  The world reads it every frame (`DungeonWorld_SpellLight.cpp`): a light per
  (member, school) above the party from lights.cat `spell_<school>`, sized
  sqrt(power / `scale_power`) within 0.7..1.8, dimming over its last tenth;
  duration = spells.cat `duration` x power / the spell's power (never under
  half). `ModifiedSpell` learned to wrap a light (`<id>_bright`, `<id>_flare`);
  the flare is the `lightFlare` cast service and `dazzle`, an effect under which
  a monster skips its turn.
- **What each school does (6c-6g),** all knobs on effects.cat [light]: FIRE
  kindles unlit fires within a step and scorches each monster beside the party;
  every skeleton resists fire (0.75) and a `flammable` monster (the mummy)
  catches from ANY fire that lands on it (`MonsterTarget::Wound`, balance.cat
  `ignite_burn` / `ignite_seconds`). WATER cuts a clear bubble in the haze (a
  negative dust puff), doubles stamina regen, and puts out any fire on the
  party. AIR reaches furthest, shocks the nearest monster in reach and sight,
  and flickers faster while the party is noticed. EARTH is SET DOWN: a stone in
  the cast's square (`placeLightStone`, a fixed 8 a level, saved with the
  level) that maps every square its light reaches and shows the TRACKS monsters
  left there (`m_tracks`, written in `StepMonsterTo`, fading over balance.cat
  `track_life`, saved as ages).
- **Checked.** `AllocTest.ps1 -Light` (the whole rotation cast inside the
  window, the flare's dazzle on a mummy, the stone showing planted tracks);
  SpellTest 39.
- **For now** each caster starts with a Sowilo tablet (`rune_light`) in the
  backpack (Michael, 2026-10-05); no level places one yet.

### Module layout

Magic is a **walled-off module** (it knows nothing of map/monsters/HUD):

- **`Spell/` — the spell classes** (decision 2026-07-07: isolate the
  hard-coding, no Lua). `Spell` is the base — id, name/description loc keys,
  the SYMBOL RECIPE (first rune = school), mana, base power — with a pure
  virtual `Cast(CastContext&)` where each spell's behaviour lives. The shared
  forms are intermediate classes (`HandSpell` for the tier-1 spells and the
  hand they look in; `BoltSpell` flies the bolt + serves `MonsterBolt` for
  monster casters; `WardSpell` lands the school-keyed ward; `SightSpell` the
  peephole), and every concrete spell is its own file pair (`Flame`, `Rock`,
  ..., `Windward`) constructed with its numbers. The tier-3 spells are the one
  exception: `ModifiedSpell` WRAPS a Bolt or Ward spell with a modifier, and
  AllSpells.cpp makes one per form spell and modifier rather than a file pair
  each. `AllSpells.cpp` is the registry list; adding a spell = file pair + one
  line there + CMakeLists. A `Cast()` reaches the world only through
  `CastServices` the host wires once (see "Built - the three tiers").
- **`Spells.h/.cpp` — the alphabet + registry.** `SpellSymbol`, shared
  `ElementColor(SpellSymbol)` (DungeonWorld::RuneGlow delegates to it), and
  the `SpellBook`: the concrete classes with the project's **spells.cat
  NUMERIC OVERRIDES** laid on top (matched by id — data tunes numbers, never
  redefines recipes; mismatched/stray entries are warned about). Kept
  lightweight (no gfx) because `Character.h` includes it.
- **`Magic.h/.cpp` — `MagicSystem`** owns the `SpellBook` and runs the COMMON
  cast gates (vocabulary, mana, the skill/fumble roll, power scaling), then
  hands the landing to `spell->Cast(ctx)`. Projectiles fly in the shared
  moving-item engine, whose three hooks (`isBlocked` / `resolveHit` /
  `onFizzle`) the owner wires once.
- **`DungeonWorld`** holds a `MagicSystem m_magic`, wires the hooks + cast
  services in its ctor; `CastSpell` is a thin façade (party eye+facing →
  `m_magic.Cast` → the common aftermath: log, learning, the firing hand's
  MRU, school XP), `ResolveSpellHit` is the impact hook.
- **`Project`** carries the `spells` catalog (`CatalogForKey "spells"` — the
  overrides). Dev console `cast <member> [hand] <sym>...`. Strings:
  `log.cast` / `spell_fizzles` / `cast_nomana` / `cast_unknown` /
  `cast_fumble` / `spell_hits` / `spell_misses` / `spell_slain` + `spell.*`
  names in `en.lang`.

### Gap between the build and the target design

- **School-first + tier-2: BUILT.** The one-school rule (exactly one element
  rune, first position) is enforced in `Spells.h`/`SpellBook::Build` and the
  spellbook UI (`SymbolAvailable`: the four schools go dark once one is down;
  form runes wait until a school leads). Three shared form runes are live:
  **Project** with its four `<school>,project` single-target bolts - Air Bolt
  carrying the engine's first displacement effect (`push`) - **Protect** with the
  shield framework (`SpellEffect::Shield`: caster-only wards that stack
  across schools — same school recast replaces — school-keyed behaviour,
  timed fade) carrying all four shields: Stone Skin
  (armor), Fire Shield (melee retaliation), Water Veil (absorb pool, bursts
  when spent), Wind Ward (bolt deflection charges, stills when spent) — and
  **Sight** (`StatusKind::Sight`, a caster-only timed marker like a ward) that
  ghosts a round PEEPHOLE through the wall block directly ahead in the
  first-person view (a scene-shader hole + thin school-tinted rim, driven by
  the `sightCell`/`sightTint`/`sightHole` frame constants — no mesh rebuild),
  carrying all four peeks: Ember Sight (fire lights the room beyond), Far Sight
  (air bores the hole deep down the row), Stone Sight (earth maps the revealed
  room + longest duration), Scrying (water's wider, clearer window; hidden-
  content reveal is future). Recipes are still matched as exact ordered
  sequences; that IS the model now (the grammar is authored into the recipes,
  not parsed).
- **Status effects are a unified list, and they STACK.** A ward lives in
  `Character::effects` (`StatusEffect`: kind + school + magnitude + time;
  save v14 "effect" lines, v13 "shield" lines still load) — the one list
  every future condition (poison, injury, item buff) joins. Effects of
  different identities coexist (all four wards at once is legal); only
  recasting the SAME school replaces its ward. The party bar draws an icon
  per active effect in the member's NAME band, right-aligned and growing
  right-to-left (school-tinted border + depleting time sliver; hover names
  it with its time left; spill-over handling deferred). The character
  sheet's fourth tab (Effects, the hourglass button) is the long-form view:
  the same icon at reading size plus name, a magnitude-formatted
  description (`<nameKey>.desc` loc keys), and the time left.
- **Casting entry point.** The spellbook panel (Magic » Spellbook) is built;
  the per-member **Magic sigil** described in "Opening the spell panel" is not
  built yet (the hand menu is the only door today).
- **Per-school caster POWER: BUILT** (docs/skills.md) — the school SKILL
  grows with successful casts; effective power scales catalog numbers by
  (1 + 0.10 × level), the skill roll can fumble higher-tier casts, and the
  skill's associated stat creeps behind. Growth is now concrete (spell-updates):
  the tier-1 spells read power against THRESHOLDS (`brazier_power`,
  `push_power`), and the modifiers scale with it (more bolts in a volley, a
  wider and harder burst). The older growth ideas (flamethrower, boulder) were
  superseded by the third tier.

### Remaining work

- **P5 — Casting UI.** Build the HUD Magic panel per the design above: the
  per-member Magic sigil opens the spell-construction panel; caster picker,
  school-first rune selection (only the four base runes to start, then tier-2),
  the built sequence row, Clear + Cast. Wire `GameUI.onCast`. Defer-rebuild the
  panel on any vocab change (like the language/video rebuilds). The character
  sheet's Runes section (known symbols, Memorize) is its sheet-side companion.
- **P6 — Content + verify.** Place runes in a level's `.ent` (Ingwaz and Hagalaz
  ride in both casters' starting packs instead - docs/spells.md); the starter recipes
  already live in `spells.cat`. Full `drive.ps1` playthrough: pick up runes,
  memorize, cast at a monster, watch the bolt fly + impact; screenshots.

## To-do / open ideas

### Dynamic symbolic-language parser (ambitious — may not be practical)

In **Dungeon Master**, there were 4 tiers of magic symbols, all **fixed**.
Clicking a symbol from one tier revealed the next tier. Spells were defined by
fixed recipes: "this symbol then this symbol (etc.) makes this spell."

The idea to explore: instead of fixed recipes, add a **symbolic-language
parser** that takes each symbol in the spell and **figures out what it does at
runtime** — i.e. the runes compose into meaning like a small language, rather
than matching against a hardcoded recipe table.

This is **quite ambitious and might not be practical**, but it's noted here as
a direction worth considering. Captured so the option isn't lost; no decision
made yet.
