# Health, healing and supplies

**Status: DESIGN, not built** (Michael, 2026-08-13). Nothing in this file exists
yet. It is the model agreed in conversation, written down so it can be argued
with before any of it is code.

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

## What exists today

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

| resource | aptitude | skill | trained by |
|---|---|---|---|
| stamina | vitality | **conditioning** | stamina spent |
| mana | int / wil | **channeling** | mana spent |
| health | vitality | **resilience** | health regained |

```
maxR   = baseR  + r_stat × aptitude        + r_cap   × Curve(skill)
R/sec  = r_base + r_apt  × Curve(aptitude) + r_regen × Curve(skill)
```

Every term is a balance.cat knob, so the eval harness can sweep them.

### Why skills rather than more stat growth

The first sketch had resource spending creep the *stats* — mana spend feeding
INT, stamina spend feeding VIT. That collapses two different ideas into one
number: with INT driving both max mana and mana regen, one action fed one stat
that did two jobs, and casters compounded.

A skill separates them, and it dissolves a second problem. Vitality was being
asked to set max health, stamina regen *and* health regen while being fed by two
loops. Under this model **VIT stays aptitude and conditioning becomes practice**,
which is also why no sixth stat is needed: *"constitution"* is the conditioning
skill, not a new attribute. No save-version bump, no new column in the sheet, and
`skillXp` already round-trips (v15).

### The training rule is throughput, not events

XP is proportional to the **points spent**, not to the number of actions — so one
expensive spell trains channeling more than three cheap ones. "The more it
channels through you" is meant literally.

### Resilience is self-limiting, and that is the point

Recovering is something that happens *to* you, so a skill for it looks like it
would reward idling. It does not: **regen only runs while below maximum**, so
resilience can only train after you have been hurt. There is nothing to regain at
full health, so it cannot be farmed by standing still.

The consequence is deliberate and worth knowing: **a flawless run trains no
resilience.** You get harder to kill by surviving damage, not by avoiding it.

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
currently does `pool += points × vit_exertion` → a VIT point. That is
docs/combat.md part 3's conditioning loop, and under this model it becomes
conditioning XP instead. Leaving both is precisely the runaway.

## Regeneration is gated by state

**Health only regenerates while stamina is not being expended.** That needs no
new machinery: the 1.5s `staminaHoldoff` after any spend already *is* that
signal.

| state | stamina | mana | health |
|---|---|---|---|
| **exerting** — holdoff active | 0 | reduced | **0** |
| **idle** — not spending | ×1 | ×1 | ×1 |
| **resting** | see below | see below | see below |

with the knobs constrained so **stamina/sec > mana/sec > health/sec** at equal
investment. That ordering stops being a hope and becomes a property the eval
harness checks.

## Rest is a time multiplier, not a regen multiplier

Resting **fast-forwards the world**. Everything else falls out of the per-second
rates above: health, stamina and mana return because *time passed*, and food and
water drain hard because *time passed*.

One knob instead of a second full set of resting rates that could drift out of
step with the ordinary ones. It is also what Dungeon Master actually did.

**And it buys the danger for free.** The world runs while you rest — monsters
keep thinking and moving through those fast-forwarded seconds — so resting near
something awake is genuinely risky with no wandering-monster mechanic to build.

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

`items.cat` already has `category = food` — `apple` and `bread` — with nothing
consuming them yet.

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
| conditioning, channeling, resilience | fog of war (`m_seen`) |
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

## Open — Michael's calls before any of this is built

1. **Skill names.** `conditioning` / `channeling` / `resilience` are placeholders.
2. **What happens at zero food or water?** Dungeon Master starves you: health
   drains and you can die. This decides whether supplies are a nuisance or the
   actual failure condition of a run.
3. **Does exertion drain food and water equally?** "More muscles need more food
   and water" suggests yes — but sweat is water, so a heavy fight might
   reasonably cost water disproportionately, which would make armour and
   stamina-heavy builds thirsty rather than merely hungry.
4. **Is `rest` a command or a state?** If it fast-forwards it is an action with a
   duration ("rest 8 hours") and needs an interrupt rule for something waking up
   nearby.
5. **Do the resource skills show on the character sheet** beside the school and
   weapon skills, or are they background numbers?

## What it touches when it is built

- `Character`: three skill ids, two supply meters, and their save lines (a new
  save version).
- `Character::RecomputeMaxima`: the skill terms.
- `DungeonWorld`'s regen tick: the state gate and the health path that does not
  exist yet.
- `SpendStamina`: re-point the `vit_exertion` creep at conditioning XP.
- `MagicSystem` / the cast path: channeling XP proportional to mana spent.
- `Balance` + balance.cat: every coefficient above, and `kBalanceFields` so they
  reach the editor's Balance dialog.
- Five lang keys per new skill (`skill.<id>`), as `avoid` and the armour skills
  have.
- The eval harness: a rung can then measure a *sequence* of fights, which is the
  whole reason this was designed (P6).
