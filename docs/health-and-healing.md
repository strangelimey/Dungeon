# Health, healing and supplies

**Status: PART BUILT** (2026-08-13). The five open questions are answered — see
"Settled" at the end; their answers are folded into the body, so read this top to
bottom and treat that section as the record of what was decided rather than as a
second source.

| part | state |
|---|---|
| **aptitude and practice** — the three skills, the maxima, the regen rates, the state gate, the training loops | **BUILT** |
| **food and water** — the two meters, conditioning's price, exertion, eating and drinking, starving and parched, save v25 | **BUILT** |
| rest | design only |
| movement pace from conditioning | design only |
| the resource skills on the sheet | design only |

What the built part changed that is worth knowing immediately: **health
regenerates**, which it never did before; **mana is much slower** and no longer
free (it was 1.2/sec and never even paused for casting, and is now 0.30/sec for a
novice, throttled to a quarter of that mid-exertion); and **`vit_exertion` is
gone** — stamina spent trains the conditioning skill instead of creeping
vitality, because vitality feeds max stamina and the loop closed on itself.

Every rate is measurable: the dev console's **`regen`** prints all three per
second for each member, plus two reference rows at equal investment that are
where the stamina > mana > health ordering is actually read. **`supplies`** does
the same for the two meters, in HOURS REMAINING rather than in units, because a
meter reading 62 means nothing until you know the drain rate behind it. The eval
harness's `resources` and `supplies` suites are the standing measurements
(`.\tools\Eval.ps1 -Only resources,supplies`).

It came out of the eval harness: a progression ladder needs to know how the party
*arrives* at the next fight, and the answer turned out to be a whole system
rather than a knob (`docs/eval-harness.md`, P6).

## The principle

**The more you do something, the better you get at it.** Skills already work this
way — they train by use, and levels derive from raw xp. This extends the same
rule to the three resources, so mana, stamina and health stop being fixed pools
that stats alone decide.

Everything below follows from that one sentence plus one constraint: **no loop
may feed itself twice**, or growth runs away.

## What existed before this (the starting point)

Kept because the new model is best read as an answer to it — and because the
three complaints at the end of this section are what the build actually fixed.

`Character::RecomputeMaxima`, with `k_health`/`k_stamina`/`k_mana` all 1.0:

```
maxHealth  = baseHealth  + k_health  × vitality
maxStamina = baseStamina + k_stamina × 0.5 × (strength + vitality)
maxMana    = baseMana    + k_mana    × 0.5 × (intelligence + willpower)
```

`baseHealth` is authored per member — it is the only thing that distinguishes the
four, and it rides the save (v17):

| | baseHealth | vitality | maxHealth |
|---|---|---|---|
| Brand | 27 | 15 | 42 |
| Sera | 19 | 11 | 30 |
| Maren | 21 | 13 | 34 |
| Tilo | 15 | 9 | 24 |

Regeneration:

```
mana/sec    = 0.4 + 0.08 × intelligence          ; NO holdoff, no gate
stamina/sec = 0.5 + 0.02 × maxStamina            ; only after stamina_holdoff (1.5s)
health/sec  = nothing at all
```

Three things about that are worth stating plainly, because the new model exists
to fix them:

- **The mana constants are hardcoded in `Character.h`**, not balance.cat knobs —
  the only resource rate that cannot be tuned.
- **Mana is currently FASTER than stamina** (7–24s to refill against 25–32s), and
  it never even pauses for casting. The new model deliberately inverts that.
- **Health has no regeneration path.** The only recoveries are unconscious
  self-stabilize (30 safe seconds → back at 20% of max) and the incidental 1 HP a
  VIT point grants. So health is the only resource carried between fights, and
  the other two are functionally infinite because waiting costs nothing.

## The model: aptitude and practice

Each resource gets an **aptitude** (a stat — what you *are*) and a **practice**
(a skill — what you have *done*), in the relationship the attack formula already
uses: skill is the main driver through a diminishing-returns curve, and the stat
shades it through a shallower one.

| resource | aptitude | practice | trained by |
|---|---|---|---|
| stamina | (STR + VIT) / 2 | **conditioning** | stamina spent |
| mana | (INT + WIL) / 2 | **attunement** | mana spent |
| health | VIT | **constitution** | health regained |

The aptitudes are unchanged from what the maxima always used; the mapping lives
in `Character::Aptitude` and nowhere else, because both formulas need it and two
copies would drift the day a race or a class shades one of them.

As built, for a resource `r`:

```
max    = base + k_<r>     × aptitude
              + Curve(practice)              ; <r>_skill_slope  .. <r>_skill_cap
/sec   = <r>_regen
              + <r>_regen_stat × Curve(aptitude)   ; the STAT curve, baseline 10
              + <r>_regen_max  × max
              + Curve(practice)              ; <r>_regen_slope .. <r>_regen_cap
```

Every term is a balance.cat knob reaching the editor's Balance dialog, so the
eval harness can sweep them. Four points about the shape, each a decision:

- **The aptitude enters `max` LINEARLY and `/sec` through the stat curve.** A
  pool is a capacity and should scale with the body it belongs to; a rate is a
  performance, and every other performance in this game tapers. The visible
  consequence is that an *average* aptitude is worth nothing to the rate (the
  stat curve's baseline is 10) while it is worth its whole self to the maximum.
- **A practice curve's slope and cap are in the unit being produced** — points of
  maximum, or points per second — rather than a normalised curve times a
  coefficient. One knob per statement, and the cap is then a number to balance
  against directly.
- **A CAP OF ZERO SWITCHES THE TERM OFF.** This is the one rule here worth a test,
  and it is not obvious: `CurveValue` reads a non-positive cap as "no meaningful
  shape, use the straight line the slope describes", which for a resource would
  mean a practice adding maximum *without limit*. It is exactly the shape of the
  armor-floor bug this project already paid for once. `resource::SkillTerm` owns
  the rule so the six call sites cannot each forget it.
- **`<r>_regen_max` pre-dates the model.** Stamina has always regenerated partly
  in proportion to its own pool (`stamina_regen_max`); it is kept and generalised
  rather than folded away, because "a bigger pool refills proportionally" and "a
  fitter body refills faster" are different statements. Health and mana default
  it to zero.

The arithmetic lives in **`src/Game/Resource.h/.cpp`**, pure (Curve.h and nothing
else), so `tools/RollTest` links the shipping code rather than a copy —
the `Defense.h` bargain. `Balance::Resources()` is the adapter that gathers the
flat knobs into it.

### Why skills rather than more stat growth

The first sketch had resource spending creep the *stats* — mana spend feeding
INT, stamina spend feeding VIT. That collapses two different ideas into one
number: with INT driving both max mana and mana regen, one action fed one stat
that did two jobs, and casters compounded.

A skill separates them, and it dissolves a second problem. Vitality was being
asked to set max health, stamina regen *and* health regen while being fed by two
loops. Under this model **VIT stays aptitude and the skills become practice**,
which is also why no sixth stat is needed: **`constitution` is a SKILL, not a new
attribute** — the thing every other game spends an attribute slot on is here
something you earn by surviving. No save-version bump for the skills, no new stat
column on the sheet, and `skillXp` already round-trips (v15).

### The training rule is throughput, not events

XP is proportional to the **points spent**, not to the number of actions — so one
expensive spell trains attunement more than three cheap ones. "The more it
channels through you" is meant literally.

### Constitution is self-limiting, and that is the point

Recovering is something that happens *to* you, so a skill for it looks like it
would reward idling. It does not: **regen only runs while below maximum**, so
constitution can only train after you have been hurt. There is nothing to regain
at full health, so it cannot be farmed by standing still.

The consequence is deliberate and worth knowing: **a flawless run trains no
constitution.** You get harder to kill by surviving damage, not by avoiding it.

### THE RULE THAT STOPS THE RUNAWAY

**Resource skills train, and creep nothing.**

This needs saying explicitly because the engine's default is the opposite:
`GrantSkillXp` creeps the source's associated stats *by design* — that is the
general rule for every skill in the game. Route the three resource skills through
it in the ordinary way and the double-dip walks straight back in.

So stat growth stays exactly where it is today (school skills, weapon classes,
avoid), and the resource skills own resource growth. One path in, one path out:

| grows | from |
|---|---|
| stats | school / weapon / avoid skills — unchanged |
| resource max + regen | the three resource skills |
| resource skills | resource throughput |

**This REPLACES an existing loop, it does not sit beside one.** `SpendStamina`
used to do `pool += points × vit_exertion` → a VIT point. That is docs/combat.md
part 3's conditioning loop, and it is now conditioning XP. Leaving both would be
precisely the runaway, so **`vit_exertion` was deleted rather than set to zero**
— a knob that must stay at zero for the game to be correct is a trap with a dial
on it.

As built, all three practices go through one function, `GrantResourceXp`, whose
entire reason to exist is that it passes an EMPTY stat list to `GrantSkillXp`.
Every other skill in the game creeps its associated stats by design; each of
these three feeds a pool its aptitude also feeds, so the ordinary award would
close the loop. One function makes that a property of the code instead of a rule
three call sites have to keep remembering.

**How to see it**: `char <n>` prints the creep pools beside the skills. A
non-zero pool next to a resource skill IS the bug — and the pools are printed
precisely because a stat that *has not moved yet* looks identical to one that
never will, until the pool crosses 1.0.

## Regeneration is gated by state

**Health only regenerates while stamina is not being expended.** That needed no
new machinery: the 1.5s `staminaHoldoff` after any spend already *is* that
signal, which is why the gate is expressed in terms of it.

| state | stamina | mana | health |
|---|---|---|---|
| **exerting** — holdoff active | 0 | × `mana_exert` (0.25) | **0** |
| **idle** — not spending | ×1 | ×1 | ×1 |

Resting is deliberately NOT a third row: it is a TIME multiplier, so it feeds
these same rates more seconds rather than different numbers, and cannot drift out
of step with them.

The knobs are constrained so **stamina/sec > mana/sec > health/sec** at equal
investment. That ordering is a property of the AUTHORED NUMBERS and not of the
arithmetic, so RollTest cannot reach it — the dev console's `regen` command
prints it instead, and the `resources` eval suite is where it is read.

**Read the ordering on `regen`'s two `ref` rows, not on the member rows.** Getting
this wrong is instructive: a first version printed a verdict per member and called
Brand BROKEN — and Brand is right. He is a brute with INT 8, so his mana crawls
behind even his health, exactly as it should. The claim is about the knobs at
EQUAL investment, and a party of four deliberately unequal characters can never
test it. The reference rows put every aptitude at the stat curve's baseline and
share one practice level across all three pools, measured untrained and trained,
because a crossing can hide at either end.

Measured at the shipped defaults:

| practice | health/s | stamina/s | mana/s |
|---|---|---|---|
| 0 | 0.150 | 0.700 | 0.300 |
| 10 | 0.288 | 1.043 | 0.443 |

## Rest is a time multiplier, not a regen multiplier

Resting **fast-forwards the world**. Everything else falls out of the per-second
rates above: health, stamina and mana return because *time passed*, and food and
water drain hard because *time passed*.

One knob instead of a second full set of resting rates that could drift out of
step with the ordinary ones. It is also what Dungeon Master actually did.

**And it buys the danger for free.** The world runs while you rest — monsters
keep thinking and moving through those fast-forwarded seconds — so resting near
something awake is genuinely risky with no wandering-monster mechanic to build.

### Rest is a STATE, not a command (settled)

You enter it and you leave it, rather than committing to "rest 8 hours" up front.
Time runs fast until you stop it — so you watch the meters fill and decide when
enough is enough, trading recovery against supplies in real time instead of
guessing a duration and living with the result.

Three consequences follow, and they are the reason this is the better half of the
choice:

- **The player is the interrupt rule.** A duration-based command would need one
  written ("abort if something comes within N cells"), with all the argument about
  what counts. A state needs none: you are watching, and you stop.
- **It still needs an automatic break for the case you are not watching** — a
  monster landing a blow drops the state, because being hit while fast-forwarding
  is how a rest becomes a wipe with no input.
- **It is one flag, so it saves and scripts trivially.** The eval harness enters
  rest, `step`s, and leaves — no new duration plumbing, and a ladder rung can
  finally answer *how does the party arrive at the next fight*.

## Movement

Conditioning makes a character faster, using the `moveSpeed` field that already
exists:

```
moveSpeed = 1 + move_skill × Curve(conditioning)
```

**The party moves at the pace of its slowest member** (`ApplyPartySpeed` feeds
the roster minimum into `Party::SetSpeed`) — confirmed as intended. So the speed
benefit is invisible until the *worst-trained* member has it: one unconditioned
mage caps the whole party. Either everyone trains, or everyone crawls.

That rule now has teeth it did not have before, because conditioning makes
members genuinely diverge in pace.

## Food and water

Two per-character meters, in the Dungeon Master shape. They are what makes rest
cost something, and therefore what makes every rate above matter.

```
food  -= (food_rate  + food_cond  × Curve(conditioning)) × dt      ; per second
water -= (water_rate + water_cond × Curve(conditioning)) × dt

food  -= stamina_spent × food_exertion                             ; per point spent
water -= stamina_spent × water_exertion
```

Resting needs no term of its own — it simply supplies a great deal of `dt`.

Two properties are deliberate:

- **Conditioning has a price.** The fitter member burns more food and water, so
  training is not free. Every other loop in this design compounds upward; this is
  the one that taxes the compounding, and it is what keeps the whole thing
  bounded.
- **Water should drain faster than food.** Thirst kills before hunger, and it
  makes the two supplies behave differently rather than being one meter drawn
  twice.

**Exertion costs water more than food (settled).** `water_exertion >
food_exertion`, so the sweat is modelled and not just the fuel. It is the second
place the two meters diverge, and it has a real consequence for builds: a heavy
fighter in plate — already paying the armour stamina surcharge, and paying it
again through `stamina_spent` here — becomes **thirsty rather than merely
hungry**. Water is the supply that decides how deep a stamina-heavy party can go,
which is a more interesting logistics problem than one meter drawn twice.

`items.cat` already has `category = food` — `apple` and `bread` — and `apple`
already authors `command = eat`. **`GameUI::EatFromHand` exists and restores 25%
of max STAMINA**, a placeholder from the items thread. It is re-pointed at the
food meter here, the same way `SpendStamina`'s `vit_exertion` creep is re-pointed
at conditioning: an existing seam, not a new one. Nutrition becomes a catalog
field, so bread feeds more than an apple, and **water needs a drink item and a
`drink` verb** — neither exists yet.

### Zero food or water KILLS (settled)

The Dungeon Master answer, and the one that makes supplies the actual failure
condition of a run rather than a nuisance meter. **Health drains at zero and a
character can die of it.**

**Build it as an EFFECT, not as a special case in the regen tick.** Starving and
parched are `fx` kinds applied when a meter empties and removed when it is
refilled, which buys the whole thing from machinery that already exists and is
already tested:

- the DoT arrives as a **`Tick`** through `fx::Deal`, so it is resisted like
  anything else and every rule about damage stays in one pipeline (docs/effects.md);
- **a Tick on a downed member kills** under the existing overkill rule, which is
  precisely "starvation can kill you" with no new death path to write;
- the icon, the party-bar strip, the sheet's Effects tab and the save round-trip
  all come for free.

Two things this needs that do not exist: a **damage type** for it (the seven are
slash/pierce/bash plus the four elements, and none of them is starvation — a
`starve` type in `damagetypes.cat` that nothing resists) and the two effect
classes with their `effects.cat` blocks and lang keys.

**Water first.** Thirst should reach zero before hunger and bite harder when it
does, so the parched effect carries the larger magnitude.

### As built

`starving` and `parched` are ordinary DoT classes (`src/Game/Effect/
SupplyEffect.*`) dealing the new `starve` type. They differ from poison and
bleed in exactly one way, and it is not in the class: **a DoT runs out and these
do not**, so the SUPPLY TICK holds `timeLeft` topped up while the meter is empty
and erases the effect when it is not.

That is deliberate rather than a shortcut. A "permanent effect" concept would
have needed its own rule for what clears it, and the meter already IS that rule
— so keeping one truth means an eaten apple cannot leave a member starving, and
a reloaded save cannot restore a starving member who is not hungry. **The effect
is the meter's shadow.**

The transition is owned by ONE place for the same reason. `ConsumeItem` refills
the meter and deliberately does not lift the effect; the next supply tick sees a
non-empty meter and lifts it, with its relief line. Two sites that could remove
it would eventually disagree.

Measured at the shipped defaults (`.\tools\Eval.ps1 -Only supplies`):

| | food | water |
|---|---|---|
| a fresh, untrained member | 7.9 h | 5.0 h |
| the same member at conditioning 25 | 5.0 h | 3.2 h |

**A measurement worth Michael's eye, not a proposal:** exertion is currently a
much smaller drain than time. Eight steps cost about 0.06 food and 0.12 water,
so marching is nearly free; a fight spending ~30 stamina costs about 2.4 food and
4.5 water, which is a real 4.5% of the water meter. Time dominates, and whether
that is the right split is a playtest question.

### What this does to the shape of a run

**Attrition moves from health to supplies.** You always arrive at the next fight
healed, but poorer. A dungeon becomes a logistics problem rather than a health
bar drained across a level, which is the answer to the question the eval harness
kept asking: *how does the party arrive at the next fight?*

## Splitting the party (later)

Everything here is **per-character**, which is what will make splitting the party
for sub-quests feasible later:

| per-character — split-safe | per-party — needs a group notion |
|---|---|
| food, water | movement pace (slowest member) |
| health / stamina / mana + maxima | position, facing |
| conditioning, attunement, constitution | fog of war (`m_seen`) |
| every regen rate | monster aggro target |

**Do not let any of this drift onto the party object.** A shared food pool or a
party-level rest timer would read as a harmless simplification and would be the
expensive thing to unpick the day a character walks off alone. Keeping it on
`Character` costs nothing now.

The real work when that day comes is not the roster — that is already resize-safe
— it is that `DungeonWorld` assumes ONE party: a single grid position and facing,
one `m_seen` mask, and an `ai::Snapshot` with a single `partyX`/`partyZ` every
monster aggros toward. Splitting makes those per-group. Nothing decided here
forces that either way.

Splitting also becomes a real tactical answer to the slowest-member rule, not
merely a quest device: the scout who trained conditioning can finally use it.

## Settled — Michael's five calls, 2026-08-13

All five were open when this doc was written; all five are now answered, and the
body above is written as though they always were. Kept here as the record of what
was chosen, and of what each choice ruled out.

| # | question | **answer** | what it rules out |
|---|---|---|---|
| 1 | skill names | **`conditioning` / `attunement` / `constitution`** | `channeling`, `resilience` |
| 2 | zero food or water | **it kills** — health drains and a character can die | supplies as a nuisance meter |
| 3 | exertion drain | **water heavier than food** (`water_exertion > food_exertion`) | one shared coefficient |
| 4 | rest | **a STATE** you enter and leave | `rest <hours>` and its interrupt rule |
| 5 | the sheet | **shown, in their own group** | background numbers |

Three of them are worth a sentence more than the table gives:

**Naming (1).** `conditioning` was kept over `endurance` because it is the skill
that also drives move speed and food drain, and the athletic reading is the one
that makes those two make sense. `constitution` landing on HEALTH is the design's
own joke made literal: the attribute every other game reserves a slot for is here
a thing you earn by being hurt and getting up.

**Starvation (2)** is the choice that decides what kind of game this is. A run can
now be lost to logistics with every monster on the level still alive — which is
the point of moving attrition from health onto supplies, and is empty unless the
meter can actually finish you.

**The sheet (5).** Own group, not mixed in with the school and weapon skills:
those are things you chose to practise, these are things your body did. Grouping
says so without a word of explanation, and it also keeps a Skills tab that lists
only trained skills from opening on three rows nobody selected.

## What it touches

### Done

- **`src/Game/Resource.h/.cpp`** (new, pure): `Kind`, the three skill ids,
  `Rules`/`PoolRules`, `SkillTerm`, `Contribution`, `Maximum`, `RegenPerSec`.
- `Character`: `Aptitude` / `PracticeLevel` / `ResourceMax` / `RegenPerSec`, and
  `RecomputeMaxima` now takes a `PoolRules` instead of three floats.
  `ManaRegenPerSec` is gone — its constants were hardcoded in the header.
- `DungeonWorld`'s regen tick: one loop for all three pools, the state gate, and
  the health path that did not exist at all.
- `GrantResourceXp` (new) — the three practices, awarded with an empty stat list.
- `SpendStamina`: trains conditioning; the cast path trains attunement; the regen
  tick trains constitution off the points regained.
- `GrantSkillXp`: looks the skill up before inserting it. The subscript built a
  `std::string` every award, which was harmless while every award was an EVENT
  and is not now that constitution trains every frame a member is healing.
- `Balance` + balance.cat: 21 knobs and `Resource()`/`Resources()`.
  **`vit_exertion` deleted.**
- Save: **no version bump** — `skillXp` has round-tripped since v15, which is the
  whole payoff of constitution being a skill and not a sixth stat.
- `skill.<id>` × 3 in all five lang files.
- Dev: **`regen`** (the rates + the ordering), and `char` now prints the creep
  pools. `tools/RollTest` +15 checks; `tools/AllocTest.ps1 -Wounded`;
  `tools/EvalScripts/resources.eval` as the `resources` eval suite.

### Done — supplies

- `resource::Supply` / `SupplyRules` / `DrainPerSec` / `Refill` in the pure TU;
  `Balance::SupplyOf`. `Character::food` / `water` / `SupplyLevel`.
- `DungeonWorld::TickSupplies` (drain by time, raise or lift the effect),
  `DrainSuppliesByExertion` (hung off `SpendStamina`, so every swing, cast and
  step in the game pays it), `ConsumeItem`.
- **starving / parched**: `Effect/SupplyEffect.*`, `AllEffects.cpp`, CMakeLists,
  `effects.cat`, and a `starve` damage type in `damagetypes.cat` that nothing
  resists.
- `GameUI::EatFromHand` re-pointed from its 25%-stamina placeholder onto the
  meters, through a new `onConsume` callback; `eat` and `drink` are ONE handler.
  `nutrition` / `hydration` on items.cat + the item schema; a `waterskin`.
- Save **v25** (`supply <i> <food> <water>`); a pre-v25 save loads FULL.
- Dev: `supplies`, `setsupply`, `consume`. Lang keys ×5 for both effects, the
  damage type, the waterskin and five log lines. RollTest +8; the `supplies`
  eval suite.

### Still to build

- **Rest**: one flag on the world, the time multiplier, and the hit-breaks-it rule.
- `moveSpeed` from conditioning, through the existing slowest-member rule.
- The character sheet's Skills tab: the resource skills as their own group.
- The eval harness: a rung that measures a *sequence* of fights, which is the
  whole reason this was designed (docs/eval-harness.md P6).
