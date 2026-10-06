# Spells

The living list of spells. Details accrete here as they are designed; the
implementation notes track what the code actually does today. Add new spells
as sections, keep the catalog (`assets/projects/dungeon-demo/catalog/spells.cat`)
and this list in step. The spell-updates thread (docs/spell-updates-notes.md,
-plan.md) reworked the whole list into three tiers in October 2026.

Rules of the system (see `docs/magic system.md` for the full model):

- A spell is a SEQUENCE of runes read like a sentence: a SCHOOL rune (one of
  four, mandatory, first - it picks the school and the spell's colour), then
  optionally a FORM rune (Project, Protect, Sight, Light), then optionally ONE
  MODIFIER rune (Ingwaz, Hagalaz). Nothing else is well formed: no modifier on
  a bare school rune or on Sight, no second modifier, nothing out of order
  (`WellFormedRecipe`, Spells.h; the spellbook greys out a rune that may not
  come next, and a malformed recipe is refused, never cast).
- Every rune is shown by its ELDER FUTHARK NAME (`rune.<id>` lang keys); what
  it means is in its description. The ids in code and catalogs stay the
  meanings (`fire`, `project`, `multiple`).
- A spell's strength scales with the caster's POWER in its school - catalog
  power x (1 + 0.10 x school level) x (1 + the school's stat term). Every
  threshold in the list below reads that cast power, never the skill level.
- A character LEARNS a spell the first time they successfully CAST it (built in
  the spellbook). Higher-tier spells can FUMBLE (35% a rune past the first, less
  10% a school level: a three-rune spell at level 0 fails about 70% of the time,
  which is intended - the balance pass owns it). A fumble teaches nothing.

The runes:

| Rune | Futhark name | Tier | Meaning |
| --- | --- | --- | --- |
| fire | Kenaz | school | fire |
| earth | Berkano | school | earth |
| air | Ansuz | school | air |
| water | Laguz | school | water |
| project | Tiwaz | form | throw it ahead |
| protect | Algiz | form | guard the caster |
| sight | Dagaz | form | see through the wall ahead |
| light | Sowilo | form | make light |
| multiple | Ingwaz | modifier | more of it: more bolts, or the whole party |
| explode | Hagalaz | modifier | it bursts: on impact, or round the caster |

## Tier 1 - the four hand spells

The school rune alone, castable the moment it is memorized. Michael's rule
(2026-10-01): these are NOT bolts. Each is something in the caster's hand, and
it acts on what the caster holds or on the square ahead. Mana 2 each. They
share `Spell/HandSpell.h`: a spell cast from a hand looks first at the OTHER
hand; one cast from no hand (the console, the book) looks at both, right first.

### Fire - Puff of Flame (`flame`, Kenaz)

A puff of flame in the hand. In order:
1. an unlit torch held in the other hand lights (its `lit_as`);
2. else the wall torch ahead lights;
3. else the brazier ahead lights - but only at cast power >= `brazier_power`
   (14; school level 0 falls short, level 30 reaches it);
4. else "nothing catches".

### Earth - Pebble (`rock`, Berkano)

A small stone (`conjures = pebble`: 0.1 kg, throwable, `command = throw`) in
the casting hand if it is empty, else the other hand, else at the caster's
feet. It is a real item - throw it, stow it, drop it.

### Air - Puff of Wind (`gust`, Ansuz)

A breeze from the hand. It FLARES a fire ahead (a sconce or a brazier burns
bigger and brighter for a moment). At cast power >= `push_power` (8) it also:
- shoves the monster ahead back `push` (1) square, and
- REPELS a shot in the square ahead: against a projectile of strength S, a gust
  of power P < S weakens it to S - P and it flies on; P >= S flings it back the
  way it came carrying min(P - S, S) (Michael, 2026-10-01). Arrows, bolts,
  thrown items - anything in flight. WHAT IT CARRIES GOES WITH IT
  (code-review C18): the share of S it keeps is the share of its blast (damage,
  reach in squares, linger) and of its on-hit effects' strength it keeps, so a
  magus's burst bolt slowed to half bursts at half and its burn lands at half
  (`ProjectilePayload::Scale`, the rule `throw_scale` uses). A shot left with
  nothing (P = S exactly) falls where it is, and nothing it carried goes off.
  (Thrown items are in fact left alone - nothing throws one at the party.)
Otherwise not much use, as designed.

### Water - Splash (`splash`, Laguz)

A handful of water. In order:
1. a water container held in the other hand fills ONE STEP (its `fill_as`:
   empty -> half -> full waterskin; a full one is skipped);
2. else the wall torch ahead goes out;
3. else the brazier ahead goes out - only at cast power >= `brazier_power` (12);
4. else it splashes harmlessly.
A fire put out SMOKES: the fixture kind's `on_douse` effects (effects.cat
`smoke`, a haze effect) land on the fire and raise the square's turbidity,
thinning away over a few seconds. See docs/torches-and-fire.md.

## Tier 2 - the shared form runes

Tier-2 runes are SHARED FORMS (settled 2026-07-06): one form rune combines with
every school, and the authored recipe gives each combination its
school-flavoured behaviour. The spell's colour always comes from the school
rune. Form tablets and UI use a neutral arcane gold.

### Project (Tiwaz) - the bolts

`symbols = <school>,project`. Each is a SINGLE-TARGET bolt: it flies the
caster's quadrant lane down the faced row and strikes the first body in its
lane. It does NOT explode - a blast is Hagalaz's job.

| Spell | Id | Power | Notes |
| --- | --- | --- | --- |
| Fire Bolt | `firebolt` | 14 | `on_hit = burn 2 4` - it catches |
| Earth Bolt | `earthbolt` | 18 | the "magic missile": the hardest, a fast bolt |
| Water Bolt | `waterbolt` | 12 | fast middleweight |
| Air Bolt | `airbolt` | 4 | token damage; `push = 1` shoves what it strikes back a square |

The ids were `fireburst` / `slingshot` / `push` until October 2026; a save that
knew the old ids drops them from its known list (the usual dev-cycle cost).

### Protect (Algiz) - the wards

The defensive form: a WARD on the caster, the school picking HOW it guards -
earth HARDENS, air DEFLECTS, water ABSORBS, fire RETALIATES. Each is an effect
class overriding the pipeline stage it acts at (docs/effects.md): Stone Skin at
mitigate, Wind Ward at deflect, Water Veil at absorb, Fire Shield at react.
Wards STACK across schools; recasting the SAME school replaces its ward. The
ward lasts `duration` seconds; earth and fire read `power` as a flat number,
water and air as a BUDGET (pool / charges) that ends the ward early when spent.

| Spell | Id | Power | What it does |
| --- | --- | --- | --- |
| Stone Skin | `stoneskin` | 6 | flat physical mitigation for 30 s |
| Fire Shield | `fireshield` | 6 | a monster that lands a melee blow is burned back (a Burst, itself resisted) |
| Water Veil | `waterveil` | 20 | soaks damage into a pool before health; bursts when spent |
| Wind Ward | `windward` | 3 | turns ranged shots aside outright, a charge each |

### Sight (Dagaz) - the peepholes

A round PEEPHOLE bored through the wall block directly ahead, in the first-person
view (the scene shader carves it from the `sightCell`/`sightHole` frame
constants - no mesh rebuild). It follows the party's facing live for its
duration, stacks across schools (Fire's flavour wins the one camera), and rides
the save on the effects line. Sight takes NO modifier.

| Spell | Id | What it shows |
| --- | --- | --- |
| Ember Sight | `embersight` | lights the room beyond with a warm fill light |
| Far Sight | `farsight` | bores deep down the row, through several walls |
| Stone Sight | `stonesight` | writes the revealed room into the map's fog of war; lasts longest |
| Scrying | `scrying` | a wider, clearer window (its "reveal the hidden" identity waits for secret content) |

### Light (Sowilo) - the lights (lighting-updates Phase 6)

A light round the party for a while: a `light` effect on the CASTER (one kind,
told apart by school, so lights of different schools stack and a recast
replaces its own), lit from lights.cat `spell_<school>` above the party a little
ahead, bigger with the cast power (sqrt(power / `scale_power`), 0.7..1.8) and
dimming over its last tenth like a torch. It lasts spells.cat `duration` at the
spell's power, in proportion past it. Each school's light DOES something beyond
its colour (Michael's picks); the knobs are effects.cat [light]:

| Spell | Id | Lasts | What it does |
| --- | --- | --- | --- |
| Firelight | `firelight` | 60 s | casts shadows like a torch; KINDLES an unlit fire within a step (a brazier only from `kindle_brazier_power`); SCORCHES each monster beside the party every `scorch_every` s (`scorch_damage` x power / 8 of fire) |
| Tidelight | `tidelight` | 60 s | cuts a CLEAR bubble in the haze (`clear_haze`); SOOTHES - stamina regenerates `soothe` x faster; QUENCHES any fire on the party, and none catches while it lasts |
| Skylight | `skylight` | 60 s | REACHES furthest (wide and dim); CRACKLES - every `crackle_every` s a shock (`crackle_damage` x power / 8 of air) at the nearest monster in its reach and the party's sight; WARNS - its flicker runs `warn_rate` x faster while a monster near has noticed the party |
| Stonelight | `stonelight` | 150 s | is SET DOWN, not carried: a glowing stone in the square it was cast in (drawn as `stone_item`, the rock), part of that level's saved state, up to 8 a level; MAPS every square its light reaches in walking steps; SHOWS the TRACKS monsters have left within its reach as amber footprints drifting the way they went (fading over balance.cat `track_life`) |

Fire alongside it: every SKELETON resists fire (`fire 0.75` in monsters.cat),
and a monster marked `flammable` (the MUMMY) catches from ANY fire that lands on
it - a bolt, a torch's blow, a scorch - burning balance.cat `ignite_burn` a
second for `ignite_seconds` (Michael: "mummies are a human torch waiting to
happen"). Tracks are written by every monster step (`StepMonsterTo`) and saved
per level as ages; the grid records who made each one, so the PARTY can leave
tracks, scent and noise for monsters to follow later.

## Tier 3 - the modifiers

A modifier follows a FORM rune: `<school>,project,<modifier>`,
`<school>,protect,<modifier>` or `<school>,light,<modifier>`. ONE class makes
them all (`Spell/ModifiedSpell.h`): AllSpells.cpp wraps every Project, Protect
and Light spell with each modifier, giving twenty-four whole spells with their
own ids, names and spells.cat entries - so learning, the spellbook, the hand
menus and saves treat them like any other spell. Each costs about twice its
base (spells.cat `mana`). Its DEFAULTS are its form's AS TUNED - power, twice
the mana, the form's `on_hit` and `push` - taken after the forms' own entries
are read (SpellBook::Build, `Spell::DeriveFromForm`), and its own entry wins on
each: a volley bolt leaves the volley's `on_hit` (`[firebolt_volley]` burn 1 4)
and shoves by its own `push`. Its bolts used to carry the form's on-hit whatever
its entry said, and its defaults were the form's CLASS numbers (code-review C19).

### Ingwaz (multiple) on a bolt - the VOLLEY (`<bolt>_volley`)

`count` bolts (2, or 3 for water and air), plus one per `count_per_power` past
the spell's power, at most `count_max`. They fly down the CASTER'S OWN LANE a
beat apart (`gap`), each a little off the line (`jitter`, kept inside the lane
so the lane hit test holds), and each at `share` (0.6) of the cast power -
weaker than one bolt, together much stronger (Michael: "each weaker"). Later
bolts wait in a fixed pending-bolt queue in the world (cast service
`spawnBoltAfter`), since a cast happens in a frame that must not allocate.

### Hagalaz (explode) on a bolt - the BURST (`<bolt>_burst`)

The bolt detonates where it strikes or where it stops: the shared blast system
(Game/Blast.h - a wavefront that flows through open squares, round corners,
spent by walls). Damage scales as cast power over the spell's power; reach
grows by one square per `blast_force_per_power` past it. NOTE `blast_force`
counts SQUARES the wave may fill, not a radius. FIREBALL (`firebolt_burst`;
Michael named it, 2026-10-02 - the others are "Bursting <bolt>") carries the old
Fire Burst's tuned blast whole (force 7, damage 5), and the fire flask's
`throw_spell` bursts as it.

### Ingwaz on a ward - the PARTY WARD (`<ward>_party`)

The ward lands on every STANDING member at `share` (0.75; Wind Ward 1.0) of its
power each.

### Hagalaz on a ward - the WARD BURST (`<ward>_burst`)

The ward's power spent as a blast of the school's element round the caster -
whose own square is SPARED (`Detonate`'s `spareCentre`: the centre is treated
as impassable, so the wave starts beside it) - and NO WARD is left (Michael:
"burst instead"). Force 6 to 8: the four squares round the caster are the
first ring, and force 3 once left one of them untouched.

### Ingwaz on a light - ONE BIGGER LIGHT (`<light>_bright`)

The same light cast at `grow` (2) x the power - brighter and further - for the
time the plain power would buy (Michael: "one bigger light"). A Stonelight's
stone is the bigger one.

### Hagalaz on a light - the FLARE (`<light>_flare`)

No lasting light: a 0.7 s flash round the party (`spell_flare`, a hand-glow
slot) and a cloud of motes, and every monster within 3 walking steps DAZZLED -
the `dazzle` effect, under which it does nothing - for 1.5 + power / 4 seconds
(2..8). A step is into an OPEN square (`DungeonWorld::WalkReach`, a blast's own
test), so neither rock nor a shut door lets it by. Then the school's light acts
ONCE over that reach: fire scorches every monster THIS flare dazzled (never one
still dazzled by an earlier flare elsewhere) and kindles every fire it reached;
water quenches the party and gives each member `power` stamina; air shocks
every monster it dazzled; earth maps every square its light would reach and
shows the tracks in them. (The flare once dazzled by the light budget's reach
map, which a shut door passes, and kindled by Manhattan distance, through rock
- code-review C17.)

## Monster casters

`monsters.cat` `spell` names any spell id, a modified one included. The mage
LADDER is authored as three kinds rather than a per-instance level (monsters have
none): `skel_mage` casts `firebolt`, `skel_mage_adept` casts `firebolt_volley`,
`skel_magus` casts `firebolt_burst` (Michael: "higher level mages shoot with
Ingwaz, and even higher ones do Hagalaz"). A monster volley rides the same
pending-bolt queue (`Spell::MonsterVolley`). A monster's BURST bolt that reaches
a member's lane goes off in the party's square, on every member there, as a
party burst does on a monster - it used to strike one member as a plain bolt and
be retired with its blast unspent (code-review C1). A Wind Ward on the member it
would strike turns it first, and a turned bolt does not land, so nothing goes off
(Michael). `threat` prices the magus by that blast: on every member, unrolled.

## Where the runes are found

The starter kit (`CreateDefaultParty`): Maren holds Kenaz and Tiwaz, Tilo
Berkano and Algiz, and EACH caster carries an Ingwaz and a Hagalaz tablet in the
backpack (Michael, 2026-10-02 - this replaced Q10's "placed in a level, deeper
and guarded"). Each gets both because a tablet is memorized by one member and
spent. A rune in the pack is memorized from its use menu on the sheet. The other
schools and forms are found as tablets in the levels. FOR NOW (Michael,
2026-10-05) each caster also starts with a Sowilo tablet (`rune_light`) in the
backpack, since no level places one yet; move it into a level once there is
somewhere for the player to find it.

## Checked by

`tools\SpellTest.py` (runs `tools\EvalScripts\spells.eval` and judges 33 checks,
one per outcome above; `--selftest` cuts every cast and demands exactly the
spell-free checks still pass) and `tools\AllocTest.ps1 -Hand` (the hand spells
inside a guarded window), `-Cast` (a bolt in flight + an open book),
`-Impact` (bolts landing, a burst) and `-Light` (each light cast in the window,
a flare dazzling a mummy, a stone showing planted tracks).
