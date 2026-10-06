# code-review - findings

A full review of the codebase as of main d62f5e66 (2026-10-05), against
Michael's brief in docs/code-review-notes.md. This report is the readable
part: a summary, one section per item in the brief, the order of work and the
questions only Michael can answer. Every issue it names by C-number is written
up in full in docs/code-review-issues.md.

## Summary

### How it was done

- **Scope.** All of src/ (117k lines of C++), the six shaders, the C++ tools
  (AssetBaker, RollTest, DiagTest, Bc7Test, ThreadStress, AnimTest) and, in a
  second round, the Python and PowerShell harnesses and authoring scripts.
  Nothing was edited, built or run.
- **66 review tasks.** 27 followed a theme from the brief across the whole
  codebase (the hit pipeline six ways, AI, dialogs, structure, graphics,
  globals, allocation, redundancy). 29 swept a slice each, so every source
  file was read at least once. After those, a completeness critic named what
  was still thin, and 10 more tasks covered it: training per hit, line of
  sight and blocking, allocation growth, world-switch lifetimes, GPU objects
  replaced in flight, data the editor can write but the loader aborts on, the
  reset paths, the script harnesses, script/C++ mirrors and the display
  lifecycle.
- **Every finding was checked.** An independent verifier re-read the code for
  each of the 919 findings, and each high-severity one also got a skeptic told
  to refute it. 4 were refuted and 309 were narrowed or corrected; the
  corrected version is what this report says. Three headline bugs were also
  re-checked by hand (C0, C1 and the facing note in C473).
- **Merged into 483 distinct issues**, each one that a single fix would close:
  41 high, 203 medium, 239 low. 184 of them are bugs in today's behaviour, 36
  break a stated project rule, 58 are structural and 205 are cleanup.

### The verdict

The foundations hold. The nine libraries still depend on each other one way
only. Globals are disciplined: 64 mutable ones, 52 of them justified (Core's
allocation tracker, crash path, logs and registries must have no owner).
Steady-state frames really are allocation-free in the hot loops. And the core
of the effect pipeline, fx::Deal plus the three ITarget adapters and the
ledger, is sound.

What has not held is one pattern, and it accounts for most of the 184 bugs: a
rule written down in several places, with the copies drifting apart. It shows
up everywhere the review looked:

- the steps around fx::Deal, rebuilt at about 20 call sites;
- seven reset paths that each keep their own list of what to clear;
- the editor's live and remote editing twins;
- the catalog defaults, which the schema, the loader and the threat rating
  each state with different values;
- 19 separate statements of what blocks a square;
- three ways to turn a catalog kind into draw parts;
- about 20 hand-kept C++/HLSL mirrors;
- the list of dialogs, kept by hand in five places in Game.

The two god classes, DungeonWorld (a 4,761-line header, about 620 methods,
included by 54 files) and Game (155 dev commands, 20 dialogs, a 735-line
UpdateStates), are where those copies collect. So the structural work and the
bug fixing are the same job: give each rule one home, and the drift stops.

### Against the brief

- **One hit pipeline (VERY IMPORTANT).** Half there. The dice, soak, resist,
  absorption and wards are in one place, and every damage source does reach
  fx::Deal. But the attacker's side of a hit (skills, stats, stance, potency,
  the enchantment rider) is assembled by four separate builders. A hit's
  consequences (procs, crit and fumble handling, training, React) are
  hand-written at about ten sites. And "water puts out fire" is not in the
  pipeline at all: element rules live in six places in world code, each
  covering one side and one source. The bolt and ward spells are already
  "create a projectile / create an effect"; the hand and light spells are not
  (12 bespoke CastServices verbs). Section 1.
- **AI behaviours as classes.** Not started. Behaviour is one enum switched
  across two threads and two files. The enum also mixes three axes
  (perception, engagement, per-instance modifiers), so the right base class
  is one per ENGAGEMENT behaviour, with perception as data and flee, leash
  and patrol as shared modifiers. Section 2.
- **Dialogs: generic chrome, one base.** The inside of each card is generic
  already (BuildDialogChrome). The shell around it is copied about 16 times,
  and only the seven instance inspectors share a base. Game has no modal
  stack, and its hand-kept lists have drifted into real bugs. Section 3.
- **Does the structure still make sense.** Between libraries, yes. Inside
  Game (83% of the code, one flat target), no: two god classes, and contracts
  in CLAUDE.md and the headers that no longer describe them. The split into
  rules / world / HUD / editor / app libraries is feasible and mostly
  mechanical. Section 4.
- **Graphics pipeline.** The library is in good shape; the redundancy is at
  its seams: two PBR owners, three kind-to-parts builders, the offscreen
  recipe written five times, four upload arenas, and about 20 unchecked
  shader mirrors. Section 5.
- **Globals, allocation, redundancy.** Twelve unnecessary globals, one of
  them a real clipping bug (section 6). Allocation is guarded where AllocTest
  drives the game, but its warm-up hides a whole class of first-instance and
  high-water allocations, and CheckAll runs only two of its modes (section
  7). Redundancy is mostly small utilities with no home, and the copies have
  drifted (section 8).

### Bugs worth knowing about now

The full list is in the issues file. These are the ones a player or a
session could hit today:

- **C0** Armour heals: a rolled blow weaker than the target's soak goes
  negative, and Deal hands it to Absorb.
- **C1** A skel_magus burst bolt never explodes when it hits the party.
- **C2** Monster ranged and caster attacks, and their blasts, drop strength,
  stance, crit-pierce and potency.
- **C292** A lingering gas cloud, monster DoTs and smashed props survive a
  new game, a load and a level change.
- **C311 / C326** Type saves, renames and undo rebuild the level from stale
  records, and a level save writes monsters where they have wandered to.
- **C301** An item imported through the editor makes a world that aborts on
  the next load (hard-coded model extensions).
- **C232** Saving a monster type while its inspector is open draws freed
  meshes (use-after-free).
- **C53 / C58** Monsters see and shoot through shut doors, and formation
  sends them to squares they cannot enter.
- **C178** Shadows go stale after a door opens, an item is lifted or a
  monster dies.
- **C194** Alt+Tab out of exclusive fullscreen aborts the game.
- **C383** Typing an accented or Cyrillic letter produces garbage, and one
  of them acts as Enter.
- **C417** Bc7Test's baseline gate never fires, and RollTest, EditorTest and
  WorldTest are not run by CheckAll at all.

### How to read the rest

Sections 1-5 follow the brief's big-picture items, and each ends with the
issues that matter most. Sections 6-8 are the low-level items. Sections 9-13
are where the review found further problems the brief did not name: data and
editor integrity (the most bugs of any area), the UI library, Core and
platform, the harnesses, and documentation drift. The order of work and the
open questions close the report.

## 1. The one hit pipeline (brief C - VERY IMPORTANT)

### Where things stand

The core is sound and worth protecting. `fx::Deal` (src/Game/Effect/Effect.cpp:198) is the only way health changes. It runs deflect, strike, mitigate, absorb and apply. `React` is split out so a reprisal reads after the blow it answers. Three adapters (PartyTarget, MonsterTarget, BreakableTarget) are the only health writers under ledger `Reason::Pipeline`, and the ledger checks that at runtime. The presets (Blow/Bolt/Impact/Burst/Tick, src/Game/Effect/Effect.h:225-231) make a caller name what happened instead of setting flags.

Other things that already work:
- Each ward is an effect class that overrides one stage hook.
- Resist past 1 heals the target.
- A DoT never lands on a target immune to it.
- The pure layer underneath (Roll, `ResolveAttack`, Defense, Mishap, Blast) is small, holds no state, and is linked into RollTest.
- Every flying thing is one `ProjectileSpec` through one `Launch`.
- The bolt and ward spells already have the shape you asked for: one `spawnBolt` or one `applyEffect`.

The problem is everything on either side of `Deal`. `fx::Deal` is called from 22 places, 15 of them in DungeonWorld_Combat.cpp. Each one rebuilds two halves by hand:
- **The attacker half:** stance, stat and skill curves, strength, potency and the enchantment rider. There are four separate builders: PartyAttackProfile, `MagicSystem::Cast`, `MonsterAttack`, and the monster shot/bolt path.
- **The consequences:** on_hit and on_crit procs, fumble tables, React, flinch, provoke, training and the slain line.

Those copies have drifted apart, and most of the bugs below come from that.

The core itself also has two copies of one sum:
- The unrolled branch clamps soak at zero (src/Game/Effect/Effect.cpp:234).
- `ResolveAttack` does not (src/Game/Combat.cpp:109-111). A rolled blow weaker than the armour goes negative, `Deal` sends it to `Absorb`, and the armour heals the target (C0).

Element rules live in six places outside fx:
- ignition in `MonsterTarget::Wound` (src/Game/DungeonWorld_Combat.cpp:1214);
- the quench in `QuenchParty`, which erases effects by the presentation flag `Plume` (src/Game/DungeonWorld_SpellLight.cpp:223);
- dousing and lighting inside the Splash and Flame classes;
- `KindleNear`;
- Gust's repel inside `ProjectileSystem::Repel` (src/Game/Projectiles.h:347).

Effect classes have only the four damage hooks (src/Game/Effect/Effect.h:322-333). The `StatBonus`/`SpeedScale` hooks at :339 are never called. So the DoT bite, light, sight and dazzle are school switches inside DungeonWorld (src/Game/DungeonWorld.h:3601).

### Against the brief

What already matches:
- The dice, soak, resist, immunity, absorption and wards are in one place.
- Every damage source reaches `Deal`. The one exception, the health cost of over-exertion, is declared in CLAUDE.md and should stay.
- Bolt and ward spells are pure "make a projectile" or "make an effect".

What does not match:
- **The pipeline does not fold in skills, stats or potency.** The caller hands `Deal` a finished amount and bonus.
  - `Balance::Potent` says it is "THE one place" (src/Game/Balance.h:419), but it is called by hand at 8 sites, and several sources skip it.
  - `MagicSystem::Cast` keeps its own 0.35/0.10/0.01 constants (src/Game/Magic.cpp:64-80) alongside the swing's balance knobs.
- **"Water puts out fire" is not in the pipeline at all.** A waterbolt douses nothing, a firebolt lights no sconce, a burning monster cannot be put out, and a party member never catches fire.
  - CLAUDE.md calls `MonsterTarget::Wound` "the one seam" for ignition. That is a documented decision, but it covers one side and one element, which is exactly what the brief rules out.
- **Hand and light spells are not "projectile + effect".** They call 12 bespoke CastServices verbs (src/Game/Spell/Spell.h:83-130), such as `setFireAhead`, `shoveAhead`, `lightFlare` and `placeLightStone`, and each verb's behaviour lives in DungeonWorld.
- **Monsters cast through a second API** (`MonsterBolt`/`MonsterVolley`, src/Game/Spell/Spell.h:180-185), and the world writes the volley again itself.

Why this matters for what you plan next:
- Every new interaction (oil feeding fire, lightning in water) means editing about ten hit sites and several element sites by hand.
- Every new attack (an arrow, a polearm, a trap) means the same.
- An arrow would need a fourth projectile resolver.

The drift above shows what happens to those copies.

### Recommended direction

Keep `Deal` and the adapters as the ledger's foundation. Add one layer above them and grow the hooks below them.

```cpp
// fx/Strike.h - what an attacker brings; built once, allocation-free
struct Attack {
	DamageType type; float amount, attackBonus;
	const ResistTable* potency;   // applied inside Deal
	bool pierceOnCrit; int fumbleExtra;
	std::span<const Proc> onHit, onCrit;
	Rider rider;                  // enchantment: type + amount, may be empty
	Lesson lesson;                // skill id view, stats span, xp unit
	ITarget* attacker; int source;
	std::optional<SpellSymbol> flavour; const Vec3* tint;
};
StrikeOutcome Strike(const Attack&, Delivery, ITarget& target,
                     StrikeCtx&, INarrator&);
```

- **`Strike`** runs, in order: potency, `Deal`, the rider (skipped once `slew` is set), procs (one policy for downed targets), the element stage, `Learn`, then `React`.
  - `INarrator` is a small virtual interface that the call site implements. The documented rule that the caller writes the lines, in order, still holds, and no `std::function` is needed.
- **Two builders are the only places that read stance, strength, curves and potency:**
  - `MakePartyAttack(member, hand, verb)` for swings, throws and casts;
  - `MakeMonsterAttack(monster, kind)` for melee, shots and spells.
  - Magic's constants become Balance knobs.
- **`ITarget::Learn(const DamageEvent&, Role)`** does nothing on monsters and breakables, and runs `defense::LessonFrom` on a member.
- **An element stage:**
  - Relations in damagetypes.cat (`douses = burn, lit`, `ignites = flammable`, `fans = fire`), plus `EffectKind::OnElement(Inst&, DamageType, float, ITarget&)`. A burn then removes itself under water on any target.
  - World things become targets: one `FixtureTarget` that merges `Fire` and `FixtureBreak`, plus targets for held and floor items.
  - A zero-damage `DamageEvent::Touch(type, power)` lets Splash and Flame put an element on a square instead of calling bespoke verbs.
- **`OnTick`/`OnExpire` effect hooks** with a narrow `fx::IWorld`, so the DoT bite, light, sight and dazzle move into their own classes.
- **`ProjectileSpec` carries an `Attack` and a `Delivery`.**
  - One landing function, the same for both sides, calls `Strike`.
  - "An area carrier detonates" is written once.
  - Pending bolts and active blasts move into ProjectileSystem, with fixed capacity and one `Clear`, and a blast keeps its shooter.
- **CastServices shrinks** to spawn, applyEffect (with a target selector), touch, blast and message. Monsters cast through `Cast(ctx)`, with the side in the context.

The order, and what each step unlocks:

1. **Fix the live bugs that need no refactor.**
   - One `defense::Mitigate` for both branches, checked in RollTest (C0).
   - A dead guard in `MonsterTarget::Wound` (C5).
   - Reserve `m_activeBlasts` (C49).
   - Keep a dropped torch's charge (C10).
2. **Move potency into `DamageEvent` and apply it in `Deal`.** This deletes 8 hand copies, so monster blasts and throws stop diverging (C2, C3).
3. **Build the `Attack` builders, `Strike` and the shared landing.**
   - Convert one site at a time: party melee, monster melee, bolts, throws, blasts, scorch/crackle, then fumble self-hits.
   - PipelineTest and AllocTest -Melee/-Cast/-Throw/-Impact judge each step.
   - This fixes C1 and C4 and lets monsters cast through `Cast`.
4. **Add the Learn stage.** It fixes C11 and C40 and removes TrainDefense's allocating statics (C35).
5. **Add the element stage, world targets and Touch.**
   - "Water puts out fire" becomes one rule.
   - Hand spells become a puff plus a touch.
   - The flare becomes a zero-damage blast plus a dazzle effect, and gets the blast's walls for free (C6, C17).
6. **Add the OnTick/OnExpire hooks.** CastServices shrinks (C8, C24).
7. **Write one passability rule** for where a hit can land (C43, C44, C45). Use a pure `Game/Passage.h` grid, copied into the AI snapshot so the workers keep an immutable view.

The project's rules hold throughout:
- Health still moves only in the adapters.
- `Attack` is made of views, spans and inline arrays, so a hit allocates nothing.
- The arithmetic stays in pure TUs that RollTest links.
- Hits stay on the main thread, so the async AI is untouched.

### Issues that matter most

- **C0** - Armour heals: a rolled blow below soak goes negative and is absorbed.
- **C1** - skel_magus's burst bolt never bursts when it hits the party.
- **C2** - Monster ranged and caster shots, and their blasts, drop strength, stance, crit-pierce and potency.
- **C5** - No dead guard: an enchanted killing blow hits the corpse and counts the kill twice.
- **C43** - A blast stopped by a shut door or a one-square-thick wall spreads to both sides.
- **C49** - The first blast of a session allocates in a guarded frame, and AllocTest's warm-up hides it.
- **C10** - A severe fumble drops a torch without its charge and allocates in the swing frame.
- **C3** - Potency and the enchantment rider are applied by hand at each site, and the copies disagree.
- **C4** - The steps after `Deal` are hand-written at about 10 sites, and most of the bugs above come from them.
- **C6** - Element interactions live outside fx, one side and one source at a time.
- **C8** - Effect behaviour lives in DungeonWorld switches, so the effect classes only hold data.
- **C24** - CastServices keeps growing a list of world verbs that each serve one spell.

## 2. AI behaviours as classes (brief C)

### Where things stand

The threading underneath the AI is in good shape:

- The worker side is walled off behind `ai::IWorldView` (src/Game/MonsterAI.h:50).
- Workers read a flat POD `Snapshot` and plans are keyed by a stable `runtimeId`, so a dead or shifted monster never takes a neighbour's orders.
- Buffers are pooled, the BFS polls the stop token, the IQ buckets run at prime cadences, and lockstep reuses the same `ComputeBucket` path.
- The THINK/ACT split works as documented: a dim monster changes its mind late but still moves and swings at full speed.
- Melee (`MonsterAttack`) and shots (`ResolveMonsterProjectileHit`) both go through `fx::Deal`, so the ledger holds for monsters too.

Behaviour itself is not built the same way. It is one enum, `ai::Archetype { Brute, Skirmisher, Caster, Swarm, Lurker, Sentry }` (src/Game/MonsterAI.h:84). Only about five lines actually decide anything:

- lurker dormancy (src/Game/MonsterAI.cpp:122)
- the sentry cone (src/Game/MonsterAI.cpp:135)
- the kite choice (src/Game/MonsterAI.cpp:157)
- swarm's omnidirectional flag, set on the host while building the snapshot (src/Game/DungeonWorld.cpp:2152)
- the threat profile's ranged test (src/Game/DungeonWorld_Load.cpp:580)

The host then dispatches on `Intent::Mode`, not on archetype (src/Game/DungeonWorld.cpp:1313-1331):

- The brute executor is about 70 lines inline in `UpdateMonsters` (src/Game/DungeonWorld.cpp:1333-1399).
- Kite, flee, patrol and return are `DungeonWorld` members built on `GreedyStep` (src/Game/DungeonWorld_Combat.cpp:1637-1720).
- Caster versus skirmisher is decided by `monster.Spell().empty()`, not by archetype (src/Game/DungeonWorld_Combat.cpp:1761).

Around that sits copy-and-paste:

- four string tables plus an if-chain for the archetype names, none checked against the enum (C56)
- five copies of `Skirmisher || Caster`
- two near-identical behaviour forms (src/Game/EntityInspector.cpp:70, src/Game/MonsterConfigDialog.cpp:131)

Two documented features do not exist in practice:

- The lurker's "relentless" pursuit (src/Game/MonsterAI.h:79) is not implemented. Once aware, a lurker gives up exactly like a brute.
- Sentry, patrol and leash have no content and no eval coverage (C60).

Several real bugs sit in the shared machinery that every behaviour will use:

- Two line-of-sight functions disagree, and neither sees a shut door (C53).
- Formation hands out cells no monster can enter (C58).
- Stale plans re-latch awareness after a load (C52).
- The BFS allocates a `std::deque` per search, which allocates in guarded frames while resting (C62).

### Against the brief

What already matches: the worker/host seam, an interface-only world view, and catalog data (monsters.cat `archetype`, `spell`, `keeprange`, `fleebelow`) selecting behaviour. That is the spells/effects split of "data picks, C++ does".

What does not match:

- There is no base class, no registry and no `AllBehaviours.cpp`.
- A behaviour's thinking (src/Game/MonsterAI.cpp) and acting (src/Game/DungeonWorld.cpp, src/Game/DungeonWorld_Combat.cpp) live in different files, and both acting files are well past the 2000-line guideline.
- A new archetype means touching the enum, five tables, up to five predicates, `Think`, `BuildAISnapshot`, both editor forms and five lang files. A missed table indexes past its array on save.

The deeper problem: the enum mixes three independent axes.

- **Perception.** Swarm, lurker and sentry differ only here.
- **Engagement.** Melee chase versus keep-range-and-shoot.
- **Per-instance modifiers.** Flee, leash, patrol, asleep.

A literal one-class-per-archetype split would copy the brute executor four times and leave flee and leash still special-cased. A "thief" is exactly the case today's shape cannot express:

- it needs its own mode (approach, steal, run)
- a payload on the plan (the thing stolen)
- per-instance state that must survive a save, and C68 shows there is no seam for that

docs/ai.md Layer 1 (docs/ai.md:148) chose "enum plus an executor per mode". Your brief supersedes that, and I recommend saying so in the doc. Layer 1's perception parameters (`sightcone`, `hearing`, docs/ai.md:169-171) are still right and should be kept.

### Recommended direction

The target is the spells/effects pattern applied to the ENGAGEMENT axis only. Perception becomes data, the modifiers stay shared, and movement stays a set of shared host primitives that a behaviour calls.

```cpp
// src/Game/AI/Behaviour.h - pure, linked into RollTest
struct Decision { Intent intent; bool wantsPath; int goalX, goalZ; };
class Behaviour {
public:
  virtual std::string_view Token() const = 0;            // monsters.cat `behaviour =`
  virtual Traits GetTraits() const;                      // ranged, usesSpell, takesSide
  virtual std::span<const FieldSpec> Params() const;     // editor rows
  // Worker: const singleton, POD in, no allocation, same under lockstep.
  virtual Decision Decide(const Agent&, const IWorldView&) const = 0;
  // Main thread: default runs the shared executor for the mode.
  virtual void Act(MonsterRef, MonsterServices&) const;
  virtual void OnProvoked(MonsterRef, MonsterServices&) const; // default: latch aware only
};
std::vector<std::unique_ptr<Behaviour>> MakeAllBehaviours(); // AllBehaviours.cpp
```

`MonsterServices` is an interface that `DungeonWorld` implements once, the way it implements `fx::ITarget`. It offers FollowPath, KeepRange, FleeFrom, ReturnTo, Strike, Shoot, Announce and LineOfSight. Every move still goes through `StepMonsterTo`/`FreeSlotInCell`, and every attack through `MonsterAttack`/`Launch`, so tracks, occupancy and the ledger stay atomic. `Agent` carries a behaviour index plus a fixed parameter block: no strings, no `MonsterKind*`.

The steps, in order. Each is gated on the eval suites, `AllocTest -Melee` and PipelineTest.

1. **Fix the shared ground first** (no change of shape).
   - One pure line-of-sight rule over a door-aware sight grid (C53).
   - One "may a monster stand here" predicate (C58).
   - A plan epoch, so a stale batch or an in-flight think cannot override a load or a provoke (C52, C54).
   - A member scratch ring for the BFS and reserved pools (C62, C66, C71).
   - *Unlocks:* every future behaviour inherits correct primitives instead of copying bugs.
2. **One archetype table** in src/Game/MonsterAI.h, static_asserted against the enum.
   - Holds token, loc key, kites and usesSpell. Parse, write and the five predicates read it (C56).
   - *Unlocks:* this becomes the registry's `Traits()` later.
3. **Perception as data** on `Agent` (`sightcone`, `wake`, later `hearing`).
   - Move swarm's flag out of `BuildAISnapshot`.
   - Implement or delete the lurker's relentlessness.
   - *Unlocks:* swarm, lurker and sentry become catalog presets of Melee rather than classes.
4. **The worker half.**
   - `Brain::Think` runs the modifiers (leash, dormancy, perception, flee), then `behaviour->Decide`.
   - The BFS runs for any decision with `wantsPath`, so return and patrol finally get real paths (C60).
   - Ship Melee and Ranged (spell as a parameter).
   - *Unlocks:* `Decide` is testable in RollTest against a fake `IWorldView`.
5. **The host half.**
   - Lift the Engage block out of `UpdateMonsters` into shared primitives behind `MonsterServices`.
   - Add `OnProvoked`.
   - Formation asks `Traits().takesSide` (C57).
   - Move the monster AI into a new `DungeonWorld_Monsters.cpp`.
   - Keep kite/flee execution on the main thread: that is acting, as documented.
   - *Unlocks:* a behaviour that acts differently.
6. **Data, editor and save.**
   - Behaviour params become `FieldSpec` rows, with one form for the type editor and the instance inspector (C56). This reverses CLAUDE.md's "MonsterConfigDialog owns the archetype rows". That rule made sense with one editor; with two copies it no longer holds.
   - `behaviour =` with `archetype` kept as an alias, the way `poison`/`bleed` were.
   - A fixed per-monster state block on a save line (C68).
7. **The first new behaviour (Thief)** as the proof.
   - File pair + `AllBehaviours.cpp` + CMakeLists + lang keys x5 + an eval suite.
   - Eval suites for sentry, patrol and leash alongside it.

### Issues that matter most

- **C52** - stale worker plans re-latch `aware` after a new game or load, for the rest of that game.
- **C53** - two line-of-sight functions disagree, and both see through a shut door, so monsters notice and shoot through it.
- **C58** - formation assigns attack cells beside a brazier, a statue or a shut door, and the monster stalls there.
- **C62** - the BFS allocates a deque per search, on workers and in armed frames while resting.
- **C69** - dead worker slots capture a destroyed director, so a reboot is a use-after-free. The trigger is narrow: only a switch between worlds.
- **C66 / C71** - the AI snapshot pools and the formation scratch grow lazily inside guarded frames.
- **C54** - provoke forces Engage on kiters and fleers. This is a short stall until the next think, not a charge; the fix is the epoch plus `OnProvoked`.
- **C57** - kiters and fleers take melee formation sides.
- **C60** - leash-return and patrol stall behind walls (greedy steps), and none of the three is tested.
- **C55** - the core structural gap against the brief: an enum switched across two threads, with no class per behaviour.
- **C56** - five name tables and two copies of the behaviour form, unchecked against the enum.
- **C68** - no save seam for per-behaviour state, which a thief will need.

## 3. Dialogs and inspectors (brief B)

### Where things stand

The inside of each card is in good shape. `BuildDialogChrome` (src/Game/DialogLayout.h:102) builds every modal's title band, close slot, body and footer as one Stack. Most of the 22 modal classes use it, along with `FooterIcon`, `RowIcon`, `TabStack`, `PreviewPane` and `BuildTypedConfirm`. Rows say how tall they are and never where they sit, so the old title-over-first-row overlap cannot come back. The two in-game dialogs (`ItemDetailsDialog`, `PortraitPicker`) are built once with reserved strings, so opening item details from a guarded sheet frame stays allocation-free. Every preview hands out its rect from a `PreviewPane` widget, so the backing and the 3D blit cannot disagree. `InstanceInspector` (src/Game/InstanceInspector.h:75) already works the way Michael wants: seven subclasses provide `Title`/`Panel`/`BuildContent`/`ApplyLive`/`Persist`/`Revert`, and the base owns everything else. Game.h:1215-1221 states the right rule for them: one array, so Update, Render and Active "cannot disagree".

Everything around the card is still written per dialog:

- **The shell is copied about 16 times.** The constructor (`UIContext(fonts, Body, 18)`, root `kDialogTextScale`, `CloseIcon`), the open flag, the font clamp `std::clamp(h * 0.020f, 12.0f, 24.0f)`, the Esc branch, the deferred rebuild (under two names) and the Render backing (dim + `th.panel` + `DrawBorder`). Compare src/Game/BalanceDialog.cpp:55-59 and :236-271 with src/Game/InstanceInspector.cpp:58-64 and :176-208.
- **The cancel path is written twice per dialog**, once in the close-box lambda and once in the Esc branch.
- **Help overlay, busy overlay, typed delete, inline rename, numeric field and id filter** each exist in two to twelve copies.
- **Game has no modal stack.** Every dialog is wired into 3-5 hand-kept lists: the WorldMap update chain (src/Game/Game.cpp:2189), the Playing chain (:2272-2395), the Render list (:2906-2963), the 19-line close list in UnloadWorld (:583-608) and the preview arbitration (:2680-2757). Two hacks hold up the stacked cases: `m_typeOverBalance` (:2299-2311) and `generate && !validate` (:2325).
- **There are five preview contracts.** The monster dialog's preview state even lives in Game (Game.h:1205-1208).
- **Two laggards.** `AssetDialog` still uses the legacy owned-font `UIContext` (Consolas at 18 px, no dialog scales) and ignores Esc. And no editor dialog's widgets get the user Theme, only their hand-drawn panel does (C90).

### Against the brief

Michael asked for two things: chrome and footer built generically, with each dialog supplying only its main content; and editor dialogs and inspectors sharing ONE base class.

**What already matches.** The title, close box and footer slots are generic. The inspectors meet the brief among themselves.

**What does not match:**

- **Only 7 of 22 modal classes share a base.** `ProjectileInspector` opted out (C97) because `InstanceInspector` mixes the generic modal shell with instance-only parts: the Facing strip, Save/Delete and `PreviewSpec`. That same mix is why Door and Button return `{}` for facing.
- **Footer placement is per dialog.** Buttons sit left, centred or right, and "?" lands in three places. CLAUDE.md says "right-aligned". The inspectors left-align on purpose ("so the preview pane keeps its side", InstanceInspector.cpp:156), so one rule needs choosing. That is Michael's call; the base should then enforce whichever rule he picks.
- **The game/editor split is a skin question, not a class question.** `ui::DrawPanelFace` (src/UI/Controls.cpp:26) already draws stone when the context has a skin, and exactly `th.panel` + border when it does not. So ItemDetails and PortraitPicker can share the editor base. Their real differences become flags: built once, mouse-only modality, carved title, opaque backing.

**Why it matters for what comes next.** Every new dialog (quest tools, sound, anything the editor grows) currently costs a class plus edits to 3-5 lists in Game, each placed in the right order. The bugs below show what happens when one of those lists is missed:

- C77/C79: the Worlds dialogs are drawn but never routed.
- C78: the console path runs the world through the editor pause.
- C81: Esc closes a whole dialog while one of its drop-downs is open.

All of these are one rule written per path instead of once.

### Recommended direction

**Target shape.** A new src/Game/ModalDialog.h/.cpp beside DialogLayout:

```cpp
class ModalDialog {
public:
  bool IsOpen() const; void Close();          // calls OnClosed()
  bool Update(const Input&, float w, float h, float dt); // true = took input
  void Render(gfx::SpriteBatch&, float w, float h);
  void SetTheme(const ui::Theme&); void SetBusy(bool);
  virtual bool Preview(PreviewView&) const { return false; }
protected:
  struct Shape { gfx::Rect panel; bool footer = true, closeBox = true,
    buildOnce = false, mouseOnly = false, skinned = false; float dim = 0.6f; };
  virtual Shape GetShape() const = 0;
  virtual std::string Title() const;           // or BuildTitle(Box&) for rename titles
  virtual void BuildBody(ui::Stack& body) = 0; // the dialog's only real job
  virtual void BuildFooter(Footer&) {}         // f.Action(icon,name,fn), f.Help(paras)
  virtual void Cancel() { Close(); }           // close box AND Esc, one path
  virtual bool OnEscape() { return false; }    // back out one layer first
  void OpenModal(ModalHost&); void RequestRebuild();
};
template <class Cfg> class EditDialog : public ModalDialog { /* m_cfg, m_original,
  onApply/onSave; Cancel = apply original; Save = apply + persist */ };
```

**What the base owns:**

- The context, font sizing, open flag and one rebuild flag.
- The Esc ladder: `PopupOpen()` first, then `OnEscape`, then `Cancel`.
- The backing, drawn through `DrawPanelFace` with the pushed Theme. It stays outside the tree, as DialogLayout.h:22 intends.
- The help and busy overlays.
- A `Footer` builder that applies one placement rule.

**A modal stack.** Game also needs a `ModalStack`: fixed capacity (`std::array<ModalDialog*, 8>` + count), so a push from a guarded right-click allocates nothing. Its operations:

- `Update` - the top dialog takes input, in every AppState.
- `Render` - bottom to top.
- `CloseAll` - for UnloadWorld.
- `TopPreview` - which dialog's preview Game renders.
- `MouseConsumed` - also read by GameUI.

Open order gives stacking for free: the type editor opened from Balance is on top, and an `onClosed` callback replaces `m_typeOverBalance`.

**Order of work:**

1. **Fix C101 now.** It is one missing `case`, independent of everything else.
2. **Add `ModalDialog` and rebase `InstanceInspector` on it.** The Facing strip, Delete and the preview column stay in the subclass. Seven classes move with no behaviour change, which proves the shape. `ProjectileInspector` rejoins at the `ModalDialog` level.
3. **Port the editor dialogs one per commit**, simplest first: Validate, InspectPicker, Level, Balance, WorldSettings, Generate, NewWorld, Worlds, Monster, Type, AssetPicker. `AssetDialog` goes last and leaves the legacy font path (C89), which lets `Font::SetHeight`'s second path go. This step fixes C81 and C90 everywhere at once.
4. **Add `ModalStack` and delete the chains.** This removes the hand lists and console-side state gates, and fixes C77/C79 by construction. It also gives C78 one place to decide what freezes the world. After this, a new dialog is a class plus one constructor line.
5. **Move the game dialogs onto the base.** `ItemDetailsDialog` and `PortraitPicker` use `buildOnce + mouseOnly + skinned`. A `ConfirmDialog` (no close box, per the Yes/No exemption) replaces GameUI's window-fraction prompt and SlotList's hand-drawn confirm (C91). Re-run `AllocTest.ps1 -Sheet` here, since ItemDetails opens in a guarded frame.
6. **Shared field helpers** in src/Game/DialogFields.h:
   - `NumericField`, and an id predicate on `TextField` (C85);
   - `RenameTitle` (C88);
   - the stale-value drop-down, generalising `FlagDropDown` (C103);
   - one word-wrap (C86).
7. **One preview contract.** A `PreviewView` of spans (parts, palette, fit, pivot, yaw, tilt, clip), so ItemDetails stays allocation-free. Game's preview state for the monster dialog moves into it. Then delete the dead UI API (C92) and correct CLAUDE.md, which still describes `AddCloseButton(ctx, panelRect)`, `DialogTitleBand`, `FitDialogTitle` and `kDialogTitleBandH`.

**Kept out of the base:** full-screen pages (PageCard, Back is navigation) and the non-modal floating windows (sheet, party window). They should still share one close-slot rule and one status-line helper (C95).

**Checks after each step:** `uioverlap` over every dialog, EditorTest, and `/check-alloc` once step 5 lands.

### Issues that matter most

- **C101** The type editor builds no control for `FieldKind::DamageType`, so a monster's `dmgtype` and an effect's `damage_type` cannot be edited.
- **C77** The Worlds and World-settings dialogs can open in Playing but only get input in WorldMap, so they sit on screen and cannot be reached.
- **C79** WorldMapView keeps Editor mode as the Playing overlay page, so terrain paint there skips the stroke commit and escapes undo.
- **C78** With the console open the world runs through the editor pause and any open modal, and every `editor ...` subcommand clears the pause.
- **C81** Esc with a drop-down or colour picker open reverts and closes the whole dialog. AssetDialog ignores Esc entirely.
- **C80** Finishing a patrol route reopens the inspector from a shared `m_inspectCfg`, which may belong to another monster.
- **C100** Renaming a quest stage to another stage's id wipes that stage's log line.
- **C102** WorldSettingsDialog's single note label ends up on the wrong tab when a doorway is selected.
- **C89** AssetDialog is the last user of the legacy owned-font context, so it draws in fixed Consolas and keeps a second font path alive.
- **C91** The Yes/No confirm and InspectPicker are placed by window fractions, and their bodies overrun. SlotList carries a second hand-built confirm.
- **C75** There is no common modal base, so the shell is copied about 15 times. This is the root of C81, C90 and the drift in footer placement.
- **C76** There is no modal stack, so routing lives in hand-ordered lists that already disagree. This is the root of C77, C79 and the `m_typeOverBalance` hack.

## 4. Structure as it has grown (brief B)

### Where things stand

**The outer architecture holds.** The nine libraries still depend on each other in one direction only. Every include is module-qualified, `src/` is the only include root, and no engine module includes anything from `Game/`. Core, Platform, Assets, Audio and UI contain no game concepts. The drift is small: UI reads `Assets/File.h`, and Main includes Graphics and Platform directly because it is the composition root.

**The inside of Game is better than its flat target suggests.**
- The rules files (combat, effects, spells, character, map, generator, validation, AI) are nearly pure. There are exactly three leaks: `src/Game/Character.cpp:5` includes GameSettings (and through it UI), `Party.h` includes `Platform/Input`, and `src/Game/Projectiles.h:35` includes `Graphics/ParticleBatch.h`.
- No HUD file includes `DungeonWorld.h`, and no dialog or inspector does. HUD and World are already siblings.
- The pure-TU pattern works: RollTest links the real rules TUs (`tools/RollTest/CMakeLists.txt:47`).
- `InstanceInspectors()` (`src/Game/Game.h:1221`) and `DungeonWorld::m_harness` show the right fix for parallel lists and loose state. They have just not been applied further.

**What has not held is the two classes in the middle.**

- **Game is 83% of all C++ (about 97k lines) in one flat target** (`src/Game/CMakeLists.txt:1`). The only build-enforced wall inside it is RollTest's hand-written source list.
- **DungeonWorld** has a 4,761-line header, about 620 member functions (about 344 public, half of them editor seams) and 26 .cpp files. Its comment calls it "the simulation of one level" (`src/Game/DungeonWorld.h:1531`). In practice it is also:
  - the whole project's level store;
  - the editor backend;
  - the renderer and every icon bake;
  - the asset picker's tile framing (`src/Game/DungeonWorld.h:218`, `:405`).

  Its header reaches 54 TUs. `MapEditor.h:22` includes it for one nested type. It was touched by 116 of the 386 commits since September 1, and every one of those recompiled all 54 TUs.
- **Game** calls itself "the thin coordinator" (`src/Game/Game.h:4`). It actually holds 20 dialogs and pickers, the overworld, party creation, catalog create/rename/delete, and 155 dev commands. The dev commands alone are 6,237 lines, 43% of the class. `UpdateStates` (`src/Game/Game.cpp:1911`) is 735 lines.

**The structure is already producing defects.**
- The modal stack is written out by hand in about six places: Playing input, World Map input, the draw order, the preview choice, UnloadWorld, and GameUI's own chains. Input order and draw order have drifted apart.
- The "tick world, check transition, refresh HUD" tail is copied four times, and the copies differ.
- Hand lists have fallen behind the schema. One example: `SweepCatalogRefs` never sweeps `light`, `trail` or the door parts.
- DungeonWorld's read paths stash levels just to read them.
- The throw and the blow copy the post-`fx::Deal` steps, and the copies have drifted apart.

### Against the brief

Michael asked whether it all still makes sense as it has grown.

**What still matches:**
- the layered engine;
- the pure rules TUs;
- the decisions CLAUDE.md records (harness code not behind `#ifdef`, the world tier above DungeonWorld, the bounds/rem UI model). Nothing here argues against them.

**What does not match** is the contract each big class claims for itself. CLAUDE.md says "Game.cpp is just the app state machine + wiring", and DungeonWorld says it simulates one level. Neither is true. These descriptions are the handoff to the next session, so they now mislead it. Several Game.h and GameUI.h comments have drifted the same way. For example, GameUI.h says "five UIContexts" when there are seven.

**Why it matters for what is coming:**
- **AI behaviours as classes.** Kiter, Fleer, Returner and Patroller are DungeonWorld member functions inside `DungeonWorld_Combat.cpp` (C123). There is no seam to put one class per behaviour behind.
- **More spells and effects.** Element interactions ("water puts out fire") live in world callbacks, not in fx, so each new one grows DungeonWorld.
- **More dialogs and editor tools.** Each new dialog is another hand edit in six places. Each new editor feature adds public methods to a header that recompiles 54 TUs.
- **Parallel worktrees.** Sessions collide in `DungeonWorld.h` more than anywhere else.

### Recommended direction

**Target shape.** Split Game into six libraries:

- **GameRules**: Combat, Defense, Roll, Curve, Resource, Balance, `Effect/*`, `Spell/*`, Character, Inventory, Party, DungeonMap, Entities, WorldMap, Generate, Carve, Area, Style, Validate, SaveGame, MonsterAI, Projectiles, LightProfile, Trail, DamageLedger. Depends on Core and Assets only. RollTest and ThreadStress link it instead of listing TUs.
- **GameWorld**: the simulation and rendering of the active level, the mesh builder, fires, shadows, the model cache, the sound bank. Depends on GameRules and the engine modules.
- **GameHud**: GameUI, the sheet, panels, pickers, party creation. Depends on GameRules and UI, and not on GameWorld.
- **GameEditor**: MapView, MapEditor, AssetPicker, every dialog and inspector, CatalogSchema, and a WorldEditor class taken out of DungeonWorld. Depends on GameWorld and UI.
- **GameApp**: the state machine, wiring, settings, dev commands and the eval runner. Main sees it through a small facade, not the 1,305-line `Game.h`.
- **DevTools**: DevConsole and its panels, as an engine-level module.

Generic pieces move down:
- Catalog and Serialize go to Assets. FontLibrary can then read `fonts.cat` itself.
- The texture loaders and an IconBaker go to Graphics.
- DialogLayout and ThumbCache go to UI.
- LoadQueue goes to Core.

**Two interfaces carry most of the Game-side cleanup.**

```cpp
// GameApp: one ordered list drives input, draw, preview and unload
class ModalDialog {
public:
	virtual ~ModalDialog() = default;
	virtual bool IsOpen() const = 0;
	virtual bool Update(float dt, Input& in) = 0;   // true = consumed
	virtual void Render(gfx::SpriteBatch& b, const ui::Theme& t) = 0;
	virtual void Close() = 0;
	virtual bool WantsPreview() const { return false; }
};
std::span<ModalDialog* const> Game::ModalStack();  // topmost first

// GameHud: replaces the 24 pass-through std::functions
struct IWorldActions {
	virtual const std::vector<std::string>& ItemCommands(std::string_view id) const = 0;
	virtual bool Consume(size_t member, ItemPlace place) = 0;
	virtual bool Memorize(size_t member, ItemPlace place) = 0;
	// ...
};
```

A virtual function that returns `const&` cannot hit the `std::function` return-type deduction trap that `GameUI.h:431` warns about. That makes `IWorldActions` the allocation-safe shape as well as the tidy one.

**Order, and what each step unlocks:**

1. **Cut the three rules leaks and make GameRules a CMake target.** This is cheap, and it moves the purity wall from a test's file list into the build.
2. **Move DungeonWorld's nested types into `WorldTypes.h`.** MoveTarget and LevelBrowse get small headers of their own, and PoolModelLook gets its own TU. MapEditor, MapView and AssetPicker then stop including `DungeonWorld.h`, and most of the 54-TU recompile goes away.
3. **Add the ModalDialog base and one `Game::TickWorld(dt)` tail.** This fixes the drift bugs now, and a new dialog becomes one list entry.
4. **Pull three pieces out of DungeonWorld:**
   - a **LightSystem** (budget, fades, profiles);
   - a **LevelStore** with one read path that never stashes, so a read cannot dirty a level;
   - the undo history, moved up to Game as an **EditHistory**. That removes the raw `WorldMap`/opening pointers (`src/Game/DungeonWorld.h:466`).
5. **Create WorldEditor, the GameEditor library and DevTools.** Dev commands become per-group registrar functions instead of Game members.
6. **Do the large extractions as the features that need them arrive:**
   - a **CombatResolver** with one post-Deal sequence, needed for the balance pass;
   - a **MonsterDirector** with behaviour classes registered like AllSpells/AllEffects, needed for the AI work;
   - **PartyVitals** next to `m_characters`.
7. **Create the GameHud library and the Main facade.**

**Constraints this plan keeps:**
- **Allocation rule.** Moving code changes no allocation. New interfaces return views or `const&`.
- **Async AI.** Brain, IWorldView and the snapshot/plan handoff stay as they are. MonsterDirector stays the main-thread executor.
- **Ledger.** The ITarget adapters keep their `ledger::Explained` scopes, and the checkpoints stay in world Update.
- **Data plus classes.** Catalogs stay data, and behaviour stays in C++ classes.

### Issues that matter most

- **C125**: the four world-tick tails have already diverged. With the console open over the sheet, the world freezes, and the map overlay rewrites the level-transition step inline.
- **C127**: DropDown has its own scrollbar, against the documented rule that ScrollArea owns all scrolling.
- **C152**: the palette re-implements text field, checkbox, scroll and tooltip outside the control tree, so neither `uioverlap` nor input clipping sees them.
- **C136**: undo and stair moves write Game's WorldMap and the project opening through raw pointers. The world holds the project as `const Project&` and writes its opening fields anyway.
- **C140**: item-use rules (memorize, eat/drink, the throw clear) live in GameUI, so the harness's `consume` takes a different path from the player.
- **C144**: GameUI is a hub of 48 `std::function`s, one of which is a dangling-reference trap if a lambda's return type is left off.
- **C118**: one flat 331-file library means the one-way rule is unchecked for 83% of the code.
- **C119**: DungeonWorld is runtime, editor, renderer and asset picker in one class, and its header recompiles 54 TUs.
- **C124**: Game is a god class, and its hand-written modal stack is where the drift bugs come from.
- **C120**: private nested types force whole-header includes for a single type each.
- **C123**: the AI behaviours sit inside `DungeonWorld_Combat.cpp`, which blocks one class per behaviour.
- **C128, C129**: rules headers pull in UI and D3D12, and the world reaches up into settings and the HUD draw code. Both are cheap to cut and are step 1.

## 5. The graphics pipeline (brief D)

### Where things stand

Most of the Graphics library is in good shape. It is about 4,800 lines in 15 files, none over 1,000 lines, and it depends only on Core and Assets. These parts work:

- **No allocation in a normal frame.** The light candidates are reserved to 256, the fade table is fixed, the reach BFS reuses its buffers, the transparent queue is fixed at 256 with a "dropped" counter, and the particle scratch is reserved to the worst case.
- **Pure, tested units.** LightTiles is pure and RollTest checks it, and so is LightProfile. The shadow cube cache is keyed by light id with flicker pacing. The SRV free list has its live/peak gauge.
- **The constant blocks match.** I recomputed the FrameConstants (8,000 B) and ObjectConstants (160 B) layouts by hand, and both match the HLSL packing today.
- **Fast loading.** The BC7 sidecars and staged loading keep loads quick.
- **Resizing.** A resize you start yourself drains before releasing anything, and the WARP and adapter-by-LUID fallbacks degrade instead of crashing.

The problems are about structure, and several have already caused bugs:

- **Loading has many owners.** PBR sets are loaded by two owners that don't share textures: surface variants and `m_propTextures` (src/Game/DungeonWorld_Load.cpp:397, :176). Five rules decide "use the bound set or the model's own materials?" (:613, :628, plus the picker). The quality hot-swap reloads only the surfaces (:1726). Two related high/medium bugs were filed in the data section:
  - C301: item models get a hardcoded `.glb` extension, so an item imported through the editor aborts the next load.
  - C302: one kind cache keyed by bare id serves four catalogs, and `portcullis` is in both decorations.cat and doors.cat.
- **Each pipeline owner keeps its own upload arenas.** Renderer, SpriteBatch, the world ParticleBatch and a second ParticleBatch inside Game each hold their own fixed arenas and each need their own `NewFrame`. An overflow aborts at src/Graphics/UploadAllocator.cpp:18, and nothing shows how full they are.
- **Offscreen 3D is hand-rolled at five sites:** ModelPreview, Texture::RenderTarget, PostProcess, the shadow cubes, and DungeonWorld's own depth buffer, DSV heap and barriers. The last one puts raw D3D12 in the Game lib. Every call site has to remember to rebind the back buffer and flush the glass.
- **Turning a catalog kind into draw parts is done three times:** for the world draw, the icon bake and the inspector preview (src/Game/DungeonWorld_Render.cpp:395, :429). The copies have already drifted apart: the brazier icon has no coal bed, the door preview has no trim, and levers ignore `alpha_test`.
- **The C++/HLSL boundary is copied by hand and only three asserts check it.** ShaderCompiler passes no defines and no include handler (src/Graphics/ShaderCompiler.cpp:104). shadow.hlsl:25 still declares the pre-PBR `gSpecStrength/gSpecPower`.
- **GPU lifetime rests on about 22 hand-placed `WaitIdle` calls.** One site that needed a drain did not get one (C193).

### Against the brief

You asked for redundancy, repetition and stale code from load to HLSL.

**Redundancy is real at every stage:**
- Loading: two PBR owners, three model-preview loaders, two set-preview loaders, five material-source rules, and `glow_radial` loaded twice.
- Draw assembly: three kind-to-parts builders, three icon-camera copies (the comment claims there is one), and two studio light rigs, so an icon and its preview are lit differently.
- GPU plumbing: the offscreen recipe written five times, root-signature/sampler/PSO boilerplate written four times (the copies already disagree on DepthClipEnable), and four separate upload arenas.
- Shaders: AcesTonemap in scene.hlsl and post.hlsl, the skinning VS in scene and shadow, the falloff and the 0.012 bias written twice, and about 20 hand mirrors in all.

**Stale code is present but small:**
- shadow.hlsl's ObjectConstants.
- `gPointLightCount` and the directional light: uploaded but never used.
- `BarKind::Solid`, which has no caller.
- `Font::SetHeight`, now dead.
- Four unused accessors.
- Stale comments: "Blinn-Phong", "t7", "t3..t6", "two PSOs", "Always RGBA8", and LoadTextureFile's "aborts" (it returns magenta).

**Why this matters for what comes next.** Every planned addition crosses one of these seams. A placed Sowilo rune, more glass items, more bought rigs, new door or lever types, ranged weapons with trails, and a higher light ceiling each need either:

- a new kind family drawn in three places,
- a new offscreen view, or
- a new array size or constant crossing into HLSL.

Each of those is a copy that can drift without any error, and several already have.

CLAUDE.md records two decisions this review argues against:

- **"Mirrors the registers BY HAND"** (also for LIGHT_TILE_COUNT). Its own list of mirrors leaves out MAX_DUST_PUFFS, shadow.hlsl's copy, the bar kinds and the colours. GameSettings.h also claims a static_assert keeps kMaxPointLights in sync with the shader. The real assert only checks `<= 64`, so 48 would compile and silently shift `gTileGrid`.
- **"Drain the GPU before overwriting"** as the lifetime rule. It works, but each new call site has to remember it. C193 is a call site that forgot.

### Recommended direction

The steps are in order. Each one is small enough to land and check on its own.

**1. Fix the bugs first, with no restructuring.**
- C178: add a world `m_casterRevision`, bumped by:
  - door `openT` movement,
  - a lever toggle,
  - a floor item added, removed or lifted,
  - thrown cargo landing,
  - a monster dying or being erased,
  - a sconce emptying.

  `ShadowSlotCache` stores it beside the map revision.
- C181: null-check at src/Game/DungeonWorld.cpp:799.
- C193: a `WaitIdle` at the top of `BuildDungeonMeshes` (src/Game/DungeonWorld_Load.cpp:528).
- C189: key the palette cache on the animator object plus the frame, not on `palette.data()`.
- C194: on WM_ACTIVATEAPP and WM_SIZE, compare `GetFullscreenState` with the last known state and call `RecreateSwapChainBuffers` when it changes.
- C195: `DN_HR` keeps the HRESULT, calls `GetDeviceRemovedReason` and enables DRED in debug.
- C163: SpriteBatch stops accepting quads when the arena is full and counts the dropped ones, like the transparent queue does. MapView also culls off-screen cells.

**2. Make the shader boundary checked.**
- ShaderCompiler takes `std::span<const D3D_SHADER_MACRO>`, built from the C++ constants (`MAX_POINT_LIGHTS`, `MAX_SKIN_JOINTS`, `MAX_DUST_PUFFS`, `LIGHT_TILE_COUNT`, `SHADOW_SLOTS`), and folds them into the cache key.
- An `ID3DInclude` handler serves `assets/shaders/common.hlsli`: the tonemap, the skinning VS, falloff, bias and the shared cbuffers. Each included file's hash goes into the key.
- At startup, `D3DReflect` compares each cbuffer's offsets and size against `offsetof`/`sizeof` and asserts.
- The shadow pass gets its own `ShadowConstants { Mat4 viewProj; Vec4 light; }` instead of 8 KB per face (C169).

This step unlocks C170, C171, C172, C176, C177 and C203, and it makes raising the light ceiling safe.

**3. One frame arena and one offscreen scope, both in Graphics.**

```cpp
class FrameArena {            // one per frame slot, owned by GraphicsDevice
	UploadAllocation Allocate(u64 size, u64 align); // returns {} when full, counts drops
	u64 Live() const; u64 HighWater() const;        // console gauge beside SRV
};
class OffscreenTarget { OffscreenTarget(GraphicsDevice&, u32 w, u32 h, DXGI_FORMAT, bool depth); };
class OffscreenScope {        // RAII: barriers, viewport, clear on entry
	OffscreenScope(GraphicsDevice&, Renderer&, ID3D12GraphicsCommandList*, OffscreenTarget&);
	~OffscreenScope();        // FlushTransparent, barrier back, BindBackBuffer
};
```

- GraphicsDevice::BeginFrame resets the arena, which removes the four `NewFrame` calls.
- The scope replaces the three bake rigs, ModelPreview's own heaps, Texture::RenderTarget's private RTV heaps, and the raw D3D12 in DungeonWorld_Render.
- With that in place, Game::Render's preview chain (src/Game/Game.cpp:2680) can become one "who owns the preview target" decision, made once (C188).
- The torch preview's ParticleBatch can draw into the shared arena (C165).

This step unlocks C164, C165, C166, C168 and C188.

**4. One kind-to-parts function per family.**

```cpp
struct DrawPart { const gfx::Mesh* mesh; gfx::MaterialParams mat; Mat4 local; };
using DrawParts = FixedVector<DrawPart, 8>;          // no heap
void AppendParts(const DecorationKind&, const PartState&, DrawParts& out);
MaterialSource ResolveMaterialSource(const CatalogEntry*, const ModelData&); // the ONE rule
```

The world draw, the icon bake, the inspector preview and `LoadPoolModelLook` all call these, which fixes C179, C157, C182 and C183. The floor-item pose (C180) gets one resolver in the same way. This keeps CLAUDE.md's split: the catalogs stay data, and the C++ decides the behaviour.

**5. Texture cache keyed by (set, resolution, sRGB).**

LoadPbrSet goes through the cache. A quality swap becomes "invalidate everything and reload", which fixes C154 for props, fixtures, doors and runes. One set is then resident once (C155, C156, C161). The model cache should also drop CPU-side images after upload. The thumbnail colour fix (C158) belongs here too: a linear view for the 2D pass.

**6. A deferred-release queue (`GraphicsDevice::Retire(ComPtr<ID3D12Resource>)`, freed once the fence passes).**

This would retire most of the `WaitIdle` sites (C167) and the drain amortising in ThumbCache. It changes a documented rule, so do it only after steps 3 and 5 have reduced the number of owners.

**The display-lifecycle issues (C196 to C201) are independent and can go alongside any step.** Two changes cover several of them:
- Save the adapter by vendor id, device id and description instead of LUID, and the monitor by device name.
- Declare per-monitor DPI awareness in the manifest.

They also need a dev command that reaches `OnVideoApply`. Today no check exercises that path.

### Issues that matter most

- **C178:** Stale shadows survive door openings, item lifts and kills, because the cube cache only notices living monsters.
- **C163:** The editor map of a 128x128 level needs about 5.5 MB of sprite vertices, and SpriteBatch's fixed 4 MB arena aborts the game.
- **C194:** Alt+Tab out of exclusive fullscreen skips ResizeBuffers when the sizes match, so the next Present aborts.
- **C193:** `BuildDungeonMeshes` frees chunk meshes with frames still in flight when called from the eval reset and the arena.
- **C181:** The fire-light loop dereferences a null `PushLight` result once the 256-candidate ceiling is reached.
- **C189:** The palette cache is keyed by a temporary animator's address, so two monster-icon bakes in one frame can share a pose.
- **C154:** The quality hot-swap leaves every prop, fixture, door and rune texture at the tier it was first loaded at.
- **C195:** A GPU failure aborts without the HRESULT, the device-removed reason or DRED data, against the "every crash leaves evidence" rule.
- **C179:** Kind-to-parts is built three times, and the previews already omit parts the world draws.
- **C170:** About 20 C++/HLSL mirrors with three checks, and shadow.hlsl's copy has already drifted.
- **C168:** The offscreen bake rig is raw D3D12 in Game, written three times beside ModelPreview.
- **C167:** GPU lifetime depends on about 22 caller-side drains, and one was missed (C193).

## 6. Globals (brief A)

### Where things stand

The sweep covered every .cpp and .h under src/ and tools/. The codebase is disciplined about globals. It has no singletons, no static data members, no mutable inline header variables, no global RNG, and no function-local scratch caches in Game, Graphics, UI, Animation, Audio, Platform, Assets or Main. Spells, effects, fonts, settings and the thread manager are all owned objects.

There are 64 mutable non-local objects in all. Forty of them are in Core, and they are global by design:

- AllocTrack replaces `::operator new`, so it must have no owner and be constant-initialized.
- CrashHandler's paths are snapshotted for a heap-free crash path.
- The Diagnostics and Profile registries keep their storage outside TLS so it survives `TerminateThread`.
- DbgHelp's mutex exists because that library is single-threaded by contract.
- The log sink and the Loc table are process facts.

All of these match what CLAUDE.md documents, and none should move.

Twelve objects are unnecessary, in four groups. Each one exists for convenience in a place that already had an owner:

- **The UI clip:** src/UI/Widget.cpp:19-20 (`g_clip`, `g_clipRect`).
- **Shared icons:** src/Game/AssetUtil.cpp:90-100 (five owners) plus the `ui::ControlIcons` registry at src/UI/ControlIcons.cpp:8.
- **Save filter:** src/Game/SaveGame.cpp:847.
- **D3D12 message throttle:** src/Graphics/GraphicsDevice.cpp:424-426.

### Against the brief

Michael asked whether any globals are unnecessary. The answer is yes: twelve, and one of them is a real bug.

**The clip walk (C208).** Widget.cpp:120 and :145 restore the outer clip only when `g_clip != outer`. Every push writes the same address (`&g_clipRect`), so when a ScrollArea sits inside another ScrollArea:

- The inner clip never pops.
- Siblings drawn after the inner area are clipped to its rectangle.

The walk is also not exception-safe. Game keeps running after a throw (the frame try/catch allows up to 10 in a row), and every UIContext shares this one global. So a throw under a clipping area leaves every context clipped on every later frame. `ScopedClip` gets the same job right (Widget.cpp:36-47), so the walk should use the same pattern.

**The shared icons (C205).** Sharing one close icon cut 15 SRV slots to 1, and CLAUDE.md records that as a deliberate decision. Keep the sharing. The storage is the problem. Because the icons live in globals:

- Lifetime depends on a manual `ReleaseSharedIcons()` call in ~Game (src/Game/Game.cpp:502).
- Correctness depends on loading them before any dialog is built. Breaking that load-order rule already cost the character sheet its close icon for months.
- The UI library reads a registry that the Game layer fills, which is a dependency running against the layering.

**The save filter (C207).** Game makes a policy decision (src/Game/Game.cpp:441) and stores it inside SaveGame, where every `ListSaves()` caller picks it up without knowing. It is set once and never re-evaluated, so after a world switch it still filters by the launch world. The two editor sweeps (src/Game/Game_Editor.cpp:920 and :1156) already filter by `slot.world` themselves.

**The D3D12 throttle (C206).** The throttle state is file-scope even though `RegisterMessageCallback` has a context parameter, which currently receives `nullptr` (src/Graphics/GraphicsDevice.cpp:469).

### Recommended direction

1. **Fix the clip walk, then move it.**
   - Restore the outer clip whenever this widget pushed one, using a bool or the same RAII guard `ScopedClip` uses. That fixes the nested-clip bug and the exception case together.
   - Then move the clip state onto `UIContext`. Each walk is already one context's pass, and only four `ScopedClip` sites would need the context passed in.
2. **Give the shared icons one owner.**
   - Use a small `SharedIcons` object, owned by Game and loaded before `BuildStaticUi`, and pass it by reference to GameUI and the dialogs.
   - Hand `ControlIcons` to the controls through `UIContext`. `DropDown::DrawSelf` already has the context (src/UI/Controls.cpp:953).
   - Member order then handles the release, and the load-order rule becomes a constructor-order fact the compiler enforces. `ReleaseSharedIcons` goes away.
3. **Pass the save filter in.** Change the signature to `ListSaves(std::string_view world = {})` and keep the policy in Game, derived from the current project rather than latched at startup.
4. **Move the throttle onto GraphicsDevice.** Make it members and pass `this` as the callback context. The polling path (GraphicsDevice.cpp:497) already runs on the device.

Two other results from the same sweep are filed outside this section:

- Direction and archetype name tables duplicated as function-local statics. Two of the archetype arrays are indexed by the enum, so an archetype missing from one array reads past its end.
- `uitree`'s inspector allocates in guarded frames when it is on and the console is closed, so `allocguard strict on` then aborts.

### Issues that matter most

These four issues are the whole of this section's index.

- **C208** Real bug: a nested clip never pops, and one throw leaves every UI context clipped for the rest of the process.
- **C205** GPU resources in namespace scope with a manual release and a load-order trap that has already broken the sheet once. The UI library also reads a registry that Game fills.
- **C207** A hidden process-wide policy inside `ListSaves` that is stale after a world switch. The two editor callers already filter for themselves.
- **C206** File-scope throttle state while the callback's context goes unused. A cleanup with no current bug.

## 7. Allocation is guarded (brief A)

### Where things stand

The guard is sound. `src/Core/AllocTrack.cpp` counts per thread without locks. The registry owns the slots, so a hard kill cannot leave a dangling pointer. Every `operator new` form is replaced, and `allocpoke` with `AllocTest.ps1 -SelfTest` proves the guard can fail. There are about 15 `alloc::Excused` sites. Nearly all are fair: crash and log reporters, first font bakes, `GameSettings::Save` on a click.

The hot loops keep the rule well. Effect lists are reserved to `kMaxEffects`. Lights, shadows, particles, sparks, pending bolts and the glass queue all use fixed storage. Messages go through `loc::View` into the MessageLog ring, and tooltips, use menus and the details dialog are allocation-free. Ownership is mostly clean too: the only raw `new`s are two factories that go straight into `unique_ptr` (src/Graphics/Mesh.cpp:53, src/Graphics/Texture.cpp:46), and COM sits in ComPtr.

What is not sound falls into four groups:

- **Event paths in armed Playing frames that no AllocTest mode reaches:**
  - the exit-stair prompt and the pit-fall latch (C210);
  - niche reveal on a lever (C211);
  - lifting a potion id longer than 15 characters (C218, src/Game/Game.cpp:2580);
  - a member's first status effect (C219);
  - quest and flag hooks (C212).
- **Blind spots in reporting.** The first armed frame after a disarm records no stacks (C214, src/Core/AllocTrack.cpp:216). `log::` templates format without excusing themselves (C215, src/Core/Log.h:39-50).
- **Growth that is lazy, or bounded only on paper:** C221, C222, C227, C228 and C220.
- **Lifetimes.** C232 is a mouse-reachable use-after-free. C233, C234 and C235 are editor state that survives a world switch or a reload.

One more thing belongs here but is filed in the AI/threading section: `Brain::FindPath` builds a `std::queue` on every think (src/Game/MonsterAI.cpp:199). Rest forces lockstep AI, so that queue is built on the main thread inside armed frames.

### Against the brief

CLAUDE.md says "an allocation in a settled frame is a bug, full stop". The code keeps that rule in the frames AllocTest drives. But every mode warms up its event before the measured window opens. The 120-frame warm-up treats two kinds of "first time" the same way:

- a first time for the process (a PSO, a font size), which happens once;
- a first time for one instance, or a new high-water mark (a 33rd spell row, the 65th floor drop, a member's first effect), which keeps happening at random points in real play.

The guard cannot tell these apart, so the second kind goes unchecked. CheckAll runs only the default mode and `-Hand` (C213). Nothing drives a party swing, levers, rest, the exit stair or monster chases.

Two places conflict with documented decisions:

- **The overlay (C209).** CLAUDE.md deliberately leaves the map overlay unarmed. But the world keeps simulating under the player's M map, so a whole fight can run there unchecked. The editor exemption is reasonable. Player mode is ordinary play and deserves a second look.
- **Two policies where CLAUDE.md promises one (C212, C229).** CLAUDE.md says the event-frame exemption is gone and only one policy is left: reporters excuse themselves. The comment in `OnItemFound` (src/Game/DungeonWorld_Combat.cpp:596) brings the exemption back. `Font::EnsureGlyph` excusing its glyph misses is a second policy nobody wrote down.

On ownership, `UnloadWorld` is a hand-kept list of borrowers (C236), and in-world kind reloads have no equivalent. That gap is what produces C232.

### Recommended direction

1. **Fix the lifetime bugs first.**
   - Add a content epoch that `ReloadTypeKind` bumps. Game-side previews re-resolve or drop when it moves, or they hold `shared_ptr` keep-alives the way MonsterKind already does (C232).
   - Move world-scoped Game and editor state into one `WorldSession` that dies just before `m_world.reset()` (C233, C234, C236).
2. **Close the known armed-frame allocations.**
   - Reserve one item-id capacity (the project's longest id) into every slot, cursor and scratch string (C218).
   - Warm the effect-strip Repeater to `kMaxEffects` (C219).
   - Pre-build the exit prompt, or disarm on it (C210).
   - Reveal a niche without a chunk rebuild in play (C211).
   - Pre-reserve the flag store and delete the exemption comment (C212).
3. **Make reporting part of the guard itself.**
   - Decide stack capture from the warm-up counter, not from last frame's verdict (C214).
   - Have the `log::` templates excuse their own formatting, so the one documented policy is enforced in one place (C215).
4. **Make reserves check themselves.** A container that grows past the capacity it promised should log its own site in an armed frame, whatever the warm-up. Then add AllocTest modes for a party swing with a fumble, levers and niches, a long-id lift, the exit stair, and rest with a monster chasing. Add a `-Cold` mode that arms from the first Playing frame after load, and put more modes into CheckAll (C213).
5. **Bound the caches.** FontLibrary should bake only at settled sizes or evict old ones (C221). Free a model's embedded image bytes once they are uploaded, which is roughly 188 MB (C222).

### Issues that matter most

- **C232** A palette Save on an open monster inspector draws freed meshes and skeletons: a use-after-free.
- **C210** The exit-stair prompt and the pit fall allocate in frames that stay in Playing, so the transition disarm never catches them.
- **C211** A lever that reveals a niche runs a GPU WaitIdle and a mesh rebuild mid-play. This repeats the documented Revision trap.
- **C218** Lifting crypt1's long-id potions allocates. Today's move-without-allocating depends on every id fitting in 15 characters.
- **C219** A member's first status effect builds widgets during play, even though the effects list itself is reserved for exactly that reason.
- **C214** A violation on the first armed frame after a disarm is counted but leaves no stack anywhere.
- **C233** Patrol-route and inspector state survive a world switch, so Save and Delete act on the next world's monster.
- **C212** Quest and flag hooks allocate, and a code comment revives the retired event exemption.
- **C213** Most gameplay paths are never driven through the guard, and CheckAll runs two modes.
- **C215** Logging from a guarded frame counts as a violation. "Reporters excuse themselves" is a convention at each call site, not something the code enforces.
- **C221** Resize-dragging a panel bakes a permanent font atlas, with a GPU drain, for every pixel size it passes through.
- **C222** Every model's embedded image bytes stay in CPU RAM for the life of the world.

## 8. Repetitive and redundant code (brief A)

### Where things stand

The big shared pieces are in good shape. There is one damage pipeline, and `ResolveSurfaceVariant` is the single surface resolver. The roster widgets are shared through `RosterMember`, doors spawn through one `SpawnDoor`, `devargs::Need` handles every arity error, and `ThumbCache` serves both pickers. Settings, catalogs and the Balance dialog are driven by tables (`kThemeFields`, `CatalogSchema.h`). Where the code has a home for a rule, it uses it.

The duplication sits at the small end, in utilities that have no home:

- **Text helpers.** `src/Core/StringUtil.h:6` holds only Widen and Narrow. Trim, split and lowercase have been rewritten in about 20 places (C238, C239), and Style's Trim already behaves differently.
- **Number parsing.** Dev commands use unchecked `atoi`/`atof`, while strict `from_chars` lambdas are retyped record by record in `src/Game/DungeonMap.cpp:143` (C240).
- **Member index.** About 22 commands parse it by hand (C241).
- **Grid code.** `src/Game/Entity.h:41` already has `DirDX`/`DirDZ`/`DirToken`, but direction tables are declared again across files (C242, C243). The first-solid-wall scan appears six or more times (C251). What blocks a square is coded three times, for the party, the monster slot and the AI grid (C255).
- **Mesh and world.** The decoration world transform is rebuilt at seven sites (C257). The five-lambda mesh-builder call is copied three times (C254), and about ten AABB loops are hand-written (C252).

Some copies have already drifted. The one real bug is C253. `BakeNodeTransform` at `src/Game/DungeonWorld_Load.cpp:1133` is copied inline at `:1850` and in `DungeonWorld_Models.cpp`. Every copy sends normals through the plain node matrix instead of its inverse-transpose, and none flips the triangle winding (the order of each triangle's corners) for a mirrored node. On top of that, the multi-material cull radius ignores the node transform. Two FNV-1a hash copies also use a different offset basis from the others (C244).

There is also a long tail of dead code (C278 to C291). C281 matters most: `ResetThisThread` has no callers, but it would corrupt the allocation verdict if anyone called it. C290 is a stale `preview` command that keeps a render branch and four Game members alive.

### Against the brief

You asked whether there is repetitive or redundant code. There is a lot of it, but almost none is a duplicated subsystem. The larger duplications this review found (the live/remote editor twins and the shared prop-kind cache) are written up in their own sections. Line count is not the cost here. Drift is: copies of one rule start to disagree, and the disagreement shows up as a lighting bug (C253), a typo that silently targets member 0 (C241), or a `timescale abc` that freezes the world (C240). That last case breaks the harness's own rule to "refuse rather than measure whatever the world held".

### Recommended direction

1. **Fix C253 first.** Put one `BakeNodeTransform` in Assets: inverse-transpose for normals, and a winding flip when the matrix determinant is negative. Call it from all seven sites, and take the cull radius from the baked bounds. Add `assets::Bounds` while there (C252).
2. **Grow Core/StringUtil into the text module.** It should cover Trim, Split, ToLower, ContainsNoCase and strict ParseInt/ParseFloat/ParseBool. Use `string_view` forms so guarded frames stay allocation-free. RollTest links Core, so the pure TUs can use it too. This settles C238, C239, C240 and most of C249.
3. **Add `devargs::Member` / `Int` / `OnOff` / `Dir` helpers.** Each refuses on a bad token the way `Need` does (C241, C273).
4. **Add one grid header.** It needs a Direction-ordered step table, `DungeonMap::FirstSolidWall` and one blocking query (C242, C243, C251, C255). The AI BFS and the combat best-slot search depend on their neighbour order: keep that order, or re-run the eval suites after moving them.
5. **Add small world helpers.** That means `DecorationWorld` (C257), one mesh-builder wrapper (C254), and one ray-sphere pick that respects `kUnit` (C258).
6. **Do one deletion pass for C278 to C291.** Run `/check-build` afterwards, since release is where an "unused" removal breaks.

C266 runs against a documented decision. CLAUDE.md says every spell is a class and spells.cat holds numeric overrides only. The classes should stay. The only real point is that firebolt's power, mana, speed and range sit in both places with the same values, so decide which side is the source of truth.

### Issues that matter most

- **C253** - Copied node bake transforms normals wrongly and keeps mirrored winding; the cull radius ignores it.
- **C241** - A typo in about 22 dev commands silently targets member 0, so a script measures the wrong member.
- **C240** - Unchecked `atoi`/`atof` turns bad input into 0; strict parsers are retyped per record.
- **C255** - Blocking is decided three ways, so the party, monster slots and the AI can disagree.
- **C281** - Dead `ResetThisThread` would corrupt the allocation verdict if anyone called it.
- **C238** - Core has no Trim or split; there are about 20 local copies and one already differs.
- **C242** - Direction tables are re-declared despite `DirToken`; compass tokens are parsed four ways.
- **C251** - The first-solid-wall scan is copied six or more times.
- **C254** - The mesh-builder call and its five resolver lambdas are copied three times.
- **C257** - The decoration world transform is rebuilt at seven sites.
- **C290** - A stale `preview` command keeps dead render state alive.
- **C267** - Literal `4` used where `party::kMaxMembers` exists.

## 9. Data, saves and editor integrity

### Where things stand

A lot of this area is sound. The block format (serialize::ParseBlocks/WriteBlocks) is small, round-trips catalogs faithfully, and has its own `catround` check. The generator in src/Game/Generate.cpp is pure and deterministic. The checker in Validate*.cpp stays pure, and the save is one readable text dialect. Catalog kinds are cached at load, so no frame looks a catalog up. `m_levelStates` and `m_worldState` survive every path, because each is stored whole.

The bugs come from a few things that are written down several times and have drifted apart:

- **Reset is hand-copied.** Seven paths keep their own list of what to clear: ResetForNewGame (src/Game/DungeonWorld_Save.cpp:58), ResetForEval (:119), BeginLevelLoad, InstallLevel, BuildArena, RestoreEditorState and RespawnFromRecords. No two lists agree. As a result, lingering blasts, monster DoTs, smashed props, rest, undo history and the cursor's item leak across a new game, a load or a level change.
- **The active level has two truths.** Monsters and decorations exist only as live instances, while doors, items and buttons are backed by records. Anything that rebuilds from records therefore loses or resurrects unsaved work. The same split produced a parallel `*Remote` editing API (src/Game/DungeonWorld_Remote.cpp), which has already drifted from the live one.
- **Catalog knowledge is restated.** CatalogSchema rows carry a `def` (src/Game/CatalogSchema.h:81) and CatalogRef kinds, but no loader reads the `def` and the rename sweep ignores the CatalogRef kinds. Defaults, references, category lists and value grammars (bool, colour, list) are each re-coded per reader, and they disagree. For example, `GetBool` reads "no" as true.
- **Level files abort the process.** DungeonMap and WorldMap stop on any parse error (DN_ASSERT aborts in every build). That includes the editor's read-only scans, so one bad line ends an editing session.

### Against the brief

The question here is whether the structure still makes sense. At the module level it does. The gap is that CLAUDE.md promises rules the code no longer keeps:

- "A delete REFUSES while anything still references the type." The sweep is a hand-kept list that misses lights, trails, door parts, flags and the `lit_as`/`drink_as` chains.
- "NEVER STASH TO READ." C307 breaks it in five places.
- `reset` is defined as "where a new game would leave it". Both paths keep the held item and the undo stack, and ResetForNewGame keeps blasts, DoTs and smashed props as well.

Raising the save floor to `kMinReadableVersion = 2` was a deliberate decision, and it stands. But SaveGame still carries the pre-floor migration shims, and CLAUDE.md still cites the v14-v25 ladder (C321). That is cleanup and a doc fix, not a design change.

### Recommended direction

1. **One runtime reset.** Put a level's transient state in one struct that is reset as a whole. This is the `m_harness = {}` pattern already at src/Game/DungeonWorld_Save.cpp:180. Every path then calls one reset, and Game clears the held item beside it. This fixes C292-C297 and C300.
2. **The schema as the single source.** Loaders read the schema's `def`. SweepCatalogRefs (src/Game/Game_Editor.cpp:757) is derived from the CatalogRef rows instead of a closed list. Add one `{catalog, id, consequence}` table for the ids the C++ names (`stairs_exit`, `wooden_door`, the `rune_` prefix, `torch_lit`); Validate reports a missing one, and rename/delete refuse.
3. **A TryParse for DungeonMap and WorldMap.** It returns errors instead of asserting. Writers verify their output with it, and the checker and browsing turn a bad file into an Issue. The documented rule that a stale type is not a soft failure stays as it is. The change is that it shows up as a message, not as a dead editor.
4. **One truth for the active level.** Either back monsters and decorations with records, or sync instances into records before any rebuild, as the stash already does for decorations. Then merge live and `*Remote` into one API that takes a level as its target.
5. **One text-parse module and one .map writer.** This covers C312-C318.

### Issues that matter most

- **C292** - Lingering gas, DoTs and smashed props survive a new game, a load and a level change, because reset is copied into seven places.
- **C311** - A type editor Save, a rename or an undo rebuilds from stale records, which drops or resurrects unsaved edits.
- **C326** - A level save writes monsters at the square they have roamed to, not their spawn, so `savemap` slowly rewrites the level.
- **C301** - The model file extension is hard-coded per loader, so an editor Import of an item or decoration makes a world that aborts on load.
- **C345** - A terrain glyph edit or delete makes WorldMap::Load assert, and the world can no longer be opened.
- **C344** - Opening a wall on a browsed level leaves wall decorations hanging on open floor, and the parser then asserts.
- **C310** - The bore (window) brush on a browsed level edits the active level instead.
- **C299** - After a random encounter, CurrentLevel stays `~encounter`, and every save on the world map is refused.
- **C366** - The title menu never gains Continue/Load after the first save, because two builders share one has-saves flag (src/Game/GameUI.cpp:355, :887).
- **C296** - The cursor's item carries into a new game and through an eval `reset`.
- **C304** - The rename/delete sweep is a closed list. Renaming `torch_lit` or a light profile strands its references, and deleting one is not refused.
- **C327** - DungeonEntities::Add can reuse a removed record's id, so per-id level-state diffs land on the wrong entity.

## 10. UI library and HUD

### Where things stand

The library in `src/UI` (about 6.6k lines) is layered correctly. It depends only on Core, Platform and Graphics and knows nothing about the game. The control tree, rem units, `ui::Stack`, `ScrollArea` and `ui::FitText` all exist and are used. The hot paths are allocation-clean where the guard looks: ContextMenu, FitText, and the Button and FloatingPanel draws.

The HUD and sheet hold no mutable globals. The sheet reuses RowPools and formats numbers into stack buffers. In GameUI, the hand menus, the message ring and the resource-bar tick allocate nothing.

What is not sound:

- **Real bugs.** A language switch leaves the party page holding dead widgets (C370). Editor wheel zoom drifts away from the cursor (C373). The armor tooltip no longer adds up (C372). Descriptions are cut mid-character (C371).
- **A bug outside the index.** `m_menuHasSaves` is written by both the title list and the pause list (`src/Game/GameUI.cpp:355`, `:887`). Pausing after a first save sets it to true, so `RefreshMenuEntriesIfDirty` (`:2103`) skips the title rebuild and the title shows no Continue or Load.
- **Oversized files.** `src/UI/Controls.cpp` is 2508 lines and `src/Game/GameUI.cpp` is 2484, both past the ~2000-line rule. MapView's Render is 986 lines.
- **Copies that drift.** The tooltip box is private to the library, so 8+ sites outside it repaint it in three different looks. Button, SlotRow and MenuList each keep their own copy of the push clock and the carved-stone face. MapView and WorldMapView copy each other's chrome (C375).

### Against the brief

Several rules from CLAUDE.md are not being held:

- **"ScrollArea owns ALL scroll."** DropDown draws its own scrollbar. The MapView/MapEditor palette hand-rolls its own scrolling, text field, checkbox and dropdown. The dev console scrolls by hand and leaves its hit rects live after they scroll out of view (C379).
- **Rem except hairlines.** ContextMenu and MenuList draw with raw 10 px and 16 px offsets (C382).
- **One resolver, one palette.** The player-map tint reads the raw variant instead of `ResolveSurfaceVariant` (C376). Map inks live as literals outside `MapColors.h` (C378).
- **Steady-state allocation.** `SteadyStateFrame` excludes the whole player M-map, although opening it is ordinary play, and its frames allocate every frame. The party bar's effect-icon Repeater is never warmed, so a member's first effect builds widgets in a guarded frame, and AllocTest's warm-up hides it.
- **FontLibrary's own contract** ("settled sizes only, never evicted"). Panel resize drags request a new pixel size every frame. Each new size bakes an atlas, drains the GPU and is kept forever.
- **Stale docs.** CLAUDE.md still describes `kDialogTitleBandH`, `DialogTitleBand` and `FitDialogTitle`, which no longer exist. Only `DialogTitleFont`/`DialogTextFont` remain (`src/UI/Controls.cpp:2495`), and nothing calls them.

### Recommended direction

1. **Fix the bugs first.**
   - C370: rebuild the party page in `RebuildForLanguage`, or defer the rebuild while the page is up.
   - C373: compute the zoom through `ComputeTransform`'s fit (`src/Game/MapView.cpp:304`) instead of the inline copy at `:609`.
   - The menu flag: split it into one flag per list.
   - C372: restore the avoidance-skill row.
   - C371: show the description through `loc::ViewKey` rather than copying it into a 255-byte `loc::Line` (`src/Core/Loc.h:77`).
2. **Share the map chrome.** Move the toolbar band, icon buttons, tooltip and label trim (through `ui::FitText`) into one module that MapView and WorldMapView both call. That closes C375 and C374 and removes the byte-level UTF-8 trims. Do this before any move of the palette into the control tree, which is a much larger job.
3. **Let the library own its repeated looks.** Make the tooltip box public. Use one push-clock and cut-stone helper in Button, SlotRow and MenuList. Put DropDown's list on `ScrollArea`. After that, split `Controls.cpp` by control family and `GameUI.cpp` by concern (menus, settings, HUD wiring, item use).
4. **Close the allocation gaps.** Warm the effect-icon Repeater. Bring the player map under the guard. Give FontLibrary a settle rule: draw at the nearest settled size during a drag and commit the new size on release.
5. **Do a hygiene sweep.** Covers C376, C377, C378, C382 and C381. Also remove the dead API (`ui::TextOutput`, the panel-fraction `AddCloseButton`), make the Widget clip reset per walk (RAII), and correct the stale CLAUDE.md paragraphs.

### Issues that matter most

- **C370** A language switch on the party page leaves `PartyCreationPage::Tick` writing through dangling widget pointers (`src/Game/GameUI.cpp:1292`).
- **C373** Editor wheel zoom ignores the edge-handle margin, so the map slides out from under the cursor on every step (`src/Game/MapView.cpp:609`).
- **C372** The armor tooltip lost the avoidance-skill row, so an unarmored member's breakdown no longer sums to Roll (`src/Game/CharacterSheet_Inventory.cpp:425`).
- **C371** Item, spell and effect descriptions are cut at 255 bytes, which can split a UTF-8 character (`src/Game/ItemDetailsDialog.cpp:266`).
- **C379** The THREADS/HEALTH buttons in the dev console still respond after they have scrolled out of view.
- **C375** MapView and WorldMapView each carry their own toolbar, trim and size code, and the copies already disagree (22 px vs 16 px button floor).
- **C374** In MapView, a toolbar tooltip stays drawn under the dialog its button opened.
- **C380** Clicking a health mark shows the window's newest event, not the one clicked.
- **C376** The player-map tint bypasses the one surface resolver.
- **C382** Raw pixel padding in ContextMenu and MenuList breaks the rem rule (`src/UI/Controls.cpp:1317`).
- **C378** Map inks are scattered as literals outside `MapColors.h`.

## 11. Core and platform

### Where things stand

Most of this layer is in good shape:

- **Layering is clean.** No Core file includes a header from another library.
- **Files are small.** The largest is src/Core/Profile.h at 641 lines.
- **Core's globals are justified.** The allocation counters, the crash-path fixed buffers, the log sink, the language table and the lazily built paths all have to work without an owner.
- **The rest of the slice has no mutable globals at all.** That covers Platform, Audio, Animation, Assets and Main.
- **Steady-state frames are allocation-free on these paths.** The voice pool is filled at load, Input's typed-text buffer keeps its capacity, and the Animator copies into vectors that are already sized.
- **The diagnostics ring is sound.** Its lock-free design and absolute claim index are well built.

The weak spots are where Core and Platform meet the operating system:

- **Crash order.** The crash path does not run in the order its comments promise.
  - `diag::Record` logs synchronously (src/Core/Diagnostics.cpp:404).
  - So `FaultFilter` formats, allocates and takes the log mutex before `WriteDump` runs (src/Core/CrashHandler.cpp:99-100).
  - It then logs the same crash a second time.
- **Stack overflow.** Nothing calls `SetThreadStackGuarantee`, so the filter runs on the exhausted stack.
- **ThreadManager lifetime.** `Manager::Get` hands out a raw `Worker*` after releasing `m_mx` (src/Core/ThreadManager.cpp:272). `Reap` can free that Worker while the supervisor is still reading it, or in the middle of a Restart.
- **Typed text.** `Input::OnChar` keeps only the low byte of each UTF-16 unit (src/Platform/Input.cpp:31). U+010D arrives as `'\r'`, which is `kTypedEnter`, so typing "č" submits the console line.
- **Headless.** `Window::SetWindowed` always passes `SWP_SHOWWINDOW` (src/Platform/Window.cpp:68), and the Game constructor calls it on every launch.
- **Alt+F4.** `WM_SYSKEYDOWN` returns 0 without reaching `DefWindowProc` (src/Platform/Window.cpp:126), so Alt+F4 never fires.
- **Asset formats are spelled out more than once.**
  - The DDS header layout is written twice, by the reader and the baker's writer. That is the drift that hid BC7 for months.
  - glTF node transforms are applied by five hand-copied loops.
  - OBJ faces written `v//n` lose their normals.

### Against the brief

The hygiene rules mostly hold here: layering, file size, globals, RAII and main-thread allocation. The gap is that several documented contracts in CLAUDE.md are not what the code does:

- **"Record, then dump, then log"** is not the order that runs (C385).
- **"Stack overflow"** is listed as covered but is not (C388).
- **"Headless steals no focus"** is broken, and the multi-session harnesses depend on it (C391).
- **"No state can ever be unquittable"** is broken.
  - Console commands are gated off during a load (src/Game/Game.cpp:2015), and Esc never quits.
  - In Borderless or Exclusive mode there is no close button, and Alt+F4 is swallowed.
  - So a load in those modes can only be killed from Task Manager (C392).
- **"A crash, a hang and a reboot must each leave evidence"** is only partly met. Stalls and kills are recorded without a stack, even though `WalkThread` exists for exactly that (C387).

The other hygiene problems are duplication, stale text and scope:

- **Duplication.** There are three per-thread slot registries, and one reset routine has already drifted (C389). Add the DDS layout, the glTF transform loop and a lowercase helper copied about 12 times.
- **Stale text.** Phase-era comments are out of date ("Restart will block", "~2 MB" for a table of about 20 MB), and about seven functions have no callers.
- **Allocation rule scope.** It is judged only on main-thread frames. That matches what CLAUDE.md documents. But the supervisor allocates a vector every 100 ms (C390), and worker ticks are counted but never judged, so "a settled frame allocates nothing" says nothing about the threads.

### Recommended direction

1. **Fix the platform edge first.** These are small, contained changes with visible payoff:
   - Decode UTF-16, including surrogate pairs, to UTF-8 in `OnChar`, and use the W API in `KeyName`.
   - Let `SetWindowed` and `SetBorderless` leave a hidden window hidden.
   - Pass `WM_SYSKEYDOWN` on to `DefWindowProc` so Alt+F4 becomes `SC_CLOSE`.
   - While there, give `RestartApp` the running exe name and its original arguments (C398).
2. **Make the crash path match its docs.**
   - Split `Record` into a ring-only store and a separate log step.
   - Have the handlers do: store, dump, then log once.
   - Set a stack guarantee on the main thread and in `ThreadManager::Run`. If that is not wanted, drop stack overflow from the covered list.
3. **Fix worker lifetime.** Either `Get` returns a `shared_ptr`, or `Reap` is serialized against the supervisor and against `Restart` through `controlMx`. Register the supervisor and reuse its id buffer.
4. **One per-thread slot table.** Merge the registries in AllocTrack, Diagnostics and Profile, after fixing `ResetEntry`. CLAUDE.md's separate-SeenSet decision is about stack sites, not slots, so it is not affected.
5. **One statement per file format.** A shared DDS header struct for reader and writer, glTF node transforms applied in the loader, and the OBJ `v//n` fix.
6. **Sweep the stale comments and dead functions** in the same pass.

### Issues that matter most

- **C383** - Accented or Cyrillic input becomes garbage, Enter or Backspace, and the Russian language file makes that input reachable.
- **C391** - `-headless` shows and can activate its window, which breaks the multi-session harness rule.
- **C392** - Alt+F4 is swallowed and borderless has no close button, so a load there cannot be quit.
- **C386** - `Reap` can free a Worker the supervisor is using (a use-after-free).
- **C385** - Crash handlers allocate, lock and log twice before the minidump.
- **C388** - Stack overflow is claimed as covered but the filter has no stack to run on.
- **C394** - OBJ `v//n` faces load with no normals, in both the game and the baker.
- **C387** - Stalls and kills are recorded with no stack, though the probe exists.
- **C398** - `RestartApp` drops `-project`, quits even when the relaunch fails, and lets the child truncate the log.
- **C389** - Diagnostics `ResetEntry` misses the throttle fields; this is the first drift among three near-duplicate registries.

## 12. Harnesses and tools

### Where things stand

A lot of this is sound. The C++ test tools link the shipping code, not a copy. Each prints one verdict line and exits with a status code. The PID rule holds everywhere: every driver launches with `-PassThru`, posts input to its own window and kills by process object, and only `docs/drive.ps1` takes screenshots, through PrintWindow. SpellTest's self-test is the right model, because only the checks that depend on a cast are allowed to fail. PipelineTest and AllocTest refuse to PASS unless the run actually did work. LevelBuildTest edits a scratch copy and passes `-project`, which is the safe pattern. The AssetBaker has no mutable globals. Most of the Python-to-C++ mirrors still agree: bar-frame aspect, UI frame insets, etch and icon names, rune colours, portrait vocabulary and stone families.

The weak spot is checks that look like they run but don't:
- **Bc7Test.** `LoadBaseline` (tools/Bc7Test/Main.cpp:355) reads `name >> psnr`. The first `#` comment line in `tools/bc7-baseline.txt` makes the double parse fail, which ends the loop. So every run compares against an empty map and the quality gate never fires.
- **RollTest** is cited in CLAUDE.md as the check for about a dozen pure rule files, but CheckAll (tools/CheckAll.ps1:55-152) never runs it. The same goes for EditorTest, WorldTest, LevelBuildTest and Eval's `-SelfTest` equivalence checks. 16 of AllocTest's 18 modes are not wired in either.
- **ThreadStress** never sets agent targets (tools/ThreadStress/Main.cpp:113). Every monster paths to (0,0), which is a wall, so the "reachable" phases are not reachable.
- **Self-tests in RollTest, ProfileTest, InGameTest, HealthTest and TypingTest** count any failure as success. RollTest's injected 90-sided die never even reaches `ResolveAttack` (tools/RollTest/Main.cpp:619).

On the baker side, worn-block relief, wear and seed come from four sources that disagree (tools/AssetBaker/ModelBaker.cpp:2554). They are also baked into per-set files, so two catalog types that share a set overwrite each other's geometry. `TextureBaker.cpp` is dead code. `ModelBaker.cpp` (2,677 lines) and RollTest's single `main()` (about 2,500 lines) are both past the file-size rule.

### Against the brief

CLAUDE.md's standard is "CHECKED, not assumed": every harness must be able to fail, and `-SelfTest` proves it. The tooling meets that standard for the checks CheckAll runs (diag, pipeline, spells, alloc), with the self-test caveat above. It misses it in three ways:
- **Gates that cannot fire:** Bc7Test and ThreadStress.
- **Checks never wired in:** RollTest, EditorTest and WorldTest are all described as checked.
- **Stale builds:** nothing notices a stale exe. `-Only` skips the build row (C426), and check-eval.md admits a stale exe "reports plausible numbers".

The CLAUDE.md claim that "untouched types bake as before" is contradicted by C406/C407. Separately, `FetchTextures.ps1` still reads the dead `assets/maps/level1.map`, so a fresh clone fetches only 2 of the 12 sets the crypt levels use. That breaks the "regenerable alternative" CLAUDE.md promises.

### Recommended direction

1. **Make the existing gates honest.** Each change is small:
   - Fix `LoadBaseline` to skip comment lines.
   - Have each self-test name the check it expects to fail, as SpellTest does.
   - Treat an unreadable log as a FAIL in DiagTest.
   - Have InGameTest demand a line that only the opened screen logs.
   - Have HealthTest wait for a line the game logs after the level load finishes.
   - Give ThreadStress real targets.
2. **Wire in what CLAUDE.md calls checked.** RollTest takes seconds, so put it in the quick tier. Put EditorTest, WorldTest, LevelBuildTest and `Eval.ps1 -SelfTest` in the full tier. Add a stale-exe guard that compares the exe's time with the newest source, or have `-Only` build first.
3. **Move the harness plumbing into one shared module.** Make one dot-sourced `.ps1` and one Python module covering launch, the window by PID, waiting for a fresh log line, the exit code, a per-worktree run lock and muting. Move EditorTest and WorldTest onto LevelBuildTest's scratch-copy model so a killed run can't empty `dungeon-demo` again.
4. **Report declined dev commands with Refuse, not Print.** About 20 paths in Game_DevCommands and Game_DevWorld decline with Print, so the eval runner never counts them. While in there, fix `rest until` (C444), the StepWorld wipe (C443) and the batch re-emit (C445).
5. **Give the worn bake one authority.** The catalog should win, with the baker's per-kind values only as fallbacks. Worn files should be keyed so that two types sharing a set can't overwrite each other. Also delete `TextureBaker.cpp`, and have `runes` write a `.dds` as well.
6. **Fix the fetch scripts.** FetchTextures should read the project's palettes. FetchModels and FetchAnimLibrary should run `AssetBaker model-images`.

### Issues that matter most

- **C417** Bc7Test's baseline loader reads nothing, so a BC7 quality regression can never fail the run.
- **C419** Self-tests accept any failure, so they don't prove the injected fault was the thing caught.
- **C426** No harness notices a stale exe, and `-Only` never builds, so a PASS can be for old code.
- **C418** ThreadStress paths every monster into a wall, so its reachable-path phases test nothing.
- **C420** DiagTest's log checks pass when the log is missing or unreadable.
- **C442** About 20 declined dev commands Print instead of Refuse, so eval scripts never count those failures.
- **C431** EditorTest restores the real project with a delete followed by a copy, and a kill between the two has already emptied `dungeon-demo` once.
- **C430** Eval and the Python judges ignore exit codes and share one `dungeon.log`. A dead headless run can compare a log with itself and pass.
- **C402** FetchTextures reads the dead `level1.map`, so a fresh clone can't rebuild the shipped levels' textures.
- **C406** Worn-block parameters have four disagreeing sources, so an editor save, `models` or ReplayImports silently reshapes other types (see also C407 and C409).
- **C410** `runes` writes only PNGs, and the loader prefers a stale `.dds` without checking freshness.
- **C427** InGameTest's coverage label shows the command ran, not that the screen opened, and substring matching lets one label satisfy another.

## 13. Documentation drift

### Where things stand

Most of CLAUDE.md is accurate. The review checked about 400 names it mentions (constants, files, console commands, AllocTest switches, ini keys, catalog fields) against src/ and tools/, and nearly all are still there with the values it gives. That includes kMaxPointLights=64, kSrvHeapCapacity=1024, kShadowSlots and its slot sizes, the 32x18 light tiles, the prime AI cadences, the diagnostics ring sizes and kMaxPackSlots=16. Everything from the recent branches is correct: lighting, transparency, party creation, tool refinement and ui-panels.

The drift is in older paragraphs that later branches rebuilt without editing. New work was added as a new section and the old one was left alone. So a session reading top to bottom meets the stale version first:

- **Facing.** CLAUDE.md:72 says facing +1 is on-screen LEFT. The code says the opposite: src/Game/Party.cpp:281 "+1 is clockwise (the on-screen RIGHT direction)". This sits in the "memorize, they bite" list.
- **Save versions.** CLAUDE.md:187, :347, :392 and :538 quote save versions v14-v25. The format was reset, and src/Game/SaveGame.h:326 sets the floor at 2. The comment above it contradicts itself: it says the floor is 1, then that it is 2.
- **Settings and dialogs.** The settings-page Flow helper and uiScale (CLAUDE.md:1069) and the dialog title-band helpers (kDialogTitleBandH, DialogTitleBand, FitDialogTitle) no longer exist. Nothing in src/ matches them.
- **Smaller items.** Damage types are a catalog of eight, not "seven". floorfeatures.cat is now surfacefeatures.cat. The AssetBaker full bake no longer makes title art or portraits. assets/maps/level1.* is described as live (CLAUDE.md:1001) but is dead.

The design docs have the same problem in their status lines:

- docs/effects.md:3 still says "PLAN. Nothing built yet".
- docs/health-and-healing.md:3 says "PART BUILT" above a table where every row is built.
- docs/ai.md:82 says "exactly two modes", with no archetypes.

docs/ARCHITECTURE.md has drifted furthest of the core docs:

- It says assets are copied beside the exe.
- Its UI row lists TextOutput.
- Its frame flow names functions that are gone.
- Its rule 3 is broken by Graphics' BarKind and emissiveGroove. CLAUDE.md documents both as deliberate, so it is the rule that needs restating, not the code that needs moving.

docs/DIAGRAMS.md still shows the app from before world-on-demand.

Comments in the code drift the same way:

- src/Game/DungeonWorld.h:476 still says SpendStamina feeds the deleted vit_exertion.
- src/Game/Game.h:30 says the character sheet freezes the world, which contradicts Michael's 2026-09-28 rule.
- src/Game/Game.h:34 says assets load from next to the exe.

One piece of drift is a real tool bug, filed elsewhere. tools/FetchTextures.ps1:108 still collects materials from `assets\maps\*.map`, so the documented texture rebuild picks 7 sets instead of the 54 the project uses.

### Against the brief

CLAUDE.md is the handoff between sessions, so the cost of a wrong line is a wrong first move by the next session. Three examples:

- A session that trusts the facing note writes strafe code backwards.
- One that looks for FitDialogTitle finds nothing.
- One that starts an AI or effects refactor from docs/ai.md or docs/effects.md starts from older code.

The project's own rule fits here: a fallback that hides its own firing is how the DDS bug survived. A stale paragraph does the same thing. It reads as authoritative and nothing flags it.

### Recommended direction

1. **One correction pass on CLAUDE.md.** Fix the lines that teach the wrong thing: facing, the save ladder, Flow/uiScale, the dialog helpers, damage types, surfacefeatures, the AssetBaker bake, the level1.* paragraphs and the MSVC noexcept explanation. Delete or rewrite them. Do not add "superseded" notes beside them.
2. **Fix the status headers of effects.md, health-and-healing.md and ai.md.** These are where a refactor of the hit pipeline or AI behaviour would start.
3. **Rewrite ARCHITECTURE.md's build section, module table, rule 3 and frame flow.** Then either redraw DIAGRAMS.md or mark it as history.
4. **Sweep code comments for deleted names.** For example: vit_exertion, ShadowSlotCache, "two PSOs", "live count is 7", phase-era banners.
5. **Make it a check, in house style.** A small script could pull the backticked names out of CLAUDE.md and docs/ and report any that grep cannot find in src/, tools/ or assets/. That would have caught most of this section.
6. **Branch habit.** When a branch rebuilds a system, edit the old paragraph in the same commit. Do not only add a new one.

### Issues that matter most

- **C473** - The facing rule in the "memorize" list is reversed relative to the code.
- **C482** - The usage lines in FetchTextures.ps1 and FetchModels.ps1 show the `-File a,b,c` form that CLAUDE.md warns imports nothing.
- **C466** - Game.h comments contradict two documented decisions: the sheet is not a pause, and nothing is copied beside the exe.
- **C453** - CLAUDE.md and combat.md say a blow on a downed member kills. But IsAlive() (src/Game/Character.h:159) skips the downed in melee, bolt lanes and blasts, so that route never fires. Decide whether the doc or the code is the intent.
- **C455** - CLAUDE.md and Controls.h name dialog title helpers that are deleted.
- **C456** - The CLAUDE.md settings-page section describes the removed Flow helper and uiScale.
- **C451** - effects.md says "nothing built", and Effect.h still carries phase-plan comments.
- **C454** - ai.md describes a two-mode AI with no archetypes.
- **C458** - ARCHITECTURE.md's module table, rule 3 and frame flow are out of date.
- **C464** - Comments still describe the deleted vit_exertion creep, against RESOURCES rule (1).
- **C481** - The check-* skill files describe old harnesses, and sessions run these.
- **C460** - CLAUDE.md says seven damage types; the catalog has eight.

## Order of work

Each phase is judged before the next one starts. Each phase also updates its CLAUDE.md paragraph in the same commit. Sizes: S is one or two commits, M is a handful, and L is many, landed in steps.

### Phase 0 - fixes that need no refactor

Goal: stop the live bugs and rule-breaks. Risk is low. The groups run smallest first, and each can land alone.

- **0a. Judges that cannot fail (S).** C417, C418, C419, C420, C426, C427, C429, C430, C431, C442.
  - Add RollTest to CheckAll's quick tier.
  - Add EditorTest, WorldTest, LevelBuildTest and `Eval.ps1 -SelfTest` to the full tier (part of C213).
  - This group goes first because every later phase is judged by these checks.
  - Judge: `/check-selftest`, with each self-test naming the check it expects to fail, as SpellTest already does.
- **0b. Docs that teach the wrong thing (S).** C455, C456, C460, C462, C463, C473 (the facing note is reversed), C480, C482.
- **0c. Combat and AI (S).** C0, C1, C5, C9, C10, C11, C36, C40, C49, C52, C62, C65, C66, C71.
  - C0: one `defense::Mitigate`, plus a RollTest case where soak is above the roll.
  - C1: a local detonate in `ResolveMonsterProjectileHit` (src/Game/DungeonWorld_Combat.cpp:2259). Phase 5 replaces it.
  - C52: a plan epoch.
  - Judges: RollTest, PipelineTest, AllocTest -Melee/-Cast/-Impact, the Eval suites.
  - Missing: an eval case where skel_magus's burst bolt hits the party.
- **0d. Platform and UI (S).** C366, C370, C371, C372, C373, C379, C380, C383, C391, C392, C394, C398.
  - Judges: TypingTest (it needs a non-ASCII case), InGameTest, `uioverlap`.
- **0e. Rendering and the device (S-M).** C163, C178 (a caster revision), C181, C189, C193, C194, C195, C253 (one `BakeNodeTransform` in Assets).
  - Judges: AllocTest -Lights/-Glass, `geomhash`, a picker survey.
- **0f. Crashes, races and lifetimes (S-M).** C63, C69, C208, C232 (a content epoch), C233, C385, C386, C388.
  - Judges: HealthTest, DiagTest, `/check-threads`, EditorTest.
- **0g. Allocation (S-M).** C210, C211, C212, C214, C215, C218, C219, C228.
  - For C212, also delete the code comment that brings back the retired event exemption.
  - Judge: AllocTest default. Most of these paths are not driven until phase 2.
- **0h. Data and editor (S-M).** C80, C100, C101, C102, C296, C299, C301, C310, C326, C327, C344, C345, C351, C353.
  - Judges: EditorTest, WorldTest, InGameTest.

### Phase 1 - build walls (M, low risk)

Goal: make purity a build fact, and make rebuilds cheaper before the refactors touch DungeonWorld.

- Cut the three rules leaks (C128, C129).
- Make GameRules a CMake target that RollTest and ThreadStress link (first step of C118).
- Move DungeonWorld's nested types into `WorldTypes.h`, and PoolModelLook into its own TU (C120, C121).

There is no behaviour change. Judges: `/check-build` in both configs, `geomhash`, and unchanged `Eval.ps1 -SelfTest` output. This unlocks: the new pure headers in phases 4-6 sit behind a wall the build enforces.

### Phase 2 - shared helpers and guard coverage (M, low risk)

- **Text and parsing.** StringUtil gains Trim, Split, ToLower and strict ParseInt/ParseFloat. Add `devargs::Member/Int/OnOff/Dir` (C238, C239, C240, C241, C249, C273).
- **Grid.** One step table and `DungeonMap::FirstSolidWall` (C242, C243, C251). Keep the BFS neighbour order, or re-run the Eval suites.
- **AllocTest modes** for:
  - a party swing with a fumble;
  - levers and niches;
  - a long-id lift;
  - the exit stair;
  - rest with a monster chasing;
  - a `-Cold` mode.

  Also run more modes in CheckAll (C213). Containers should report growth past the capacity they promised (C220, C227, C230).

Why here: phase 5 rewrites exactly the paths these new modes drive. Judges: RollTest, Eval suites, `AllocTest -SelfTest`.

### Phase 3 - one reset (M, medium risk)

Goal: close the whole class of reset bugs.

- One `LevelRuntime` struct, reset as a whole. This is the `m_harness = {}` pattern at src/Game/DungeonWorld_Save.cpp:180.
- The held item is cleared beside it.
- A `WorldSession` holds world-scoped Game and editor state, and dies before `m_world.reset()`.

Closes C115, C234, C235, C236, C292, C293, C294, C295, C297, C300.

Judge: `Eval.ps1 -SelfTest`, once its baseline prints blasts, DoTs, smashed props, the held item, undo depth and rest. Today it cannot see any of them.

### Phase 4 - one passage rule (M, medium risk: eval numbers will move)

A pure, door-aware `Game/Passage.h` says, for each square, what stops a body, sight and a flight. It is copied into the AI snapshot. Closes C43, C44, C45, C46, C53, C58, C74, C191, C255.

Judges: a new RollTest section, the Eval suites, AllocTest -Melee/-Impact. Missing: eval cases for a shot at a shut door, a blast at a door, and formation beside a brazier. Phases 5 and 6 both build on it.

### Phase 5 - the one hit pipeline (L, in steps, medium-high risk)

- **5a.** Potency moves inside `Deal`, and Magic's constants become Balance knobs (C3, C7, part of C2). S.
- **5b.** The payload says what it is (blast type, flavour, strike kind), in one flight struct with one unit (C28, C29, C30). Both the shared landing and spells as "projectile + effect" need this.
- **5c.** The `Attack` builders, `Strike` and one landing (C2, C4, C12, C13, C14, C18, C19, C33, C34, C39, C47, C48). Convert one site per commit, in this order: party melee, monster melee, bolts, throws, blasts, scorch/crackle, fumbles.
- **5d.** The Learn stage (C35, C40, C41, C42).
- **5e.** Monsters cast through `Cast(ctx)`, and pending bolts and blasts move into ProjectileSystem (C20, C23, C59).
- **5f.** The element stage, world targets and `Touch` (C6, C15, C16, C17, C21). This waits on question 4. It retires CLAUDE.md's "MonsterTarget::Wound is the one seam" for ignition.
- **5g.** `OnTick`/`OnExpire` hooks, and CastServices shrinks (C8, C22, C24, C25, C26, C50, C51). Update docs/effects.md here (C451).

Judges: PipelineTest (every route must move), SpellTest with `--selftest`, AllocTest -Melee/-Cast/-Throw/-Impact/-Hand/-Light, RollTest, the Eval suites.

Missing checks:
- A RollTest table proving both builders give the same `Attack` for the same inputs.
- A pipeline route for an element landing on a fixture.

The adapters, the ledger and the arithmetic stay as they are, which limits the risk.

### Phase 6 - AI behaviours as classes (L, medium risk)

Needs phases 4 and 5e.

1. One archetype table, static_asserted against the enum (C56).
2. Perception as data on `Agent`. Swarm, lurker and sentry become presets of Melee.
3. The worker half: `Behaviour::Decide`, tested in RollTest against a fake `IWorldView`, and a real BFS for return and patrol (C60).
4. The host half: `MonsterServices`, `OnProvoked`, `takesSide`, and a new `DungeonWorld_Monsters.cpp` (C54, C55, C57, C67, C123).
5. Editor and save (C68, C99):
   - one behaviour form;
   - `behaviour =`, with `archetype` kept as an alias;
   - a per-monster state line in the save.

   This reverses "MonsterConfigDialog owns the archetype rows". Rewrite docs/ai.md here (C454).
6. Rest pacing, pool hand-off and RNG (C64, C70, C73).
7. A Thief, as the proof.

Judges: the Eval suites, AllocTest -Melee, PipelineTest, ThreadStress (honest after 0a). Missing: eval suites for sentry, patrol, leash and kite.

### Phase 7 - dialogs on one base (L, low-medium risk)

This is independent of phases 3-6, so it can run in its own worktree.

1. Add `ModalDialog`. Rebase `InstanceInspector` on it, and bring ProjectileInspector back in (C97, C98).
2. Port the editor dialogs one per commit, AssetDialog last (C75, C81, C85, C86, C87, C88, C89, C90, C103).
3. `ModalStack` replaces Game's routing chains, with one `TickWorld` tail (C76, C77, C78, C79, C82, C125).
4. Move the game dialogs over, and add a `ConfirmDialog` (C91).
5. One preview contract, then delete the dead UI API (C92, C188).

Judges: `uioverlap` over every dialog, EditorTest, InGameTest, and AllocTest -Sheet at step 4.

### Phase 8 - graphics seams (L, medium risk)

- **8a.** A checked shader boundary (C169, C170, C171, C172, C176, C177, C203). Waits on question 3.
  - defines passed from C++;
  - an include handler for `common.hlsli`;
  - a `D3DReflect` check at startup;
  - a separate `ShadowConstants`.
- **8b.** `FrameArena` and `OffscreenScope` in Graphics (C164, C165, C166, C168).
- **8c.** One kind-to-parts function per family, and one material-source rule (C157, C179, C180, C182, C183, C190).
- **8d.** A texture cache keyed by set, resolution and sRGB (C154, C155, C156, C158, C161, C222).
- **8e.** A deferred-release queue (C167), only after 8b and 8d. Waits on question 9.

The display issues C196-C201 can land beside any step. They first need a dev command that reaches `OnVideoApply`. Judges: AllocTest -Glass/-Lights, `/check-profile`, a survey of all 89 picker tiles.

### Phase 9 - data integrity (L, medium risk)

- **The schema as the single source** (C303, C304, C305, C306, C319, C330, C333, C334, C337, C347):
  - loaders read `def`;
  - the rename sweep is derived from the CatalogRef rows;
  - a required-id table covers the ids the C++ names.
- **`TryParse` for DungeonMap and WorldMap**, so a bad file becomes an Issue, not an abort (C317, C318, C343).
- **One truth for the active level and one editing API** (C307, C308, C309, C311, C312, C313, C314). Waits on question 10.
- **One value grammar** (C315, C316).

Judges: EditorTest, WorldTest, LevelBuildTest, `catround`. Missing: a check that the sweep covers every CatalogRef row.

### Phase 10 - extractions, as features need them (L, ongoing)

Pull these out of DungeonWorld and Game:
- LightSystem (C138);
- LevelStore (from phase 9);
- EditHistory (C136);
- WorldEditor and a GameEditor target;
- DevTools (C130, C143);
- GameHud with `IWorldActions` (C140, C144);
- then the Main facade (C135).

Together these close C119 and C124. Judges: the quick tier and `/check-build`.

### Phase 11 - deletion and the doc check (M, low risk)

- Delete the dead code (C278-C291).
- Sweep the remaining stale docs (C458, C459, C464-C472, C481).
- Add a script that lists every backticked name in CLAUDE.md and docs/ that grep cannot find in src/, tools/ or assets/.
- Then run `/check-build`, because the release build is where removing something "unused" breaks.

## Questions for Michael

**1. Bugs first, or refactor first?**
Options: (a) all of phase 0 first; (b) refactor first, fixing bugs on the way; (c) only 0a, 0c and 0f first.
Recommend (a): phase 0 is mostly small, a third of it is high severity, and the refactors are then judged by checks that can fail.

**2. Do worker threads fall under the steady-state allocation rule?**
Worker ticks are counted but never judged (C62, C390).
Options: (a) main thread only, as documented; (b) workers judged too, after their warm-up; (c) workers reported on their own line, never failing the run.
Recommend (b): the BFS deque is the only known offender, and a rule that stops at the thread boundary hides the same defect on the other side.

**3. HLSL mirrors: keep "by hand", or check them?**
About 20 mirrors have only three checks, and shadow.hlsl has already drifted.
Options: (a) keep them by hand and add static_asserts; (b) pass the C++ constants as defines and share a `common.hlsli`; (c) option (b) plus a `D3DReflect` check at startup.
Recommend (c): drift becomes a startup assert, and raising the light ceiling becomes safe.

**4. How should element interactions reach world things?**
Options:
- (a) per-source world code, as documented today;
- (b) relations in damagetypes.cat plus an `OnElement` hook, with fixtures and items as `ITarget`s and a zero-damage `Touch`;
- (c) a separate reaction system beside fx.

Recommend (b): "water puts out fire" becomes one rule for every target, and the ledger still sees every health change.

**5. Split the Game library into several CMake targets?**
Options: (a) keep one target; (b) GameRules now, the rest as features need them; (c) all six now.
Recommend (b): it is cheap and makes purity a build fact. The wider split is mostly churn until DungeonWorld is cut up.

**6. What should one monster behaviour class be?**
Options: (a) one class per archetype; (b) one class per engagement style (melee, ranged, thief), with perception as data and flee, leash and patrol as shared modifiers.
Recommend (b): (a) copies the brute executor four times and still special-cases flee and leash. Either way, docs/ai.md Layer 1 is superseded, and the lurker's "relentless" pursuit should be either built or deleted.

**7. Should the player's M map be under the allocation guard?**
The world keeps running under it, so a whole fight can happen there unchecked. The editor stays exempt either way.
Recommend yes: it is ordinary play.

**8. Can a blow kill a downed member?**
The docs say yes. But `IsAlive()` (src/Game/Character.h:159) skips downed members in melee, bolt lanes and blasts, so it never happens (C453). DoTs already reach them.
Options: (a) the code is right, fix the docs; (b) blasts reach the downed too; (c) every source does.
Recommend (b): a fireball landing on a body should matter, without monsters executing the fallen.

**9. Replace "drain the GPU before overwriting" with a deferred-release queue?**
Options: (a) keep the documented rule; (b) a fence-gated `Retire` queue, after phases 8b and 8d.
Recommend (b): C193 is a drain someone forgot, and a queue cannot be forgotten.

**10. One truth for the active level: how?**
Options: (a) back monsters and decorations with records, like doors and items; (b) keep live instances and sync them into records before every rebuild.
Recommend (a): it removes the stale-record bugs at the root, and it lets the live and `*Remote` APIs merge.

**11. Where should dialog footer buttons sit?**
CLAUDE.md says right-aligned. The inspectors left-align on purpose so the preview keeps its side.
Recommend right-aligned everywhere, with the footer running under both columns: one rule the base class can enforce.
