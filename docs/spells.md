# Spells

The living list of spells. Details accrete here as they are designed; the
implementation column tracks what the code actually does today. Add new spells
as sections, keep the catalog (`assets/projects/dungeon-demo/catalog/spells.cat`)
and this list in step.

Rules of the system (see `docs/magic system.md` for the full model):

- A spell is a SEQUENCE of runes. The four element runes are SCHOOL runes —
  mutually exclusive, exactly one per spell, in first position: the first rune
  picks the school (and the spell's colour). Tier-2 runes are SHARED FORM
  runes usable under any school; each authored school+form recipe gives the
  combination its school-flavoured reading.
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

## Tier 2 — the shared form runes

Tier-2 runes are SHARED FORMS (settled 2026-07-06, see docs/magic system.md):
one form rune combines with every school, and the authored recipe gives each
combination its school-flavoured behaviour. The spell's colour always comes
from the school rune. Form tablets/UI use a neutral arcane gold.

### Project (Tiwaz, the up arrow) — "throw it ahead"

The directed/thrown form: the school's substance, projected hard down the
faced row. Four spells, `symbols = <school>,project`:

#### Fire — Fire Burst (`fireburst`)

A directed burst of flame — the flame puff turned weapon.

- Today: a strong fire bolt (power 14).
- Growth: with fire power it becomes a sustained FLAMETHROWER — a held jet
  rather than a single burst.

#### Earth — Slingshot (`slingshot`)

The pebble slung with real violence — the heaviest tier-2 hit.

- Today: the hardest, and a fast, bolt (power 18).
- Growth: projectile size/weight scales with earth power (gravel → stone →
  boulder), inheriting Pebble's growth line.

#### Water — Water Bolt (`waterbolt`)

A jet of water thrown as a projectile.

- Today: a fast middleweight bolt (power 12).
- Growth: douses fires it passes through/hits (sconces, braziers — the
  inverse of Fire Burst's ignition), scaling toward Splash's deluge.

#### Air — Push (`push`)

The air school's identity: its "bolt" MOVES the target rather than hurting
it. A gust projected down the row that shoves the first monster it strikes
backward — the first DISPLACEMENT effect in the engine.

- Today: BUILT as designed — catalog `push = 1` shoves a struck, surviving
  monster one cell along the bolt's travel (`ResolveSpellHit` →
  `StepMonsterTo`; walls, closed doors, occupied cells, and the party's cell
  stop the shove — `FreeSlotInCell` is the predicate). Damage is token
  (power 4). A pushed monster glides visually like a normal step.
- Growth: push distance (`push = 2, 3...`) and affected weight class scale
  with air power; a future tier-3 turns it into a sweeping line/cone.

### Protect (Algiz, the warding stave) — "guard the caster"

The defensive form: a WARD on the caster, the school picking HOW it guards —
earth HARDENS, air DEFLECTS, water ABSORBS, fire RETALIATES. Four different
answers to "protect me", no overlap. All four are BUILT. Framework: ONE
active ward per member — casting another (any school) replaces it; wards are
caster-only day one ("grows to cover the party" is a high-power growth form;
Dungeon Master's fire shield was party-wide, so there's precedent for that
endpoint). The ward lasts `duration` seconds (spells.cat), `power` is its
school magnitude — earth/fire read it as a flat number for their whole
lifetime, water/air read it as a BUDGET (pool/charges) they spend, ending
early when it runs dry (burst/stilled). Every ward ticks/fades in
DungeonWorld with a log line; active wards ride the save (v13 "shield"
lines, budget spend included). No HUD indicator yet — the cast/fade log
lines are the only feedback (a portrait tint or timer pip is a natural
polish item).

#### Earth — Stone Skin (`stoneskin`) — BUILT

The caster's skin turns to stone: a flat armor bonus (power 6) for the
duration. Rides `Character::Armor()`, so it reduces BOTH melee and ranged
hits through the normal strike resolver.

- Growth: armor scales with earth power. Tier-3 outward form: a stone wall
  filling a cell.

#### Fire — Fire Shield (`fireshield`) — BUILT

Fire guards by burning back — even its defense is aggression. A monster that
LANDS a melee blow on the warded member is scorched for the ward's power
(6). The incoming hit is NOT reduced (that's earth's job); ranged attackers
are out of its reach.

- Growth: retaliation damage with fire power; igniting flavour later.
  Tier-3 outward form: the fire WALL (the aura turned into a burning cell).

#### Water — Water Veil (`waterveil`) — BUILT

Water guards by absorbing: a flowing film soaks damage into a POOL (power
20) before any reaches health, and BURSTS when the pool is spent — it dies
by spending, not by the clock (though an unspent veil still fades at its
60 s duration). The intercept sits in WoundMember — the one place a member
takes damage — so it soaks every source alike: melee, ranged bolts, even a
wall bump. A partial soak lets the remainder through.

- Growth: pool size with water power; quenching fire damage entirely once
  monsters have elemental attacks. Tier-3: mist/deluge wall.

#### Air — Wind Ward (`windward`) — BUILT

Air guards by deflecting — the school that moves things moves ATTACKS.
A ranged bolt aimed at the warded member is turned aside OUTRIGHT (no
strike roll), spending one of the ward's CHARGES (power 3); the last
deflection stills the wind (spend-to-end like the veil, 60 s fade
otherwise). Bolts aimed at unwarded neighbours fly true — the ward wraps
its caster alone. Melee is out of its reach: the defensive mirror of Push.

- Growth: charge count with air power, then melee attacks straying too.
  Tier-3: a wind wall cell bolts can't cross (reusing push for whatever
  walks in).
