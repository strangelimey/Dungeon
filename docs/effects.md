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
    float magnitude, power, timeLeft, duration;
    int source;                       // who applied it (threat credit)
};
```

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

**P1 — the module, with today's behaviour.**
`Game/Effect.h/.cpp` (namespace `fx`), the kind registry loaded from
`effects.cat`, `Inst` replacing `StatusEffect`, and `Character::effects`
ported to it. Ward/poison/bleed/sight behaviour stays exactly where it
is — this phase only changes what the list holds. HUD + sheet + save read
the new type. *Verify: nothing changed.*

**P2 — the pipeline, and the four wards move home.**
`ITarget` + the two adapters, `DamageEvent`, the six stages. Every damage
site — party melee, monster melee, both projectile resolvers, the bump,
the DoT ticks, the fire-shield retaliation — becomes "build the event,
hand it over". The four hard-coded ward sites become `OnIncoming` /
`ResistFor` / `OnStruck` overrides and the originals are deleted. This is
the phase that pays for the whole plan, and the one to feel-test hardest:
every ward, an overkill death, a party wipe, a monster slain by each of
melee/bolt/burn/retaliation.

**P3 — monsters get the list.**
`Monster::effects` replaces the burn slot; `BurnEffect` becomes a kind.
Poison/bleed become applicable to monsters for free. The plume + glow
become presentation *of an effect* (`effects.cat` fields: `plume`,
`tint`, `icon`) rather than fields on `Monster`.

**P4 — content authors effects by id.**
Weapons: `on_hit = <effect id> <numbers>` (superseding `element_dot`,
which stays parsed as a deprecated alias for one release). Spells:
`WardSpell`/`BoltSpell` apply effects through the system instead of
pushing onto `caster.effects` directly. Monsters: `poison`/`bleed`
become the same `on_hit` list.

**P5 — save.**
Effect lines become id-keyed (old `ward`/`poison`/`bleed`/`sight` tokens
map forward on load); monster effects round-trip in `EntityState`.
Version bump, `CaptureState`/`ApplyState` + `SaveData` as always.

**P6 — the editor.**
`effects.cat` gets a `CatalogSchema` entry and a palette/dialog like
every other catalog, so effects are tunable live next to Balance.

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
