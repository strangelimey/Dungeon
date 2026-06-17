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
