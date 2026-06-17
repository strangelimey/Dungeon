# Magic System

Design notes for the dungeon's magic system. This is the living reference for
how schools, runes, and spells fit together. Start here.

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
