# Skills & statistics

Design notes for the skill system: what skills exist, how they shape spells
and attacks, and how using them feeds back into the statistics. This is the
living reference — like `docs/spells.md` it carries both the target design
and what the code does today. Settled with Michael 2026-07-06: melee skills
are PER WEAPON CLASS; skills grow quickly and each skill's associated stat
creeps up far more slowly behind it; skill-gated cast failure ships from day
one (tier-1/2 tuned to rarely fail at starting skill).

## The skill list

A skill is a named proficiency a member trains BY USING it. Two families:

| Skill id | Family | Trained by | Associated stat (creep) |
|----------|--------|------------|--------------------------|
| `fire` / `earth` / `air` / `water` | Magic (one per school) | successfully casting that school's spells | fire→strength, air→dexterity, earth→max stamina, water→max health |
| `blade` | Weapon class | landing a blow with a bladed weapon (catalog `skill = blade`) | dexterity |
| `blunt` | Weapon class | landing a blow with a bludgeon (no weapons authored yet) | strength |
| `unarmed` | Weapon class | landing a bare-handed blow (punch/kick) | strength |

- Magic skill ids are the school symbol ids — one namespace, no mapping.
- A weapon names its class in items.cat (`skill = blade`); a weapon without
  the field trains nothing (it still swings). New classes are data + one
  row here.
- The school↔stat pairs follow the magic-system table (Earth/Stamina,
  Air/Agility, Fire/Strength, Water/Health); stamina/health creep raises
  the MAX (and current with it).
- Intelligence (mana regen) and willpower (casting composure, below) have
  no feeder yet — an open slot, not an oversight.

## Experience and levels

Skills store raw XP (float); the LEVEL is derived:

    level = floor(sqrt(xp))     — 1 xp → L1, 4 → L2, 9 → L3, 25 → L5 ...

so early levels come fast (the fun part of learning) and the curve
stretches naturally with no table to author.

XP awards (the feedback loop; all award sites are main-thread world code):

- **Successful cast** → school XP = the spell's mana cost × 0.25 (a dearer
  spell teaches more). A FAILED cast teaches nothing (settled rule), and a
  fizzle/no-mana/unknown attempt teaches nothing.
- **Landed melee blow** → weapon-class XP 1.0. A miss teaches nothing.
- On a level-up the member announces it in the log (log.skill_up, identity
  tint). Skills and their XP are saved per member (v15 "skill" lines).

**Stat creep**: every XP award also drips into the skill's associated stat:
`statProgress[stat] += xp × 0.04` — when the pool passes 1.0 the stat gains
a point (log.stat_up) and the pool keeps the remainder. Roughly: 25 blade
hits = +1 dexterity; ~12 tier-2 casts = +1 of the school's stat. Saved
per member (v15 "statxp" lines).

## Skills → spells

- **Difficulty** = the recipe's rune count (tier), so tier-1 = 1, tier-2 = 2.
  (A future spells.cat `difficulty` field may override per spell.)
- **Cast failure** (rolled in MagicSystem::Cast, after the vocabulary/mana
  gates, before any effect):

      failChance = 0.35 × (difficulty − 1) − 0.10 × schoolLevel − 0.01 × willpower
      clamped to [0, 0.9]

  Tier-1 never fails. Tier-2 at skill 0 fails ~1-in-4 for a low-willpower
  fighter, ~1-in-5 for the casters — and a few successful casts push it to
  zero: the act of learning is visible. Future tier-3 (difficulty 3) demands
  real school skill. A failed cast SPENDS the mana (the energy slips away),
  logs log.cast_fumble, teaches nothing, and learns nothing.
- **Power**: effective power = catalog power × (1 + 0.10 × schoolLevel) —
  the per-school caster POWER the growth forms in docs/spells.md scale by.
  Applies to bolt damage and shield magnitude alike.
- **Bolt accuracy**: + 0.02 × schoolLevel on top of the intelligence base.

## Skills/stats → melee

PartyAttack resolves the swinging hand's weapon class (catalog `skill`,
bare hand = `unarmed`) and folds the class level into the profile:

- damage × (1 + 0.08 × level)
- accuracy + 0.02 × level (ResolveAttack's [0.05, 0.95] clamp still rules)
- Attack interval is unchanged by skill for now — weapon speed folds in
  with the combat-depth thread (per the AttackInterval stub note).

## Display

- The character sheet's **Skills tab** lists every skill the member has XP
  in — name, level, and a progress bar toward the next level (schools
  first, then weapon classes; the tab keeps its "No skills yet." line for
  a fresh member).
- A future skill roll-up on the HUD is NOT planned — the log lines are the
  in-combat feedback.

## Current implementation status

Built on branch `magic-system` (this doc's first commit): the whole model
above — Character::skillXp/statProgress, XP awards + stat creep +
level-up/stat-up log lines (DungeonWorld::GrantSkillXp), cast failure +
power/accuracy scaling (MagicSystem::Cast rolls with the world's RNG),
melee class resolution + profile scaling (PartyAttack), items.cat `skill =
blade` on the four daggers, sheet Skills tab, save v15.

Open / deferred:

- Weapon speed into AttackInterval (combat depth).
- spells.cat `difficulty` override; tier-3 spells to actually stress the
  failure curve.
- Feeders for intelligence and willpower.
- Encumbrance (MaxCarryLoad) still display-only.
