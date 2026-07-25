# The effects system

**Status:** PLAN (Michael, 2026-07-24). Nothing built yet.

One pipeline for everything that happens *to* a combatant — a sword blow,
a fire bolt, a burn, a poison, a ward, a future slow or blessing — so
that resistances, bonuses and knobs are applied in exactly one place, the
same way, whether the thing on the receiving end is a party member or a
monster.

## Why

Today there are two of everything, and they have drifted:

- **Damage application.** `WoundMember` (party: water-veil absorb, splat,
  overkill/unconscious/death, wipe latch) vs `monster.hp -= dmg` written
  out longhand at five call sites (party melee, spell hit, fire-shield
  retaliation, burn tick, and the slain message each time).
- **Defense assembly.** `PartyDefense` (nature + worn gear + stone skin)
  vs `MonsterDefense` (one catalog table).
- **Status effects.** `Character::effects`, a real list with kinds,
  schools, durations and a HUD — vs `Monster`'s single burn slot, added
  with the fire sword because there was nowhere else to put it.
- **Ward behaviour.** Four wards, four hard-coded sites, none of them
  near each other: the air deflect lives inside the monster-projectile
  resolver, the water absorb at the top of `WoundMember`, the earth
  resist inside `PartyDefense`, the fire retaliation halfway through
  `MonsterAttack`. Adding a fifth ward means finding a sixth site.

The cost is not just tidiness. It is that **a monster cannot be warded, a
member cannot be set alight, and a new effect has to be threaded through
both halves by hand** — every content idea pays an engineering tax.

## The shape

### One event, six stages

Every source of damage builds a `DamageEvent` and hands it to the system.
The system walks fixed, named stages; effects on either side subscribe to
the stages they care about. Every behaviour that exists today lands on
exactly one stage — that is the test of whether the stage list is right.

1. **Deflect** — the event may be cancelled outright, before any roll.
   *(Wind Ward: turns the bolt aside, spends a charge.)*
2. **Strike** — the to-hit roll: accuracy vs evasion, `ResolveAttack`'s
   existing job. Skipped for events flagged `unavoidable` (DoT ticks,
   retaliation, a wall bump).
3. **Mitigate** — soak, then resist for the event's damage type. The
   resist is summed from *every* contributor: nature, worn gear, and any
   effect that offers one. *(Stone Skin becomes a contributor instead of
   a special case inside `PartyDefense`.)*
4. **Absorb** — pooled effects eat what is left. *(Water Veil, which
   dies by spending rather than by timing out.)*
5. **Apply** — hp, the splat, unconscious/overkill/death, the slain
   message, the party-wipe latch, threat credit. One implementation,
   parameterised by the target adapter.
6. **React** — what the landed blow *causes*: on-hit procs from the
   attacker's side (the fire sword's ignite, a monster's poison), and
   on-struck effects from the defender's side *(Fire Shield's scorch)*,
   plus skill XP for a landed blow.

An event that deals no damage still runs the stages — a deflect or a full
absorb is a legitimate outcome, and stage 6 must know it did not land.

### Effects are KINDS + instances (the flyweight)

Following the settled precedent — spells are CLASSES, `spells.cat` is
numeric overrides only — an effect's *identity and behaviour* is C++, its
*numbers and presentation* are data.

```
class EffectKind {                    // ONE shared instance per id
    virtual void  Tick(Inst&, ITarget&, float dt) const;
    virtual void  OnIncoming(Inst&, DamageEvent&) const;   // deflect / absorb
    virtual float ResistFor(const Inst&, DamageType) const;
    virtual void  OnStruck(Inst&, ITarget& self, ITarget& by) const;
    virtual void  OnExpire(Inst&, ITarget&) const;
    virtual void  ApplyOverrides(const CatalogEntry&);     // effects.cat
    Stacking stacking;   // Refresh | RefreshPerSchool | Stack
};

struct Inst {                         // the per-combatant POD (saved)
    const EffectKind* kind;
    SpellSymbol school;               // tint, and school-keyed behaviour
    float magnitude, timeLeft, duration;
    int source;                       // who applied it (threat credit)
};
```

(This sketch once carried a separate `power` beside `magnitude`. As built
there is one number: nothing today has two, and a dead field is worse
than a rename later. The P2 hooks are likewise absent from the class
until P2 — inventing `DamageEvent` and `ITarget` before their callers
exist is how you design them wrong.)

This is the codebase's dominant idiom (`ItemKind`/`Item`,
`MonsterKind`/`Monster`): the fat, behaviour-carrying half is shared and
loaded once; the instance is a small POD in a vector, so a combatant with
no effects costs nothing and applying one is a `push_back`.

Concrete kinds at the start: `burn`, `poison`, `bleed`, `stoneskin`,
`waterveil`, `fireshield`, `windward`, `sight`. Every one of them is an
existing behaviour moved, not new code.

### One target, two adapters

`fx::ITarget` is the only thing the system knows about a combatant —
`Effects()`, `NatureResist(type)`, `GearResist(type)`, `Soak()`,
`Evasion()`, `Wound(amount, flags)`, `IsDown()`, `Name()`, `Message()`.
`DungeonWorld` implements it twice, over `Character&` and over `Monster&`.

The module is walled off exactly like `MagicSystem` and `ai::` — it knows
nothing of `DungeonWorld`, the map, or the party, and reaches the world
only through the interface. That wall is what makes the two sides
genuinely share one implementation instead of two that look alike.

### What a "damage effect" is (and isn't)

Instantaneous damage runs the **pipeline** but is not **stored**: a sword
blow is a `DamageEvent`, not an `Inst` parked on the target for one frame
and deleted. Same stages, same knobs, same resist maths — that is the
part that matters. Only something with a *duration* becomes an instance.

## What it unlocks

- **A monster can carry anything a member can.** Warded, slowed,
  blessed, poisoned by another monster — all free once the list is
  symmetric. A skel_mage that shields its friends becomes a catalog
  entry, not a feature.
- **A member can catch fire.** The fire sword's burn already has a party
  mirror waiting; today it would need writing twice.
- **A new effect is one class + one catalog block + five lang keys.**
  No hunting for the sixth site.
- **Numbers move to data.** `effects.cat` means a burn's dps, a ward's
  pool, and a poison's duration tune live in the editor next to
  `balance.cat` and `attacks.cat`, in the same dialog idiom.
- **Weapons and spells reference effects by id.** `element_dot = burn
  3 6 0.5` becomes `on_hit = burn 3 6 0.5` and stops being fire-specific;
  a serrated blade authors `on_hit = bleed`, a frost axe `on_hit =
  chill`, with no engine change.

## Phases

Each phase compiles, runs, and is verifiable on its own; each is one
commit. The order is chosen so that behaviour is preserved until the last
possible moment — the risky phase (2) has the previous phase's tests
still meaningful.

**P1 — the module, with today's behaviour. LANDED 2026-07-24.**
`Game/Effect/` (namespace `fx`) laid out like `Game/Spell/`: `Effect.h/.cpp`
(the base, `Inst`, `EffectBook`), a file pair per kind, and `AllEffects.cpp`
hand-listing the eight. `Inst` replaces `StatusEffect`; `Character::effects`
holds them. Ward/poison/bleed/sight behaviour stays exactly where it is —
this phase only changes what the list holds.

As built, three things worth knowing:
- **Each ward is its own kind** (`stoneskin`/`fireshield`/`waterveil`/
  `windward`), because in P2 each overrides a different hook. Wards
  stacking across schools now falls out of that — a kind only refreshes
  itself — instead of being a rule spelled out at the cast site. The four
  Sight spells DO share one kind (they differ only in flavour, which the
  school carries) and it names itself per school.
- **`fx::Apply` owns the stacking rule.** Every landing site used to
  open-code its own `RemoveEffect`/`RemoveWard` first; now the kind's
  policy decides, and `CastServices::applyEffect` lets a spell land one
  without knowing either the classes or the policy.
- **The save is id-keyed already** (this was P5's job on paper). The kind
  token was always a free-form string, so writing the effect id costs
  nothing and `EffectBook::FindLegacy` maps the old category tokens
  ("ward" + school → that school's kind) forward. No version bump, and
  P5 shrinks to the monster side.

*Verified in game:* wards cast and stack, the HUD strip and the sheet's
Effects tab read identically (names, magnitude-formatted descriptions,
time left, borrowed rune icons), poison lands and fades with its name, a
save round-trips, a hand-edited legacy-token save loads as the right
wards, and three of the four ward BEHAVIOURS were caught live — water
veil soaking then bursting, fire shield scorching for 9, stone skin
halving a blow. The air deflect needs a caster monster's bolt and wasn't
observed; its site is the same one-line type change as the other three.
Dev: `effect <id> [member] [magnitude] [seconds]` lands one directly,
because setting a ward up by casting is a coin toss (vocabulary, mana,
fumble) and then a monster still has to choose to hit its bearer.

**P2 — the pipeline, and the four wards move home. LANDED 2026-07-24.**
`fx::ITarget` with its two adapters (`DungeonWorld::PartyTarget` /
`MonsterTarget`), `fx::DamageEvent`, and `fx::Deal` walking the stages.
Every damage site now builds an event and hands it over: party melee,
monster melee, both projectile resolvers, the wall bump, the party DoT
tick, the monster burn tick. `PartyDefense`/`MonsterDefense` are gone
(the adapters assemble it, and the effect term is a sum over whatever the
target carries, not a hard-coded Stone Skin branch), and the four wards
are four hooks in `Effect/WardEffect.cpp`.

Three things the build taught us:
- **Stage 6 is the CALLER's to run.** A reaction writes a line ("the blob
  is scorched by the fire shield") that has to read *after* the blow it
  answers. With react inside `Deal` the log came out backwards, so it
  split into `fx::React`, called once the caller has narrated. The same
  problem in miniature — the fall lines — is why `WoundMember` now
  *reports* a `Fall` and `PartyTarget::NarrateFall` says it, and why the
  monster's slain LINE stayed with its caller while the death PATH moved
  into the adapter.
- **Flags, not delivery, decide the maths.** An enchanted weapon's
  elemental half is resisted by element but neither rolled nor soaked, so
  `DamageEvent` carries `rolled`/`soaked`/`resisted` separately from
  `Delivery`; the presets set the usual combinations. (P2 shipped the
  presets preserving the old numbers — *everything* is resisted now, see
  "Everything is resisted" below.)
- **A reprisal goes straight to `Wound`,** not back through `Deal` — it
  is not itself deflectable or soakable, which also means a reaction can
  never recurse into another one.

*Verified in game:* stone skin halving a blow (4→2), the fire shield
scorching for 9 — including a reprisal that KILLED the blob and narrated
it — the water veil soaking a wall bump with no health lost, the wind
ward correctly NOT deflecting melee, monster melee with its poison proc
and fall lines, a party melee kill, and a spell bolt ("seared for 8" then
"destroyed") — every line in its original order. NOT verified live: the
wind ward deflecting an actual bolt (the test level's one caster never
fired — the swarms always closed first), and a burn kill after the
retiming (its `slew` mechanism is the one three other kills proved).

**P2 — the pipeline, and the four wards move home.**
`ITarget` + the two adapters, `DamageEvent`, the six stages. Every damage
site — party melee, monster melee, both projectile resolvers, the bump,
the DoT ticks, the fire-shield retaliation — becomes "build the event,
hand it over". The four hard-coded ward sites become `OnIncoming` /
`ResistFor` / `OnStruck` overrides and the originals are deleted. This is
the phase that pays for the whole plan, and the one to feel-test hardest:
every ward, an overkill death, a party wipe, a monster slain by each of
melee/bolt/burn/retaliation.

**P3 — monsters get the list. LANDED 2026-07-24.**
`Monster::effects` is the only status storage: `burnDps`/`burnLeft`/
`burnSchool`/`burnSource` are gone, `BurnEffect` is a kind like any other,
and `TickBurn` is gone too — both sides age and bite through ONE
`TickEffects`, differing only in the lambda that words an expiry. Poison
and bleed became applicable to monsters for free, which is the whole
point of the symmetry.

As built:
- **The plume is derived, not stored.** `Monster::plume` is created and
  dropped each frame from "does any effect on me have `plume = 1`", and
  its light reads the same lookup. The effect list is the truth; the fire
  is what that truth looks like.
- **Resist moved to per-tick** (decision 1). A DoT stores RAW magnitude
  and is resisted as it bites, so a ward raised mid-burn helps at once.
  This also gives party poison/bleed a resist they never had — a
  deliberate balance change, not a refactor.
- **A DoT's damage type is authored, not derived from its school.**
  Bleeding rides fire red in the HUD and wounds as PIERCE; poison is
  earth; a burn is per-INSTANCE — whatever element lit it, so a frost
  weapon's burn is resisted as water. `SchoolDamageType` moved from
  Balance.h to Combat.h for this: it is a fact about damage, not a knob,
  and the effects module can't see Balance.
- **A death by DoT reads by cause**: burning away to nothing if it was
  alight, plain "slain" otherwise.

*Verified in game:* `effect burn ahead` on a fire-VULNERABLE mummy — it
lit up, burned visibly faster than its raw dps (per-tick resist doing its
work), and burned away to nothing; `effect poison ahead` on a blob —
ticked with no plume and killed it as "slain", the first time a monster
has ever carried a poison. Dev: `effect <id> ahead [magnitude] [seconds]`
is the only hand-authored way onto a monster, and the way to watch an
effect tick without a weapon that procs it.

**P4 — content authors effects by id. LANDED 2026-07-24.**
`fx::Proc` — an effect id plus its numbers — is what a weapon or a monster
authors: `on_hit = burn 3 6 0.5, bleed 2 10`, comma-separated, parsed by
`fx::ParseProcs` and rolled by `fx::ApplyProcs`. Both sides use the same
field and the same function; `IgniteMonster` and `ApplyHitEffect` are
gone, and with them the last per-side copy of "roll it, land it, announce
it". The older one-effect-per-line fields (`poison`/`bleed` on a monster,
`element_dot` on a weapon) still load, appended as procs naming the same
effects, so no catalog had to be rewritten — though the demo's were.

(The spell half of this phase was already done: wards and sights have
applied through `CastServices::applyEffect` since P1.)

As built:
- **Each effect owns its two announce lines** (effects.cat `apply_party` /
  `apply_monster`), because the two sides word the same affliction
  differently — "Sera is poisoned!" against "The blob is poisoned!". The
  target picks the one that fits its grammar (`ITarget::SayApplied`), and
  only a NEW affliction announces: a refresh is the same thing lasting
  longer, not a fresh alarm.
- **An element is a flavour, not a separate mechanism.** A weapon's
  `element` lends its school to whatever its procs land, so the *same*
  `on_hit = burn` is fire on the flamebrand and a freezing burn on the
  frostbrand — resisted as water, plume running cold blue.
- **Immunity refuses outright** rather than letting something burn for
  nothing.
- `fx::Deal` lost its `attacker` and `knobs` parameters — React took the
  first, the target's adapter applies the second. An unused parameter
  that suggests otherwise is worse than none.

*Content proving it:* `[frostbrand]` (same burn, water element) and
`[serrated_blade]` (no enchantment at all — just `on_hit = bleed`), both
pure catalog entries, placed on the start level. The demo's monsters were
converted to `on_hit` in place.

*Verified in game:* both new weapons load, equip and fight; the monster
side lands its converted `on_hit = poison` on a member; and a traced run
confirmed the weapon path — proc parsed (1 proc from `on_hit`), landed on
the blob, announced once, then silent on the refreshing hits.

**P5 — save. LANDED 2026-07-24.** (Half had already landed in P1: the
character side was id-keyed, with the legacy tokens mapping forward.) The
monster side now rides `EntityState::effects`, written as `enteffect`
lines — save **v22**.

As built:
- **One shared `SaveData::EffectState`**, hoisted out of `CharState`, used
  by both sides. The effects system is symmetric, so its save record had
  no reason not to be; it gained `source` (a DoT's threat credit) in the
  move.
- **The effect lines ATTACH to the entity line above them** rather than
  carrying an index. The reader hangs each on `entities.back()`, so the
  two can never drift out of step — and a stray line before any entity is
  simply dropped.
- **Nothing about the presentation is saved.** A reloaded monster's plume
  and its light come back purely from the restored effect list, because
  P3 made them derived.
- A monster whose only change is an affliction now qualifies for a diff
  (`!m.effects.empty()` joins the "worth saving" test).

*Verified in game:* a blob given a burn and a poison, saved (both lines
present, correct schools, v22), reloaded — visibly ablaze again — and
re-saved, showing both effects still on it with their timers ticked down
by the elapsed seconds, which is the proof they came back LIVE rather
than as an echo.

**P6 — the editor. LANDED 2026-07-24.**
`effects.cat` has a `CatalogSchema` entry (name / icon / school / plume /
damage_type / stacking / the two apply lines) and an **Effects** palette
category, so an effect is browsable and editable like every other type.

The interesting part was what an effect is NOT: content you author and
tune, never content you place. So `CatInfo` gained a `placeable` flag, and
where it is false:
- a row click opens the type editor instead of arming a brush (there is
  nothing to arm, and doing nothing would just look broken);
- there is no **+ New...** row — an effect needs a C++ CLASS, and data
  alone cannot make one;
- **Delete refuses**, with a reason. Removing the entry would not remove
  the effect; it would silently revert it to its class defaults, which is
  not what a Delete button promises.

Known wart, inherited rather than introduced: a field the entry OMITS
shows the schema default, indistinguishable from an explicit value. It
bites `burn`, which deliberately has no `damage_type` (its class resolves
one per instance from whatever lit it) and so displays "bash". The help
text names the exception; the real fix is a dialog that renders "unset"
distinctly — an open item from the type-authoring thread.

*Verified in game:* the Effects category lists the kinds with no "+ New";
clicking `burn` opens "Effects type — burn" with its four tabs; Look shows
icon/school/plume read from the catalog (plume ticked); poison's Stats
shows its explicit `earth`; and Save round-trips the file with every field
and comment intact, still valid UTF-8.

---

## Everything is resisted (Michael, 2026-07-24)

> "A bump is a bludgeon attack which is resisted by plate but not by cloth
> or skin."

P2 shipped the event presets set to reproduce the pre-refactor numbers,
which left collisions and DoTs landing raw. That was refactor caution, not
a design position, and the design position is the one above: if it damages
you, your defenses answer it. The presets now name a KIND of damage rather
than a set of flags, and each says what applies:

| preset | rolled | soaked | resisted | what it is |
|---|---|---|---|---|
| `Blow` / `Bolt` | ✓ | ✓ | ✓ | a swing or a shot |
| `Impact` | — | ✓ | ✓ | a COLLISION: a wall, a door, a falling rock |
| `Burst` | — | — | ✓ | magic riding something else: an enchanted blade's element, a ward's reprisal |
| `Tick` | — | — | ✓ | a DoT's bite |

So a wall is bash damage that armour blunts and Stone Skin turns; only
`Burst` and `Tick` skip soak, because plate does not help against a flame
or against poison already in the blood. No caller sets flags by hand any
more — picking the preset that describes what happened is the whole API.

The **fire shield's reprisal** was the last thing bypassing mitigation: it
went straight to `Wound` with its raw magnitude, so a fire-immune monster
took a full scorching. It is a `Burst` now — resisted by the target's fire
resist, silent when they are immune, and reporting what it actually dealt
rather than the ward's magnitude.

One presentation consequence: members no longer take the SAME amount from
a collision, so the bump line reports the worst of them and says nothing
at all when the jar rounds to zero — the log speaks in whole points, and a
wall the party shrugged off is not news.

*Verified in game, one continuous run:* an unarmoured party bumps a wall
and is "jarred for 2 damage"; with Stone Skin up, the same wall still
reports "You bump into a wall" and no damage line, health untouched.

---

## Status — all six phases landed

What began as "the fire sword needs somewhere to put a burn" is now one
pipeline: every source of damage builds a `DamageEvent`, one `ITarget`
serves members and monsters alike, effects are classes with catalog-tuned
numbers, content names them by id, they survive a save, and they are
editable in the editor.

Left undone, deliberately:
- The **wind ward deflecting a real bolt** has never been observed live —
  the test level's one caster never fires before the swarms close. It
  wants a scratch level with a single ranged monster.
- **Party poison and bleed are now resisted** (P3's per-tick rule), which
  they never were. Nothing in the current content has enough earth or
  pierce resistance for it to show, but it is a live balance change.
- The **modifier hook** (`StatBonus`/`SpeedScale`) ships unused, waiting
  for the first slow or blessing.

## Decisions (Michael, 2026-07-24 — SETTLED)

1. **Resist resolves PER TICK.** A stored `Inst` holds the RAW magnitude
   and stage 3 runs on every tick, so a ward cast *while* burning starts
   helping immediately. (Supersedes the fire sword's scale-once-at-
   ignition rule, which P3 removes.) It is the whole point of everything
   feeding through the same knobs, and it costs nothing.
2. **Full symmetry.** Monsters carry the same list as characters — wards,
   buffs, slows, DoTs. Same code either way, and it makes a monster that
   shields its friends a catalog entry rather than a feature.
3. **Stacking is a per-kind policy, defaulting to Refresh** (today's rule
   for both wards and DoTs). A second burn from a different member
   refreshes; a kind that wants true stacking opts in.
4. **The modifier hook ships in P1, unused.** `ModifyStat`/`ModifySpeed`
   goes in the interface with no kind implementing it — retrofitting a
   query hook once read sites exist means touching all of them.
5. **A fresh branch off a merged main**, with its own worktree.
   `editor-updates` (type authoring + polish + the fire sword) merges and
   pushes first.

## Conventions this must keep

- CMakeLists lists Game sources BY HAND — new .cpp files must be added.
- New user-facing strings: `en.lang` first, then the other four.
- New dynamic state rounds through `CaptureState`/`ApplyState` + `SaveData`.
- Steady-state frames allocate nothing: the effect lists are `push_back`
  on a rare event, never per frame, and the tick must not build scratch
  containers.
- Effects reach the world only through `ITarget` — no `DungeonWorld` in
  the module, the `MagicSystem`/`ai::` rule.
