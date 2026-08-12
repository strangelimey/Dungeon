# The damage system

**Status:** BUILT on the `damage-system` branch (Michael, 2026-08-11).
Over-exertion is designed but deferred; everything else below is live.

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

The share is **0..N, not 0..1**. Over-exertion — spending past 100% by
burning stamina, health or a stat for one last huge hit — pushes it above 1.
That mechanism is deferred, but its shape is not: a clamp added now would
only have to be found and removed later.

Monsters take a stance too (`monsters.cat offense`), authored **per kind**
rather than per archetype: an archetype says how a creature moves and
perceives, not how boldly it does so, and a wild caster and a cautious one
share one. It couples both sides from one number — dropping the stance takes
the same points off the attack that it adds to the guard.

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

## Knobs

All in `balance.cat` and the editor's Balance dialog: `crit_threshold`,
`fumble_threshold`, `margin_damage`, `margin_cap`, `skill_curve`,
`skill_bonus`, `skill_cap`, `stat_curve`, `stat_bonus`, `stat_cap`,
`stat_baseline`, `defense_base`, `avoid_slope`, `avoid_cap`,
`armor_<class>_penalty` / `_floor` / `_str` / `_learn`,
`armor_offset_slope`, `armor_short_penalty`, `armor_short_stamina`.

Damage types are data too (`damagetypes.cat`) — C++ names none of them.

## Dev commands

`guard <share> [member]` — set the stance; **no upper clamp**, so it is the
only way to try over-exertion before its cost exists.
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
