# Spells

The living list of spells. Details accrete here as they are designed; the
implementation column tracks what the code actually does today. Add new spells
as sections, keep the catalog (`assets/projects/dungeon-demo/catalog/spells.cat`)
and this list in step.

Rules of the system (see `docs/magic system.md` for the full model):

- A spell is a SEQUENCE of runes. The four element runes are SCHOOL runes —
  mutually exclusive, exactly one per spell, in first position: the first rune
  picks the school.
- A spell's strength scales with the caster's POWER in its school — a
  progression stat that grows as the caster uses that school's magic (system
  TBD; today spells have fixed catalog numbers).
- A character LEARNS a spell the first time they successfully CAST it (built
  in the spellbook). Only learned spells appear in the hand menu's Magic
  quick-cast list or can be armed as a hand default; learning is saved per
  character. Higher-tier spells will demand higher school skill, so a cast
  can FAIL (failure system TBD) — a failed cast teaches nothing.

## Tier 1 — the four one-rune spells

The base rune alone, castable the moment the symbol is memorized. Each starts
almost trivial and GROWS with the caster's school power.

### Earth — Pebble (`rock` in spells.cat)

Summons a small pebble, thrown as a projectile. Its SIZE (and punch) increases
as the caster's earth powers increase — from gravel toward a real stone.

- Today: a plain damage bolt (the heaviest, slowest tier-1 projectile).
- Growth: size/damage scale with earth power.

### Air — Puff of Wind (`gust` in spells.cat)

A puff of wind. Not much use at the start — but it will grow into a gust that
PUSHES MONSTERS AWAY (a shove down the faced row, not a damage bolt).

- Today: a light, fast damage bolt (placeholder behaviour).
- Growth: the push effect — knockback distance/weight class scales with air
  power. Needs a push/displacement effect kind in the engine.

### Fire — Puff of Flame (`flame` in spells.cat)

A puff of fire. Useful to LIGHT TORCHES and SCONCES; gives a brief FLASH — a
short-lived light source in the dark. Eventually grows into a fire blast.

- Today: a plain damage bolt.
- Growth: interactions first (igniting sconces/braziers, a transient point
  light on cast), then the fire-blast damage form scaling with fire power.

### Water — Splash (`splash` in spells.cat)

Summons a splash of water. With an EMPTY VIAL in the caster's OTHER hand, the
cast FILLS IT with water — the feedstock for later potions (the Conjure idea
from the magic-system doc). Grows into a huge deluge that can sweep monsters
away, put out fires, and so on.

- Today: a plain damage bolt.
- Growth: the vial-filling interaction (needs vial items + the containers
  system), dousing fires, then the deluge — a sweeping push + extinguish that
  scales with water power.

## Deeper tiers

Tier-2 (non-school) runes are TBD. When they exist, multi-rune spells return —
one school rune leading, tier-2 runes shaping (the retired `firebolt` becomes
fire + an air-flavoured tier-2 rune rather than two schools).
