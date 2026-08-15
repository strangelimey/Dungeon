# The damage system

**Status:** BUILT on the `damage-system` branch (Michael, 2026-08-11;
over-exertion and the crit/fumble consequences 2026-08-13). Everything below is
live.

Rolemaster-shaped resolution, replacing the probability check the game
shipped with. Attacker and defender each add a **d100** to a bonus and the
higher total wins; the margin between them multiplies the blow. Everything
that reaches a combatant still goes through the one effects pipeline
(`docs/effects.md`) — this is about what happens *inside* its strike stage,
and about the bonuses both sides bring to it.

## Why

The old model was one-sided: `clamp(accuracy − evasion)` as a probability,
then flat damage. It had two problems that only showed once the game had
skills and armor to express.

A probability difference is **linear**, so nothing could be dramatic — no
critical, no fumble, no "that blow should have killed you". And a 0..1
accuracy has nowhere to put a career: a fighter who has swung a sword ten
thousand times can only ever creep from 0.6 to 0.9.

## The roll (`Game/Roll.h`)

```
attack  = attack bonus  + d100
defense = defense bonus + d100
hit if attack > defense
```

**Open-ended.** A face of **95 or more** rolls again and adds, and a further
95 re-triggers, without limit. A first face of **5 or less** is a **fumble**.
Both sides roll, so a defender can crit a dodge or fumble a block.

Both are inclusive, settled (Michael, 2026-08-11), and the difference
mattered: `>= 95` fires on 6% of rolls where `== 95` would fire on 1%, and
the escalation compounds that. **Fumbling does not escalate downward** — it
is a flag on the first face, not a mirror of the crit.

Two rules that are decisions rather than arithmetic, stated at the header
because neither is inferable from the code:

- **Only the first roll can fumble.** Checking every roll would make a
  fumble *more* likely the better you rolled.
- **A tie goes to the defender.** An attack must beat the defense, not
  match it. It matters about 1% of the time.

`maxEscalations` is a **termination guard, not a balance knob**: the
escalation is meant to be unbounded and at the default threshold
effectively is, but the threshold is *data*, and `crit_threshold = 1`
would otherwise spin forever.

### The margin multiplies

Beating a defense by a hair lands a normal blow; overwhelming it lands a
devastating one. Capped (`margin_cap`), because the roll is open-ended and
the margin scales the damage, so the two compound — measured, an extreme
margin is ~9x a typical winning one.

## Where the bonuses come from

Nothing in the formula is a probability any more; every term is in **d100
points**, and the unit that makes them legible is that **two opposed d100s
deviate by about 41 points**. A difference much under that is noise.

### Attack

```
skill curve + stat curve + the verb's own points
```

Skill is the main driver. In Rolemaster each point spent gave +5; this game
spends no points — skill rises continuously by use — so the taper is a
**curve** over a continuous value (`Game/Curve.h`), not a per-rank table.

Three forms share two knobs (initial `slope`, a `cap`) and all three are
built so `slope` means the same thing in each, which is what makes them
comparable on one graph without re-tuning:

| form | behaviour |
|---|---|
| hyperbolic | approaches the cap, never exceeds it |
| exponential | same start, harder shoulder, flattens almost dead |
| logarithmic | passes the cap and keeps creeping — the unbounded one |

The Balance dialog **draws both curves live**, against the 41-point dice
deviation as a dashed rule. That line is the point: a curve sitting under it
across its useful range is decoration, however impressive its numbers look
in a text field.

Stats taper too, with a **baseline of 10** — an average stat is worth
nothing and a poor one is a real penalty — and are bounded far below skill,
because unbounded skill against an unbounded stat makes one of them
decoration.

### Defense

```
base + DEX + stance + (unarmored ? avoid : 0) − armor penalty
```

Defense is **typed**: `fx::ITarget::Evasion` takes the incoming damage type,
because a guard is no longer one number.

## The stance

A character spends `offenseShare` of their skill pressing the attack and
keeps the rest to guard with. **One stance per character**, not per hand —
the two hands still guard *differently* and the better of them answers each
blow, but the fighter makes one decision about how hard to press. A slider
under the hand icons; a hand guards **while on cooldown**, because a stance
is not an action.

What a held-back hand answers depends on what it holds:

- **Physical** — parried with a hand, off its weapon class. An empty hand
  parries `unarmed`: bare-handed, but not nothing. The two hands combine by
  **MAX, not sum** — summing would make holding both back strictly better
  and turn the slider into a free defense button.
- **Magical** — warded with the skill in the **incoming school**. Knowing
  fire is what turns fire aside, so a fire specialist shrugs off a firebolt
  and is no better than anyone else against frost. *The hands play no part.*

Monsters take a stance too (`monsters.cat offense`), authored **per kind**
rather than per archetype: an archetype says how a creature moves and
perceives, not how boldly it does so, and a wild caster and a cautious one
share one.

### One number, both sides

The share scales the **skill term of the attack bonus** and the held-back
remainder guards with what is left, so `attack + guard` is constant across
every share — the points the stance takes off one side are exactly the ones
it puts on the other (`defense::StanceAttack`, checked as an identity across
seven shares rather than at a point). A stat is *not* skill: DEX on a swing,
and the school's stat on a cast, ride at full weight whatever the stance.

Monsters were coupled this way from the start (`accuracy × offense`), and
**characters were not** — until 2026-08-13 a member's slider only ever
subtracted, so pressing the attack cost you your guard and bought nothing.
The slider was a defense dial wearing a trade's clothes. Both sides now trade
on the same terms.

## Over-exertion

The share is **0..exert_max, not 0..1**. Past 1 a fighter is spending skill
they have not got, and the arithmetic simply continues: the attack term keeps
climbing and the held-back share goes **negative**, so the guard becomes a
penalty rather than merely nothing. Over-committing does not just fail to
defend you — it leaves you wide open, which is what makes it a decision
instead of a damage button.

The bill is charged **per swing, per cast**, on the points the over-exertion
bought (`defense::ExertionPoints` — the attack above what an honest full
commitment would have given):

```
stamina, then health  ←  exert_cost × (StanceAttack(share) − StanceAttack(1))
```

`exert_cost` (3) and `exert_max` (2) are balance.cat knobs like everything
else; 3 is a first cut and expected to move. Four properties are worth
knowing, each of which is a decision rather than an implementation detail:

- **Every exit pays.** A whiff at air, a miss, a landed blow and a *fumbled
  cast* are all billed — the effort was spent whether or not it connected.
  Only the outcomes where nothing was thrown at all (no recipe, no mana) are
  free.
- **The bill is charged last**, after the blow resolves, because it can put
  its own owner down and that line has to read *after* the blow it paid for.
- **It can down you; it cannot kill you.** The health payment is capped at the
  health you have, which keeps it clear of the overkill rule (`docs/combat.md`
  Phase 5). At `exert_max` the raw bill genuinely can exceed a low-level
  member's whole health several times over, so without the cap a single
  reckless swing would have been instant death by a rule meant for definitive
  blows. You can collapse from over-exertion; you cannot burst.
- **An untrained fighter borrows nothing.** The points scale with the skill
  curve, so a reckless stance on a skill you do not have is free *and*
  useless — there is nothing to over-spend. Recklessness is a veteran's option.

The whole bill trains VIT (it routes through `SpendStamina`, the one exertion
path, so the armor-shortfall scale and the exhaustion latch apply to it too) —
including the part paid in blood, because conditioning is what the body did,
not what the bar could afford.

**Reading the slider:** the track now runs to 2.0 with a hairline at the
full-commit mark. Left of the mark the fill is the ordinary accent; right of
it, its own alarm colour — a different *kind* of spending deserves a different
colour, not more of the same bar.

## When it goes wrong (`Game/Mishap.h`)

A fumble has always *decided* the exchange — the swing cannot land, whatever the
bonus behind it — and then done nothing else. A critical has always fired its
source's `on_crit` procs. This is what the two extremes cost beyond that.

### The crit side is one word

```
crit = pierce
```

A critical with the right edge goes **under** the armour, so soak is not
subtracted. Resists still answer it: soak is a thing you *wear* and a gap can be
found in it, while a resist is what the target *is* and no edge finds a gap in
that.

A separate crit damage multiplier was specced and **cut**. The margin already
multiplies, and a critical already widens the margin because the re-roll adds to
it — two multipliers riding one lucky roll is exactly the compounding tail
`margin_cap` exists to stop.

### The fumble side is a table

Two authored lines, plus a proc list for anything that genuinely *is* an effect:

```
on_fumble     = bleed 2 5 0.35   ; effects, landed on the WIELDER
fumble        = recover 2.6      ; what a fumble costs its thrower
fumble_severe = drop             ; ...and what a BAD one costs
```

`on_fumble` and `fumble` coexist rather than one subsuming the other, because a
proc names an *effect* and lands it on a target, while the interesting fumbles
are one-shot events against the attacker and the exchange that no status can
express — "the weapon leaves your hand" is not a condition anyone is *in*.

The vocabulary, all acting on the person who threw the swing:

| token | does |
|---|---|
| `recover <mul>` | the hand takes longer to come back — the tempo consequence |
| `stumble <pts>` | a burst of stamina, billed as **exertion** (so it feeds VIT and can reach health) |
| `drop` | the weapon hits the floor at your feet |
| `fling` | ...or clatters into a random adjacent walkable square |
| `self_hit <frac>` | the blow you threw lands on **you** at a fraction of its force |
| `wild` | ...or at full force on whoever stands beside you |

A consequence with nothing to act on is a **no-op, never an error** — `drop`
with an empty hand, `stumble` on a monster with no stamina bar. That is what
lets one default table serve a knight, a bare fist and a claw.

### Severity comes from the dice you already rolled

**No second random draw.** The first face is already `1..fumble_threshold`, and
a 01 is a worse slip than a 05. `mishap::Severe` reads that face against
`fumble_severe_face`, so the mild table fires on every fumble and the severe one
only at the bottom of the band:

| | share of fumbles | share of swings |
|---|---|---|
| a fumble at all | — | 5% |
| severe (face 1) | 1 in 5 | **1%** — about one every two or three fights |

A property that falls out for free: widening `fumble_threshold` produces
proportionally *more mild* fumbles, which is the right direction — a clumsier
fighter flails more often without flailing more catastrophically.

The face travels only when it means something (`AttackResult::fumbleFace` is 0
when nothing fumbled), so `Severe` cannot be asked the same question two ways.
That zero is also why it is a function and not an inline comparison: without the
`face > 0` guard every unrolled event in the game reads as a severe fumble.

### Defaults, and what an authored table means

An authored line **replaces** its default outright rather than adding to it — a
table you cannot turn off is not a table. But the two lines default
*independently*, so a weapon that authors only `fumble` still gets the default
severe one; most weapons want to say how they slip, not to redesign the disaster.

The defaults live in **C++** (`mishap::DefaultFumble` / `DefaultSevere`) rather
than in balance.cat, because a table is a vocabulary and not a number — the
spells.cat rule. Their *numbers* are knobs and arrive as arguments, and the table
is resolved at the moment of the fumble, so a Balance-dialog change to
`fumble_recover` lands on the next swing rather than the next level load.

The default is tempo and nothing else (`recover`), with `drop` at the bottom of
the band. At 5% of every swing, what happens on *most* fumbles has to be
survivable enough to shrug at.

### Both sides fumble

Monsters carry the same four fields. `PartyFumble` and `MonsterFumble` are two
functions rather than one taking an abstraction, deliberately: the six
consequences act on inventories, stamina bars and neighbours, and the two sides
share none of those. It is the same split as `PartyTarget`/`MonsterTarget`, and
it keeps the part that *is* shared — which entries fire — in the pure, tested
`mishap::` layer. Three of the six are silent no-ops for a monster, which is
exactly what lets every clawed creature in the game share the default table.

A monster's `wild` catches the nearest adjacent monster, which is the reason
that token was worth keeping on both sides: a swarm hurting itself in a corridor
is the fumble a player most enjoys watching.

**Deliberately NOT built:** consequences for a *defender's* fumble. The
`defenderFumbled` flag exists and still just means the blow lands automatically.

### Authoring, and why two weapons should not fumble alike

```
[serrated_blade]                    [club]
on_crit       = bleed 4 12          fumble        = recover 3.0, stumble 4
crit          = pierce              fumble_severe = wild
on_fumble     = bleed 2 5 0.35
fumble        = recover 2.6
fumble_severe = drop
```

The saw edge finds a gap on the way out and catches on the way back, opening the
hand that holds it. Three kilos of swung wood has no edge to find a gap with, so
no `pierce`; a mis-swing carries its own weight *past* you rather than snagging —
a long recovery and a wrenched shoulder — and at the bottom of the band it goes
right on round into whoever is beside you. A club is not dropped, it is followed.

## Armor

Heavier armor blunts more and is harder to avoid in, and the penalty has a
**floor that training can never reach past**.

| | penalty | floor | offsettable | soak | STR | learn | skill → stat |
|---|---|---|---|---|---|---|---|
| light | 10 | 3 | 7 | small | 8 | 1.00 | `light_armor` → DEX |
| medium | 25 | 10 | 15 | middling | 11 | 0.70 | `medium_armor` → DEX |
| heavy | 45 | 20 | 25 | most | 14 | 0.45 | `heavy_armor` → STR |
| none | — | — | — | — | — | — | `avoid` → DEX |

**The floor enforces itself.** The skill offset is `CurveValue` with its cap
set to `penalty − floor`, and a hyperbolic curve approaches its cap without
reaching it — so "no matter how much you practise, plate still makes you
easier to hit" is a property of maths already built and tested, not a clamp
bolted on beside it.

**Avoid is unarmored-only**, which makes going bare a build rather than the
poor man's option — and it solves a problem the design had: light and medium
armor creep DEX, and so does avoid. Since you cannot practise both, exactly
one loop runs at a time.

The two feedback loops train on **opposite outcomes**, so no blow trains
both: a miss while unarmored teaches `avoid`; a landed blow that armor
blunted teaches that class. You learn what you actually survive. Only
*rolled* events count — a bump, a fall and a poison tick were never evaded
and never turned.

A **strength shortfall is paid twice**: points off the defense roll, and a
steeper stamina bill on every swing and step. Easier to hit *and* quickly
spent.

## What the numbers do (measured, `tools/RollTest`)

Hit rate by skill, DEX 10, against a defense of 30:

```
L0 0.258   L1 0.285   L5 0.406   L10 0.539   L20 0.693   L40 0.826   L80 0.900
```

A career from barely-competent to dominant — which the opposed roll made
*possible* and the +5-a-point curve made *happen*. The two halves were
arrived at independently and agree.

Damage taken per swing from a monster attacking at 70, misses averaged in:

```
                          dmg 6   dmg 12   dmg 20   dmg 30
fresh, unarmored           6.19    12.38    20.63    30.94
trained dodger             3.64     7.28    12.14    18.21
veteran dodger             2.56     5.12     8.53    12.80
brigandine, untrained      3.98    10.17    18.43    28.76
plate, untrained           1.76     6.56    13.09    21.26
plate, skill 30            1.34     5.43    11.04    18.05
```

**Armor wins against many small hits; evasion wins against few big ones.**
That crossover is why monster damage was doubled to 8–16 — at the old 4–8
the choice never arose, because armor simply won everywhere.

### Two decisions that look like bugs

**An unarmored, low-level character is very weak, by design** (Michael,
2026-08-11). Roughly three swings from a mid-tier monster. That is the
intended shape of the difficulty curve, not lethality to be tuned away, and
party health was deliberately left alone.

**Armor always costs you the roll.** Even fully trained, every class is
worse at being missed than wearing nothing. That is the trade, not an
imbalance: armor buys mitigation with evasion.

## The carrier and its payload

A projectile is a **carrier**: as Michael put it (2026-08-11), "when it hits
something *or expires*, it causes an effect on the target". Impact worked;
**expiry was dead** — a shot out of range fizzled into sparks and told nobody,
and the hook it fizzled through passed only a position, for a sound.

What a carrier delivers is a `ProjectilePayload`: a list of `fx::Proc`, the
same `"<id> <magnitude> <seconds> [chance]"` form a weapon already authors as
`on_hit`. So a firebolt names an effect exactly the way a serrated blade does,
and neither the projectile engine nor the effects module learns a new
vocabulary. Everything it lands still goes through the one pipeline
(`docs/effects.md`) — a payload is a *source*, not a second path.

**The payload rides the CARRIER, not the spell.** Optional `hit`/`expire`
overrides on `Spell` were the obvious shape and were rejected: `Projectiles.h`
promises to be the one home for "a cast spell bolt today, a monster's ranged
attack next, thrown items / traps later", and a hook on `Spell` serves only the
first of those. A monster's plain shot carries its kind's `on_hit` — a
venomous thing's dart is venomous too — and a trap dart will need no new
mechanism. It also keeps the payload *data*, which is what lets the editor's
ProjectileInspector show it.

### A hit is lane-wide; an expiry is cell-wide

This is the mechanic, not an inconsistency. A bolt reaches only what stands in
its lane (`kLaneHalfWidth`, the quadrant rule); a bolt that **bursts** fills
the square it burst in. So the shot that flew past you down the far side of the
corridor and broke on the wall behind you *still catches you*. Nothing in the
expiry resolver reads the lane width, deliberately.

Michael chose "the cell it died in" as an expiry's target over an area burst,
which would have needed `fx::ITarget` iterated over a *set* — something nothing
does today, since it is implemented exactly twice, for one party member and one
monster. An area effect is now a short step from here rather than a rewrite.

A burst stays on its **target side**: a carrier may only ever affect the side it
was flying against, which is what `TargetSide` means everywhere else in the
engine, so it cannot friendly-fire. Whether it *should* is a live design
question and is deliberately unanswered.

`ExpiryCause` distinguishes bursting on stone from running out of reach. It is
carried and not yet acted on — both burst identically — because the reason a
flight ended is exactly the kind of thing a future spell will want and the
hardest thing to add afterwards.

### Two details that are decisions

**The school lends the flavour.** One authored `on_hit = burn` reads as fire
from a firebolt and as frost from a waterbolt, the same rule an enchanted
weapon's `element` follows. A monster's plain shot lends nothing, so each
effect keeps its own colours — matching what its melee already does.

**The proc list is an inline fixed array, copied.** Not a vector, because a
spawn happens mid-fight and an `Item` lives in a per-frame-simulated vector, so
allocating per shot would violate the steady-state rule the alloc guard
asserts on. Not a borrowed span into the spell that fired it, because a bolt in
flight has to survive an editor catalog rebuild reseating the registry
underneath it. Effect ids fit `std::string`'s small buffer, so a realistic
payload really does allocate nothing.

### What is verified (in-game, 2026-08-11)

Both moments, using `monsters` — which now prints hp and carried effects, since
the old name-and-cell line could not answer the question the console is usually
open to answer:

- **hit** — `hp 16` → bolt lands → `hp 2` → `[burn ...]` ticks → *"skeleton
  raider burns away to nothing!"*
- **expiry** — a medium body out of the caster's lane, flame's range cut so the
  bolt dies inside its cell: `hp 6` → `hp 3  [burn 2.0 2.6s]`. **Zero bolt
  damage**; the lost hp is the burn's own ticks. A lane hit does ~14 and would
  have killed a 6-hp body outright, so the burst is what caught it.

## The defender's half is checked too (`Game/Defense.h`)

The dice and curves had 37 checks; the *defender's* three rules had none, and
each is a decision rather than a sum — the penalty floor, the two-hand MAX, the
training-loop split. They were unreachable because testing them meant linking
the map, the monsters and the catalogs.

So the arithmetic moved into **`Game/Defense.h`** — a deliberately pure TU
(`Combat.h` for `ArmorClass`, `Curve.h` for the curves, nothing else) — and
`DungeonWorld` kept only the *adapters* that gather its inputs: an inventory to a
worn class, a weapon id to a skill level. `RollTest` compiles it straight in, so
what is measured is the shipping rule. **If that ever stops linking, something
world-shaped has been added to it.** 90 checks now; `--self-test` also swaps the
offset curve for the logarithmic form, so the armor section cannot pass vacuously
while only the dice prove themselves.

Lookups stayed in the world on purpose (`WornArmorClass`, `Soak`, `Resist`):
they have nothing to get wrong that a test would catch.

### It immediately found a real bug

A profile with **`floor == penalty`** — a designer saying "this cost cannot be
trained away at all" — made armor an enormous *evasion bonus*.

The floor rule works by setting the offset curve's **cap** to `penalty − floor`.
But `CurveValue` documents `cap <= 0` as "no meaningful shape, fall back to the
straight line the slope describes" — correct for a stat term switched off, and
catastrophic here, because **a straight line has no ceiling**. At training 200
the penalty came out near **−200000**. Authorable from `balance.cat`, silent, and
fatal to the whole defense roll.

`defense::ArmorPenalty` now consults no curve when nothing is offsettable: the
penalty *is* the floor. The lesson generalises — two correct designs collided at
their shared edge (a cap used as a ceiling vs. a cap of zero meaning "unshaped"),
and only a deliberately awkward test profile sat on that edge.

### Typed defense and unarmored-only avoid, also checked

The last two rules followed into `Defense.h`, and `PartyTarget::Evasion` is now
*only* resolution — an inventory to a worn class, a damage type to its two flags,
a skill id to a level. **108 checks.**

`avoid` is unarmored-only: an armored defender's `avoidLevel` is never read, so
training it while armored cannot leak into the roll at any level. Checked at
100,000. And the armor penalty is *subtracted* where avoid is *added* — while an
unarmored defender ignores a penalty however large a number is handed in, because
the branch decides, not the value.

Typed defense is a three-way dispatch, each kind reading its own term and no
other, with **a school winning over physical**. That precedence is pinned rather
than left to the order two `if`s sat in: nothing stops a project marking a
school's type physical as well, and the branches guard with completely different
skills. Magical reads the incoming school and **the hands play no part** — the
rule most easily got wrong, and the one the old comment in `Evasion` described
*incorrectly*.

The damage-type book is *not* linked into the harness: `DamageTypes.cpp` includes
`Game/Catalog.h`, which would drag the catalog layer through the purity wall. The
world asks the book and passes the two flags in, so what stays measured is the
decision made from them.

**These checks cannot pass vacuously, by construction rather than by assertion:**
every "ignores X" is paired with a "reads Y" against the same defender, so a
`Guard` that ignored everything would fail the second half of each pair. Confirmed
by mutation — making the magical branch also add `HandGuard` fails "magical
ignores the hands entirely" and nothing else. (The `--self-test` curve injection
cannot reach these; they are dispatch decisions, not curve shapes.)

The refactor is behaviour-preserving: the same walk into the bone swarm produces a
byte-identical combat log — same misses, same 5/7/8/6 damage, same Avoidance tick,
same crit.

## The area blast (`Game/Blast.h`)

**A blast has a FORCE, measured in SQUARES, and stone consumes none of it**
(Michael, 2026-08-11). It is deliberately *not* a radius. The blast floods
outward from where it went off, 4-cardinally — the grid's own rule, which LoS,
movement and projectiles all hold to — spending one of its force per square it
fills, and keeps going until the force is used up.

So the same blast fills a room nine squares wide and runs **eight squares** down a
dead-end corridor: the force that would have gone into the walls goes down the
corridor instead. Distance is measured *through open squares*, so a blast that has
to come round a corner arrives weakened by the journey rather than by how close it
looks on the map.

Then the second half, for when even expanding cannot spend it: force with nowhere
left to go **concentrates** on what the blast did reach. Sealed into a single
cell, a nine-square blast puts all nine squares' worth into that square — which is
what a confined explosion does.

One force-9 blast, `full` 10, `falloff` 3, in four geometries (measured):

| geometry | cells | reach | concentration | centre |
|---|---|---|---|---|
| open room | 9 | 2 | 1.00 | 20.0 |
| dead-end corridor | 9 | **8** | 1.00 | 20.0 |
| two-square pocket | 2 | 1 | 4.50 | 90.0 |
| sealed cell | 1 | 0 | 9.00 | 180.0 |

Same force throughout — the geometry decides whether it travels or concentrates.

**A BLAST HAS NO SIDE AND NO LANE.** It catches everything in its squares, the
party included (Michael's call): friendly fire is the price of throwing one, and
positioning is how you avoid paying it. That is deliberately unlike every
single-target path, where `TargetSide` is the whole rule. It is also **not
rolled** — an explosion filling your square is not something you parry, so there
is no opposed roll and no evasion — and it arrives as a `Burst`, so it is resisted
but **not soaked**: plate turns a blade, not a blast.

Authored on a spell as `blast_force` (squares — the gate; 0 means "not an area
effect"), `blast_damage` and `blast_falloff`. `fireburst` is the first, and the
carrier detonates at **either** of its two moments: a bomb that connects explodes
where it touched, and one that breaks against a wall explodes there — a centre
inside stone is handled, since nothing stands in a wall and the room beyond is one
step out.

`kMaxCells` (64) is a **ceiling, not a knob**: the spread runs on a fixed array so
a detonation allocates nothing, and `Result::clamped` says when force exceeded it
rather than letting it pass silently.

`Spread` is pure — no map, no catalogs, no combatants; the caller says which cells
are open. That is what lets `RollTest` measure the geometry (**142 checks**),
including the corridor reach, the concentration arithmetic, that nothing leaks
diagonally or through walls, a burst centred inside stone, and the degenerate
cases content can produce (zero force, force past the ceiling, a blast entombed
with nowhere to go at all).

Verified in game as well as measured: a Fire Burst on a bone swarm one step from
the party read "The blast catches the bone swarm for 10 damage!" then "caught in
the blast for 7 damage" for all four members — full at the centre, full minus one
step of falloff at distance 1, and friendly fire doing what it says.

## The dungeon as a target

`fx::ITarget` was implemented twice, for the two combatant kinds. **`BreakableTarget`
is the third, and the first that is not a combatant** — so a door, a barrel or a
crate reaches the damage pipeline through the same interface a monster does, and
everything already built works on them for free: soak, typed resists, absorption
past 1.0, DoTs (a burning door burns *down*), and a blast.

**Damageability is opt-in and OFF by default** — `destructible` in the catalog,
Michael's requirement: *"otherwise switches and keys will be useless"*. If props
and doors were breakable unless told otherwise, a party would chop through every
locked door the moment it could swing at one. `maxHp` of 0 means "not a target at
all" and every ask short-circuits on it. Toughness is `hp`, `armor` and `resists`
— the same two mitigation fields armour wears, so a barrel can be weak to fire
(`fire -0.6`) and stone can shrug off a blade.

One adapter serves every kind; what differs is only what *breaking* means, and
that is a callback rather than a subclass, because the damage side of a barrel and
of a door are identical:

- **A door's way opens for good.** Not `open = true` alone — a broken door can
  never be shut again, which is the whole difference from opening one. The
  invariant is enforced in `ToggleDoor`, the one place every route in arrives:
  the party's click, a wired button, and the editor's inspector.
- **A prop is gone** for drawing, collision and the map — but its record *stays in
  the list*, flagged broken. Erasing it would dangle the reference its adapter
  still holds, and the save has to be able to name what broke, which an erased
  record cannot do. `Decoration::Gone()` / `::Blocks()` are how everything else
  skips it, so smashing a crate in a doorway clears the way.

Reached today by a **blast** and by the dev command `smash <x> <z> [amount]`. A
smash is a `Blow` — rolled and soaked like any swing, so a door's `armor` and
resists both answer it, and it can **fumble**: the first scripted smash of the
wooden door missed for exactly that reason, which is correct and not a bug.

The break line is said by the **caller**, after it has narrated the blow — the
same rule a monster's death line follows. A callback fired from inside the
pipeline runs before the caller has said anything, which read as *"the barrel is
smashed to pieces!"* then *"the blast catches the barrel for 313 damage!"*.

### Persistence (save v24)

Decorations are **static `.map` records**, so being broken is dynamic state and
rides the save — the same split `seen` makes. One `broken <x> <z> <type>` line per
level, mirroring the `niche` record.

**Keyed by cell + type, deliberately not by index.** An index into the prop list
is stable only until the editor inserts a record ahead of it, at which point every
saved index past it names the wrong prop. Doors go through the same record even
though they *do* ride `entities` — that only carries open/closed, and a broken
door is not merely an open one: it must not come back at full hp to be broken
again. A saved entry naming a prop the level no longer has is dropped, which is
the outcome the entry wanted anyway.

Verified after a true reload, with a control: the smashed barrel and the wrecked
door both came back broken (`"nothing breakable"`), while the crate that was
*not* smashed that session was still struck — so the answers are specific to what
actually broke, not a blanket failure.

### Fixtures, and why they needed a side-table

Sconces and braziers are `WallSconce` / `FloorBrazier` in `DungeonMap` — the
*static* layer — so unlike a decoration they have no instance struct of their own
to carry a `Breakable`. Their damage state lives in `m_fixtureBreaks`, keyed by
**cell + wall** because several sconces may share a cell (`wall = -1` for a
floor-standing brazier). That is the same static/dynamic split `m_seen` makes,
rather than hanging dynamic state on a static record.

**Breaking one puts its light out** — which is the point of being able to break
one. `lit` gates the point light, the flame particles and the smoke together, so
one flag does all three, and the turbidity map is rebuilt so a doused brazier does
not leave its own god rays hanging in the air. The mesh stays: a wrecked sconce is
still bolted to the wall, just dark.

Not all of them opt in, deliberately, the same way most doors don't: `sconce` (hp
6 — a torch in a bracket) and `brazier` (hp 30, `armor` 4 — cast iron on three
legs) are breakable; **`brazier_empty` authors none of the fields and cannot be
touched**. Both are `fire 1.0` — immune to fire, since they are already alight.

**The bug worth remembering.** A restored broken brazier came back *whole*. The
record was written and read correctly; the problem was that the table is *derived
from the map*, so re-seeding it after a save had been applied replaced the entry
and lost the flag. `SeedFixtureBreakables` now **carries broken/hp/effects over**
from the entries it replaces. Fixing it that way rather than by reordering the load
tasks means it cannot break again if that order changes — a derived table holding
dynamic state has to preserve it across a rebuild, or the rebuild is a data-loss
bug waiting for a reorder.

That one also hid behind a *false positive*: a fixture missing from the table
reports "nothing breakable" — exactly what a correctly-broken one reports. The
control that separated them was smashing an untouched fixture in the same run.

## Two axes: the attacker's type potency

The defender's half — `resists` — was there from the start. This is its mirror:
**`powers`**, a per-damage-type table saying how hard a thing strikes *with* that
type. `0` is ordinary, positive is potent, negative is feeble. The two axes meet in
one multiplication — potency scales the blow, the resist answers it.

| | defender | attacker |
|---|---|---|
| field | `resists` | `powers` |
| clamp | `resist_clamp` (0.8) | `potency_clamp` (0.6) |
| escapes | 1.0 = immunity, past it = **absorption** | **none** |

The clamp is **tighter** on the attack side because potency stacks from a weapon
*and* every worn piece, so it has more sources to pile up than a resist does. And
it has **no escapes**, deliberately: a resist of 1.0 means immunity and past it
absorption because those say what a thing *is* — a fire golem — whereas "I deal
150% fire" is stacking, not identity, so there is nothing to exempt. A feeble blow
also never becomes healing; that is the absorb stage's business on the defender's
side and must not be reachable from here.

**Who carries it.** Monsters (`monsters.cat powers`) and equipment — weapons and
armor both, summed across the wielded hand plus every worn piece, exactly the way
`PartyTarget::Resist` sums the defender's. Characters carry no innate cell, because
**their attack-side axis is already skill**; adding a second one would be two knobs
doing the same job. For a monster it is the reverse: it has no skills, so `powers`
*is* its "this one is dangerous with fire specifically".

The other hand's weapon lends nothing to this swing — what is in your left hand does
not make your right hand's blade burn — and a bolt carries no hand at all, so a cast
gets the worn half only.

Applied wherever an attack's `DamageEvent` is built: party melee, the enchanted
weapon's elemental burst (in **its own** element, not the blade's physical one —
the case the feature is for), both bolt-impact sites, and every square of a blast.

`defense::Potent` holds the arithmetic and `Balance::Potent` is a thin adapter
passing the knob — the same split the armor floor uses, and for the same reason:
linking `Balance.cpp` into `RollTest` dragged `Catalog` → `assets::ReadBinaryFile`,
i.e. the file layer. That is the purity wall working, and the attempt is recorded in
the harness's CMakeLists so nobody repeats it. **12 checks**, including that 1.0 is
*not* special on this side and that a feeble blow never heals.

Verified live rather than only measured: the skeleton mage authors
`powers = fire 0.45, bash -0.3`, and a probe on its real bolt read
**`authored 8.00 -> potent 11.60`** — exactly ×1.45.

### Tuning a blast (worked example: `fireburst`)

The numbers `fireburst` carried were the **flood model's**, and that model was
replaced — so they were retuned against the propagation rather than adjusted by
eye. The measured matrix, one candidate per row, is what the choice was made from:

| candidate | open room | dead end at your feet | T-junction centre |
|---|---|---|---|
| f5 d10 fall3 *(old)* | 10 centre / 7 at d1 | **35** | 14 |
| f7 d7 fall3 | 7 / 4 | 20 | **42** |
| f7 d5 fall1.5 ✔ | 5 / 3.5 | 17.5 | 30 |
| f8 d4 fall1 | 4 / 3 | 15 | 24 |

Against party health (Brand ~40, Maren ~33, Sera ~30, Tilo ~25).

**The ceiling is set by the ×6 a junction can converge, not by the open case** —
which was the surprise. A T-junction's arms reflect off their far ends and slosh
back onto the square you detonated in, so the worst number in the game is a
point-blank cast at a junction, not one in a dead end. The old `d10` made that 35–42:
an instant death for the caster. `d5` puts it at 30 — lethal to Tilo, survivable by
everyone else, and only reachable by detonating at your own feet in a junction.

The chosen row reads: **barely singes you when well placed** (3.5 at one step), an
unmistakable lesson when careless (17.5 each in a dead end), and a genuine
catastrophe only when you earn it.

**`on_hit` had to come down too.** Once the blast applies its payload's procs, a
`burn 3 6` lands 18 damage of DoT on *everything* in up to seven squares — more
than the blast itself, on every target at once. `burn 2 4` keeps fire's lingering
without the DoT quietly becoming the whole spell.

`blast_rate = 0.06` is fire rushing: a two-tick blast resolves in about an eighth
of a second. No `blast_persist`, because fire is transient — the front passes and
what it set alight keeps burning through `on_hit`.

Verified in play: a Fire Burst on a bone swarm one step from the party read *"caught
in the blast for 4 damage"* for all four (5 − 1.5, rounded) and *"catches fire"* on
each; the swarm died to the burn rather than the blow.

## Traps

**A measurement that omits a term is not weaker, it is wrong.** The defender
table was first run with `resist 0` while the armor content already carried
fractional resists — the half of mitigation that *scales*. It produced two
confident and completely false conclusions (that medium armor was dominated,
and that armor collapsed as damage rose) before the omission was spotted.

**Soak and resist are not interchangeable.** Flat soak is a fixed
subtraction and shrinks to nothing beside a big blow; a resist is worth the
same proportion however hard the monster hits. Which one dominates depends
entirely on the damage scale, so tuning either without stating the damage
you mean is meaningless.

**Bonuses must be large.** An opposed d100 deviates by ~41 points, so a
whole design expressed in a 25-point range is drowned by the dice. This was
measured once with a bridging scale factor and the entire span of the old
0..1 model came to a 0.25 spread in hit rate.

## Over-exertion is the one route that skips the pipeline

Every other source of damage in this document builds an `fx::DamageEvent` and
goes through `fx::Deal`. Over-exertion's health half does not: `SpendExertion`
calls `WoundMember` directly, so the bill is not resisted, not soaked, and not
answered by a ward. **That is a decision, taken by Michael on 2026-08-15 when the
one-pipeline check turned it up** — collapsing under your own effort is not
something armour turns, and a "second wind" ward absorbing your own exhaustion
would be a different game.

It is *declared* rather than quietly permitted: it is the `exertion` row in the
`pipeline` readout, and `tools\PipelineTest.ps1` fails if that row is ever zero.
So the exception cannot rot into an accident, and the day it should become a
`Burst` it is one edit rather than a discovery. See docs/effects.md, "The
invariant, CHECKED".

## Knobs

All in `balance.cat` and the editor's Balance dialog: `crit_threshold`,
`fumble_threshold`, `margin_damage`, `margin_cap`, `skill_curve`,
`skill_bonus`, `skill_cap`, `stat_curve`, `stat_bonus`, `stat_cap`,
`stat_baseline`, `defense_base`, `avoid_slope`, `avoid_cap`,
`armor_<class>_penalty` / `_floor` / `_str` / `_learn`,
`armor_offset_slope`, `armor_short_penalty`, `armor_short_stamina`.

Damage types are data too (`damagetypes.cat`) — C++ names none of them.

## Dev commands

`pipeline` / `pipelineguard [on|off|strict on|off|reset]` / `pipelinepoke
[member]` — the one-pipeline check (docs/effects.md): what moved health and by
which route, the arming switch, and a deliberate violation so the check can be
seen to catch one.

`guard <share> [member]` — set the stance; **no upper clamp at all**, unlike
the slider (which stops at `exert_max`), so it is the way to try a stance past
what the UI will let a player reach.
`wear <item|none> [member]` — put armor on the doll, where worn armor
counts.
`monsters` — each monster's cell, **hp and carried effects**; the readout that
makes a landed blow and whatever it left behind observable at all.

### Driving a combat test from a script

Four things cost a run each, all avoidable (`docs/drive.ps1`):

- The title menu opens with **Settings** pre-selected (it is the last entry), and
  a posted arrow key is sometimes dropped. **Click** "Start New Game" instead.
- The party knows **no spells** at start, so `cast` needs a `learn <member>
  <school>` first — and a caster with mana (Tilo, member 3).
- Dev commands reach the **world** even from the menu, because it is built at
  load. A `cast away` in the console is *not* evidence the game is in Playing.
- A monster adjacent to a fresh party nearly wipes it in seconds. `timescale
  0.02` while setting up, back to 1 to act.

`Send` types through **WM_CHAR**, which is what `Input::OnChar` reads, so it
handles shifted characters — underscores included, contrary to the long-held
belief that `wear plate_cuirass 0` could not be driven. (It is not named `Type`:
that is a built-in alias for `Get-Content` and silently shadows the function.)

### The showcase encounters

Until 2026-08-11 **no level placed a single monster**, so combat was unreachable
in ordinary play — most of why every number here is measured and none was played.
`showcase.ent` now carries six, graded because a fresh unarmored party is about
three swings from death *by design*:

| where | who | why it is there |
|---|---|---|
| 13,13 hall mouth | `skel_swarm` | hp 6, dmg 4 — a warm-up you cannot lose to |
| 12,6 mid hall | `skeleton` | hp 16, dmg 10 — the first real fight, 7 cells further in |
| 12,1 north alcove | `skel_coward` | dmg 0, harmless; exercises the flee archetype |
| 25,7 east room | `skel_archer` | skirmisher: kites and shoots, so it tests the lanes |
| 23,9 east room | `skel_warrior` | hp 22, dmg 14, large |
| 24,10 east room | `skel_mage` | caster: the one that throws a PAYLOAD at the party |

The east three sit behind `door_slab` (closed), so meeting the dangerous group is
a choice — a closed door stops movement, so they can only mass at it.

**Aggro ranges are 4-9 cells and the hall is 8x12, so anything sharing it
converges.** The spacing above is the most isolation that room allows, not a
guarantee of one fight at a time. Move them in the editor.

Note the reasoning lives HERE and not in the `.ent`: that file is regenerated by
the editor, which drops hand-written comments — and its records are
whitespace-tokenised, so a trailing `;` note is not safely ignored either.
