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
| **Project** | Tiwaz (the up arrow) | "throw it ahead" — the directed/thrown form | BUILT — see the four `<school>,project` spells in docs/spells.md |
| **Protect** | Algiz (the warding stave) | "guard the caster" — a ward whose behaviour the school picks: earth hardens, air deflects, water absorbs, fire retaliates | BUILT — all four shields (docs/spells.md) |

Form runes carry no school: their tablets/UI ink use a neutral **arcane gold**
(`ElementColor(Project)`), and a cast spell always tints by its SCHOOL — the
first rune colours the whole spell.

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
  member's default for the item type and — per the Settings → Controls "Hands"
  checkbox — performs it. Menu-only commands (memorize) always perform and
  never become defaults.
- **Left-click on a hand with NO default yet** (bare hand, or an item with no
  defaultable command — rune, key) → the same use menu opens, so the first
  click picks what future clicks will do. For those hands the menu is
  TWO-LEVEL: **Combat** → Punch / Kick, and **Magic** → the Spellbook plus the
  spells the member has LEARNED — a spell is learned the first time the member
  successfully CASTS it (built in the spellbook; saved per character), so the
  quick-cast list is earned, not implied by vocabulary. Higher-tier spells
  will demand higher school skill and can FAIL to cast (failure system TBD);
  a failed cast teaches nothing. A spell pick stores as `cast:<id>` in the same
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
  the sequence matches a recipe, and Cast (fires DungeonWorld::CastSpell —
  exact match casts, a miss fizzles) / Clear. This panel is the seed the
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

### Module layout

Magic is a **walled-off module** (it knows nothing of map/monsters/HUD):

- **`Magic.h/.cpp` — `MagicSystem`** owns the `SpellBook`, live projectiles, and
  sparks; does `Cast`/`Update`/`AppendBillboards`. It reaches the world only
  through three `std::function` hooks the owner wires once: `isBlocked(pos)`
  (wall/OOB), `resolveHit(pos, AttackProfile)→bool` (combat + feedback; true =
  consume bolt), `onFizzle(pos)` (sound).
- **`Spells.h/.cpp` — data layer.** `SpellSymbol` alphabet, `SpellDef`/
  `SpellEffect`, the `SpellBook` recipe table, and shared
  `ElementColor(SpellSymbol)` (DungeonWorld::RuneGlow delegates to it). Kept
  lightweight (no gfx) because `Character.h` includes it.
- **`DungeonWorld`** holds a `MagicSystem m_magic`, wires the hooks in its ctor;
  `CastSpell` is a thin façade (party eye+facing → `m_magic.Cast` → turn the
  `CastReport` into log + sound), `ResolveSpellHit` is the impact hook.
- **`Project`** gained a `spells` catalog (`CatalogForKey "spells"`). Dev console
  `cast <member> <sym>...`. Strings: `log.cast` / `spell_fizzles` /
  `cast_nomana` / `cast_unknown` / `spell_hits` / `spell_misses` / `spell_slain`
  + `spell.*` names in `en.lang`.

### Gap between the build and the target design

- **School-first + tier-2: BUILT.** The one-school rule (exactly one element
  rune, first position) is enforced in `Spells.h`/`SpellBook::Build` and the
  spellbook UI (`SymbolAvailable`: the four schools go dark once one is down;
  form runes wait until a school leads). Two shared form runes are live:
  **Project** with its four `<school>,project` spells — including the engine's
  first displacement effect (`push`, the air shove) — and **Protect** with the
  shield framework (`SpellEffect::Shield`: one caster-only ward per member,
  school-keyed behaviour, timed fade, save v13) carrying all four shields:
  Stone Skin (armor), Fire Shield (melee retaliation), Water Veil (absorb
  pool, bursts when spent), Wind Ward (bolt deflection charges, stills when
  spent). Recipes are still matched as exact ordered sequences; that IS the
  model now (the grammar is authored into the recipes, not parsed).
- **Casting entry point.** The spellbook panel (Magic » Spellbook) is built;
  the per-member **Magic sigil** described in "Opening the spell panel" is not
  built yet (the hand menu is the only door today).
- **Per-school caster POWER** (the growth forms in docs/spells.md scale by it)
  has no progression system yet — spells cast at fixed catalog numbers.

### Remaining work

- **P5 — Casting UI.** Build the HUD Magic panel per the design above: the
  per-member Magic sigil opens the spell-construction panel; caster picker,
  school-first rune selection (only the four base runes to start, then tier-2),
  the built sequence row, Clear + Cast. Wire `GameUI.onCast`. Defer-rebuild the
  panel on any vocab change (like the language/video rebuilds). The character
  sheet's Runes section (known symbols, Memorize) is its sheet-side companion.
- **P6 — Content + verify.** Place runes in a level's `.ent`; the starter recipes
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
