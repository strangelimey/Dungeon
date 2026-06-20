# Magic System

Design notes for the dungeon's magic system. This is the living reference for
how schools, runes, and spells fit together. Start here.

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

After the first rune is selected, the **2nd-tier runes** for that school
appear as the next available choices. The specific tier-2 runes are **to be
determined**.

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

Casting is its own verb and must not fight the hand-slot gestures. Current hand
behaviour (`GameUI::OnHandLeftClick`):

- **Left-click, holding a tablet on the cursor, empty hand** → place it in the
  hand (cursor cleared).
- **Left-click, holding a tablet on the cursor, occupied hand** → swap: the
  hand's item goes onto the cursor, the cursor's item goes into the hand
  (nothing destroyed).
- **Left-click, empty cursor, hand has an item** → pick that item up onto the
  cursor.
- **Left-click, empty cursor, empty hand** → **nothing happens (for now).**
  (Unarmed attack / "activate" is being moved off the left click.)

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

- **Flat recipes vs. school-first / tier-2.** The code matches an **exact ordered
  symbol sequence** against `spells.cat`; there is no "school" concept and no
  tier-gating in code. The school-first model (first rune picks the school, then
  that school's tier-2 runes appear) and the per-school base-rune/color/stat
  table above are the **target**, not yet built.
- **Casting entry point.** The HUD Magic panel is still a placeholder
  ("No spells known"); the per-member **Magic sigil** described in "Opening the
  spell panel" is not built yet.

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
